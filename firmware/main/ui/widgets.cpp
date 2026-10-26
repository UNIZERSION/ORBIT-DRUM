// Widget kit, part 1: icons, buttons, panels, labels, ripple and the small animations.
// Knob/fader/meter: widgets_knob.cpp. Toggle/select/popup/slider/text field/keyboard:
// widgets_input.cpp. Modal/toast: widgets_overlay.cpp.
#include "ui/ui.h"
#include "ui/widgets.h"

#include <string.h>

#include "ui/icons/icons.h"
#include "ui/widgets_internal.h"

static_assert(ORBIT_ICON_COUNT == (int)orbit::ui::Icon::Count, "icongen order != enum Icon");

namespace orbit::ui {

// ================================================================ internals
namespace wk {

void* findState(lv_obj_t* obj, lv_event_cb_t cb)
{
    if (!obj) return nullptr;
    const uint32_t n = lv_obj_get_event_count(obj);
    for (uint32_t i = 0; i < n; ++i) {
        lv_event_dsc_t* d = lv_obj_get_event_dsc(obj, i);
        if (d && lv_event_dsc_get_cb(d) == cb) return lv_event_dsc_get_user_data(d);
    }
    return nullptr;
}

bool feedbackOn()
{
    const Settings& s = app().settings();
    return s.touch_feedback && !s.low_power;
}

bool animOn()
{
    const Settings& s = app().settings();
    return s.animations && !s.low_power;
}

void syncFeedback()
{
    static int applied = -1;
    const int want = feedbackOn() ? 1 : 0;
    if (want == applied) return;
    lv_style_t* st = &styles().pressed;
    if (applied < 0) {
        // First call: themeInit() already gave the style its "on" values. Restyling every object
        // that uses it (invalidate + relayout of most buttons) is only needed when they differ.
        lv_style_value_t v;
        const bool on_now = lv_style_get_prop(st, LV_STYLE_TRANSFORM_SCALE_X, &v) == LV_STYLE_RES_FOUND && v.num != 256;
        if ((on_now ? 1 : 0) == want) {
            applied = want;
            return;
        }
    }
    applied = want;
    lv_style_set_transform_scale_x(st, want ? 247 : 256);
    lv_style_set_transform_scale_y(st, want ? 247 : 256);
    lv_style_set_color_filter_opa(st, want ? 41 : 0);
    lv_obj_report_style_change(st);
}

lv_obj_t* firstLabel(lv_obj_t* obj)
{
    const uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* ch = lv_obj_get_child(obj, (int32_t)i);
        if (lv_obj_check_type(ch, &lv_label_class)) return ch;
    }
    return nullptr;
}

void setTextIfChanged(lv_obj_t* label, const char* text)
{
    if (!label || !text) return;
    const char* cur = lv_label_get_text(label);
    if (cur && strcmp(cur, text) == 0) return;
    lv_label_set_text(label, text);
}

// Largest button that keeps the scale part of the press look. A transform_scale != 256 renders
// the object into an ARGB8888 layer on every frame of the press transition and the ripple; above
// ~120x44 px (> 16 KB) that layer is also malloc'd from PSRAM each frame.
constexpr int32_t kScaleMaxW = 120, kScaleMaxH = 44;

// LV_EVENT_PRESSED | LV_EVENT_PREPROCESS: runs before the class handler adds LV_STATE_PRESSED, so
// the press transition already sees the override. Wide / tall buttons get a local "scale 256
// while pressed" (local styles win over the shared one); the brightness part stays.
static void pressGateCb(lv_event_t* e)
{
    lv_obj_t* o = (lv_obj_t*)lv_event_get_current_target(e);
    const bool big = lv_obj_get_width(o) > kScaleMaxW || lv_obj_get_height(o) > kScaleMaxH;
    lv_style_value_t v;
    const bool has = lv_obj_get_local_style_prop(o, LV_STYLE_TRANSFORM_SCALE_X, &v, LV_STATE_PRESSED) ==
                     LV_STYLE_RES_FOUND;
    if (big == has) return;
    if (big) {
        lv_obj_set_style_transform_scale_x(o, 256, LV_STATE_PRESSED);
        lv_obj_set_style_transform_scale_y(o, 256, LV_STATE_PRESSED);
    } else {
        lv_obj_remove_local_style_prop(o, LV_STYLE_TRANSFORM_SCALE_X, LV_STATE_PRESSED);
        lv_obj_remove_local_style_prop(o, LV_STYLE_TRANSFORM_SCALE_Y, LV_STATE_PRESSED);
    }
}

void buttonCommon(lv_obj_t* btn, int hit)
{
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_style(btn, &styles().pressed, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, pressGateCb, (lv_event_code_t)(LV_EVENT_PRESSED | LV_EVENT_PREPROCESS), nullptr);
    if (hit > 0) lv_obj_set_ext_click_area(btn, hit + hitBonus());
    rippleAttach(btn);
}

}  // namespace wk

void pressFeedbackSync() { wk::syncFeedback(); }

int hitBonus()
{
    const int i = app().settings().ui_size;
    return i == 1 ? 4 : (i == 2 ? 8 : 0);
}

// ================================================================ icons
namespace {

int iconSlot(int size)
{
    int best = 0;
    for (int i = 1; i < ORBIT_ICON_SIZES; ++i) {
        const int d = orbit_icon_sizes[i] - size, db = orbit_icon_sizes[best] - size;
        if ((d < 0 ? -d : d) < (db < 0 ? -db : db)) best = i;
    }
    return best;
}

lv_obj_t* imageA8(lv_obj_t* parent, int index, int size, uint32_t color)
{
    lv_obj_t* img = lv_image_create(parent);
    lv_image_set_src(img, orbit_icons[index][iconSlot(size)]);
    lv_obj_set_style_image_recolor(img, c(color), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
    return img;
}

}  // namespace

lv_obj_t* wk::chevDown(lv_obj_t* parent, int size, uint32_t color)
{
    return imageA8(parent, ORBIT_ICON_CHEV_DOWN, size, color);
}

lv_obj_t* iconCreate(lv_obj_t* parent, Icon icon, int size, uint32_t color)
{
    const int i = (int)icon < ORBIT_ICON_COUNT ? (int)icon : (int)Icon::Info;
    return imageA8(parent, i, size, color);
}

void iconSet(lv_obj_t* img, Icon icon, int size)
{
    const int i = (int)icon < ORBIT_ICON_COUNT ? (int)icon : (int)Icon::Info;
    size = (int)lroundf((float)size * uiScaleOf(img).f);   // inside a maximized panel: the larger bitmap
    const void* src = orbit_icons[i][iconSlot(size)];
    if (lv_image_get_src(img) != src) lv_image_set_src(img, src);
}

// ================================================================ buttons
namespace {

lv_style_t s_icon_btn, s_icon_btn_pr, s_icon_btn_on;
bool s_btn_styles = false;

void btnStyles()
{
    if (s_btn_styles) return;
    s_btn_styles = true;
    static const lv_style_prop_t props[] = {LV_STYLE_BG_OPA, LV_STYLE_BG_COLOR, LV_STYLE_TRANSFORM_SCALE_X,
                                            LV_STYLE_TRANSFORM_SCALE_Y, LV_STYLE_COLOR_FILTER_OPA,
                                            LV_STYLE_PROP_INV};
    static lv_style_transition_dsc_t tr;
    lv_style_transition_dsc_init(&tr, props, lv_anim_path_ease_out, 120, 0, nullptr);
    // .icon-btn: transparent, radius 6, min 30x30; :hover background on touch.
    lv_style_init(&s_icon_btn);
    lv_style_set_radius(&s_icon_btn, 6);
    lv_style_set_bg_opa(&s_icon_btn, LV_OPA_TRANSP);
    lv_style_set_bg_color(&s_icon_btn, c(col::hover));
    lv_style_set_pad_all(&s_icon_btn, 0);
    lv_style_set_transition(&s_icon_btn, &tr);
    lv_style_init(&s_icon_btn_pr);
    lv_style_set_bg_opa(&s_icon_btn_pr, LV_OPA_COVER);
    lv_style_init(&s_icon_btn_on);
    lv_style_set_image_recolor(&s_icon_btn_on, c(col::accent));
}

lv_obj_t* textButton(lv_obj_t* parent, const char* text, int w, int h)
{
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_size(b, w > 0 ? w : LV_SIZE_CONTENT, h);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, text ? text : "");
    lv_obj_center(l);
    return b;
}

enum class BtnKind : uint8_t { Action, Micro, Icon, Tab };
// The kind is remembered as a tiny event-cb tag so btnSetActive knows which look to switch.
void kindTagCb(lv_event_t*) {}
void setKind(lv_obj_t* b, BtnKind k)
{
    lv_obj_add_event_cb(b, kindTagCb, LV_EVENT_DELETE, (void*)(uintptr_t)((int)k + 1));
}
BtnKind getKind(lv_obj_t* b)
{
    const uintptr_t v = (uintptr_t)wk::findState(b, kindTagCb);
    return v ? (BtnKind)(v - 1) : BtnKind::Action;
}

}  // namespace

lv_obj_t* actionBtn(lv_obj_t* parent, const char* text, bool primary, int w, int h)
{
    const Styles& st = styles();
    lv_obj_t* b = textButton(parent, text, w, h);
    lv_obj_add_style(b, const_cast<lv_style_t*>(&st.action_btn), 0);
    lv_obj_add_style(b, const_cast<lv_style_t*>(&st.action_btn_pr), LV_STATE_PRESSED);
    lv_obj_add_style(b, const_cast<lv_style_t*>(&st.action_primary), LV_STATE_CHECKED);
    if (primary) lv_obj_add_style(b, const_cast<lv_style_t*>(&st.action_primary), 0);
    setKind(b, BtnKind::Action);
    wk::buttonCommon(b, 6);
    return b;
}

lv_obj_t* microBtn(lv_obj_t* parent, const char* text, int w, int h)
{
    lv_obj_t* b = textButton(parent, text, w, h);
    lv_obj_add_style(b, &styles().micro_btn, 0);
    lv_obj_add_style(b, &styles().micro_lit, LV_STATE_CHECKED);
    setKind(b, BtnKind::Micro);
    wk::buttonCommon(b, 8);
    return b;
}

lv_obj_t* iconBtn(lv_obj_t* parent, Icon icon, int size, int box)
{
    btnStyles();
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_size(b, box, box);
    lv_obj_add_style(b, &s_icon_btn, 0);
    lv_obj_add_style(b, &s_icon_btn_pr, LV_STATE_PRESSED);
    lv_obj_t* img = iconCreate(b, icon, size, col::ink);
    lv_obj_center(img);
    setKind(b, BtnKind::Icon);
    wk::buttonCommon(b, box < 44 ? (44 - box) / 2 + 2 : 4);
    return b;
}

lv_obj_t* tabBtn(lv_obj_t* parent, const char* text, int w, int h)
{
    lv_obj_t* b = textButton(parent, text, w, h);
    lv_obj_add_style(b, &styles().tab, 0);
    lv_obj_add_style(b, &styles().tab_active, LV_STATE_CHECKED);
    setKind(b, BtnKind::Tab);
    wk::buttonCommon(b, 4);
    return b;
}

void btnSetActive(lv_obj_t* btn, bool active)
{
    if (!btn) return;
    if (lv_obj_has_state(btn, LV_STATE_CHECKED) == active) return;
    lv_obj_set_state(btn, LV_STATE_CHECKED, active);
    if (getKind(btn) == BtnKind::Icon) {
        btnStyles();
        const uint32_t n = lv_obj_get_child_count(btn);
        for (uint32_t i = 0; i < n; ++i) {
            lv_obj_t* ch = lv_obj_get_child(btn, (int32_t)i);
            if (!lv_obj_check_type(ch, &lv_image_class)) continue;
            if (active) lv_obj_add_style(ch, &s_icon_btn_on, 0);
            else lv_obj_remove_style(ch, &s_icon_btn_on, 0);
        }
    }
}

void btnSetText(lv_obj_t* btn, const char* text)
{
    wk::setTextIfChanged(wk::firstLabel(btn), text);
}

// ---------------------------------------------------------------- hold-to-repeat
namespace {

struct Repeat {
    void (*cb)(void*);
    void* user;
    lv_timer_t* timer;
    uint32_t period;
};

void repeatTimer(lv_timer_t* t)
{
    Repeat* r = (Repeat*)lv_timer_get_user_data(t);
    r->cb(r->user);
    r->period = r->period * 3 / 4;
    if (r->period < 60) r->period = 60;
    lv_timer_set_period(t, r->period);
}

void repeatStop(Repeat* r)
{
    if (r->timer) {
        lv_timer_delete(r->timer);
        r->timer = nullptr;
    }
}

void repeatCb(lv_event_t* e)
{
    Repeat* r = (Repeat*)lv_event_get_user_data(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        r->cb(r->user);
        repeatStop(r);
        r->period = 400;
        r->timer = lv_timer_create(repeatTimer, r->period, r);
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        repeatStop(r);
        break;
    case LV_EVENT_DELETE:
        repeatStop(r);
        lv_free(r);
        break;
    default:
        break;
    }
}

}  // namespace

void btnRepeat(lv_obj_t* btn, void (*cb)(void* user), void* user)
{
    if (!btn || !cb) return;
    Repeat* r = (Repeat*)lv_malloc(sizeof(Repeat));
    if (!r) return;
    r->cb = cb;
    r->user = user;
    r->timer = nullptr;
    r->period = 400;
    lv_obj_add_event_cb(btn, repeatCb, LV_EVENT_ALL, r);
}

// ================================================================ ripple
namespace {

// .ripple: 12 px accent dot, opacity .42, scale x19 + fade to 0 in 560 ms ease-out. The dot is
// grown by size (not a transform), so no intermediate layer is needed.
void rippleAnim(lv_anim_t* a, int32_t v)
{
    lv_obj_t* dot = (lv_obj_t*)a->var;
    // Centre (parent content coordinates) packed in the user data: the dot's own coordinates are
    // not valid before the next layout pass.
    const intptr_t packed = (intptr_t)lv_anim_get_user_data(a);
    const int32_t cx = (int16_t)(packed >> 16), cy = (int16_t)(packed & 0xFFFF);
    const int32_t d = 12 + (228 - 12) * v / 1024;
    lv_obj_set_size(dot, d, d);
    lv_obj_set_pos(dot, cx - d / 2, cy - d / 2);
    lv_obj_set_style_bg_opa(dot, (lv_opa_t)(107 * (1024 - v) / 1024), 0);
}

void rippleDone(lv_anim_t* a)
{
    lv_obj_delete_async((lv_obj_t*)a->var);
}

void rippleCb(lv_event_t* e)
{
    wk::syncFeedback();
    if (!wk::feedbackOn()) return;
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_current_target(e);
    lv_indev_t* indev = lv_indev_active();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return;
    const lv_point_t p = pointerIn(obj);   // the panel may be drawn scaled (maximize)
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    lv_obj_t* dot = lv_obj_create(obj);
    lv_obj_remove_style_all(dot);
    lv_obj_add_flag(dot, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, c(col::accent), 0);
    lv_obj_set_style_bg_opa(dot, 107, 0);
    lv_obj_set_size(dot, 12, 12);
    // Child coordinates are relative to the parent's content area (minus padding/border).
    const int32_t ox = a.x1 + lv_obj_get_style_pad_left(obj, 0) + lv_obj_get_style_border_width(obj, 0);
    const int32_t oy = a.y1 + lv_obj_get_style_pad_top(obj, 0) + lv_obj_get_style_border_width(obj, 0);
    const int32_t cx = p.x - ox + lv_obj_get_scroll_x(obj), cy = p.y - oy + lv_obj_get_scroll_y(obj);
    lv_obj_set_pos(dot, cx - 6, cy - 6);
    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, dot);
    lv_anim_set_custom_exec_cb(&an, rippleAnim);
    lv_anim_set_user_data(&an, (void*)(intptr_t)(((cx & 0xFFFF) << 16) | (cy & 0xFFFF)));
    lv_anim_set_values(&an, 0, 1024);
    lv_anim_set_duration(&an, 560);
    lv_anim_set_path_cb(&an, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&an, rippleDone);
    lv_anim_start(&an);
}

}  // namespace

void rippleAttach(lv_obj_t* obj)
{
    if (!obj || wk::findState(obj, rippleCb) != nullptr) return;
    // user_data = obj marks "attached" for findState (never dereferenced).
    lv_obj_add_event_cb(obj, rippleCb, LV_EVENT_PRESSED, obj);
}

// ================================================================ screen enter / hit
namespace {

// Screen enter, cheap form: the view appears 4 px low and settles once (lv_anim_path_step), i.e.
// two frames of the view instead of ~16. No opacity: a style opa < 255 is multiplied into every
// child draw and makes LVGL repaint the work panel underneath on each frame.
constexpr int32_t kEnterDy = 4;
constexpr uint32_t kEnterMs = 90;

void enterExec(void* var, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t*)var, v, 0);
}

void enterDone(lv_anim_t* a)
{
    lv_obj_remove_local_style_prop((lv_obj_t*)a->var, LV_STYLE_TRANSLATE_Y, 0);
}

// pad-hit keyframes: 40% { glow 22 px #ff673ec0, scale .96 } -> 100% { no glow, scale 1 }.
struct HitSave {
    bool has_w, has_c, has_o, has_s;
    lv_style_value_t w, c, o, s;
};

void hitExec(void* var, int32_t v)
{
    lv_obj_t* o = (lv_obj_t*)var;
    int32_t glow, scale;
    if (v < 410) {   // 0..40 %
        glow = 192 * v / 410;
        scale = 256 - 10 * v / 410;
    } else {
        glow = 192 * (1024 - v) / 614;
        scale = 246 + 10 * (v - 410) / 614;
    }
    lv_obj_set_style_shadow_opa(o, (lv_opa_t)glow, 0);
    lv_obj_set_style_transform_scale(o, scale, 0);
}

void hitDone(lv_anim_t* a)
{
    lv_obj_t* o = (lv_obj_t*)a->var;
    HitSave* hs = (HitSave*)lv_anim_get_user_data(a);
    auto restore = [&](lv_style_prop_t p, bool has, lv_style_value_t v) {
        if (has) lv_obj_set_local_style_prop(o, p, v, 0);
        else lv_obj_remove_local_style_prop(o, p, 0);
    };
    if (hs) {
        restore(LV_STYLE_SHADOW_WIDTH, hs->has_w, hs->w);
        restore(LV_STYLE_SHADOW_COLOR, hs->has_c, hs->c);
        restore(LV_STYLE_SHADOW_OPA, hs->has_o, hs->o);
        restore(LV_STYLE_TRANSFORM_SCALE_X, hs->has_s, hs->s);
        restore(LV_STYLE_TRANSFORM_SCALE_Y, hs->has_s, hs->s);
        lv_free(hs);
    }
}

}  // namespace

void animScreenEnter(lv_obj_t* obj)
{
    if (!obj || !wk::animOn()) return;
    lv_anim_delete(obj, enterExec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, enterExec);
    lv_anim_set_values(&a, kEnterDy, 0);
    lv_anim_set_duration(&a, kEnterMs);
    lv_anim_set_path_cb(&a, lv_anim_path_step);   // start value until the end, then the end value
    lv_anim_set_completed_cb(&a, enterDone);
    enterExec(obj, kEnterDy);
    lv_anim_start(&a);
}

void animHit(lv_obj_t* obj)
{
    if (!obj || !wk::animOn()) return;
    if (lv_anim_t* run = lv_anim_get(obj, hitExec)) {   // restart, keep the saved originals
        run->act_time = 0;
        return;
    }
    HitSave* hs = (HitSave*)lv_malloc(sizeof(HitSave));
    if (!hs) return;
    hs->has_w = lv_obj_get_local_style_prop(obj, LV_STYLE_SHADOW_WIDTH, &hs->w, 0) == LV_STYLE_RES_FOUND;
    hs->has_c = lv_obj_get_local_style_prop(obj, LV_STYLE_SHADOW_COLOR, &hs->c, 0) == LV_STYLE_RES_FOUND;
    hs->has_o = lv_obj_get_local_style_prop(obj, LV_STYLE_SHADOW_OPA, &hs->o, 0) == LV_STYLE_RES_FOUND;
    hs->has_s = lv_obj_get_local_style_prop(obj, LV_STYLE_TRANSFORM_SCALE_X, &hs->s, 0) == LV_STYLE_RES_FOUND;
    lv_obj_set_style_shadow_width(obj, 22, 0);
    lv_obj_set_style_shadow_color(obj, c(col::glow), 0);
    lv_obj_set_style_transform_pivot_x(obj, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(obj, LV_PCT(50), 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, hitExec);
    lv_anim_set_values(&a, 0, 1024);
    lv_anim_set_duration(&a, 280);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_user_data(&a, hs);
    lv_anim_set_completed_cb(&a, hitDone);
    lv_anim_start(&a);
}

// ================================================================ misc helpers
// ui.h: custom draw callback profile (0.4.9); here because every UI build links widgets.cpp
DrawProf& drawProf()
{
    static DrawProf p;
    return p;
}

lv_obj_t* panelCreate(lv_obj_t* parent, int x, int y, int w, int h)
{
    lv_obj_t* p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_add_style(p, &styles().panel, 0);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

lv_obj_t* panelHead(lv_obj_t* panel, Icon icon, const char* title)
{
    // .panel-head: min-height 36, padding 0 13; .panel-title: icon + title, gap 10.
    lv_obj_t* head = lv_obj_create(panel);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), geo::panel_head_h);
    lv_obj_set_pos(head, 0, 0);
    lv_obj_set_style_pad_hor(head, 13, 0);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* left = lv_obj_create(head);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 10, 0);
    iconCreate(left, icon, 18, col::ink);
    lv_obj_t* t = lv_label_create(left);
    lv_label_set_text(t, title ? title : "");
    lv_obj_add_style(t, &styles().panel_title, 0);

    lv_obj_t* right = lv_obj_create(head);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    // START, not END: LVGL sizes a content-sized END-aligned flex row from the last child only.
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 6, 0);
    return right;
}

lv_obj_t* label(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text ? text : "");
    if (font) lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, c(color), 0);
    return l;
}

lv_obj_t* overline(lv_obj_t* parent, const char* text)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text ? text : "");
    lv_obj_add_style(l, &styles().overline, 0);
    return l;
}

void setHit(lv_obj_t* obj, int extra_px)
{
    if (obj) lv_obj_set_ext_click_area(obj, extra_px);
}

// ================================================================ scaled panels (maximize)
lv_point_t pointerIn(const lv_obj_t* obj)
{
    lv_point_t p = {0, 0};
    lv_indev_t* indev = lv_indev_active();
    if (indev) lv_indev_get_point(indev, &p);
    if (!obj) return p;
    const UiScale s = uiScaleOf(obj);
    if (s.unit()) return p;
    // offsets inside obj in design px, measured from obj's top-left corner on the screen
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    p.x = a.x1 + (int32_t)lroundf((float)(p.x - a.x1) / s.sx);
    p.y = a.y1 + (int32_t)lroundf((float)(p.y - a.y1) / s.sy);
    return p;
}

void screenArea(const lv_obj_t* obj, lv_area_t* out)
{
    if (!obj || !out) return;
    lv_obj_get_coords(obj, out);
    lv_obj_get_transformed_area(obj, out, LV_OBJ_POINT_TRANSFORM_FLAG_RECURSIVE);
}

}  // namespace orbit::ui
