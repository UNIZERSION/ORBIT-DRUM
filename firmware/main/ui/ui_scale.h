#pragma once
// Native scaling of maximized panels (0.3.2). A maximized panel is built again at its normal size and then laid
// out at the larger size by uiScaleTree(): positions, sizes, paddings, radii, borders and letter spacing are
// multiplied, fonts are swapped for larger rasterised ones (ui/fonts, tools/fontgen/build_max.sh) and icons for
// larger bitmaps, so text and lines stay sharp (a style transform would scale the finished picture and blur it).
// Custom-drawn widgets read the scale of the panel they live in (uiScaleOf) and map their design coordinates
// with an Xf; code that reads the pointer itself gets design coordinates from pointerIn() (widgets.h).
#include <math.h>
#include <stdint.h>

#include "lvgl.h"

namespace orbit::ui {

// sx / sy: geometry along x / y (a panel may stretch a little to fill the screen); f: fonts, strokes, radii,
// icons (the smaller of the two, capped where no larger font exists).
struct UiScale {
    float sx = 1.f, sy = 1.f, f = 1.f;
    bool unit() const { return sx == 1.f && sy == 1.f && f == 1.f; }
};

// The scale a panel is being built at (inside a UiBuildScope), else 1: for things fixed at creation
// (baked images, measured texts).
UiScale uiBuildScale();
struct UiBuildScope {
    explicit UiBuildScope(UiScale s);
    ~UiBuildScope();
    UiScale prev;
};

// The scale of the registered panel root that contains obj (1 when none does).
UiScale uiScaleOf(const lv_obj_t* obj);
// Registers root as drawn at s (s = unit removes it); dropped automatically when root is deleted.
void uiScaleRegister(lv_obj_t* root, UiScale s);

// Lays out a freshly built subtree natively at s (see the top). Run it once per object: on a panel after
// building it, on children created later inside a scaled panel right after creating them.
void uiScaleTree(lv_obj_t* root, UiScale s);

// The largest font of the same weight that is at most f's size times s (f itself when s <= 1).
const lv_font_t* fontScaled(const lv_font_t* f, float s);

// Design -> screen mapping for custom draw code: (dx, dy) is the design position of the object's top-left
// corner, (ox, oy) where that corner is on the screen now.
struct Xf {
    UiScale s;
    int32_t ox = 0, oy = 0, dx = 0, dy = 0;
    int32_t X(float x) const { return ox + (int32_t)lroundf((x - (float)dx) * s.sx); }
    int32_t Y(float y) const { return oy + (int32_t)lroundf((y - (float)dy) * s.sy); }
    int32_t W(float w) const { return (int32_t)lroundf(w * s.sx); }
    int32_t H(float h) const { return (int32_t)lroundf(h * s.sy); }
    // strokes, radii, offsets that must keep their shape: at least 1 px when v >= 1
    int32_t F(float v) const
    {
        const int32_t r = (int32_t)lroundf(v * s.f);
        return v >= 1.f && r < 1 ? 1 : r;
    }
    const lv_font_t* font(const lv_font_t* f) const { return fontScaled(f, s.f); }
    // design rectangle [x1, x2) x [y1, y2) -> inclusive screen area (neighbours meet without gaps)
    lv_area_t area(float x1, float y1, float x2, float y2) const
    {
        return lv_area_t{X(x1), Y(y1), X(x2) - 1, Y(y2) - 1};
    }
};
// For obj placed at design (dx, dy): the mapping in effect now.
Xf xfOf(const lv_obj_t* obj, int32_t dx = 0, int32_t dy = 0);

// Geometry set at run time (after the panel was built) for an object o inside a maybe-maximized panel:
// design px along x / y, and a font, in what o's panel draws now.
inline int32_t uiSx(const lv_obj_t* o, float v)
{
    const float s = uiScaleOf(o).sx;
    return s == 1.f ? (int32_t)v : (int32_t)lroundf(v * s);
}
inline int32_t uiSy(const lv_obj_t* o, float v)
{
    const float s = uiScaleOf(o).sy;
    return s == 1.f ? (int32_t)v : (int32_t)lroundf(v * s);
}
inline const lv_font_t* uiFont(const lv_obj_t* o, const lv_font_t* f) { return fontScaled(f, uiScaleOf(o).f); }

}  // namespace orbit::ui
