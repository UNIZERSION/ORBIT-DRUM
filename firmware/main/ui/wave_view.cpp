// Waveform widget (see wave_view.h). One custom-drawn LVGL object:
//   * a cached RGB565 picture of the static part (frame colour, 10x4 grid, centre line,
//     waveform) rendered once per sample / size / colour change into a PSRAM buffer and blitted
//     (a straight copy on the RGB565 display),
//   * overlays drawn live on top (cheap fills / lines): region dimming, glow bands, START/END
//     markers with handle tabs, slice markers, AHDSR envelope,
//   * two children: the playhead (moved with lv_obj_set_x only when its pixel changes, its glow is
//     a few 1 px columns, no shadow) and the time label (text set only when it changes).
// Region changes invalidate only the strips around the old and new markers.
#include "ui/wave_view.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <new>

#include "core/platform.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace orbit::ui::wave {

namespace {

constexpr int kHandleHit = 18;        // px each side of a marker that grabs it
constexpr int kDragSlop = 4;          // px of movement before a press becomes a drag
constexpr uint32_t kDoubleMs = 320;   // double tap window
constexpr float kMinGap = 0.01f;      // START < END - 1 %
constexpr int kTabW = 6, kTabH = 22;  // handle tabs (top and bottom of a marker)
constexpr int kHeadW = 9;             // playhead object: 1 px line + 4 px glow each side
// time readout: plate padding, distance of the plate from the frame edge, plate top
constexpr int kTimePadX = 5, kTimePadY = 2, kTimeEdge = 8, kTimeY = 8 - kTimePadY;
const uint32_t& kFrameBg = col::wave_frame;   // #1d2022 (a dark screen on every base)
const uint32_t& kBorder = col::wave_border;    // #343638
constexpr lv_opa_t kDimOpa = 115;     // ~45 % black outside START..END
constexpr lv_opa_t kGlowOpa = col::glow_opa;     // #ff69314a
constexpr float kAmp = 0.42f;         // full scale = 42 % of the height above/below the centre

const char* const kNums[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16"};

struct Wave {
    lv_obj_t* obj = nullptr;
    lv_obj_t* head = nullptr;     // playhead child
    lv_obj_t* time = nullptr;     // time label child
    lv_timer_t* tap_timer = nullptr;
    // ---- cache
    uint16_t* px = nullptr;
    int cw = 0, ch = 0;           // cached picture size (inner area)
    bool cache_ok = false;
    lv_image_dsc_t img = {};
    // ---- sample (never dereferenced after setSample)
    const Sample* smp = nullptr;
    uint32_t smp_id = 0;
    uint32_t frames = 0, rate = 0;
    bool has = false;
    int8_t pk_lo[kPeakBins];
    int8_t pk_hi[kPeakBins];
    uint32_t accent = col::accent;
    // ---- overlays
    float start = 0.f, end = 1.f;
    float slices[kMaxSlices] = {};
    int nslices = 0, sel = -1;
    Overlay overlay = Overlay::None;
    float env[5] = {0.f, 0.f, 0.f, 1.f, 0.f};
    int play_x = -1;
    bool show_time = true, handles = true;
    char time_text[96] = {};
    int time_x = -1;              // readout plate x drawn (placeTime)
    Callbacks cb;
    // ---- interaction
    int8_t grab = -1;             // handle under the press: -1 none, 0 START, 1 END
    bool moved = false, dragging = false, swallow = false;
    int32_t press_x = 0, grab_off = 0;
    uint32_t last_tap_ms = 0;
    int8_t last_tap_handle = -1;
    float pending_tap = 0.f;
};

void waveEventCb(lv_event_t* e);
void headDrawCb(lv_event_t* e);

Wave* get(lv_obj_t* obj)
{
    if (!obj) return nullptr;
    const uint32_t n = lv_obj_get_event_count(obj);
    for (uint32_t i = 0; i < n; ++i) {
        lv_event_dsc_t* d = lv_obj_get_event_dsc(obj, i);
        if (d && lv_event_dsc_get_cb(d) == waveEventCb) return (Wave*)lv_event_dsc_get_user_data(d);
    }
    return nullptr;
}

inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Inner (content) area in screen coordinates: the object minus its border (1 px; more in a maximized panel).
void inner(const Wave* w, lv_area_t* a)
{
    lv_obj_get_coords(w->obj, a);
    const int32_t b = lv_obj_get_style_border_width(w->obj, LV_PART_MAIN);
    a->x1 += b;
    a->y1 += b;
    a->x2 -= b;
    a->y2 -= b;
}
// Sizes of the marks, tabs and hit zones: the design px times the panel's scale (maximize, ui_scale.h).
int sc(const Wave* w, float v)
{
    const float f = uiScaleOf(w->obj).f;
    if (f == 1.f) return (int)v;
    const int r = (int)lroundf(v * f);
    return v >= 1.f && r < 1 ? 1 : r;
}
int innerW(const Wave* w)
{
    lv_area_t a;
    inner(w, &a);
    return lv_area_get_width(&a);
}
int innerH(const Wave* w)
{
    lv_area_t a;
    inner(w, &a);
    return lv_area_get_height(&a);
}
int xOf(const Wave*, float pos, int wi) { return (int)lroundf(clamp01(pos) * (float)(wi > 1 ? wi - 1 : 1)); }

// ================================================================ cache rendering
struct Rgb {
    float r, g, b;
};
Rgb rgbOf(uint32_t hex)
{
    return {(float)((hex >> 16) & 0xFF), (float)((hex >> 8) & 0xFF), (float)(hex & 0xFF)};
}
inline Rgb mix(Rgb a, Rgb b, float t)   // t = weight of b
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
inline uint16_t to565(Rgb c)
{
    const int r = clampi((int)lroundf(c.r * 31.f / 255.f), 0, 31);
    const int g = clampi((int)lroundf(c.g * 63.f / 255.f), 0, 63);
    const int b = clampi((int)lroundf(c.b * 31.f / 255.f), 0, 31);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// Vertical extent of the waveform in column x (raw min/max) in pixel units (row y covers [y, y+1)).
void columnSpan(const Wave* w, int x, int W, float mid, float amp, float* top, float* bot)
{
    int b0 = (int)((int64_t)x * kPeakBins / W);
    int b1 = (int)((int64_t)(x + 1) * kPeakBins / W);
    if (b1 <= b0) b1 = b0 + 1;
    if (b1 > kPeakBins) b1 = kPeakBins;
    int lo = 127, hi = -127;
    for (int b = b0; b < b1; ++b) {
        if (w->pk_lo[b] < lo) lo = w->pk_lo[b];
        if (w->pk_hi[b] > hi) hi = w->pk_hi[b];
    }
    if (lo > hi) lo = hi = 0;
    *top = mid - (float)hi / 127.f * amp;
    *bot = mid - (float)lo / 127.f * amp;
}

// Coverage of pixel row y by the stroked span [t, b] (stroke half width 0.9 px).
inline float rowCover(float t, float b, int y)
{
    const float a0 = t - 0.9f, a1 = b + 0.9f;
    const float lo = a0 > (float)y ? a0 : (float)y;
    const float hi = a1 < (float)(y + 1) ? a1 : (float)(y + 1);
    return hi > lo ? (hi - lo) : 0.f;
}

void renderCache(Wave* w, int W, int H)
{
    if (W <= 0 || H <= 0) return;
    if (!w->px || w->cw != W || w->ch != H) {
        if (w->px) bigFree(w->px);
        w->px = (uint16_t*)bigAlloc((size_t)W * (size_t)H * 2u);
        w->cw = W;
        w->ch = H;
        if (!w->px) {
            w->cw = w->ch = 0;
            w->cache_ok = false;
            return;
        }
    }
    const Rgb bg = rgbOf(kFrameBg);
    const Rgb white = {255.f, 255.f, 255.f};
    const Rgb acc = rgbOf(w->accent);
    const Rgb grid = mix(bg, white, 0.055f);
    const int mid_row = H / 2;

    // Grid columns / rows (CSS canvas: 1 px lines at i*w/10 and i*h/4, white 5.5 %), centre line
    // (accent 12 %). Pass 1 writes the static background row by row from three templates.
    static uint8_t vline[2048];
    static uint8_t hline[1024];
    const int Wc = W < 2048 ? W : 2048;
    const int Hc = H < 1024 ? H : 1024;
    memset(vline, 0, (size_t)Wc);
    memset(hline, 0, (size_t)Hc);
    for (int i = 0; i <= 10; ++i) vline[clampi((int)lroundf((float)i * (float)(W - 1) / 10.f), 0, Wc - 1)] = 1;
    for (int i = 0; i <= 4; ++i) hline[clampi((int)lroundf((float)i * (float)(H - 1) / 4.f), 0, Hc - 1)] = 1;
    auto baseAt = [&](int x, int y) {
        Rgb c = ((y < Hc && hline[y]) || (x < Wc && vline[x])) ? grid : bg;
        return y == mid_row ? mix(c, acc, 0.12f) : c;
    };
    {
        const uint16_t bg565 = to565(bg), grid565 = to565(grid);
        const uint16_t bgc565 = to565(mix(bg, acc, 0.12f)), gridc565 = to565(mix(grid, acc, 0.12f));
        uint16_t* plain = w->px;   // row 0 is a grid row; build the plain-row template in row 1
        for (int y = 0; y < H; ++y) {
            uint16_t* row = w->px + (size_t)y * (size_t)W;
            const bool hl = y < Hc && hline[y];
            if (y == mid_row) {
                for (int x = 0; x < W; ++x) row[x] = (hl || (x < Wc && vline[x])) ? gridc565 : bgc565;
            } else if (hl) {
                for (int x = 0; x < W; ++x) row[x] = grid565;
            } else if (plain != w->px) {
                memcpy(row, plain, (size_t)W * 2u);
            } else {
                for (int x = 0; x < W; ++x) row[x] = (x < Wc && vline[x]) ? grid565 : bg565;
                plain = row;
            }
        }
    }

    // Pass 2: waveform spans (connected: neighbouring columns share the gap between them), blended
    // column by column only where a column (or its neighbours, for the ~1.9 px stroke) has coverage.
    if (w->has) {
        const float mid = (float)H * 0.5f, amp = (float)H * kAmp;
        float* buf = (float*)bigAlloc((size_t)W * 4u * sizeof(float));
        if (buf) {
            float* top = buf;
            float* bot = buf + W;
            float* rtop = buf + 2 * W;
            float* rbot = buf + 3 * W;
            for (int x = 0; x < W; ++x) {
                columnSpan(w, x, W, mid, amp, &rtop[x], &rbot[x]);
                top[x] = rtop[x];
                bot[x] = rbot[x];
            }
            for (int x = 1; x < W; ++x) {
                if (rbot[x - 1] < rtop[x]) {          // previous column above this one
                    const float m = (rbot[x - 1] + rtop[x]) * 0.5f;
                    if (m > bot[x - 1]) bot[x - 1] = m;
                    if (m < top[x]) top[x] = m;
                } else if (rtop[x - 1] > rbot[x]) {   // previous column below
                    const float m = (rtop[x - 1] + rbot[x]) * 0.5f;
                    if (m < top[x - 1]) top[x - 1] = m;
                    if (m > bot[x]) bot[x] = m;
                }
            }
            const Rgb fill = mix(bg, acc, 0.88f);   // interior of tall (noisy) columns: a touch darker
            for (int x = 0; x < W; ++x) {
                float lo = top[x], hi = bot[x];
                if (x > 0) lo = lo < top[x - 1] ? lo : top[x - 1], hi = hi > bot[x - 1] ? hi : bot[x - 1];
                if (x + 1 < W) lo = lo < top[x + 1] ? lo : top[x + 1], hi = hi > bot[x + 1] ? hi : bot[x + 1];
                const int y0 = clampi((int)floorf(lo - 1.f), 0, H - 1), y1 = clampi((int)ceilf(hi + 1.f), 0, H - 1);
                const bool tall = (rbot[x] - rtop[x]) > 10.f;
                for (int y = y0; y <= y1; ++y) {
                    float cov = rowCover(top[x], bot[x], y);
                    if (cov < 1.f) {
                        const float l = x > 0 ? rowCover(top[x - 1], bot[x - 1], y) : 0.f;
                        const float r = x + 1 < W ? rowCover(top[x + 1], bot[x + 1], y) : 0.f;
                        const float side = 0.45f * (l > r ? l : r);
                        if (side > cov) cov = side;
                    }
                    if (cov <= 0.f) continue;
                    const bool interior = tall && (float)y > rtop[x] + 2.f && (float)(y + 1) < rbot[x] - 2.f;
                    w->px[(size_t)y * (size_t)W + (size_t)x] = to565(mix(baseAt(x, y), interior ? fill : acc, cov > 1.f ? 1.f : cov));
                }
            }
            bigFree(buf);
        }
    }

    memset(&w->img, 0, sizeof w->img);
    w->img.header.magic = LV_IMAGE_HEADER_MAGIC;
    w->img.header.cf = LV_COLOR_FORMAT_RGB565;
    w->img.header.w = (uint32_t)W;
    w->img.header.h = (uint32_t)H;
    w->img.header.stride = (uint32_t)W * 2u;
    w->img.data_size = (uint32_t)W * (uint32_t)H * 2u;
    w->img.data = (const uint8_t*)w->px;
    lv_image_cache_drop(&w->img);
    w->cache_ok = true;
}

void ensureCache(Wave* w)
{
    const int W = innerW(w), H = innerH(w);
    if (W <= 0 || H <= 0) return;
    if (!w->cache_ok || W != w->cw || H != w->ch) renderCache(w, W, H);
}

// ================================================================ time label
void fmtRate(char* out, int len, uint32_t rate)
{
    if (rate >= 1000) snprintf(out, (size_t)len, "%u %03u Hz", (unsigned)(rate / 1000u), (unsigned)(rate % 1000u));
    else snprintf(out, (size_t)len, "%u Hz", (unsigned)rate);
}

// The readout sits on a dark plate and moves out of the way of the START/END handles: top right
// (prototype), else top left, else centred between the handles; when none of these is free it
// takes the side away from END (the plate keeps it readable over a marker).
void placeTime(Wave* w)
{
    const int W = lv_obj_get_width(w->obj);
    lv_point_t sz;
    lv_text_get_size(&sz, w->time_text, lv_obj_get_style_text_font(w->time, LV_PART_MAIN), 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    const int lw = (int)sz.x + 2 * (int)lv_obj_get_style_pad_left(w->time, LV_PART_MAIN);   // plate width
    const int edge = sc(w, kTimeEdge);
    int x = W - edge - lw;                       // right
    if (w->handles) {
        const int Wi = innerW(w);
        const int hs = 1 + xOf(w, w->start, Wi), he = 1 + xOf(w, w->end, Wi);   // handle x in the object
        auto clear = [&](int x0) {
            const int x1 = x0 + lw - 1, m = sc(w, kTabW / 2 + 2);
            return (hs + m < x0 || hs - m > x1) && (he + m < x0 || he - m > x1);
        };
        const int left = edge, mid = (hs + he) / 2 - lw / 2;
        if (clear(x)) {
        } else if (clear(left)) {
            x = left;
        } else if (mid > left && mid < W - edge - lw && clear(mid)) {
            x = mid;
        } else if (w->end > 0.6f) {
            x = left;
        }
    }
    if (x < 0) x = 0;
    if (x != w->time_x) {
        w->time_x = x;
        lv_obj_set_x(w->time, x);   // y: kTimeY as created (scaled with the panel)
    }
}

void updateTime(Wave* w)
{
    if (!w->time) return;
    const bool show = w->show_time && w->has && w->rate > 0;
    if (!show) {
        if (!lv_obj_has_flag(w->time, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(w->time, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const float secs = (float)w->frames / (float)w->rate;
    char rate[24];
    fmtRate(rate, sizeof rate, w->rate);
    char t[sizeof(Wave::time_text)];
    snprintf(t, sizeof t, "%.3fs \xE2\x80\x94 %.3fs   %s", (double)(w->start * secs), (double)(w->end * secs), rate);
    if (strcmp(t, w->time_text) != 0) {
        memcpy(w->time_text, t, sizeof w->time_text);
        lv_label_set_text(w->time, t);
    }
    placeTime(w);
    if (lv_obj_has_flag(w->time, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(w->time, LV_OBJ_FLAG_HIDDEN);
}

// ================================================================ drawing
void fillRect(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t col, lv_opa_t opa,
              int radius = 0)
{
    if (x2 < x1 || y2 < y1) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = col;
    d.bg_opa = opa;
    d.radius = radius;
    lv_area_t a = {x1, y1, x2, y2};
    lv_draw_rect(layer, &d, &a);
}

void glowBand(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, bool fade_right)
{
    if (x2 < x1 || y2 < y1) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;
    d.bg_grad.dir = LV_GRAD_DIR_HOR;
    d.bg_grad.stops_count = 2;
    const lv_color_t g = c(col::glow);
    d.bg_grad.stops[0].color = g;
    d.bg_grad.stops[0].opa = fade_right ? kGlowOpa : 0;
    d.bg_grad.stops[0].frac = 0;
    d.bg_grad.stops[1].color = g;
    d.bg_grad.stops[1].opa = fade_right ? 0 : kGlowOpa;
    d.bg_grad.stops[1].frac = 255;
    d.bg_color = g;
    lv_area_t a = {x1, y1, x2, y2};
    lv_draw_rect(layer, &d, &a);
}

void line(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t col, lv_opa_t opa, int width)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    d.color = col;
    d.opa = opa;
    d.width = width;
    d.round_start = 1;
    d.round_end = 1;
    lv_draw_line(layer, &d);
}

void drawEnvelope(Wave* w, lv_layer_t* layer, const lv_area_t& in, int xs, int xe)
{
    if (!w->has || w->rate == 0 || xe <= xs) return;
    const float region_ms = (w->end - w->start) * (float)w->frames * 1000.f / (float)w->rate;
    if (region_ms <= 0.f) return;
    const float k = (float)(xe - xs) / region_ms;   // px per ms
    const int Hi = lv_area_get_height(&in);
    const float ytop = (float)in.y1 + 0.12f * (float)Hi, ybot = (float)in.y1 + 0.88f * (float)Hi;
    const float a = w->env[0] * k, h = w->env[1] * k, d = w->env[2] * k, s = clamp01(w->env[3]), r = w->env[4] * k;
    float x[6], y[6];
    x[0] = (float)xs;
    y[0] = ybot;
    x[1] = x[0] + a;
    y[1] = ytop;
    x[2] = x[1] + h;
    y[2] = ytop;
    x[3] = x[2] + d;
    y[3] = ybot - s * (ybot - ytop);
    x[4] = (float)xe - r;
    if (x[4] < x[3]) x[4] = x[3];
    y[4] = y[3];
    x[5] = x[4] + r;
    y[5] = ybot;
    const lv_color_t col = c(w->accent);
    const float xmax = (float)in.x2;
    for (int i = 0; i < 5; ++i) {
        if (x[i] > xmax) break;
        float x2 = x[i + 1], y2 = y[i + 1];
        if (x2 > xmax && x[i + 1] > x[i]) {   // clip to the frame
            const float t = (xmax - x[i]) / (x[i + 1] - x[i]);
            x2 = xmax;
            y2 = y[i] + (y[i + 1] - y[i]) * t;
        }
        line(layer, (int32_t)lroundf(x[i]), (int32_t)lroundf(y[i]), (int32_t)lroundf(x2), (int32_t)lroundf(y2), col,
             220, sc(w, 2));
    }
    const int dot = sc(w, 3);
    for (int i = 1; i <= 4; ++i) {
        if (x[i] > xmax) break;
        const int32_t cx = (int32_t)lroundf(x[i]), cy = (int32_t)lroundf(y[i]);
        fillRect(layer, cx - dot, cy - dot, cx + dot, cy + dot, col, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    }
}

void drawMain(Wave* w, lv_event_t* e)
{
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t in;
    inner(w, &in);
    const int Wi = lv_area_get_width(&in), Hi = lv_area_get_height(&in);
    if (Wi <= 2 || Hi <= 2) return;
    ensureCache(w);
    if (w->cache_ok && w->cw == Wi && w->ch == Hi) {
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.src = &w->img;
        lv_draw_image(layer, &d, &in);
    }
    // Overlays never leave the inner area (region invalidation is limited to it as well).
    const lv_area_t clip_old = layer->_clip_area;
    lv_area_t clip;
    clip.x1 = clip_old.x1 > in.x1 ? clip_old.x1 : in.x1;
    clip.y1 = clip_old.y1 > in.y1 ? clip_old.y1 : in.y1;
    clip.x2 = clip_old.x2 < in.x2 ? clip_old.x2 : in.x2;
    clip.y2 = clip_old.y2 < in.y2 ? clip_old.y2 : in.y2;
    if (clip.x1 > clip.x2 || clip.y1 > clip.y2) return;
    layer->_clip_area = clip;
    const lv_color_t acc = c(w->accent);
    const int xs = in.x1 + xOf(w, w->start, Wi), xe = in.x1 + xOf(w, w->end, Wi);
    const int ys = in.y1 + (int)lroundf(0.06f * (float)Hi), ye = in.y2 - (int)lroundf(0.06f * (float)Hi);

    if (w->handles && w->has) {
        // outside START..END dimmed, glow bands inside the markers
        fillRect(layer, in.x1, in.y1, xs - 1, in.y2, lv_color_black(), kDimOpa);
        fillRect(layer, xe + 1, in.y1, in.x2, in.y2, lv_color_black(), kDimOpa);
        const int span = xe - xs;
        int band = (int)lroundf(0.15f * (float)span);
        if (band < 3) band = 3;
        if (band > span / 2) band = span / 2;
        if (band > 0) {
            glowBand(layer, xs + 1, ys, xs + band, ye, true);
            glowBand(layer, xe - band, ys, xe - 1, ye, false);
        }
    }

    // slice markers (thin lines + numbers at the bottom; the selected slice in accent)
    if (w->has && w->nslices > 0) {
        lv_draw_label_dsc_t ld;
        lv_draw_label_dsc_init(&ld);
        ld.font = fontScaled(f_mono12, uiScaleOf(w->obj).f);
        for (int i = 0; i < w->nslices; ++i) {
            const int x = in.x1 + xOf(w, w->slices[i], Wi);
            const bool selected = i == w->sel;
            if (i > 0 || x > in.x1 + 1)
                fillRect(layer, x, in.y1, x, in.y2, selected ? acc : lv_color_white(), selected ? LV_OPA_COVER : 90);
            ld.color = selected ? acc : lv_color_white();
            ld.opa = selected ? LV_OPA_COVER : 160;
            ld.text = kNums[i < 16 ? i : 15];
            lv_area_t ta = {x + sc(w, 5), in.y2 - sc(w, 18), x + sc(w, 40), in.y2 - sc(w, 4)};
            if (ta.x1 < in.x2 - sc(w, 8)) lv_draw_label(layer, &ld, &ta);
        }
    }

    if (w->overlay == Overlay::Envelope) drawEnvelope(w, layer, in, xs, xe);

    if (w->handles && w->has) {
        // START / END markers: 2 px accent lines with rounded tabs top and bottom
        const int m[2] = {xs, xe};
        const int lw = sc(w, 2), tw = sc(w, kTabW), th = sc(w, kTabH), tr = sc(w, 3);
        for (int i = 0; i < 2; ++i) {
            const int x = m[i];
            const int lx1 = i == 0 ? x : x - (lw - 1);
            fillRect(layer, lx1, ys, lx1 + lw - 1, ye, acc, LV_OPA_COVER);
            const int tx1 = x - tw / 2 + (i == 0 ? 1 : 0);
            fillRect(layer, tx1, ys, tx1 + tw - 1, ys + th - 1, acc, LV_OPA_COVER, tr);
            fillRect(layer, tx1, ye - th + 1, tx1 + tw - 1, ye, acc, LV_OPA_COVER, tr);
        }
    }
    layer->_clip_area = clip_old;
}

// Playhead child: 1 px accent line with a soft glow made of fading 1 px columns.
void headDrawCb(lv_event_t* e)
{
    lv_obj_t* o = (lv_obj_t*)lv_event_get_current_target(e);
    Wave* w = (Wave*)lv_event_get_user_data(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const int cx = a.x1 + lv_area_get_width(&a) / 2;
    const lv_color_t acc = c(w ? w->accent : col::accent);
    static const lv_opa_t glow[4] = {110, 55, 26, 10};
    for (int i = 0; i < 4; ++i) {
        fillRect(layer, cx - 1 - i, a.y1, cx - 1 - i, a.y2, acc, glow[i]);
        fillRect(layer, cx + 1 + i, a.y1, cx + 1 + i, a.y2, acc, glow[i]);
    }
    fillRect(layer, cx, a.y1, cx, a.y2, acc, LV_OPA_COVER);
}

// ================================================================ invalidation
void invalidateCols(Wave* w, int x1, int x2)   // inner-relative columns
{
    lv_area_t in;
    inner(w, &in);
    if (x1 > x2) {
        const int t = x1;
        x1 = x2;
        x2 = t;
    }
    lv_area_t a = {in.x1 + x1, in.y1, in.x1 + x2, in.y2};
    if (a.x1 < in.x1) a.x1 = in.x1;
    if (a.x2 > in.x2) a.x2 = in.x2;
    if (a.x2 >= a.x1) lv_obj_invalidate_area(w->obj, &a);
}

int bandOf(int xs, int xe)
{
    const int span = xe - xs;
    int band = (int)lroundf(0.15f * (float)span);
    if (band < 3) band = 3;
    return band;
}

void applyRegion(Wave* w, float s, float e)
{
    s = clamp01(s);
    e = clamp01(e);
    if (e <= s) e = s + 0.001f > 1.f ? 1.f : s + 0.001f;
    if (s == w->start && e == w->end) return;
    const int Wi = innerW(w);
    const int os = xOf(w, w->start, Wi), oe = xOf(w, w->end, Wi);
    const int ns = xOf(w, s, Wi), ne = xOf(w, e, Wi);
    w->start = s;
    w->end = e;
    updateTime(w);
    if (os == ns && oe == ne) return;
    if (w->overlay == Overlay::Envelope) {
        lv_obj_invalidate(w->obj);
        return;
    }
    const int ob = bandOf(os, oe), nb = bandOf(ns, ne);
    const int pad = sc(w, kTabW + 4);
    if (os != ns || ob != nb) {
        const int lo = (os < ns ? os : ns) - pad;
        const int hi = (os + ob > ns + nb ? os + ob : ns + nb) + pad;
        invalidateCols(w, lo, hi);
    }
    if (oe != ne || ob != nb) {
        const int lo = (oe - ob < ne - nb ? oe - ob : ne - nb) - pad;
        const int hi = (oe > ne ? oe : ne) + pad;
        invalidateCols(w, lo, hi);
    }
}

// ================================================================ interaction
int pointerX(Wave* w)   // inner-relative, screen px (the geometry here follows the drawn size)
{
    lv_point_t p = {0, 0};
    lv_indev_t* indev = lv_indev_active();
    if (indev) lv_indev_get_point(indev, &p);
    lv_area_t in;
    inner(w, &in);
    return p.x - in.x1;
}

float posAt(Wave* w, int x)
{
    const int Wi = innerW(w);
    return clamp01((float)x / (float)(Wi > 1 ? Wi - 1 : 1));
}

int handleAt(Wave* w, int x)
{
    if (!w->handles || !w->has) return -1;
    const int Wi = innerW(w);
    const int xs = xOf(w, w->start, Wi), xe = xOf(w, w->end, Wi);
    const int ds = x > xs ? x - xs : xs - x, de = x > xe ? x - xe : xe - x;
    const int hit = sc(w, kHandleHit);
    if (ds > hit && de > hit) return -1;
    if (ds == de) return x < xs ? 0 : 1;   // overlapping markers: side of the press decides
    return ds < de ? 0 : 1;
}

void tapTimerCb(lv_timer_t* t)
{
    Wave* w = (Wave*)lv_timer_get_user_data(t);
    lv_timer_pause(t);
    w->last_tap_handle = -1;
    if (w->cb.tap) w->cb.tap(w->pending_tap, w->cb.user);
}

void onPressed(Wave* w)
{
    const int x = pointerX(w);
    const uint32_t now = lv_tick_get();
    w->press_x = x;
    w->moved = false;
    w->dragging = false;
    w->swallow = false;
    w->grab = (int8_t)handleAt(w, x);
    if (w->grab >= 0 && w->last_tap_handle == w->grab && (uint32_t)(now - w->last_tap_ms) <= kDoubleMs) {
        // double tap on a handle: snap (the pending single tap is dropped)
        if (w->tap_timer) lv_timer_pause(w->tap_timer);
        w->last_tap_handle = -1;
        w->swallow = true;
        if (w->cb.snap) w->cb.snap(w->grab, w->cb.user);
        return;
    }
    if (w->grab >= 0) {
        const int Wi = innerW(w);
        const int mx = xOf(w, w->grab == 0 ? w->start : w->end, Wi);
        w->grab_off = x - mx;
    }
}

void onPressing(Wave* w)
{
    if (w->swallow) return;
    const int x = pointerX(w);
    const int d = x - w->press_x;
    const int slop = sc(w, kDragSlop);
    if (!w->moved && (d > slop || d < -slop)) {
        w->moved = true;
        if (w->grab >= 0) {
            w->dragging = true;
            if (w->tap_timer) lv_timer_pause(w->tap_timer);
            w->last_tap_handle = -1;
        }
    }
    if (!w->dragging) return;
    const float p = posAt(w, x - w->grab_off);
    float s = w->start, e = w->end;
    if (w->grab == 0) {
        s = p > e - kMinGap ? e - kMinGap : p;
        if (s < 0.f) s = 0.f;
    } else {
        e = p < s + kMinGap ? s + kMinGap : p;
        if (e > 1.f) e = 1.f;
    }
    const int Wi = innerW(w);
    const bool changed_px = xOf(w, s, Wi) != xOf(w, w->start, Wi) || xOf(w, e, Wi) != xOf(w, w->end, Wi);
    if (s == w->start && e == w->end) return;
    applyRegion(w, s, e);
    if (changed_px && w->cb.region) w->cb.region(w->start, w->end, false, w->cb.user);
}

void onReleased(Wave* w, bool lost)
{
    if (w->swallow) {
        w->swallow = false;
        return;
    }
    if (w->dragging) {
        w->dragging = false;
        if (w->cb.region) w->cb.region(w->start, w->end, true, w->cb.user);
        return;
    }
    if (lost || w->moved) return;
    const float pos = posAt(w, w->press_x);
    if (w->grab >= 0) {
        // a tap on a handle may be the first half of a double tap: defer the plain tap
        w->last_tap_handle = w->grab;
        w->last_tap_ms = lv_tick_get();
        w->pending_tap = pos;
        if (w->cb.tap && w->tap_timer) {
            lv_timer_reset(w->tap_timer);
            lv_timer_resume(w->tap_timer);
        }
        return;
    }
    w->last_tap_handle = -1;
    if (w->cb.tap) w->cb.tap(pos, w->cb.user);
}

void waveEventCb(lv_event_t* e)
{
    Wave* w = (Wave*)lv_event_get_user_data(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_DRAW_MAIN: drawMain(w, e); break;
    case LV_EVENT_PRESSED: onPressed(w); break;
    case LV_EVENT_PRESSING: onPressing(w); break;
    case LV_EVENT_RELEASED: onReleased(w, false); break;
    case LV_EVENT_PRESS_LOST: onReleased(w, true); break;
    case LV_EVENT_SIZE_CHANGED:
        w->cache_ok = false;
        if (w->head) lv_obj_set_height(w->head, innerH(w));
        if (w->play_x >= 0 && w->head) {
            lv_obj_add_flag(w->head, LV_OBJ_FLAG_HIDDEN);
            w->play_x = -1;
        }
        updateTime(w);   // readout position depends on the width
        lv_obj_invalidate(w->obj);
        break;
    case LV_EVENT_DELETE:
        if (w->tap_timer) lv_timer_delete(w->tap_timer);
        if (w->px) {
            lv_image_cache_drop(&w->img);
            bigFree(w->px);
        }
        w->~Wave();
        lv_free(w);
        break;
    default: break;
    }
}

}  // namespace

// ================================================================ public API
lv_obj_t* create(lv_obj_t* parent, int width, int height)
{
    void* mem = lv_malloc(sizeof(Wave));
    if (!mem) return nullptr;
    Wave* w = new (mem) Wave();
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, width, height);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, c(kFrameBg), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, c(kBorder), 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_ext_click_area(o, 4);
    w->obj = o;

    lv_obj_t* h = lv_obj_create(o);
    lv_obj_remove_style_all(h);
    lv_obj_set_size(h, kHeadW, height - 2);
    lv_obj_set_pos(h, 0, 0);
    lv_obj_remove_flag(h, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(h, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(h, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(h, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_event_cb(h, headDrawCb, LV_EVENT_DRAW_MAIN, w);
    w->head = h;

    lv_obj_t* t = lv_label_create(o);
    lv_obj_set_style_text_font(t, f_mono12, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_obj_set_style_text_opa(t, 170, 0);   // #ffffffaa
    // dark plate: the readout stays legible where a START/END marker runs behind it
    lv_obj_set_style_bg_opa(t, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(t, c(kFrameBg), 0);
    lv_obj_set_style_radius(t, 4, 0);
    lv_obj_set_style_pad_hor(t, kTimePadX, 0);
    lv_obj_set_style_pad_ver(t, kTimePadY, 0);
    lv_label_set_text(t, "");
    lv_obj_set_pos(t, 0, kTimeY);   // x: placeTime()
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(t, LV_OBJ_FLAG_IGNORE_LAYOUT);
    w->time = t;

    w->tap_timer = lv_timer_create(tapTimerCb, kDoubleMs, w);
    if (w->tap_timer) lv_timer_pause(w->tap_timer);

    lv_obj_add_event_cb(o, waveEventCb, LV_EVENT_ALL, w);
    return o;
}

void setSample(lv_obj_t* obj, const Sample* s)
{
    Wave* w = get(obj);
    if (!w) return;
    const uint32_t id = s ? s->id : 0;
    if (s == w->smp && id == w->smp_id && (s != nullptr) == w->has) return;
    w->smp = s;
    w->smp_id = id;
    w->has = s && s->frames > 0;
    if (w->has) {
        w->frames = s->frames;
        w->rate = s->rate;
        memcpy(w->pk_lo, s->pk_min, sizeof w->pk_lo);
        memcpy(w->pk_hi, s->pk_max, sizeof w->pk_hi);
    } else {
        w->frames = 0;
        w->rate = 0;
    }
    w->cache_ok = false;
    ensureCache(w);
    updateTime(w);
    lv_obj_invalidate(obj);
}

void setRegion(lv_obj_t* obj, float start, float end)
{
    Wave* w = get(obj);
    if (!w || w->dragging) return;   // the finger owns the markers while dragging
    applyRegion(w, start, end);
}

void setSlices(lv_obj_t* obj, const float* pos, int n, int sel)
{
    Wave* w = get(obj);
    if (!w) return;
    if (n < 0 || !pos) n = 0;
    if (n > kMaxSlices) n = kMaxSlices;
    bool same = n == w->nslices && sel == w->sel;
    for (int i = 0; same && i < n; ++i) same = pos[i] == w->slices[i];
    if (same) return;
    for (int i = 0; i < n; ++i) w->slices[i] = pos[i];
    w->nslices = n;
    w->sel = sel;
    lv_obj_invalidate(obj);
}

void setOverlay(lv_obj_t* obj, Overlay o)
{
    Wave* w = get(obj);
    if (!w || w->overlay == o) return;
    w->overlay = o;
    lv_obj_invalidate(obj);
}

void setEnvelope(lv_obj_t* obj, float a_ms, float h_ms, float d_ms, float s, float r_ms)
{
    Wave* w = get(obj);
    if (!w) return;
    const float v[5] = {a_ms, h_ms, d_ms, s, r_ms};
    if (memcmp(v, w->env, sizeof v) == 0) return;
    memcpy(w->env, v, sizeof v);
    if (w->overlay == Overlay::Envelope) lv_obj_invalidate(obj);
}

void setPlayhead(lv_obj_t* obj, float pos01)
{
    Wave* w = get(obj);
    if (!w || !w->head) return;
    if (pos01 < 0.f || !w->has) {
        if (w->play_x >= 0) {
            w->play_x = -1;
            lv_obj_add_flag(w->head, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    const int x = xOf(w, pos01, innerW(w));
    if (x == w->play_x) return;
    lv_obj_set_x(w->head, x - lv_obj_get_width(w->head) / 2);
    if (w->play_x < 0) lv_obj_remove_flag(w->head, LV_OBJ_FLAG_HIDDEN);
    w->play_x = x;
}

void setShowTime(lv_obj_t* obj, bool on)
{
    Wave* w = get(obj);
    if (!w || w->show_time == on) return;
    w->show_time = on;
    updateTime(w);
}

void setHandles(lv_obj_t* obj, bool on)
{
    Wave* w = get(obj);
    if (!w || w->handles == on) return;
    w->handles = on;
    updateTime(w);
    lv_obj_invalidate(obj);
}

void setAccent(lv_obj_t* obj, lv_color_t col)
{
    Wave* w = get(obj);
    if (!w) return;
    const uint32_t hex = lv_color_to_u32(col) & 0xFFFFFFu;
    if (hex == w->accent) return;
    w->accent = hex;
    w->cache_ok = false;
    ensureCache(w);
    lv_obj_invalidate(obj);
}

void setCallbacks(lv_obj_t* obj, const Callbacks& cb)
{
    Wave* w = get(obj);
    if (w) w->cb = cb;
}

int posToX(lv_obj_t* obj, float pos01)
{
    Wave* w = get(obj);
    return w ? 1 + xOf(w, pos01, innerW(w)) : 0;
}

float xToPos(lv_obj_t* obj, int x)
{
    Wave* w = get(obj);
    return w ? posAt(w, x - 1) : 0.f;
}

bool dragging(lv_obj_t* obj)
{
    Wave* w = get(obj);
    return w && w->dragging;
}

}  // namespace orbit::ui::wave
