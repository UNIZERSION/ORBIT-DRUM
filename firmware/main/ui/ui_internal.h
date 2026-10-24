#pragma once
// Internals shared by the UI root (ui.cpp), the baked background (ui_bg.cpp) and the persistent
// panels (scr_*.cpp). Not for views or dialogs.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "src/core/lv_obj_event_private.h"   // lv_hit_test_info_t (setHitBox)
#include "ui/theme.h"                        // palette (panelTopHighlight)
#include "ui/ui_scale.h"

namespace orbit::ui {

// Asymmetric touch target: extra px on each side (0..255) instead of LVGL's uniform ext click
// area. The ext click area is set to the largest side and an LV_EVENT_HIT_TEST handler trims the
// other sides, so neighbours that sit close on one side do not steal each other's taps.
inline void hitBoxCb(lv_event_t* e)
{
    lv_hit_test_info_t* info = lv_event_get_hit_test_info(e);
    lv_obj_t* o = (lv_obj_t*)lv_event_get_current_target(e);
    if (!info || !o || !info->point) return;
    const uintptr_t k = (uintptr_t)lv_event_get_user_data(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const float f = uiScaleOf(o).f;   // a maximized panel's targets grow with it
    a.x1 -= (int32_t)lroundf((float)(k & 0xFFu) * f);
    a.y1 -= (int32_t)lroundf((float)((k >> 8) & 0xFFu) * f);
    a.x2 += (int32_t)lroundf((float)((k >> 16) & 0xFFu) * f);
    a.y2 += (int32_t)lroundf((float)((k >> 24) & 0xFFu) * f);
    const lv_point_t& p = *info->point;
    info->res = info->res && p.x >= a.x1 && p.x <= a.x2 && p.y >= a.y1 && p.y <= a.y2;
}
inline void setHitBox(lv_obj_t* o, int left, int top, int right, int bottom)
{
    if (!o) return;
    auto cl = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
    left = cl(left);
    top = cl(top);
    right = cl(right);
    bottom = cl(bottom);
    int m = left > top ? left : top;
    m = m > right ? m : right;
    m = m > bottom ? m : bottom;
    lv_obj_set_ext_click_area(o, m);
    lv_obj_add_flag(o, LV_OBJ_FLAG_ADV_HITTEST);
    const uintptr_t k = (uintptr_t)left | ((uintptr_t)top << 8) | ((uintptr_t)right << 16) | ((uintptr_t)bottom << 24);
    lv_obj_add_event_cb(o, hitBoxCb, LV_EVENT_HIT_TEST, (void*)k);
}

// Sets a label's text only when it differs (no relayout / redraw for unchanged numbers).
inline void labelSet(lv_obj_t* label, const char* text)
{
    if (!label || !text) return;
    const char* cur = lv_label_get_text(label);
    if (cur && strcmp(cur, text) == 0) return;
    lv_label_set_text(label, text);
}
// Overlap test of two areas (inclusive coordinates).
inline bool areasOverlap(const lv_area_t& a, const lv_area_t& b)
{
    return a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2;
}
// .panel box-shadow "0 1px 1px #fff9 inset": the light line under the panel's top border
// (static 1 px child, inside the rounded corners). w = the panel's outer width.
inline lv_obj_t* panelTopHighlight(lv_obj_t* panel, int w)
{
    lv_obj_t* h = lv_obj_create(panel);
    lv_obj_remove_style_all(h);
    lv_obj_set_style_bg_opa(h, col::hilite_opa, 0);   // LIGHT: #fff at 150 (theme.h, per base)
    lv_obj_set_style_bg_color(h, c(col::hilite), 0);
    lv_obj_set_pos(h, 5, 0);
    lv_obj_set_size(h, w - 2 - 10, 1);
    lv_obj_remove_flag(h, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(h, LV_OBJ_FLAG_SCROLLABLE);
    return h;
}
// "120 BPM" / "122.5 BPM" (one decimal only when the tempo is fractional, e.g. after TAP).
inline void formatBpm(char* buf, size_t n, float bpm, bool unit)
{
    const float r = floorf(bpm + 0.5f);
    if (fabsf(bpm - r) < 0.05f) snprintf(buf, n, unit ? "%d BPM" : "%d", (int)r);
    else snprintf(buf, n, unit ? "%.1f BPM" : "%.1f", (double)bpm);
}

namespace bg {
// Bakes the stage once into an RGB565 image in PSRAM and puts it behind everything on scr (the
// screen's own background is switched off). Baked: the 140deg stage gradient, the 5 px scanline
// texture, the top bar gradient + bottom line, the soft shadows of the six panels and the inner
// vignette. Returns the image object, or nullptr when out of memory (the theme gradient stays).
lv_obj_t* create(lv_obj_t* scr);
// The bake alone (bgPrebake in ui.h): any task, no LVGL calls; create() waits for it / reuses it.
void prebake(int w, int h);
// true when the panel shadows are part of the baked background (panels then drop their own).
bool shadowsBaked();
}  // namespace bg

// Software rasteriser helpers for the static images (background, pads). Colors in 0..255 floats.
namespace bake {

struct Rgb {
    float r, g, b;
};
inline Rgb rgb(uint32_t hex)
{
    return {(float)((hex >> 16) & 0xFF), (float)((hex >> 8) & 0xFF), (float)(hex & 0xFF)};
}
inline Rgb mix(Rgb a, Rgb b, float t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
// A palette color snapped to the RGB565 grid exactly like LVGL fills it (theme c()), so baked flat
// areas next to LVGL-filled ones (e.g. pad image margins on the panel) match pixel for pixel.
inline float gridValue(uint32_t v, uint32_t levels)
{
    const uint32_t n = (v * levels + 127u) / 255u;
    return (float)n * 255.f / (float)levels;
}
inline Rgb rgbGrid(uint32_t hex)
{
    return {gridValue((hex >> 16) & 0xFF, 31), gridValue((hex >> 8) & 0xFF, 63), gridValue(hex & 0xFF, 31)};
}
// Signed distance from (px,py) to the rounded rectangle [x0,x1]x[y0,y1], corner radius r
// (negative inside). Used for anti-aliased coverage (0.5 - d) and blurred shapes.
float rrectDist(float px, float py, float x0, float y0, float x1, float y1, float r);
// Coverage of a shape blurred like CSS box-shadow (sigma = blur / 2) at signed distance d.
inline float blurCov(float d, float sigma) { return 0.5f * erfcf(d / (sigma * 1.41421356f)); }
// Dithered RGB565 (interleaved-gradient-noise threshold at pixel x,y): no banding on the soft
// grey gradients of the palette.
uint16_t to565(Rgb c, int x, int y);
// RGB565 image descriptor + pixels in PSRAM (bigAlloc, never freed). nullptr on failure.
lv_image_dsc_t* image565(int w, int h);

}  // namespace bake

}  // namespace orbit::ui
