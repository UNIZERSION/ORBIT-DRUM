// Native scaling of maximized panels: the font ladder, the scale registry and the subtree re-layout (ui_scale.h).
#include "ui/ui_scale.h"

#include "src/core/lv_obj_private.h"
#include "src/core/lv_obj_style_private.h"
#include "ui/icons/icons.h"

extern "C" {
extern const lv_font_t orbit_font_r12, orbit_font_r13, orbit_font_r14, orbit_font_r16, orbit_font_r18, orbit_font_r20,
    orbit_font_r22, orbit_font_r24, orbit_font_r26, orbit_font_r28;
extern const lv_font_t orbit_font_m12, orbit_font_m13, orbit_font_m14, orbit_font_m16, orbit_font_m18, orbit_font_m20,
    orbit_font_m22, orbit_font_m24, orbit_font_m26, orbit_font_m28, orbit_font_m32, orbit_font_m36, orbit_font_m40,
    orbit_font_m44, orbit_font_m48;
extern const lv_font_t orbit_font_s11, orbit_font_s13, orbit_font_s15, orbit_font_s16, orbit_font_s18, orbit_font_s20,
    orbit_font_s22, orbit_font_s24, orbit_font_s26, orbit_font_s28, orbit_font_s30;
extern const lv_font_t orbit_font_mono12, orbit_font_mono16, orbit_font_mono18, orbit_font_mono20, orbit_font_mono24;
}

namespace orbit::ui {

namespace {

// ---------------------------------------------------------------- font ladder
struct FontEntry {
    const lv_font_t* font;
    char weight;   // r m s o(mono)
    uint8_t px;
};
const FontEntry kLadder[] = {
    {&orbit_font_r12, 'r', 12},    {&orbit_font_r13, 'r', 13},    {&orbit_font_r14, 'r', 14},
    {&orbit_font_r16, 'r', 16},    {&orbit_font_r18, 'r', 18},    {&orbit_font_r20, 'r', 20},
    {&orbit_font_r22, 'r', 22},    {&orbit_font_r24, 'r', 24},    {&orbit_font_r26, 'r', 26},
    {&orbit_font_r28, 'r', 28},    {&orbit_font_m12, 'm', 12},    {&orbit_font_m13, 'm', 13},
    {&orbit_font_m14, 'm', 14},    {&orbit_font_m16, 'm', 16},    {&orbit_font_m18, 'm', 18},
    {&orbit_font_m20, 'm', 20},    {&orbit_font_m22, 'm', 22},    {&orbit_font_m24, 'm', 24},
    {&orbit_font_m26, 'm', 26},    {&orbit_font_m28, 'm', 28},    {&orbit_font_m32, 'm', 32},
    {&orbit_font_m36, 'm', 36},    {&orbit_font_m40, 'm', 40},    {&orbit_font_m44, 'm', 44},
    {&orbit_font_m48, 'm', 48},    {&orbit_font_s11, 's', 11},    {&orbit_font_s13, 's', 13},
    {&orbit_font_s15, 's', 15},    {&orbit_font_s16, 's', 16},    {&orbit_font_s18, 's', 18},
    {&orbit_font_s20, 's', 20},    {&orbit_font_s22, 's', 22},    {&orbit_font_s24, 's', 24},
    {&orbit_font_s26, 's', 26},    {&orbit_font_s28, 's', 28},    {&orbit_font_s30, 's', 30},
    {&orbit_font_mono12, 'o', 12}, {&orbit_font_mono16, 'o', 16}, {&orbit_font_mono18, 'o', 18},
    {&orbit_font_mono20, 'o', 20}, {&orbit_font_mono24, 'o', 24},
};

// ---------------------------------------------------------------- registry + build scope
struct Reg {
    lv_obj_t* root;
    UiScale s;
};
constexpr int kRegMax = 6;   // maximized panel(s) + a scaled dialog (0.4.6)
Reg g_reg[kRegMax] = {};
UiScale g_build;

void regDeleteCb(lv_event_t* e)
{
    lv_obj_t* o = (lv_obj_t*)lv_event_get_current_target(e);
    for (Reg& r : g_reg)
        if (r.root == o) r = Reg{};
}

// ---------------------------------------------------------------- subtree re-layout
enum Axis : uint8_t { kAxX, kAxY, kAxF };
struct GeoProp {
    lv_style_prop_t prop;
    Axis axis;
    bool stroke;   // keeps at least 1 px
};
const GeoProp kGeo[] = {
    {LV_STYLE_X, kAxX, false},              {LV_STYLE_WIDTH, kAxX, false},
    {LV_STYLE_MIN_WIDTH, kAxX, false},      {LV_STYLE_MAX_WIDTH, kAxX, false},
    {LV_STYLE_PAD_LEFT, kAxX, false},       {LV_STYLE_PAD_RIGHT, kAxX, false},
    {LV_STYLE_PAD_COLUMN, kAxX, false},     {LV_STYLE_MARGIN_LEFT, kAxX, false},
    {LV_STYLE_MARGIN_RIGHT, kAxX, false},   {LV_STYLE_TRANSLATE_X, kAxX, false},
    {LV_STYLE_TRANSFORM_WIDTH, kAxX, false}, {LV_STYLE_SHADOW_OFFSET_X, kAxX, false},
    {LV_STYLE_Y, kAxY, false},              {LV_STYLE_HEIGHT, kAxY, false},
    {LV_STYLE_MIN_HEIGHT, kAxY, false},     {LV_STYLE_MAX_HEIGHT, kAxY, false},
    {LV_STYLE_PAD_TOP, kAxY, false},        {LV_STYLE_PAD_BOTTOM, kAxY, false},
    {LV_STYLE_PAD_ROW, kAxY, false},        {LV_STYLE_MARGIN_TOP, kAxY, false},
    {LV_STYLE_MARGIN_BOTTOM, kAxY, false},  {LV_STYLE_TRANSLATE_Y, kAxY, false},
    {LV_STYLE_TRANSFORM_HEIGHT, kAxY, false}, {LV_STYLE_SHADOW_OFFSET_Y, kAxY, false},
    {LV_STYLE_RADIUS, kAxF, false},         {LV_STYLE_BORDER_WIDTH, kAxF, true},
    {LV_STYLE_OUTLINE_WIDTH, kAxF, true},   {LV_STYLE_OUTLINE_PAD, kAxF, false},
    {LV_STYLE_SHADOW_WIDTH, kAxF, false},   {LV_STYLE_SHADOW_SPREAD, kAxF, false},
    {LV_STYLE_LINE_WIDTH, kAxF, true},      {LV_STYLE_LINE_DASH_WIDTH, kAxF, false},
    {LV_STYLE_LINE_DASH_GAP, kAxF, false},  {LV_STYLE_ARC_WIDTH, kAxF, true},
    {LV_STYLE_TEXT_LETTER_SPACE, kAxF, false}, {LV_STYLE_TEXT_LINE_SPACE, kAxF, false},
    {LV_STYLE_LENGTH, kAxF, false},
};

int32_t scaleCoord(int32_t v, float k, bool stroke)
{
    if (LV_COORD_IS_SPEC(v) || v >= LV_RADIUS_CIRCLE || v <= -LV_RADIUS_CIRCLE) return v;   // %, content, circle
    int32_t r = (int32_t)lroundf((float)v * k);
    if (stroke && v >= 1 && r < 1) r = 1;
    return r;
}

struct Pending {
    lv_style_selector_t sel;
    lv_style_prop_t prop;
    lv_style_value_t v;
};

void scaleObj(lv_obj_t* o, const UiScale& s)
{
    // Every style that reaches the object (shared ones included) becomes a local override with the scaled
    // value, per selector; read everything first, lowest priority first, so the value LVGL would pick wins.
    constexpr int kMax = 160;
    Pending pend[kMax];
    int n = 0;
    for (int i = (int)o->style_cnt - 1; i >= 0; --i) {
        const lv_obj_style_t& e = o->styles[i];
        if (e.is_trans || !e.style) continue;
        const lv_style_selector_t sel = e.selector;
        auto put = [&](lv_style_prop_t prop, const lv_style_value_t& v) {
            for (int k = 0; k < n; ++k) {
                if (pend[k].sel == sel && pend[k].prop == prop) {
                    pend[k].v = v;
                    return;
                }
            }
            if (n < kMax) pend[n++] = Pending{sel, prop, v};
        };
        lv_style_value_t v;
        for (const GeoProp& g : kGeo) {
            if (lv_style_get_prop(e.style, g.prop, &v) != LV_STYLE_RES_FOUND) continue;
            const float k = g.axis == kAxX ? s.sx : (g.axis == kAxY ? s.sy : s.f);
            lv_style_value_t sv;
            sv.num = scaleCoord(v.num, k, g.stroke);
            put(g.prop, sv);
        }
        if (lv_style_get_prop(e.style, LV_STYLE_TEXT_FONT, &v) == LV_STYLE_RES_FOUND && v.ptr) {
            lv_style_value_t sv;
            sv.ptr = fontScaled((const lv_font_t*)v.ptr, s.f);
            put(LV_STYLE_TEXT_FONT, sv);
        }
    }
    for (int k = 0; k < n; ++k) lv_obj_set_local_style_prop(o, pend[k].prop, pend[k].v, pend[k].sel);

    if (o->spec_attr && o->spec_attr->ext_click_pad > 0)
        lv_obj_set_ext_click_area(o, (int32_t)lroundf((float)o->spec_attr->ext_click_pad * s.f));

    // stroke icons: the bitmap rasterised nearest to the new size
    if (lv_obj_check_type(o, &lv_image_class)) {
        const void* src = lv_image_get_src(o);
        for (int i = 0; i < ORBIT_ICON_TOTAL; ++i) {
            for (int z = 0; z < ORBIT_ICON_SIZES; ++z) {
                if (orbit_icons[i][z] != src) continue;
                const float want = (float)orbit_icon_sizes[z] * s.f;
                int best = z;
                for (int q = 0; q < ORBIT_ICON_SIZES; ++q)
                    if (fabsf((float)orbit_icon_sizes[q] - want) < fabsf((float)orbit_icon_sizes[best] - want)) best = q;
                if (best != z) lv_image_set_src(o, orbit_icons[i][best]);
                return;
            }
        }
    }
}

void scaleRec(lv_obj_t* o, const UiScale& s)
{
    scaleObj(o, s);
    const uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; ++i) scaleRec(lv_obj_get_child(o, (int32_t)i), s);
    lv_obj_refresh_ext_draw_size(o);   // custom-drawn glows / rings reach further now (uiScaleOf in the handler)
}

}  // namespace

const lv_font_t* fontScaled(const lv_font_t* f, float s)
{
    if (!f || s <= 1.001f) return f;
    const FontEntry* base = nullptr;
    for (const FontEntry& e : kLadder) {
        if (e.font == f) {
            base = &e;
            break;
        }
    }
    if (!base) return f;
    const float target = (float)base->px * s + 0.5f;
    const FontEntry* best = base;
    for (const FontEntry& e : kLadder)
        if (e.weight == base->weight && (float)e.px <= target && e.px > best->px) best = &e;
    return best->font;
}

UiScale uiBuildScale() { return g_build; }

UiBuildScope::UiBuildScope(UiScale s) : prev(g_build) { g_build = s; }
UiBuildScope::~UiBuildScope() { g_build = prev; }

UiScale uiScaleOf(const lv_obj_t* obj)
{
    bool any = false;
    for (const Reg& r : g_reg) any = any || r.root;
    if (!any) return UiScale{};
    for (const lv_obj_t* o = obj; o; o = lv_obj_get_parent(o))
        for (const Reg& r : g_reg)
            if (r.root == o) return r.s;
    return UiScale{};
}

void uiScaleRegister(lv_obj_t* root, UiScale s)
{
    if (!root) return;
    for (Reg& r : g_reg) {
        if (r.root != root) continue;
        if (s.unit()) {
            lv_obj_remove_event_cb(root, regDeleteCb);
            r = Reg{};
        } else {
            r.s = s;
        }
        return;
    }
    if (s.unit()) return;
    for (Reg& r : g_reg) {
        if (r.root) continue;
        r = Reg{root, s};
        lv_obj_add_event_cb(root, regDeleteCb, LV_EVENT_DELETE, nullptr);
        return;
    }
}

void uiScaleTree(lv_obj_t* root, UiScale s)
{
    if (!root || s.unit()) return;
    scaleRec(root, s);
}

Xf xfOf(const lv_obj_t* obj, int32_t dx, int32_t dy)
{
    Xf t;
    t.s = uiScaleOf(obj);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    t.ox = a.x1;
    t.oy = a.y1;
    t.dx = dx;
    t.dy = dy;
    return t;
}

}  // namespace orbit::ui
