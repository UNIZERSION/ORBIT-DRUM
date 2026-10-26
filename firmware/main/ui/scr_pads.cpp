// PADS panel (#l-pads): header (MUTE / SOLO pad modes) and the 4x4 pad grid.
//
// The grid is ONE custom-drawn object. Every pad blits a pre-baked, dithered RGB565 image (the
// prototype's 145deg gradient, 1 px border, bottom shadow, inner top highlight; the selected pad:
// 135deg orange gradient, glow and the white indicator) and draws its two labels on top. Hit
// flashes, the press tint, mute dimming and the M / S badges are cheap overlays drawn only for
// the pads that need them; only the changed pad rectangles are invalidated.
//
// Playing: on the Tab5 the touch task calls the hook installed by installTouchHook() for every
// controller sample (up to 5 fingers). A NEW finger whose down point lies in a pad plays it at
// once (app().padDown, Producer::Touch) with a velocity from the vertical position (or fixed,
// Settings > Touch); lifting it calls padUp. The pad index goes through a lock-free SPSC queue
// to refresh(), which selects it (no audition: it already sounded; during a fast burst of hits
// the App selection is deferred until the hits pause, see requestSelect). The LVGL click on a
// pad then only selects it (or toggles mute / solo in those pad modes, via app().padTap()).
// Host builds (ORBIT_HOST) have no touch task: LVGL PRESSED plays (Producer::Ui), RELEASED stops.
#include "ui/ui.h"

#include <atomic>
#include <string.h>

#include "core/prof.h"
#include "core/spsc.h"
#include "hal/hal.h"
#include "ui/theme.h"
#include "ui/ui_internal.h"
#include "ui/widgets.h"

namespace orbit::ui::pads {

namespace {

// ---- geometry (prototype: pads 61.5 x 62 at x 25/93.5/161.5/230, y 118/187/255.5/324.5)
constexpr int kPadW = 61, kPadH = 62, kRadius = 6;
constexpr int kMargin = 3, kMarginB = 5;                      // baked around the pad body (bottom: shadow)
constexpr int kImgW = kPadW + 2 * kMargin, kImgH = kPadH + kMargin + kMarginB;
constexpr int kGridW = 286, kGridH = 286;                     // .l-pad-grid below the 36 px head
constexpr int kColX[4] = {10, 78, 147, 215};                  // relative to the grid object
constexpr int kRowY[4] = {8, 77, 145, 214};
constexpr int kTouchSlack = 3;                                // half a gap around each pad
constexpr uint32_t kHitMs = 280;
constexpr int kHitLevels = 8;

const char* const kIndex[kPads] = {"A1", "A2", "A3", "A4", "B1", "B2", "B3", "B4",
                                   "C1", "C2", "C3", "C4", "D1", "D2", "D3", "D4"};

struct PadView {
    char name[kNameLen + 4];   // fitted copy of project().pad[i].name (+ "…")
    char src[kNameLen];        // the name it was fitted from
    const lv_font_t* font;
    uint8_t mute, solo;
    uint8_t hit_level;         // 0..kHitLevels drawn
    bool hit_on;
    uint32_t hit_t0;
};

// Pad geometry as drawn: the design values above at scale 1, larger in the maximized panel (ui_scale.h). The
// LVGL click path works in design px (pointerIn); drawing, baking and the touch task's rectangles use these.
struct PadGeo {
    UiScale s;
    int w = kPadW, h = kPadH, r = kRadius, m = kMargin, mb = kMarginB, img_w = kImgW, img_h = kImgH;
    int colX(int c) const { return s.sx == 1.f ? kColX[c] : (int)lroundf((float)kColX[c] * s.sx); }
    int rowY(int r) const { return s.sy == 1.f ? kRowY[r] : (int)lroundf((float)kRowY[r] * s.sy); }
    int F(float v) const
    {
        const int q = (int)lroundf(v * s.f);
        return v >= 1.f && q < 1 ? 1 : q;
    }
    const lv_font_t* font(const lv_font_t* f) const { return fontScaled(f, s.f); }
};
PadGeo s_g;

void setGeo(UiScale sc)
{
    PadGeo g;
    g.s = sc;
    if (!sc.unit()) {
        g.w = (int)lroundf((float)kPadW * sc.sx);
        g.h = (int)lroundf((float)kPadH * sc.sy);
        g.r = g.F((float)kRadius);
        g.m = g.F((float)kMargin);
        g.mb = g.F((float)kMarginB);
        g.img_w = g.w + 2 * g.m;
        g.img_h = g.h + g.m + g.mb;
    }
    s_g = g;
}

lv_obj_t* s_panel = nullptr;
lv_obj_t* s_grid = nullptr;
lv_obj_t* s_mute = nullptr;
lv_obj_t* s_solo = nullptr;
lv_image_dsc_t* s_img[2] = {};   // 0 normal, 1 selected
lv_image_dsc_t* s_img_big[2] = {};   // the same baked for the maximized panel (s_big_w x s_big_h)
int s_big_w = 0, s_big_h = 0;
PadView s_pv[kPads];
int s_sel = -1;
int s_pressed = -1;              // LVGL pointer is down on this pad (press tint)
int s_press_pad = -1;            // pad of the current LVGL press (for CLICKED)
int s_mode_drawn = -1;
uint32_t s_rev_sel = 0, s_rev_pads = 0, s_rev_samples = 0, s_rev_project = 0;
// 0.4.9: draw only what reaches the area being redrawn (setCulling: off = the old way, for A/B measurements).
// LVGL makes a draw task for every lv_draw_* call, also one wholly outside the clip area (a label is still laid out
// glyph by glyph), and a pad's redraw area used to reach every neighbour by its 1 px safety margin.
bool s_cull = true;
// Diagnostics only (pads::setDebugSkip): parts left out to time the rest. 1 images, 2 hit rings, 4 tints, 8 labels.
uint8_t s_skip = 0;

// ---- shared with the touch task
struct Rect {
    int16_t x1, y1, x2, y2;
};
Rect s_abs[kPads];                       // absolute pad rectangles, written once at create
std::atomic<bool> s_enabled{false};
std::atomic<uint8_t> s_vel_position{1};
std::atomic<uint8_t> s_vel_fixed{110};
std::atomic<uint32_t> s_touched{0};      // pads played by a finger (touch) since the last LVGL click
Spsc<uint8_t, 32> s_sel_q;               // touch task -> refresh(): pads to select

// ================================================================ baked pad images
using bake::Rgb;

void bakePad(lv_image_dsc_t* img, bool active)
{
    using namespace bake;
    uint16_t* px = const_cast<uint16_t*>(reinterpret_cast<const uint16_t*>(img->data));
    const Rgb panel = bake::rgbGrid(col::panel), white = rgb(0xFFFFFF), top_hl = rgb(col::pad_hl);
    const Rgb ga = rgb(active ? col::pad_on_a : col::pad_a), gb = rgb(active ? col::pad_on_b : col::pad_b);
    const Rgb border = rgb(active ? col::pad_on_border : col::pad_border);
    const Rgb shadow = rgb(active ? col::pad_on_shadow : col::pad_shadow);
    const PadGeo& G = s_g;
    const float W = (float)G.w, H = (float)G.h, R = (float)G.r, f = G.s.f;
    // gradient direction: 145deg (normal) / 135deg (active); CSS gradient line length
    const float ang = (active ? 135.f : 145.f) * 3.14159265f / 180.f;
    const float gx = sinf(ang), gy = -cosf(ang);
    const float L = W * fabsf(gx) + H * fabsf(gy);
    const float sh_off = (active ? 3.f : 2.f) * f;
    const float px1 = f > 1.f ? f : 1.f;   // the 1 px details (inner highlight) grow with the pad
    for (int y = 0; y < G.img_h; ++y) {
        for (int x = 0; x < G.img_w; ++x) {
            const float fx = (float)(x - G.m) + 0.5f, fy = (float)(y - G.m) + 0.5f;
            Rgb c = panel;
            if (active) {   // 0 0 11px #ff642e33
                const float d = rrectDist(fx, fy, 0.f, 0.f, W, H, R);
                c = mix(c, rgb(col::glow), 0.2f * blurCov(d, 5.5f * f));
            }
            {   // 0 2px 1px #acb0b1 / 0 3px 1px #db5530
                const float d = rrectDist(fx, fy, 0.f, sh_off, W, H + sh_off, R);
                c = mix(c, shadow, blurCov(d, 0.6f * f));
            }
            const float d = rrectDist(fx, fy, 0.f, 0.f, W, H, R);
            const float cov = clamp01(0.5f - d);
            if (cov > 0.f) {
                const float t = clamp01(((fx - W * 0.5f) * gx + (fy - H * 0.5f) * gy) / L + 0.5f);
                Rgb in = mix(ga, gb, t);
                if (!active) {   // inset 0 1px 0 #fff: top line inside the border
                    const float din = rrectDist(fx, fy, px1, px1, W - px1, H - px1, R - px1);
                    const float dsh = rrectDist(fx, fy, px1, 2.f * px1, W - px1, H, R - px1);
                    const float hl = clamp01(0.5f - din) * clamp01(0.5f + dsh);
                    in = mix(in, top_hl, hl);
                }
                const Rgb body = mix(border, in, clamp01(0.5f - (d + px1)));
                c = mix(c, body, cov);
            }
            if (active) {   // ::before: 11x7 white, radius 2, at 8,8 of the padding box, glow 9px
                const float di = rrectDist(fx, fy, 9.f * f, 9.f * f, 20.f * f, 16.f * f, 2.f * f);
                c = mix(c, white, 0.85f * blurCov(di, 4.5f * f));
                c = mix(c, white, clamp01(0.5f - di));
            }
            px[y * G.img_w + x] = to565(c, x, y);
        }
    }
}

void bakeImages()
{
    if (s_g.s.unit()) {
        for (int i = 0; i < 2; ++i) {
            if (s_img[i]) continue;
            s_img[i] = bake::image565(kImgW, kImgH);
            if (s_img[i]) bakePad(s_img[i], i == 1);
        }
        return;
    }
    // maximized: baked at the drawn size (buffers kept: the panel always maximizes to the same size)
    if (s_img_big[0] && s_big_w == s_g.img_w && s_big_h == s_g.img_h) return;
    for (int i = 0; i < 2; ++i) {
        s_img_big[i] = bake::image565(s_g.img_w, s_g.img_h);
        if (s_img_big[i]) bakePad(s_img_big[i], i == 1);
    }
    s_big_w = s_g.img_w;
    s_big_h = s_g.img_h;
}

// ================================================================ geometry helpers
void padArea(int i, const lv_area_t& grid, lv_area_t& out)
{
    out.x1 = grid.x1 + s_g.colX(i & 3);
    out.y1 = grid.y1 + s_g.rowY(i >> 2);
    out.x2 = out.x1 + s_g.w - 1;
    out.y2 = out.y1 + s_g.h - 1;
}

void invalidatePad(int i)
{
    if (!s_grid || i < 0 || i >= kPads) return;
    lv_area_t g, a;
    lv_obj_get_coords(s_grid, &g);
    padArea(i, g, a);
    a.x1 -= s_g.m + 1;
    a.x2 += s_g.m + 1;
    a.y1 -= s_g.m + 1;
    a.y2 += s_g.mb + 1;
    lv_obj_invalidate_area(s_grid, &a);
}

// Nearest pad to a point inside the grid (gaps belong to the closer pad).
int padAtGrid(int32_t x, int32_t y)
{
    if (!s_grid) return -1;
    lv_area_t g;
    lv_obj_get_coords(s_grid, &g);
    const int32_t lx = x - g.x1, ly = y - g.y1;
    if (lx < 0 || ly < 0 || lx >= kGridW || ly >= kGridH) return -1;
    int col = 0, row = 0;
    for (int i = 1; i < 4; ++i) {
        if (lx >= (kColX[i - 1] + kPadW + kColX[i]) / 2) col = i;
        if (ly >= (kRowY[i - 1] + kPadH + kRowY[i]) / 2) row = i;
    }
    return row * 4 + col;
}

// Touch task: pad whose rectangle (plus half a gap) contains the point, else -1.
int padAtTouch(int x, int y)
{
    for (int i = 0; i < kPads; ++i) {
        const Rect& r = s_abs[i];
        if (x >= r.x1 - kTouchSlack && x <= r.x2 + kTouchSlack && y >= r.y1 - kTouchSlack && y <= r.y2 + kTouchSlack)
            return i;
    }
    return -1;
}

// Velocity from the vertical touch position (Settings > Touch > Position). A fingertip's touch
// centroid scatters by ~1-2 mm between hits (12-25 px at 294 ppi, a third of the 62 px pad), so
// a continuous top-to-bottom ramp turns a steady hi-hat into random accents. The middle 40 % of
// the pad is therefore flat at the fixed velocity (Settings, default 110); only deliberate edge
// hits change it: the top 30 % rises to 127 at the edge (accents), the bottom 30 % falls to half
// the plateau (ghost notes, 55 at the default). Continuous at both plateau edges.
constexpr float kVelFlatTop = 0.3f, kVelFlatBottom = 0.7f;

int velocityAt(int pad, int y)
{
    int vfix = s_vel_fixed.load(std::memory_order_relaxed);
    vfix = vfix < 1 ? 1 : (vfix > 127 ? 127 : vfix);
    if (!s_vel_position.load(std::memory_order_relaxed)) return vfix;
    const Rect& r = s_abs[pad];
    const float h = (float)(r.y2 - r.y1 + 1);
    const float t = bake::clamp01(((float)y - (float)r.y1 + 0.5f) / (h > 1.f ? h : 1.f));
    const float plateau = (float)vfix;
    float v = plateau;
    if (t < kVelFlatTop) v = 127.f - (127.f - plateau) * (t / kVelFlatTop);
    else if (t > kVelFlatBottom) v = plateau - 0.5f * plateau * ((t - kVelFlatBottom) / (1.f - kVelFlatBottom));
    const int vi = (int)(v + 0.5f);
    return vi < 1 ? 1 : (vi > 127 ? 127 : vi);
}

// ================================================================ name fitting
void fitName(PadView& pv, const char* src)
{
    snprintf(pv.src, sizeof pv.src, "%s", src ? src : "");
    snprintf(pv.name, sizeof pv.name, "%s", pv.src);
    const int32_t avail = s_g.w - s_g.F(8) - s_g.F(5);
    const lv_font_t* big = s_g.font(f_r13);
    const lv_font_t* small = s_g.font(f_r12);
    pv.font = big;
    if (lv_text_get_width(pv.name, (uint32_t)strlen(pv.name), big, 0) <= avail) return;
    pv.font = small;
    if (lv_text_get_width(pv.name, (uint32_t)strlen(pv.name), small, 0) <= avail) return;
    // Cut whole UTF-8 characters until "<prefix>…" fits.
    static const char kEll[] = "\xE2\x80\xA6";
    size_t n = strlen(pv.src);
    while (n > 0) {
        do {
            --n;
        } while (n > 0 && ((unsigned char)pv.src[n] & 0xC0) == 0x80);
        char tmp[kNameLen + 4] = {};
        snprintf(tmp, sizeof tmp, "%.*s%s", (int)n, pv.src, kEll);
        if (lv_text_get_width(tmp, (uint32_t)strlen(tmp), small, 0) <= avail || n == 0) {
            memcpy(pv.name, tmp, sizeof tmp);
            return;
        }
    }
}

// ================================================================ drawing
void drawLabel(lv_layer_t* layer, const char* text, const lv_font_t* font, lv_color_t color, lv_opa_t opa,
               int32_t x1, int32_t y1, int32_t x2, lv_text_align_t align = LV_TEXT_ALIGN_LEFT)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = text;
    d.font = font;
    d.color = color;
    d.opa = opa;
    d.align = align;
    d.flag = LV_TEXT_FLAG_EXPAND;
    lv_area_t a = {x1, y1, x2, y1 + (int32_t)lv_font_get_line_height(font) - 1};
    lv_draw_label(layer, &d, &a);
}

void fillRound(lv_layer_t* layer, const lv_area_t& a, uint32_t color, lv_opa_t opa, int radius)
{
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = c(color);
    r.bg_opa = opa;
    r.radius = radius;
    lv_draw_rect(layer, &r, &a);
}

void ringRound(lv_layer_t* layer, const lv_area_t& a, uint32_t color, lv_opa_t opa, int width, int radius)
{
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_opa = LV_OPA_TRANSP;
    r.border_color = c(color);
    r.border_opa = opa;
    r.border_width = width;
    r.radius = radius;
    lv_draw_rect(layer, &r, &a);
}

// Culling test: a (label) box reaches the clip area. Labels get 2 px of slack above and below their line box.
bool reaches(const lv_area_t& box, const lv_area_t& clip, int32_t slack = 0)
{
    if (!s_cull) return true;
    lv_area_t b = box;
    b.y1 -= slack;
    b.y2 += slack;
    return areasOverlap(b, clip);
}

// What pad r draws, at most: its baked image (margins m around, mb below) and the hit ring (3 px out = m)
lv_area_t drawnExtent(const lv_area_t& r)
{
    return lv_area_t{r.x1 - s_g.m, r.y1 - s_g.m, r.x2 + s_g.m, r.y2 + s_g.mb};
}

void badge(lv_layer_t* layer, int32_t x2, int32_t y1, const char* text, uint32_t bg, uint32_t fg,
           const lv_area_t& clip)
{
    const int32_t sz = s_g.F(14);
    lv_area_t b = {x2 - sz, y1, x2, y1 + sz};
    if (!reaches(b, clip, 2)) return;
    fillRound(layer, b, bg, LV_OPA_COVER, s_g.F(3));
    drawLabel(layer, text, s_g.font(f_m12), c(fg), LV_OPA_COVER, b.x1, b.y1 + 0, b.x2, LV_TEXT_ALIGN_CENTER);
}

void drawPadImage(lv_layer_t* layer, int i, const lv_area_t& r)
{
    const bool active = i == s_sel;
    lv_image_dsc_t* img = s_g.s.unit() ? s_img[active ? 1 : 0] : s_img_big[active ? 1 : 0];
    if (img) {
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.src = img;
        lv_area_t ia = {r.x1 - s_g.m, r.y1 - s_g.m, r.x1 - s_g.m + s_g.img_w - 1, r.y1 - s_g.m + s_g.img_h - 1};
        lv_draw_image(layer, &d, &ia);
    } else {   // out of memory: plain LVGL look
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = c(active ? col::pad_on_a : col::pad_a);
        d.bg_grad.dir = LV_GRAD_DIR_VER;
        d.bg_grad.stops_count = 2;
        d.bg_grad.stops[0].color = d.bg_color;
        d.bg_grad.stops[0].opa = LV_OPA_COVER;
        d.bg_grad.stops[0].frac = 0;
        d.bg_grad.stops[1].color = c(active ? col::pad_on_b : col::pad_b);
        d.bg_grad.stops[1].opa = LV_OPA_COVER;
        d.bg_grad.stops[1].frac = 255;
        d.border_color = c(active ? col::pad_on_border : col::pad_border);
        d.border_width = 1;
        d.radius = s_g.r;
        lv_draw_rect(layer, &d, &r);
    }
}

void drawPadOverlay(lv_layer_t* layer, int i, const lv_area_t& r, const lv_area_t& clip)
{
    const PadView& pv = s_pv[i];
    const bool active = i == s_sel;
    const bool body = reaches(r, clip);   // the tints fill the pad body only
    if (pv.hit_level) {   // pad-hit: accent ring in the gap + a light tint
        const lv_opa_t k = (lv_opa_t)(255 * pv.hit_level / kHitLevels);
        lv_area_t o = r;
        const int32_t g3 = s_g.F(3), g1 = s_g.F(1);
        lv_area_increase(&o, g3, g3);
        if (!(s_skip & 2)) ringRound(layer, o, col::glow, (lv_opa_t)(k * 110 / 255), g1, s_g.r + g3);
        lv_area_increase(&o, -g1, -g1);
        if (!(s_skip & 2)) ringRound(layer, o, col::accent, (lv_opa_t)(k * 235 / 255), s_g.F(2), s_g.r + g3 - g1);
        if (!active && body && !(s_skip & 4)) fillRound(layer, r, col::accent, (lv_opa_t)(k * 60 / 255), s_g.r);
    }
    if (s_skip & 8) return;
    if (i == s_pressed && body) fillRound(layer, r, 0xFFFFFF, 46, s_g.r);
    const bool dim = pv.mute != 0;
    if (dim && body) fillRound(layer, r, col::panel, 130, s_g.r);
    const int32_t e8 = s_g.F(8);
    if (!active) {
        const lv_font_t* f = s_g.font(f_r12);
        const lv_area_t box = {r.x1 + e8, r.y1 + e8, r.x2 + s_g.m, r.y1 + e8 + (int32_t)lv_font_get_line_height(f) - 1};
        if (reaches(box, clip, 2))
            drawLabel(layer, kIndex[i], f, c(col::pad_index), dim ? 150 : LV_OPA_COVER, r.x1 + e8, r.y1 + e8,
                      r.x2 - s_g.F(4));
    }
    const int32_t lh = (int32_t)lv_font_get_line_height(pv.font);
    const int32_t ny = r.y2 - s_g.F(6) - lh;
    const lv_area_t nbox = {r.x1 + e8, ny, r.x2 + s_g.m, ny + lh - 1};
    if (reaches(nbox, clip, 2))
        drawLabel(layer, pv.name, pv.font, c(active ? col::on_accent : col::pad_text), dim ? 140 : LV_OPA_COVER,
                  r.x1 + e8, ny, r.x2 - s_g.F(3));
    int32_t bx = r.x2 - s_g.F(5);
    if (pv.mute) {
        badge(layer, bx, r.y1 + s_g.F(6), "M", col::muted, col::badge_fg, clip);
        bx -= s_g.F(18);
    }
    if (pv.solo) badge(layer, bx, r.y1 + s_g.F(6), "S", col::accent, col::on_accent, clip);
}

void gridDraw(lv_event_t* e);

void gridDrawCb(lv_event_t* e)
{
    const uint32_t c0 = profCycles();
    gridDraw(e);
    DrawProf& p = drawProf();
    p.cyc[DrawProf::kPads] += profCycles() - c0;
    ++p.calls[DrawProf::kPads];
}

void gridDraw(lv_event_t* e)
{
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer) return;
    lv_area_t g;
    lv_obj_get_coords(s_grid, &g);
    const lv_area_t clip = layer->_clip_area;
    // Pass 1: images of the plain pads, pass 2: the selected pad (its glow / deeper shadow may
    // reach into the gaps), pass 3: overlays (hit rings live in the gaps, so they come last).
    bool vis[kPads];
    lv_area_t ra[kPads];
    for (int i = 0; i < kPads; ++i) {
        padArea(i, g, ra[i]);
        lv_area_t ext = ra[i];
        if (s_cull) {
            ext = drawnExtent(ra[i]);   // exactly what the pad draws
        } else {
            ext.x1 -= s_g.m + 1;
            ext.x2 += s_g.m + 1;
            ext.y1 -= s_g.m + 1;
            ext.y2 += s_g.mb + 1;
        }
        vis[i] = areasOverlap(ext, clip);
    }
    for (int i = 0; i < kPads; ++i)
        if (vis[i] && i != s_sel && !(s_skip & 1)) drawPadImage(layer, i, ra[i]);
    if (s_sel >= 0 && s_sel < kPads && vis[s_sel] && !(s_skip & 1)) drawPadImage(layer, s_sel, ra[s_sel]);
    for (int i = 0; i < kPads; ++i)
        if (vis[i]) drawPadOverlay(layer, i, ra[i], clip);
}

// ================================================================ selection of played pads
// A pad played by a finger becomes the selected pad. The orange "selected" look moves at once
// (two pad rectangles); App::selectPad (revs.selection: the SAMPLE view re-renders its 718x211
// waveform cache and redraws ~172 K px, knobs, sequence rows) runs for the first hit after a
// pause and then once the hits pause for kSelQuietMs, so a drum roll over several pads costs
// two selections instead of one per hit.
constexpr uint32_t kSelQuietMs = 180;
int s_sel_pending = -1;       // pad to hand to App::selectPad once the hits pause, -1 none
uint32_t s_sel_req_t = 0;     // lv_tick of the last selection request
bool s_sel_req_any = false;

void requestSelect(int pad, uint32_t now)
{
    if (pad < 0 || pad >= kPads) return;
    const bool burst = s_sel_req_any && now - s_sel_req_t < kSelQuietMs;
    s_sel_req_any = true;
    s_sel_req_t = now;
    if (!burst && s_sel_pending < 0) {
        if (pad != app().ui().pad) app().selectPad(pad, false);   // already sounded: no audition
        return;
    }
    s_sel_pending = pad;
    if (pad != s_sel) {
        invalidatePad(s_sel);
        s_sel = pad;
        invalidatePad(s_sel);
    }
}

// ================================================================ input (LVGL)
void markTouched(int pad) { s_touched.fetch_or(1u << pad, std::memory_order_relaxed); }

void gridEventCb(lv_event_t* e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t* indev = lv_indev_active();
    switch (code) {
    case LV_EVENT_PRESSED: {
        lv_point_t p = {0, 0};   // screen point (velocity: s_abs is in screen coordinates)
        if (indev) lv_indev_get_point(indev, &p);
        const lv_point_t g = pointerIn(s_grid);   // the grid may be drawn scaled (maximize)
        s_press_pad = padAtGrid(g.x, g.y);
        s_pressed = s_press_pad;
        invalidatePad(s_pressed);
#ifdef ORBIT_HOST
        // No touch task on the PC: the LVGL press plays the pad like a finger would.
        if (s_press_pad >= 0 && app().ui().pad_mode == PadMode::Play) {
            int vel = velocityAt(s_press_pad, p.y);
            app().padDown(s_press_pad, vel, Producer::Ui);
            markTouched(s_press_pad);
        }
#endif
        break;
    }
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST: {
        const int was = s_pressed;
        s_pressed = -1;
        invalidatePad(was);
#ifdef ORBIT_HOST
        if (s_press_pad >= 0) app().padUp(s_press_pad, Producer::Ui);
#endif
        break;
    }
    case LV_EVENT_CLICKED: {
        const int pad = s_press_pad;
        if (pad < 0) break;
        const uint32_t bit = 1u << pad;
        const bool played = (s_touched.fetch_and(~bit, std::memory_order_relaxed) & bit) != 0;
        if (app().ui().pad_mode == PadMode::Play && played) requestSelect(pad, lv_tick_get());   // already sounded
        else app().padTap(pad);
        break;
    }
    default:
        break;
    }
}

void muteCb(lv_event_t*)
{
    App& a = app();
    a.setPadMode(a.ui().pad_mode == PadMode::Mute ? PadMode::Play : PadMode::Mute);
}

void soloCb(lv_event_t*)
{
    App& a = app();
    a.setPadMode(a.ui().pad_mode == PadMode::Solo ? PadMode::Play : PadMode::Solo);
}

// ================================================================ touch task
struct Finger {
    uint8_t id;
    int8_t pad;   // -1: the finger started outside a pad (or pads were disabled)
    bool used;
};
Finger s_fingers[5];

void touchHook(const hal::TouchPoint* pts, int n, void*)
{
    // Fingers that are gone: release their pad.
    for (Finger& f : s_fingers) {
        if (!f.used) continue;
        bool present = false;
        for (int k = 0; k < n; ++k) present = present || pts[k].id == f.id;
        if (present) continue;
        if (f.pad >= 0) app().padUp(f.pad, Producer::Touch);
        f.used = false;
    }
    // New fingers: play the pad under the down point.
    for (int k = 0; k < n; ++k) {
        bool known = false;
        for (const Finger& f : s_fingers) known = known || (f.used && f.id == pts[k].id);
        if (known) continue;
        Finger* slot = nullptr;
        for (Finger& f : s_fingers) {
            if (!f.used) {
                slot = &f;
                break;
            }
        }
        if (!slot) break;   // more fingers than tracked: ignore the extra ones
        slot->used = true;
        slot->id = pts[k].id;
        slot->pad = -1;
        if (!s_enabled.load(std::memory_order_acquire)) continue;
        const int pad = padAtTouch(pts[k].x, pts[k].y);
        if (pad < 0) continue;
        slot->pad = (int8_t)pad;
        app().padDown(pad, velocityAt(pad, pts[k].y), Producer::Touch);
        markTouched(pad);
        s_sel_q.push((uint8_t)pad);
    }
}

}  // namespace

void setCulling(bool on)
{
    s_cull = on;
    if (s_grid) lv_obj_invalidate(s_grid);
}

bool culling() { return s_cull; }

void setDebugSkip(uint8_t mask)
{
    s_skip = mask;
    if (s_grid) lv_obj_invalidate(s_grid);
}

void invalidateForTest(int i) { invalidatePad(i); }

lv_obj_t* create(lv_obj_t* scr)
{
    setGeo(uiBuildScale());   // a maximized panel is built at its scale (ui.cpp rebuildPanel)
    s_pressed = -1;
    s_press_pad = -1;
    bakeImages();
    lv_obj_t* panel = panelCreate(scr, geo::pads_x, geo::pads_y, geo::pads_w, geo::pads_h);
    if (bg::shadowsBaked()) lv_obj_set_style_shadow_width(panel, 0, 0);
    s_panel = panel;
    panelTopHighlight(panel, geo::pads_w);
    lv_obj_t* right = panelHead(panel, Icon::Grid, "PADS");
    lv_obj_set_style_pad_column(right, 5, 0);
    panelMaxButton(right, Panel::Pads);   // 0.3: maximize (performance pads)
    s_mute = microBtn(right, "MUTE", 43, 24);
    lv_obj_set_style_text_font(s_mute, f_r12, 0);   // 9 px / 400 in the prototype
    lv_obj_add_event_cb(s_mute, muteCb, LV_EVENT_CLICKED, nullptr);
    s_solo = microBtn(right, "SOLO", 43, 24);
    lv_obj_set_style_text_font(s_solo, f_r12, 0);
    lv_obj_add_event_cb(s_solo, soloCb, LV_EVENT_CLICKED, nullptr);
    // 5 px apart: the kit's uniform 8 px hit margin would let SOLO (the later sibling, tested first)
    // take the right 3 px of MUTE. Inner sides get 2 px each, the outer ones keep 8.
    setHitBox(s_mute, 2, 8, 2, 8);   // the maximize button sits 5 px to the left
    setHitBox(s_solo, 2, 8, 8, 8);

    lv_obj_t* g = lv_obj_create(panel);
    lv_obj_remove_style_all(g);
    lv_obj_set_pos(g, 0, geo::panel_head_h);
    lv_obj_set_size(g, kGridW, kGridH);
    lv_obj_add_flag(g, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(g, gridDrawCb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(g, gridEventCb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(g, gridEventCb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(g, gridEventCb, LV_EVENT_PRESS_LOST, nullptr);
    lv_obj_add_event_cb(g, gridEventCb, LV_EVENT_CLICKED, nullptr);
    s_grid = g;

    lv_obj_update_layout(panel);
    lv_area_t ga;
    lv_obj_get_coords(g, &ga);
    for (int i = 0; i < kPads; ++i) {
        lv_area_t r;
        padArea(i, ga, r);
        s_abs[i] = {(int16_t)r.x1, (int16_t)r.y1, (int16_t)r.x2, (int16_t)r.y2};
    }
    const Project& p = app().project();
    for (int i = 0; i < kPads; ++i) {
        PadView& pv = s_pv[i];
        memset(&pv, 0, sizeof pv);
        fitName(pv, p.pad[i].name);
        pv.mute = p.pad[i].mute;
        pv.solo = p.pad[i].solo;
    }
    s_sel = app().ui().pad_shown ? app().ui().pad : -1;
    s_rev_sel = app().revs().selection;
    s_rev_pads = app().revs().pads;
    s_rev_samples = app().revs().samples;
    s_rev_project = app().revs().project;
    s_mode_drawn = -1;
    s_sel_pending = -1;
    s_sel_req_any = false;
    refresh();
    return panel;
}

// After a maximize / restore the pads are drawn elsewhere and larger: the touch task's rectangles follow.
void touchGeometry()
{
    if (!s_grid || !s_panel) return;
    lv_obj_update_layout(s_panel);
    lv_area_t ga;
    lv_obj_get_coords(s_grid, &ga);
    for (int i = 0; i < kPads; ++i) {
        lv_area_t r;
        padArea(i, ga, r);   // s_g: the geometry the grid is drawn with now
        s_abs[i] = {(int16_t)r.x1, (int16_t)r.y1, (int16_t)r.x2, (int16_t)r.y2};
    }
}

// The touch task's hook: every sample goes to the pads and to the BASS KEYS keyboard (each takes the fingers
// that came down on it).
static void touchRouter(const hal::TouchPoint* pts, int n, void*)
{
    touchHook(pts, n, nullptr);
    view_bass::touchInput(pts, n);
}

void installTouchHook()
{
    for (Finger& f : s_fingers) f = Finger{0, -1, false};
    hal::setTouchHook(touchRouter, nullptr);
}

void touchInput(const hal::TouchPoint* pts, int n) { touchHook(pts, n, nullptr); }

void refresh()
{
    if (!s_grid) return;
    App& a = app();
    const Revs& rv = a.revs();
    const Settings& st = a.settings();

    // Touch hook inputs (read by the touch task).
    const bool on_screen = !lv_obj_has_flag(s_panel, LV_OBJ_FLAG_HIDDEN) && lv_obj_get_screen(s_panel) == lv_screen_active();
    const bool en = on_screen && a.ui().pad_mode == PadMode::Play && !overlayOpen();
    if (s_enabled.load(std::memory_order_relaxed) != en) {
        s_enabled.store(en, std::memory_order_release);
        if (!en) s_touched.store(0, std::memory_order_relaxed);
    }
    s_vel_position.store(st.pad_velocity == PadVelocity::Position ? 1 : 0, std::memory_order_relaxed);
    s_vel_fixed.store(st.fixed_velocity, std::memory_order_relaxed);

    // Pads hit by a finger: select the last one (it already sounded), deferred during a burst.
    const uint32_t now = lv_tick_get();
    uint8_t q = 0;
    int last = -1;
    while (s_sel_q.pop(q)) last = q;
    if (last >= 0) requestSelect(last, now);
    if (s_sel_pending >= 0 && now - s_sel_req_t >= kSelQuietMs) {
        const int pad = s_sel_pending;
        s_sel_pending = -1;
        if (pad != a.ui().pad) a.selectPad(pad, false);
    }

    if (rv.selection != s_rev_sel) {
        s_rev_sel = rv.selection;
        // After NEW PROJECT no pad looks selected until one is touched (UiState::pad_shown).
        const int sel = s_sel_pending >= 0 ? s_sel_pending : (a.ui().pad_shown ? a.ui().pad : -1);
        if (sel != s_sel) {
            invalidatePad(s_sel);
            s_sel = sel;
            invalidatePad(s_sel);
        }
    }
    if (rv.pads != s_rev_pads || rv.samples != s_rev_samples || rv.project != s_rev_project) {
        s_rev_pads = rv.pads;
        s_rev_samples = rv.samples;
        s_rev_project = rv.project;
        const Project& p = a.project();
        for (int i = 0; i < kPads; ++i) {
            PadView& pv = s_pv[i];
            bool dirty = false;
            if (strncmp(pv.src, p.pad[i].name, sizeof pv.src) != 0) {
                fitName(pv, p.pad[i].name);
                dirty = true;
            }
            if (pv.mute != p.pad[i].mute || pv.solo != p.pad[i].solo) {
                pv.mute = p.pad[i].mute;
                pv.solo = p.pad[i].solo;
                dirty = true;
            }
            if (dirty) invalidatePad(i);
        }
        const int mode = (int)a.ui().pad_mode;
        if (mode != s_mode_drawn) {
            s_mode_drawn = mode;
            btnSetActive(s_mute, mode == (int)PadMode::Mute);
            btnSetActive(s_solo, mode == (int)PadMode::Solo);
        }
    }

    // Hit flashes: 280 ms, quickly up (60 ms) then down; redrawn only on a level change.
    const uint32_t bits = a.padFlashBits();
    if (bits && st.haptic_indicator) {
        for (int i = 0; i < kPads; ++i) {
            if (!(bits & (1u << i))) continue;
            s_pv[i].hit_on = true;
            s_pv[i].hit_t0 = now;
        }
    }
    for (int i = 0; i < kPads; ++i) {
        PadView& pv = s_pv[i];
        if (!pv.hit_on) continue;
        const uint32_t el = now - pv.hit_t0;
        int lvl;
        if (el >= kHitMs) {
            lvl = 0;
            pv.hit_on = false;
        } else if (el < 60) {
            lvl = (int)(kHitLevels * (0.6f + 0.4f * (float)el / 60.f) + 0.5f);
        } else {
            lvl = (int)(kHitLevels * (1.f - (float)(el - 60) / (float)(kHitMs - 60)) + 0.999f);
        }
        if (lvl < 0) lvl = 0;
        if (lvl > kHitLevels) lvl = kHitLevels;
        if (lvl != pv.hit_level) {
            pv.hit_level = (uint8_t)lvl;
            invalidatePad(i);
        }
    }
}

}  // namespace orbit::ui::pads
