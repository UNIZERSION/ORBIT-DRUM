// SEQUENCE step grid: one custom-drawn object (see step_grid.h).
#include "ui/step_grid.h"

#include <stdio.h>
#include <string.h>

#include "core/prof.h"
#include "ui/app.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace orbit::ui::step_grid {

namespace {

constexpr int kOutline = 3;   // outline offset 1 + width 2 (.seq-step.current: outline offset 1px)

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// a*w + b*(256-w) per channel (w = 0..256), as 0xRRGGBB.
uint32_t mixHex(uint32_t a, uint32_t b, int w)
{
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        const int ca = (int)((a >> sh) & 0xFFu), cb = (int)((b >> sh) & 0xFFu);
        out |= (uint32_t)((ca * w + cb * (256 - w) + 128) >> 8) << sh;
    }
    return out;
}

struct LaneRange {
    int lo, hi;
    bool bipolar;
};
LaneRange laneRange(StepLane l)
{
    switch (l) {
    case StepLane::Velocity: return {1, 127, false};
    case StepLane::Probability: return {0, 100, false};
    case StepLane::Ratchet: return {1, 4, false};
    case StepLane::Nudge: return {-50, 50, true};
    case StepLane::Pitch: return {-24, 24, true};
    case StepLane::Cond: return {0, (int)TrigCond::Count - 1, false};
    default: return {0, 1, false};
    }
}

struct Grid {
    lv_obj_t* obj = nullptr;
    uint32_t key[kRows][kCols] = {};
    int8_t ph[kRows] = {-1, -1, -1, -1, -1, -1, -1, -1};
    StepLane mode = StepLane::Trig;
    int num_page = -1, num_len = -1;
    // gesture
    bool active = false;
    bool paint_on = false;
    bool moved = false;
    uint16_t visited[kRows] = {};
    int last_x = 0, last_y = 0;   // screen px of the last processed point
    int press_x = 0, press_y = 0;
    int press_col = -1;
    // Trig painting stays on one line of cells: the row of the first cell (a horizontal drag that
    // drifts a few px never writes into the neighbouring track), or its column when the drag
    // starts out vertical (|dy| > 2 |dx| when the finger has moved 8 px).
    int8_t paint_row = 0;
    int8_t paint_axis = 0;             // 0: not decided yet (row), 1: row, 2: column
    lv_obj_t* cond_anchor = nullptr;   // invisible box the COND list opens under
    // sparks (Trig lane): cells lit by a hit, fading out (see tickSparks)
    uint32_t spark_t0[kRows][kCols] = {};
    uint16_t sparking[kRows] = {};     // bit per column with a spark still fading
    int8_t spark_bank = -1, spark_page = -1;
    uint32_t spark_tick_ms = 0;        // last tickSparks()
};
Grid g;

// Sparks: a step that sounds lights up like a lamp — a white-hot centre inside the orange cell and
// an accent glow around it — and fades out over kSparkMs, so at 16ths the last 2-3 hits of a row
// still glow behind the playhead. Driven by the pads that really sounded (probability,
// conditions, ratchets), not by the pattern alone.
constexpr uint32_t kSparkMs = 400;
constexpr uint8_t kGlowOpa[3] = {190, 105, 45};   // 1 px glow rings around the cell, inner to outer
constexpr int kCoreLayers = 5;                     // nested white layers 2 px apart, outer to inner
constexpr uint8_t kCoreOpa = 44;                   // each; the centre adds up to ~60 % white

char s_num[kMaxSteps][4];   // "1".."64" (static: label draw tasks keep the pointer)

const Scene& curScene()
{
    const Project& p = app().project();
    return p.scene_data[p.scene < kScenes ? p.scene : 0];
}
int sceneLen(const Scene& s) { return clampi(s.length, 1, kMaxSteps); }
// Bar shown: never past the current scene's last bar (a page left over from a longer scene would
// show and edit steps the scene does not play).
int curPage()
{
    const int pages = (sceneLen(curScene()) + kStepsPerPage - 1) / kStepsPerPage;
    return clampi(app().ui().seq_page, 0, pages - 1);
}
int curBank() { return clampi(app().ui().seq_bank, 0, 1); }

// Offset of the real object from the constant screen geometry (0 when placed as designed).
void objOffset(int* dx, int* dy)
{
    lv_area_t a;
    lv_obj_get_coords(g.obj, &a);
    *dx = a.x1 - kObjX;
    *dy = a.y1 - kObjY;
}

// Design (constant screen) geometry -> where the object is drawn now (a maximized panel: larger, ui_scale.h).
Xf gridXf() { return xfOf(g.obj, kObjX, kObjY); }
lv_area_t mapArea(const Xf& t, const lv_area_t& a) { return t.area((float)a.x1, (float)a.y1, (float)(a.x2 + 1), (float)(a.y2 + 1)); }

lv_area_t cellArea(int r, int c, int grow)
{
    lv_area_t a;
    a.x1 = colX(c) - grow;
    a.x2 = colX(c) + colW(c) - 1 + grow;
    a.y1 = rowY(r) - grow;
    a.y2 = rowY(r) + kCellH - 1 + grow;
    return a;
}
lv_area_t laneColArea(int c, int grow)
{
    lv_area_t a;
    a.x1 = colX(c) - grow;
    a.x2 = colX(c) + colW(c) - 1 + grow;
    a.y1 = kRowY0 - grow;
    a.y2 = kLaneBarsY1 + grow;
    return a;
}

void invalidateScreenArea(lv_area_t a)
{
    if (!g.obj) return;
    const Xf t = gridXf();
    lv_area_t m = mapArea(t, a);
    if (!t.s.unit()) lv_area_increase(&m, 1, 1);   // rounding of the scaled edges
    lv_obj_invalidate_area(g.obj, &m);
}

void invalidateCell(int r, int c)
{
    if (r < 0 || r >= kRows || c < 0 || c >= kCols) return;
    invalidateScreenArea(g.mode == StepLane::Trig ? cellArea(r, c, kOutline) : laneColArea(c, kOutline));
}

void invalidateNumbers()
{
    lv_area_t a = {colX(0), kNumY, colX(kCols - 1) + colW(kCols - 1) - 1, kNumY + 16};
    invalidateScreenArea(a);
}

uint32_t trigKey(const Scene& sc, int r, int c)
{
    const int pad = curBank() * kRows + r, s = curPage() * kStepsPerPage + c;
    const Step& st = sc.track[pad].step[s];
    uint32_t k = 1u;
    if (s >= trackLength(sc, pad)) k |= 4u;
    if (st.on) {
        k |= 2u;
        k |= (uint32_t)(st.vel >> 2) << 3;
        k |= (uint32_t)(st.prob < 100) << 10;
        k |= (uint32_t)(clampi(st.ratchet, 1, 4) - 1) << 11;
        k |= (uint32_t)(st.cond != TrigCond::Always) << 13;
        k |= (uint32_t)(st.flam != 0) << 14;
    }
    return k;
}

uint32_t laneKey(const Scene& sc, int c)
{
    const int pad = clampi(app().ui().pad, 0, kPads - 1), s = curPage() * kStepsPerPage + c;
    const StepLane lane = app().ui().lane;
    const int v = app().stepLaneValue(pad, s, lane);
    uint32_t k = 1u;
    if (sc.track[pad].step[s].on) k |= 2u;
    if (s >= trackLength(sc, pad)) k |= 4u;
    k |= (uint32_t)((v + 128) & 0xFF) << 3;
    k |= (uint32_t)lane << 11;
    return k;
}

void computeKeys(bool invalidate_changed)
{
    const Scene& sc = curScene();
    if (g.mode == StepLane::Trig) {
        for (int r = 0; r < kRows; ++r)
            for (int c = 0; c < kCols; ++c) {
                const uint32_t k = trigKey(sc, r, c);
                if (k != g.key[r][c]) {
                    g.key[r][c] = k;
                    if (invalidate_changed) invalidateCell(r, c);
                }
            }
    } else {
        for (int c = 0; c < kCols; ++c) {
            const uint32_t k = laneKey(sc, c);
            if (k != g.key[0][c]) {
                g.key[0][c] = k;
                if (invalidate_changed) invalidateCell(0, c);
            }
        }
    }
}

// ------------------------------------------------------------------------------ drawing
bool overlaps(const lv_area_t& a, const lv_area_t& b)
{
    return a.x1 <= b.x2 && a.x2 >= b.x1 && a.y1 <= b.y2 && a.y2 >= b.y1;
}

void fillRect(lv_layer_t* layer, const lv_area_t& a, uint32_t color, int radius, lv_opa_t opa = LV_OPA_COVER)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c(color);
    d.bg_opa = opa;
    d.radius = radius;
    lv_draw_rect(layer, &d, &a);
}

void boxRect(lv_layer_t* layer, const lv_area_t& a, uint32_t bg, uint32_t border, int radius)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c(bg);
    d.bg_opa = LV_OPA_COVER;
    d.border_color = c(border);
    d.border_width = 1;
    d.border_opa = LV_OPA_COVER;
    d.radius = radius;
    lv_draw_rect(layer, &d, &a);
}

void outlineRect(lv_layer_t* layer, const lv_area_t& a, uint32_t color, int width, int radius, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_TRANSP;
    d.border_color = c(color);
    d.border_width = width;
    d.border_opa = opa;
    d.radius = radius;
    lv_draw_rect(layer, &d, &a);
}

void text(lv_layer_t* layer, const lv_area_t& a, const char* t, const lv_font_t* f, uint32_t color, bool copy)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = t;
    d.text_local = copy ? 1 : 0;
    d.font = f;
    d.color = c(color);
    d.align = LV_TEXT_ALIGN_CENTER;
    lv_draw_label(layer, &d, &a);
}

// Velocity tint: full accent from velocity 100 up, blended towards step-off below (never under
// ~45 % accent, so a quiet step still reads as "on").
uint32_t onColor(int vel)
{
    const int w = vel >= 100 ? 256 : 116 + (vel * 140) / 100;
    return mixHex(col::accent, col::step_off, w);
}
constexpr int kDimW = 110;   // weight of the real colour for steps beyond the track length

void drawNumbers(lv_layer_t* layer, const Xf& t, const lv_area_t& clip)
{
    const int page = curPage(), len = sceneLen(curScene());
    const lv_font_t* f12 = t.font(f_r12);
    for (int c = 0; c < kCols; ++c) {
        const lv_area_t a = t.area((float)(colX(c) - 2), (float)kNumY, (float)(colX(c) + colW(c) + 2), (float)(kNumY + 16));
        if (!overlaps(a, clip)) continue;
        const int s = page * kStepsPerPage + c;
        text(layer, a, s_num[s], f12, s < len ? col::muted : mixHex(col::muted, col::seq_bg, 110), false);
    }
}

void drawMarks(lv_layer_t* layer, const Xf& tf, const lv_area_t& a, const Step& st, lv_opa_t opa)
{
    const uint32_t mc = col::on_accent;
    auto F = [&tf](int v) { return tf.F((float)v); };
    if (st.prob < 100) {   // probability: small dot, top right
        lv_area_t d = {a.x2 - F(7), a.y1 + F(4), a.x2 - F(4), a.y1 + F(7)};
        fillRect(layer, d, mc, LV_RADIUS_CIRCLE, opa);
    }
    if (st.cond != TrigCond::Always) {   // condition: small triangle, top left
        lv_draw_triangle_dsc_t t;
        lv_draw_triangle_dsc_init(&t);
        t.p[0].x = a.x1 + F(3);
        t.p[0].y = a.y1 + F(3);
        t.p[1].x = a.x1 + F(9);
        t.p[1].y = a.y1 + F(3);
        t.p[2].x = a.x1 + F(3);
        t.p[2].y = a.y1 + F(9);
        t.color = c(mc);
        t.opa = opa;
        lv_draw_triangle(layer, &t);
    }
    const int rat = clampi(st.ratchet, 1, 4);
    if (rat > 1) {   // ratchet: tiny ticks, bottom left
        for (int i = 0; i < rat; ++i) {
            lv_area_t tk = {a.x1 + F(4 + i * 4), a.y2 - F(8), a.x1 + F(5 + i * 4), a.y2 - F(4)};
            fillRect(layer, tk, mc, 0, opa);
        }
    }
    if (st.flam) {   // flam: small bar, bottom right
        lv_area_t b = {a.x2 - F(10), a.y2 - F(5), a.x2 - F(4), a.y2 - F(4)};
        fillRect(layer, b, mc, F(1), opa);
    }
}

// 1 at the hit, then a fast fall with a long soft tail; 0 when the cell has no spark.
float sparkLevel(int r, int c, uint32_t now)
{
    if (!(g.sparking[r] & (1u << c))) return 0.f;
    const uint32_t el = now - g.spark_t0[r][c];
    if (el >= kSparkMs) return 0.f;
    const float t = 1.f - (float)el / (float)kSparkMs;
    return t * t;
}

void drawTrig(lv_layer_t* layer, const Xf& t, const lv_area_t& clip)
{
    const int outl = t.F(kOutline);
    const Scene& sc = curScene();
    const int bank = curBank(), page = curPage();
    const uint32_t now = lv_tick_get();
    for (int r = 0; r < kRows; ++r) {
        const int pad = bank * kRows + r;
        const int tl = trackLength(sc, pad);
        for (int col = 0; col < kCols; ++col) {
            const lv_area_t a = mapArea(t, cellArea(r, col, 0));
            lv_area_t ext = a;
            lv_area_increase(&ext, outl, outl);
            if (!overlaps(ext, clip)) continue;
            const int s = page * kStepsPerPage + col;
            const Step& st = sc.track[pad].step[s];
            const bool dim = s >= tl;
            if (st.on) {
                uint32_t fc = onColor(st.vel);
                if (dim) fc = mixHex(fc, col::seq_bg, kDimW);
                const float f = dim ? 0.f : sparkLevel(r, col, now);
                if (f > 0.f) {   // spark glow: rings in the 3 px around the cell
                    for (int i = 0; i < 3; ++i) {
                        lv_area_t o = a;
                        lv_area_increase(&o, t.F((float)(i + 1)), t.F((float)(i + 1)));
                        outlineRect(layer, o, col::accent, t.F(1), t.F((float)(3 + i)),
                                    (lv_opa_t)((float)kGlowOpa[i] * f + 0.5f));
                    }
                }
                fillRect(layer, a, fc, t.F(2));
                if (f > 0.f) {   // spark core: nested white layers, brightest in the middle
                    const lv_opa_t opa = (lv_opa_t)((float)kCoreOpa * f + 0.5f);
                    for (int i = 0; i < kCoreLayers && opa > 0; ++i) {
                        lv_area_t in = a;
                        const int k = t.F((float)(2 + 2 * i));
                        lv_area_increase(&in, -k, -k);
                        if (in.x2 - in.x1 < 3 || in.y2 - in.y1 < 3) break;
                        fillRect(layer, in, 0xFFFFFF, t.F((float)(6 - i)), opa);
                    }
                }
                drawMarks(layer, t, a, st, dim ? 80 : 170);
            } else if (dim) {
                boxRect(layer, a, mixHex(col::step_off, col::seq_bg, kDimW),
                        mixHex(col::step_border, col::seq_bg, kDimW + 40), t.F(2));
            } else {
                boxRect(layer, a, col::step_off, col::step_border, t.F(2));
            }
            if (g.ph[r] == col) {
                lv_area_t o = a;
                lv_area_increase(&o, outl, outl);
                outlineRect(layer, o, col::ink, t.F(2), t.F(4), LV_OPA_COVER);
            }
        }
    }
}

void laneValueText(StepLane lane, int v, char* buf, int len)
{
    switch (lane) {
    case StepLane::Velocity: snprintf(buf, (size_t)len, "%d", v); break;
    case StepLane::Probability: snprintf(buf, (size_t)len, "%d%%", v); break;
    case StepLane::Ratchet: snprintf(buf, (size_t)len, "x%d", v); break;
    case StepLane::Nudge:
    case StepLane::Pitch:
        if (v == 0) snprintf(buf, (size_t)len, "0");
        else snprintf(buf, (size_t)len, "%+d", v);
        break;
    case StepLane::Cond:
        if (v == 0) snprintf(buf, (size_t)len, "\xE2\x80\x93");   // en dash: always
        else snprintf(buf, (size_t)len, "%s", trigCondName((TrigCond)v));
        break;
    default: buf[0] = 0; break;
    }
}

int valueY(const LaneRange& lr, int v)   // screen y of a value inside the bars area
{
    const int h = kLaneBarsY1 - kLaneBarsY0;
    const int n = lr.hi - lr.lo;
    return kLaneBarsY1 - (int)((long)(v - lr.lo) * h / (n > 0 ? n : 1));
}

void drawLane(lv_layer_t* layer, const Xf& t, const lv_area_t& clip)
{
    const lv_font_t* m12 = t.font(f_m12);
    const int outl = t.F(kOutline), e3 = t.F(3);
    const Scene& sc = curScene();
    const StepLane lane = g.mode;
    const LaneRange lr = laneRange(lane);
    const int pad = clampi(app().ui().pad, 0, kPads - 1), page = curPage();
    const int tl = trackLength(sc, pad);
    const uint32_t track_c = mixHex(col::step_off, col::seq_bg, 150);
    for (int col = 0; col < kCols; ++col) {
        const lv_area_t full = mapArea(t, laneColArea(col, kOutline));
        if (!overlaps(full, clip)) continue;
        const int s = page * kStepsPerPage + col;
        const Step& st = sc.track[pad].step[s];
        const bool dim = s >= tl;
        const int v = app().stepLaneValue(pad, s, lane);
        char buf[12];
        laneValueText(lane, v, buf, sizeof buf);
        const int x1 = t.X(colX(col)), x2 = t.X(colX(col) + colW(col)) - 1;
        lv_area_t bars = {x1, t.Y(kLaneBarsY0), x2, t.Y(kLaneBarsY1 + 1) - 1};
        // The bar height already shows the value: on steps are plain accent, off steps hollow.
        const uint32_t ac = dim ? mixHex(col::accent, col::seq_bg, kDimW) : col::accent;
        if (lane == StepLane::Cond) {
            // Chips: on + condition = accent; on + always = light accent tint with accent border;
            // off = step-off (accent border when a condition is set).
            const uint32_t off_c = dim ? mixHex(col::step_off, col::seq_bg, kDimW) : col::step_off;
            uint32_t tc;
            if (st.on && v) {
                fillRect(layer, bars, ac, t.F(2));
                tc = col::on_accent;
            } else if (st.on) {
                boxRect(layer, bars, dim ? mixHex(col::active_bg, col::seq_bg, kDimW) : col::active_bg, ac, t.F(2));
                tc = col::accent_dark;
            } else {
                boxRect(layer, bars, off_c, v ? ac : col::step_border, t.F(2));
                tc = dim ? col::muted : col::ink;
            }
            const int mid = (kLaneBarsY0 + kLaneBarsY1) / 2;
            lv_area_t ta = {x1 - e3, t.Y(mid - 8), x2 + e3, t.Y(mid + 8)};
            text(layer, ta, buf, m12, tc, true);
        } else {
            fillRect(layer, bars, dim ? mixHex(track_c, col::seq_bg, 160) : track_c, t.F(2));
            lv_area_t bar = bars;
            if (lr.bipolar) {
                const int mid = t.Y(valueY(lr, 0)), y = t.Y(valueY(lr, v));
                bar.y1 = y < mid ? y : mid;
                bar.y2 = y < mid ? mid : y;
                if (bar.y2 - bar.y1 < t.F(2)) {
                    bar.y1 = mid - t.F(1);
                    bar.y2 = mid + t.F(1);
                }
                // zero line across the column
                lv_area_t zl = {x1, mid, x2, mid + t.F(1) - 1};
                fillRect(layer, zl, col::step_border, 0);
            } else {
                bar.y1 = t.Y(valueY(lr, v));
                if (bar.y2 - bar.y1 < t.F(3)) bar.y1 = bar.y2 - t.F(3);
            }
            if (st.on) fillRect(layer, bar, ac, t.F(2));
            else outlineRect(layer, bar, ac, t.F(2), t.F(2), LV_OPA_COVER);
            lv_area_t ta = {x1 - e3, t.Y(kRowY0), x2 + e3, t.Y(kRowY0 + kLaneTextH) - 1};
            text(layer, ta, buf, m12, dim ? col::muted : col::ink, true);
        }
        if (g.ph[0] == col) {
            lv_area_t o = mapArea(t, laneColArea(col, 0));
            lv_area_increase(&o, outl, outl);
            outlineRect(layer, o, col::ink, t.F(2), t.F(4), LV_OPA_COVER);
        }
    }
}

void draw(lv_layer_t* layer)
{
    const Xf t = gridXf();
    const lv_area_t clip = layer->_clip_area;
    drawNumbers(layer, t, clip);
    if (g.mode == StepLane::Trig) drawTrig(layer, t, clip);
    else drawLane(layer, t, clip);
}

// ------------------------------------------------------------------------------ touch
int colAt(int x)   // screen x -> column (boundaries in the middle of the gaps)
{
    const int cx = x - kCellX0;
    return clampi((2 * cx + 5) / 75 - (cx < -2 ? 1 : 0), 0, kCols - 1);
}
int rowAt(int y)
{
    const int cy = y - kRowY0;
    return clampi((2 * cy + 5) / 68 - (cy < -2 ? 1 : 0), 0, kRows - 1);
}
int valueAt(const LaneRange& lr, int y)
{
    const int h = kLaneBarsY1 - kLaneBarsY0;
    int d = kLaneBarsY1 - y;
    d = clampi(d, 0, h);
    const int n = lr.hi - lr.lo;
    return lr.lo + (int)((d * n + h / 2) / (h > 0 ? h : 1));
}

bool paintAt(int x, int y)
{
    const bool column = g.paint_axis == 2;
    const int r = column ? rowAt(y) : clampi(g.paint_row, 0, kRows - 1);
    const int col = column ? clampi(g.press_col, 0, kCols - 1) : colAt(x);
    if (g.visited[r] & (1u << col)) return false;
    g.visited[r] |= (uint16_t)(1u << col);
    const int pad = curBank() * kRows + r, s = curPage() * kStepsPerPage + col;
    const bool was = app().step(pad, s).on != 0;
    app().setStepOn(pad, s, g.paint_on);
    return was != g.paint_on;
}

bool laneAt(int x, int y)
{
    const StepLane lane = app().ui().lane;
    const int pad = clampi(app().ui().pad, 0, kPads - 1);
    const int s = curPage() * kStepsPerPage + colAt(x);
    const int v = valueAt(laneRange(lane), y);
    if (app().stepLaneValue(pad, s, lane) == v) return false;
    app().setStepLane(pad, s, lane, v);
    return true;
}

// Walks the segment from the last processed point to (x, y) in <= 4 px steps, so a fast sweep
// never skips a cell / column.
bool walkTo(int x, int y, bool (*fn)(int, int))
{
    const int dxp = x - g.last_x, dyp = y - g.last_y;
    const int adx = dxp < 0 ? -dxp : dxp, ady = dyp < 0 ? -dyp : dyp;
    const int n = (adx > ady ? adx : ady) / 4 + 1;
    bool changed = false;
    for (int i = 1; i <= n; ++i) changed |= fn(g.last_x + dxp * i / n, g.last_y + dyp * i / n);
    g.last_x = x;
    g.last_y = y;
    return changed;
}

// COND lane, tap without a drag: the list of conditions for that step (a stray tap opens a list
// that can be dismissed instead of silently changing the condition).
void condPicked(int index, void* user)
{
    const int packed = (int)(intptr_t)user;
    const int pad = packed / kMaxSteps, s = packed % kMaxSteps;
    if (index < 0 || index >= (int)TrigCond::Count) return;
    app().setStepLane(pad, s, StepLane::Cond, index);
    refresh();
}

void openCondList(int col)
{
    static const char* items[(int)TrigCond::Count];
    for (int i = 0; i < (int)TrigCond::Count; ++i) items[i] = trigCondName((TrigCond)i);
    col = clampi(col, 0, kCols - 1);
    const int pad = clampi(app().ui().pad, 0, kPads - 1);
    const int s = curPage() * kStepsPerPage + col;
    const int cur = app().stepLaneValue(pad, s, StepLane::Cond);
    lv_obj_t* anchor = g.obj;
    if (g.cond_anchor) {
        const Xf t = gridXf();   // the anchor lives in the (maybe maximized) grid object
        lv_obj_set_pos(g.cond_anchor, t.W((float)(colX(col) - kObjX)), t.H((float)(kLaneBarsY0 - 2 - kObjY)));
        lv_obj_set_width(g.cond_anchor, t.W((float)colW(col)));
        lv_obj_update_layout(g.cond_anchor);
        anchor = g.cond_anchor;
    }
    popupMenu(anchor, items, (int)TrigCond::Count, cur, condPicked, (void*)(intptr_t)(pad * kMaxSteps + s));
}

void screenPoint(lv_event_t* e, int* x, int* y)
{
    (void)e;
    const lv_point_t p = pointerIn(g.obj);   // the work panel may be drawn scaled (maximize)
    int dx, dy;
    objOffset(&dx, &dy);
    *x = p.x - dx;   // in the constant screen geometry
    *y = p.y - dy;
}

void onEvent(lv_event_t* e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DRAW_MAIN) {
        const uint32_t c0 = profCycles();
        draw(lv_event_get_layer(e));
        DrawProf& p = drawProf();
        p.cyc[DrawProf::kStepGrid] += profCycles() - c0;
        ++p.calls[DrawProf::kStepGrid];
        return;
    }
    if (code == LV_EVENT_DELETE) {
        g.obj = nullptr;
        g.cond_anchor = nullptr;   // deleted with its parent
        return;
    }
    const StepLane lane = app().ui().lane;
    if (code == LV_EVENT_PRESSED) {
        int x, y;
        screenPoint(e, &x, &y);
        g.press_x = x;
        g.press_y = y;
        g.last_x = x;
        g.last_y = y;
        g.moved = false;
        g.press_col = colAt(x);
        g.paint_axis = 0;
        g.active = y >= kRowY0 - 3;   // the step-number row is not a cell
        if (!g.active) return;
        bool changed = false;
        if (lane == StepLane::Trig) {
            memset(g.visited, 0, sizeof g.visited);
            const int r = rowAt(y), col = colAt(x);
            g.paint_row = (int8_t)r;
            g.paint_on = app().step(curBank() * kRows + r, curPage() * kStepsPerPage + col).on == 0;
            changed = paintAt(x, y);
        } else if (lane != StepLane::Cond) {
            changed = laneAt(x, y);   // COND: nothing on press (a tap opens the list, a drag sets it)
        }
        if (changed) refresh();
    } else if (code == LV_EVENT_PRESSING) {
        if (!g.active) return;
        int x, y;
        screenPoint(e, &x, &y);
        const int dx = x - g.press_x, dy = y - g.press_y;
        if (!g.moved && dx * dx + dy * dy > 64) {
            g.moved = true;
            if (lane == StepLane::Trig && g.paint_axis == 0) {
                const int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
                g.paint_axis = ady > 2 * adx ? 2 : 1;
            }
        }
        bool changed = false;
        if (lane == StepLane::Trig) changed = walkTo(x, y, paintAt);
        else if (lane != StepLane::Cond || g.moved) changed = walkTo(x, y, laneAt);
        if (changed) refresh();
    } else if (code == LV_EVENT_CLICKED) {
        if (!g.active || g.moved || lane != StepLane::Cond || g.press_col < 0) return;
        openCondList(g.press_col);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        g.active = code == LV_EVENT_RELEASED && g.active;   // CLICKED follows RELEASED
        memset(g.visited, 0, sizeof g.visited);
    }
}

}  // namespace

lv_obj_t* create(lv_obj_t* parent, int x, int y)
{
    for (int i = 0; i < kMaxSteps; ++i) snprintf(s_num[i], sizeof s_num[i], "%d", i + 1);
    g = Grid{};
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, kObjW, kObjH);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(o, onEvent, LV_EVENT_ALL, nullptr);
    g.obj = o;
    // Anchor of the COND list: an invisible, non-clickable child moved under the tapped column.
    lv_obj_t* an = lv_obj_create(o);
    lv_obj_remove_style_all(an);
    lv_obj_set_size(an, colW(0), 1);
    lv_obj_remove_flag(an, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(an, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(an, LV_OBJ_FLAG_IGNORE_LAYOUT);
    g.cond_anchor = an;
    g.mode = app().ui().lane;
    computeKeys(false);
    g.num_page = curPage();
    g.num_len = sceneLen(curScene());
    return o;
}

void refresh()
{
    if (!g.obj) return;
    const StepLane lane = app().ui().lane;
    if (lane != g.mode) {
        g.mode = lane;
        for (auto& row : g.key)
            for (auto& k : row) k = 0;
        for (auto& p : g.ph) p = -1;
        computeKeys(false);
        invalidateAll();
    } else {
        computeKeys(true);
    }
    const int page = curPage(), len = sceneLen(curScene());
    if (page != g.num_page || len != g.num_len) {
        g.num_page = page;
        g.num_len = len;
        invalidateNumbers();
    }
    tickPlayhead();
}

namespace {

// Where the engine plays now (only for the edited scene): the column of each pad's track step on
// the shown bar, polymeter aware; -1 when that pad's step is not on this bar or nothing plays.
struct PlayPos {
    bool show;
    int step;
    uint32_t loop;
    int len, page;
};

PlayPos playPos()
{
    const Telemetry& t = app().engine().telemetry();
    PlayPos p;
    p.step = t.step.load(std::memory_order_relaxed);
    p.loop = t.loop_count.load(std::memory_order_relaxed);
    const int psc = t.scene.load(std::memory_order_relaxed);
    p.show = t.playing.load(std::memory_order_relaxed) && p.step >= 0 && psc == (int)app().project().scene;
    p.len = sceneLen(curScene());
    p.page = curPage();
    return p;
}

int playCol(const PlayPos& p, const Scene& sc, int pad)
{
    if (!p.show) return -1;
    const int tl = trackLength(sc, pad);
    const int ts = (int)(((uint64_t)p.loop * (uint64_t)p.len + (uint64_t)p.step) % (uint64_t)tl);
    const int col = ts - p.page * kStepsPerPage;
    return (col >= 0 && col < kCols) ? col : -1;
}

bool sparkable(const Scene& sc, int pad, int page, int col)   // an on step inside the track
{
    const int s = page * kStepsPerPage + col;
    return col >= 0 && col < kCols && s < trackLength(sc, pad) && sc.track[pad].step[s].on;
}

void clearSparks()
{
    for (int r = 0; r < kRows; ++r) {
        for (uint16_t m = g.sparking[r]; m; m &= (uint16_t)(m - 1)) invalidateCell(r, __builtin_ctz(m));
        g.sparking[r] = 0;
    }
}

}  // namespace

void tickSparks()
{
    if (!g.obj) return;
    const uint32_t now = lv_tick_get();
    // Drained every frame. Hits collected while the view was hidden (no ticks) are stale.
    uint32_t hits = app().takeGridHits();
    if (now - g.spark_tick_ms > 100) hits = 0;
    g.spark_tick_ms = now;
    const Settings& st = app().settings();
    const bool on = g.mode == StepLane::Trig && st.animations && !st.low_power;
    const int bank = curBank(), page = curPage();
    if (!on || bank != g.spark_bank || page != g.spark_page) {   // other cells now: old sparks go
        clearSparks();
        g.spark_bank = (int8_t)bank;
        g.spark_page = (int8_t)page;
        if (!on) return;
    }
    if (hits) {
        const Scene& sc = curScene();
        const PlayPos pp = playPos();
        for (int r = 0; r < kRows; ++r) {
            const int pad = bank * kRows + r;
            if (!(hits & (1u << pad))) continue;
            const int col = playCol(pp, sc, pad);
            if (col < 0) continue;
            // the step under the playhead, else the next one (a hit nudged early, before the step)
            const int c = sparkable(sc, pad, page, col) ? col : sparkable(sc, pad, page, col + 1) ? col + 1 : -1;
            if (c < 0) continue;
            g.spark_t0[r][c] = now;
            g.sparking[r] |= (uint16_t)(1u << c);
        }
    }
    for (int r = 0; r < kRows; ++r) {
        for (uint16_t m = g.sparking[r]; m; m &= (uint16_t)(m - 1)) {
            const int c = __builtin_ctz(m);
            if (now - g.spark_t0[r][c] >= kSparkMs) g.sparking[r] &= (uint16_t)~(1u << c);   // last frame: plain
            invalidateCell(r, c);
        }
    }
}

void tickPlayhead()
{
    if (!g.obj) return;
    const PlayPos pp = playPos();
    const Scene& sc = curScene();
    auto colFor = [&](int pad) { return playCol(pp, sc, pad); };
    if (g.mode == StepLane::Trig) {
        const int bank = curBank();
        for (int r = 0; r < kRows; ++r) {
            const int col = colFor(bank * kRows + r);
            if (col == g.ph[r]) continue;
            invalidateCell(r, g.ph[r]);
            g.ph[r] = (int8_t)col;
            invalidateCell(r, col);
        }
    } else {
        const int col = colFor(clampi(app().ui().pad, 0, kPads - 1));
        if (col != g.ph[0]) {
            invalidateCell(0, g.ph[0]);
            g.ph[0] = (int8_t)col;
            invalidateCell(0, col);
        }
    }
}

void invalidateAll()
{
    if (g.obj) lv_obj_invalidate(g.obj);
}

int playheadCol(int row) { return (row >= 0 && row < kRows) ? g.ph[row] : -1; }

int sparkPct(int row, int col)
{
    if (row < 0 || row >= kRows || col < 0 || col >= kCols) return 0;
    return (int)(sparkLevel(row, col, lv_tick_get()) * 100.f + 0.5f);
}

}  // namespace orbit::ui::step_grid
