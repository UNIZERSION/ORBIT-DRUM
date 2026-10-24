// Widget kit, part 3: toggle, select + popup menu, horizontal slider, text field and the
// on-screen keyboard (Polish letters: PL key map, or long press on a/c/e/l/n/o/s/z/x).
#include <string.h>

#include "ui/widgets.h"
#include "ui/widgets_internal.h"

namespace orbit::ui {

// ================================================================ shared bits
lv_obj_t* wk::overlayCreate(lv_opa_t scrim_opa)
{
    lv_obj_t* o = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(o, 0, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    if (scrim_opa) {
        lv_obj_set_style_bg_color(o, c(col::scrim), 0);
        lv_obj_set_style_bg_opa(o, scrim_opa, 0);
    }
    return o;
}

namespace {

char* dupStr(const char* s)
{
    if (!s) s = "";
    const size_t n = strlen(s) + 1;
    char* d = (char*)lv_malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

// ================================================================ toggle
struct Toggle {
    lv_obj_t* obj;
    lv_obj_t* knob;
    bool on;
    void (*cb)(bool, void*);
    void* user;
};

lv_style_t s_toggle_on, s_knob, s_knob_on;
bool s_toggle_styles = false;

void toggleStyles()
{
    if (s_toggle_styles) return;
    s_toggle_styles = true;
    static const lv_style_prop_t props[] = {LV_STYLE_BG_COLOR, LV_STYLE_BORDER_COLOR, LV_STYLE_TRANSLATE_X,
                                            LV_STYLE_PROP_INV};
    static lv_style_transition_dsc_t tr;
    lv_style_transition_dsc_init(&tr, props, lv_anim_path_ease_out, 160, 0, nullptr);
    // .toggle.on: accent track; .toggle>i: 16 px muted dot, on: translateX(20px) + on-accent.
    lv_style_init(&s_toggle_on);
    lv_style_set_bg_color(&s_toggle_on, c(col::accent));
    lv_style_set_border_color(&s_toggle_on, c(col::accent));
    lv_style_init(&s_knob);
    lv_style_set_bg_opa(&s_knob, LV_OPA_COVER);
    lv_style_set_bg_color(&s_knob, c(col::muted));
    lv_style_set_radius(&s_knob, LV_RADIUS_CIRCLE);
    lv_style_set_transition(&s_knob, &tr);
    lv_style_init(&s_knob_on);
    lv_style_set_bg_color(&s_knob_on, c(col::on_accent));
    lv_style_set_translate_x(&s_knob_on, 20);
    lv_style_set_transition(&s_knob_on, &tr);
}

void toggleApply(Toggle* t)
{
    lv_obj_set_state(t->obj, LV_STATE_CHECKED, t->on);
    lv_obj_set_state(t->knob, LV_STATE_CHECKED, t->on);
}

void toggleCb(lv_event_t* e)
{
    Toggle* t = (Toggle*)lv_event_get_user_data(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_CLICKED:
        t->on = !t->on;
        toggleApply(t);
        if (t->cb) t->cb(t->on, t->user);
        break;
    case LV_EVENT_DELETE:
        lv_free(t);
        break;
    default:
        break;
    }
}

}  // namespace

lv_obj_t* toggleCreate(lv_obj_t* parent, bool on, void (*cb)(bool on, void* user), void* user)
{
    toggleStyles();
    Toggle* t = (Toggle*)lv_malloc(sizeof(Toggle));
    if (!t) return nullptr;
    t->on = on;
    t->cb = cb;
    t->user = user;
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_add_style(o, &styles().toggle, 0);
    lv_obj_add_style(o, &s_toggle_on, LV_STATE_CHECKED);
    lv_obj_set_size(o, 45, 24);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(o, 10 + hitBonus());
    lv_obj_t* k = lv_obj_create(o);
    lv_obj_remove_style_all(k);
    lv_obj_add_style(k, &s_knob, 0);
    lv_obj_add_style(k, &s_knob_on, LV_STATE_CHECKED);
    lv_obj_set_size(k, 16, 16);
    lv_obj_set_pos(k, 0, 0);
    lv_obj_remove_flag(k, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(k, LV_OBJ_FLAG_SCROLLABLE);
    t->obj = o;
    t->knob = k;
    toggleApply(t);
    lv_obj_add_event_cb(o, toggleCb, LV_EVENT_ALL, t);
    return o;
}

void toggleSet(lv_obj_t* toggle, bool on)
{
    Toggle* t = (Toggle*)wk::findState(toggle, toggleCb);
    if (!t || t->on == on) return;
    t->on = on;
    toggleApply(t);
}

// ================================================================ popup menu
namespace {

struct Popup {
    lv_obj_t* overlay;
    void (*cb)(int, void*);
    void* user;
};
lv_obj_t* s_popup = nullptr;
lv_style_t s_menu_box, s_menu_row, s_menu_row_pr, s_menu_row_sel;
bool s_menu_styles = false;

void menuStyles()
{
    if (s_menu_styles) return;
    s_menu_styles = true;
    lv_style_init(&s_menu_box);
    lv_style_set_bg_opa(&s_menu_box, LV_OPA_COVER);
    lv_style_set_bg_color(&s_menu_box, c(col::modal_bg));
    lv_style_set_border_width(&s_menu_box, 1);
    lv_style_set_border_color(&s_menu_box, c(col::line));
    lv_style_set_radius(&s_menu_box, 6);
    lv_style_set_shadow_width(&s_menu_box, 24);
    lv_style_set_shadow_offset_y(&s_menu_box, 8);
    lv_style_set_shadow_color(&s_menu_box, lv_color_black());
    lv_style_set_shadow_opa(&s_menu_box, 50);
    lv_style_set_pad_all(&s_menu_box, 4);
    lv_style_set_pad_row(&s_menu_box, 2);
    lv_style_init(&s_menu_row);
    lv_style_set_radius(&s_menu_row, 4);
    lv_style_set_bg_opa(&s_menu_row, LV_OPA_TRANSP);
    lv_style_set_bg_color(&s_menu_row, c(col::hover));
    lv_style_set_pad_hor(&s_menu_row, 12);
    lv_style_set_text_font(&s_menu_row, f_r14);
    lv_style_set_text_color(&s_menu_row, c(col::ink));
    lv_style_init(&s_menu_row_pr);
    lv_style_set_bg_opa(&s_menu_row_pr, LV_OPA_COVER);
    lv_style_init(&s_menu_row_sel);
    lv_style_set_bg_opa(&s_menu_row_sel, LV_OPA_COVER);
    lv_style_set_bg_color(&s_menu_row_sel, c(col::file_sel));
    lv_style_set_text_color(&s_menu_row_sel, c(col::accent_dark));
}

void popupClose()
{
    if (s_popup) {
        lv_obj_t* o = s_popup;
        s_popup = nullptr;
        lv_obj_delete_async(o);
    }
}

void popupOverlayCb(lv_event_t* e)
{
    Popup* p = (Popup*)lv_event_get_user_data(e);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED && lv_event_get_target(e) == p->overlay) {
        popupClose();
    } else if (code == LV_EVENT_DELETE) {
        if (s_popup == p->overlay) s_popup = nullptr;
        lv_free(p);
    }
}

void popupRowCb(lv_event_t* e)
{
    lv_obj_t* row = (lv_obj_t*)lv_event_get_current_target(e);
    Popup* p = (Popup*)lv_event_get_user_data(e);
    const int index = (int)(intptr_t)lv_obj_get_user_data(row);
    void (*cb)(int, void*) = p->cb;
    void* user = p->user;
    popupClose();   // deleted asynchronously, p stays valid until then
    if (cb) cb(index, user);
}

}  // namespace

void popupMenu(lv_obj_t* anchor, const char* const* items, int count, int selected,
               void (*cb)(int index, void* user), void* user)
{
    if (!items || count <= 0) return;
    menuStyles();
    popupClose();
    Popup* p = (Popup*)lv_malloc(sizeof(Popup));
    if (!p) return;
    p->cb = cb;
    p->user = user;
    lv_obj_t* ov = wk::overlayCreate(0);
    p->overlay = ov;
    s_popup = ov;
    lv_obj_add_event_cb(ov, popupOverlayCb, LV_EVENT_ALL, p);

    // [0.4.6] Rows as large as the anchor's panel draws (a maximized panel, a larger dialog) or Display > Interface size.
    float k = anchor ? uiScaleOf(anchor).f : 1.0f;
    if (uiSizeFactor() > k) k = uiSizeFactor();
    const int kRowH = (int)lroundf(36.0f * k);
    constexpr int kMaxRows = 8;
    lv_area_t aa = {0, 0, 0, 0};
    if (anchor) screenArea(anchor, &aa);   // as drawn: the anchor's panel may be scaled (maximize)
    int32_t w = anchor ? lv_area_get_width(&aa) : 200;
    const int32_t wmin = (int32_t)lroundf(150.0f * k);
    if (w < wmin) w = wmin;
    const int32_t sh0 = lv_display_get_vertical_resolution(lv_obj_get_display(ov));
    int rows = count < kMaxRows ? count : kMaxRows;
    while (rows > 3 && rows * (kRowH + 2) + 10 > sh0 - 16) --rows;   // fits on the screen (the rest scrolls)
    const int32_t h = rows * kRowH + (rows - 1) * 2 + 10;
    lv_obj_t* box = lv_obj_create(ov);
    lv_obj_remove_style_all(box);
    lv_obj_add_style(box, &s_menu_box, 0);
    lv_obj_set_size(box, w, h);
    if (count <= rows) lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    const int32_t sw = lv_display_get_horizontal_resolution(lv_obj_get_display(ov));
    const int32_t sh = lv_display_get_vertical_resolution(lv_obj_get_display(ov));
    int32_t x = aa.x1, y = aa.y2 + 4;
    if (y + h > sh - 8) y = aa.y1 - 4 - h;   // no room below: open upwards
    if (y < 8) y = 8;
    if (x + w > sw - 8) x = sw - 8 - w;
    if (x < 8) x = 8;
    lv_obj_set_pos(box, x, y);
    for (int i = 0; i < count; ++i) {
        lv_obj_t* r = lv_obj_create(box);
        lv_obj_remove_style_all(r);
        lv_obj_add_style(r, &s_menu_row, 0);
        lv_obj_add_style(r, &s_menu_row_pr, LV_STATE_PRESSED);
        if (i == selected) lv_obj_add_style(r, &s_menu_row_sel, 0);
        lv_obj_set_size(r, LV_PCT(100), kRowH);
        if (k > 1.0f) {
            lv_obj_set_style_text_font(r, fontScaled(f_r14, k), 0);
            lv_obj_set_style_pad_hor(r, (int32_t)lroundf(12.0f * k), 0);
        }
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(r, (void*)(intptr_t)i);
        lv_obj_t* l = lv_label_create(r);
        lv_label_set_text(l, items[i] ? items[i] : "");
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_event_cb(r, popupRowCb, LV_EVENT_CLICKED, p);
    }
    if (selected >= rows && selected < count)
        lv_obj_scroll_to_y(box, (selected - rows / 2) * (kRowH + 2), LV_ANIM_OFF);
}

// ================================================================ select
namespace {

struct Select {
    lv_obj_t* obj;
    lv_obj_t* lbl;
    char** options;   // owned copies
    int count;
    int sel;
    void (*cb)(int, void*);
    void* user;
};

void selectPicked(int index, void* user)
{
    Select* s = (Select*)user;
    if (index < 0 || index >= s->count) return;
    s->sel = index;
    wk::setTextIfChanged(s->lbl, s->options[index]);
    if (s->cb) s->cb(index, s->user);
}

void selectCb(lv_event_t* e)
{
    Select* s = (Select*)lv_event_get_user_data(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_CLICKED:
        popupMenu(s->obj, (const char* const*)s->options, s->count, s->sel, selectPicked, s);
        break;
    case LV_EVENT_DELETE:
        if (Popup* p = (Popup*)wk::findState(s_popup, popupOverlayCb)) {
            if (p->user == s) {   // its menu must not call back into freed memory
                p->cb = nullptr;
                popupClose();
            }
        }
        for (int i = 0; i < s->count; ++i) lv_free(s->options[i]);
        lv_free(s->options);
        lv_free(s);
        break;
    default:
        break;
    }
}

}  // namespace

lv_obj_t* selectCreate(lv_obj_t* parent, int w, int h, const char* const* options, int count, int selected,
                       void (*cb)(int index, void* user), void* user)
{
    Select* s = (Select*)lv_malloc(sizeof(Select));
    if (!s) return nullptr;
    memset(s, 0, sizeof *s);
    s->count = count > 0 ? count : 0;
    s->options = (char**)lv_malloc(sizeof(char*) * (size_t)(s->count ? s->count : 1));
    for (int i = 0; i < s->count; ++i) s->options[i] = dupStr(options ? options[i] : "");
    s->sel = (selected >= 0 && selected < s->count) ? selected : 0;
    s->cb = cb;
    s->user = user;
    // .ui-select (pad 9, 11 px) / .mini-select (pad 4 2, 10 px) depending on the height.
    const bool mini = h < 30;
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_add_style(o, &styles().input, 0);
    lv_obj_add_style(o, &styles().action_btn_pr, LV_STATE_PRESSED);
    lv_obj_set_size(o, w, h);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(o, (mini ? 8 : 4) + hitBonus());
    if (mini) {
        lv_obj_set_style_pad_hor(o, 6, 0);
        lv_obj_set_style_pad_ver(o, 0, 0);
        lv_obj_set_style_radius(o, 3, 0);
        lv_obj_set_style_bg_color(o, c(col::mini_select), 0);
        lv_obj_set_style_text_font(o, f_r12, 0);
    }
    s->lbl = lv_label_create(o);
    lv_label_set_text(s->lbl, s->count ? s->options[s->sel] : "");
    lv_label_set_long_mode(s->lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s->lbl, w - (mini ? 12 : 20) - (mini ? 14 : 20));
    lv_obj_align(s->lbl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* chev = wk::chevDown(o, mini ? 14 : 16, col::ink);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, mini ? 0 : -2, 0);
    s->obj = o;
    lv_obj_add_event_cb(o, selectCb, LV_EVENT_ALL, s);
    rippleAttach(o);
    return o;
}

void selectSet(lv_obj_t* select, int index)
{
    Select* s = (Select*)wk::findState(select, selectCb);
    if (!s || index < 0 || index >= s->count) return;
    s->sel = index;
    wk::setTextIfChanged(s->lbl, s->options[index]);
}

// ================================================================ horizontal slider
namespace {

struct HSlider {
    void (*cb)(int, void*);
    void* user;
};

void hsliderCb(lv_event_t* e)
{
    HSlider* h = (HSlider*)lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t* sl = (lv_obj_t*)lv_event_get_current_target(e);
        if (h->cb) h->cb((int)lv_slider_get_value(sl), h->user);
    } else if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        lv_free(h);
    }
}

}  // namespace

lv_obj_t* hsliderCreate(lv_obj_t* parent, int w, int min, int max, int value, void (*cb)(int v, void* user), void* user)
{
    HSlider* h = (HSlider*)lv_malloc(sizeof(HSlider));
    if (!h) return nullptr;
    h->cb = cb;
    h->user = user;
    lv_obj_t* s = lv_slider_create(parent);   // styled by the ORBIT theme (track, accent fill, knob)
    lv_obj_set_size(s, w, 6);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_ext_click_area(s, 14 + hitBonus());
    lv_obj_add_event_cb(s, hsliderCb, LV_EVENT_ALL, h);
    return s;
}

// ================================================================ keyboard
namespace {

const char* const kMapLower[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "DEL", "\n",
                                 "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
                                 "a", "s", "d", "f", "g", "h", "j", "k", "l", "-", "_", "\n",
                                 "ABC", "z", "x", "c", "v", "b", "n", "m", ",", ".", "/", "\n",
                                 "#+=", "PL", "SPACE", "OK", ""};
const char* const kMapUpper[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "DEL", "\n",
                                 "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
                                 "A", "S", "D", "F", "G", "H", "J", "K", "L", "-", "_", "\n",
                                 "abc", "Z", "X", "C", "V", "B", "N", "M", ",", ".", "/", "\n",
                                 "#+=", "PL", "SPACE", "OK", ""};
const char* const kMapPl[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "DEL", "\n",
                              "ą", "ć", "ę", "ł", "ń", "ó", "ś", "ź", "ż", "\n",
                              "Ą", "Ć", "Ę", "Ł", "Ń", "Ó", "Ś", "Ź", "Ż", "\n",
                              "abc", "-", "_", "(", ")", "!", "?", ",", ".", "\n",
                              "#+=", "ABC", "SPACE", "OK", ""};
const char* const kMapSym[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "DEL", "\n",
                               "!", "@", "#", "$", "%", "&", "*", "(", ")", "+", "\n",
                               "=", "/", ":", ";", "'", "\"", "?", "<", ">", "~", "\n",
                               "abc", "[", "]", "{", "}", "^", "|", ",", ".", "\n",
                               "PL", "ABC", "SPACE", "OK", ""};
// Key width (1..15) | flags. Specials (CHECKED) get the muted key look.
constexpr lv_buttonmatrix_ctrl_t K(int v) { return (lv_buttonmatrix_ctrl_t)v; }
#define KC(w, f) K((w) | (f))
constexpr int kN = LV_BUTTONMATRIX_CTRL_NO_REPEAT;
constexpr int kS = LV_BUTTONMATRIX_CTRL_CHECKED | LV_BUTTONMATRIX_CTRL_NO_REPEAT;
const lv_buttonmatrix_ctrl_t kCtrlLatin[] = {
    K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), KC(2, LV_BUTTONMATRIX_CTRL_CHECKED),
    K(1), K(1), KC(1, kN), K(1), K(1), K(1), K(1), K(1), KC(1, kN), K(1),
    KC(1, kN), KC(1, kN), K(1), K(1), K(1), K(1), K(1), K(1), KC(1, kN), K(1), K(1),
    KC(2, kS), KC(1, kN), KC(1, kN), KC(1, kN), K(1), K(1), KC(1, kN), K(1), K(1), K(1), K(1),
    KC(2, kS), KC(2, kS), KC(7, kN), KC(3, kS)};
const lv_buttonmatrix_ctrl_t kCtrlPl[] = {
    K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), KC(2, LV_BUTTONMATRIX_CTRL_CHECKED),
    K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1),
    K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1),
    KC(2, kS), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1),
    KC(2, kS), KC(2, kS), KC(7, kN), KC(3, kS)};
const lv_buttonmatrix_ctrl_t kCtrlSym[] = {
    K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), KC(2, LV_BUTTONMATRIX_CTRL_CHECKED),
    K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1),
    K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1),
    KC(2, kS), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1),
    KC(2, kS), KC(2, kS), KC(7, kN), KC(3, kS)};
// Numeric pad (keyboardOpenNumeric): 3 rows x 4 large keys; digits do not auto-repeat, DEL does.
const char* const kMapNum[] = {"1", "2", "3", "DEL", "\n",
                               "4", "5", "6", ".", "\n",
                               "7", "8", "9", "0", ""};
const lv_buttonmatrix_ctrl_t kCtrlNum[] = {
    KC(1, kN), KC(1, kN), KC(1, kN), KC(1, LV_BUTTONMATRIX_CTRL_CHECKED),
    KC(1, kN), KC(1, kN), KC(1, kN), KC(1, kN),
    KC(1, kN), KC(1, kN), KC(1, kN), KC(1, kN)};
constexpr int kNumPadW = 560;   // centred under the text field
#undef KC

// Long press on a base letter replaces it with its Polish variant.
struct PlPair { const char* base; const char* pl; };
const PlPair kPlPairs[] = {{"a", "ą"}, {"c", "ć"}, {"e", "ę"}, {"l", "ł"}, {"n", "ń"}, {"o", "ó"},
                           {"s", "ś"}, {"z", "ż"}, {"x", "ź"}, {"A", "Ą"}, {"C", "Ć"}, {"E", "Ę"},
                           {"L", "Ł"}, {"N", "Ń"}, {"O", "Ó"}, {"S", "Ś"}, {"Z", "Ż"}, {"X", "Ź"}};

struct Keyboard {
    lv_obj_t* overlay;
    lv_obj_t* ta;
    lv_obj_t* keys;
    int maxlen;
    void (*on_done)(const char*, void*);
    void* user;
    uint32_t last_key;   // id of the last inserted key (long-press replacement)
    bool last_was_letter;
};
Keyboard* s_kb = nullptr;

void kbSetMap(Keyboard* k, const char* const* map, const lv_buttonmatrix_ctrl_t* ctrl)
{
    lv_buttonmatrix_set_map(k->keys, map);
    lv_buttonmatrix_set_ctrl_map(k->keys, ctrl);
}

void kbClose(bool accept)
{
    Keyboard* k = s_kb;
    if (!k) return;
    s_kb = nullptr;
    if (accept && k->on_done) {
        // Copy before closing: the callback may open another dialog.
        char buf[160];
        const char* t = lv_textarea_get_text(k->ta);
        strncpy(buf, t ? t : "", sizeof buf - 1);
        buf[sizeof buf - 1] = 0;
        void (*cb)(const char*, void*) = k->on_done;
        void* user = k->user;
        lv_obj_delete_async(k->overlay);
        cb(buf, user);
        return;
    }
    lv_obj_delete_async(k->overlay);
}

void kbInsert(Keyboard* k, const char* txt)
{
    const char* cur = lv_textarea_get_text(k->ta);
    if ((int)(strlen(cur) + strlen(txt)) > k->maxlen) return;
    lv_textarea_add_text(k->ta, txt);
}

void kbKeysCb(lv_event_t* e)
{
    Keyboard* k = (Keyboard*)lv_event_get_user_data(e);
    lv_obj_t* bm = k->keys;
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_LONG_PRESSED) {
        const uint32_t id = lv_buttonmatrix_get_selected_button(bm);
        if (id == LV_BUTTONMATRIX_BUTTON_NONE || id != k->last_key || !k->last_was_letter) return;
        const char* t = lv_buttonmatrix_get_button_text(bm, id);
        for (const PlPair& p : kPlPairs) {
            if (t && strcmp(t, p.base) == 0) {
                lv_textarea_delete_char(k->ta);
                kbInsert(k, p.pl);
                k->last_was_letter = false;
                return;
            }
        }
        return;
    }
    if (code != LV_EVENT_VALUE_CHANGED) return;
    const uint32_t id = lv_buttonmatrix_get_selected_button(bm);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char* t = lv_buttonmatrix_get_button_text(bm, id);
    if (!t) return;
    k->last_key = id;
    k->last_was_letter = false;
    if (strcmp(t, "DEL") == 0) lv_textarea_delete_char(k->ta);
    else if (strcmp(t, "OK") == 0) kbClose(true);
    else if (strcmp(t, "ABC") == 0) kbSetMap(k, kMapUpper, kCtrlLatin);
    else if (strcmp(t, "abc") == 0) kbSetMap(k, kMapLower, kCtrlLatin);
    else if (strcmp(t, "PL") == 0) kbSetMap(k, kMapPl, kCtrlPl);
    else if (strcmp(t, "#+=") == 0) kbSetMap(k, kMapSym, kCtrlSym);
    else if (strcmp(t, "SPACE") == 0) kbInsert(k, " ");
    else {
        kbInsert(k, t);
        k->last_was_letter = true;
    }
}

void kbOverlayCb(lv_event_t* e)
{
    Keyboard* k = (Keyboard*)lv_event_get_user_data(e);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED && lv_event_get_target(e) == k->overlay) {
        kbClose(false);   // tap on the scrim = cancel
    } else if (code == LV_EVENT_DELETE) {
        if (s_kb == k) s_kb = nullptr;
        lv_free(k);
    }
}

void kbBtnCb(lv_event_t* e)
{
    kbClose(lv_event_get_user_data(e) != nullptr);
}

void kbOpen(const char* title, const char* initial, int maxlen, void (*on_done)(const char* text, void* user),
            void* user, bool numeric)
{
    if (s_kb) kbClose(false);
    Keyboard* k = (Keyboard*)lv_malloc(sizeof(Keyboard));
    if (!k) return;
    memset(k, 0, sizeof *k);
    k->maxlen = maxlen > 0 ? (maxlen < 150 ? maxlen : 150) : 150;
    k->on_done = on_done;
    k->user = user;
    k->last_key = LV_BUTTONMATRIX_BUTTON_NONE;
    k->overlay = wk::overlayCreate(140);
    s_kb = k;
    lv_obj_add_event_cb(k->overlay, kbOverlayCb, LV_EVENT_ALL, k);

    // Bottom sheet: modal look (modal-bg, accent top line), title + field + buttons, keys.
    const int32_t sh = lv_display_get_vertical_resolution(lv_obj_get_display(k->overlay));
    const int32_t sw = lv_display_get_horizontal_resolution(lv_obj_get_display(k->overlay));
    constexpr int kSheetH = 352;
    lv_obj_t* sheet = lv_obj_create(k->overlay);
    lv_obj_remove_style_all(sheet);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sheet, c(col::modal_bg), 0);
    lv_obj_set_style_border_width(sheet, 1, 0);
    lv_obj_set_style_border_color(sheet, c(col::accent), 0);
    lv_obj_set_style_radius(sheet, geo::radius, 0);
    lv_obj_set_size(sheet, sw, kSheetH + geo::radius);
    lv_obj_set_pos(sheet, 0, sh - kSheetH);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);   // taps on the sheet do not cancel
    lv_obj_remove_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* cap = overline(sheet, title ? title : "TEXT");
    lv_obj_set_pos(cap, 24, 26);
    k->ta = lv_textarea_create(sheet);
    lv_textarea_set_one_line(k->ta, true);
    lv_obj_set_style_text_font(k->ta, f_m16, 0);
    lv_obj_set_size(k->ta, 760, 42);
    lv_obj_set_pos(k->ta, 210, 14);
    lv_textarea_set_text(k->ta, initial ? initial : "");
    lv_textarea_set_cursor_pos(k->ta, LV_TEXTAREA_CURSOR_LAST);
    lv_obj_add_state(k->ta, LV_STATE_FOCUSED);
    lv_obj_remove_flag(k->ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_t* cancel = actionBtn(sheet, "CANCEL", false, 120, 42);
    lv_obj_set_pos(cancel, sw - 24 - 120 - 10 - 120, 14);
    lv_obj_add_event_cb(cancel, kbBtnCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* ok = actionBtn(sheet, "OK", true, 120, 42);
    lv_obj_set_pos(ok, sw - 24 - 120, 14);
    lv_obj_add_event_cb(ok, kbBtnCb, LV_EVENT_CLICKED, (void*)1);

    k->keys = lv_buttonmatrix_create(sheet);   // styled by the ORBIT theme
    if (numeric) {
        lv_obj_set_size(k->keys, kNumPadW, kSheetH - 86);
        lv_obj_set_pos(k->keys, (sw - kNumPadW) / 2, 70);
        kbSetMap(k, kMapNum, kCtrlNum);
    } else {
        lv_obj_set_size(k->keys, sw - 48, kSheetH - 86);
        lv_obj_set_pos(k->keys, 24, 70);
        kbSetMap(k, kMapLower, kCtrlLatin);
    }
    lv_obj_add_event_cb(k->keys, kbKeysCb, LV_EVENT_VALUE_CHANGED, k);
    lv_obj_add_event_cb(k->keys, kbKeysCb, LV_EVENT_LONG_PRESSED, k);
    // No slide-in: each step of a 1280x361 sheet over the translucent scrim repaints ~0.5 Mpx.
}

}  // namespace

bool keyboardTypeKey(bool is_char, char ch, bool backspace, bool enter, bool esc)
{
    Keyboard* k = s_kb;
    if (!k) return false;
    if (esc) kbClose(false);
    else if (enter) kbClose(true);
    else if (backspace) lv_textarea_delete_char(k->ta);
    else if (is_char && (unsigned char)ch >= 0x20 && (unsigned char)ch < 0x7F) {
        const char t[2] = {ch, 0};
        kbInsert(k, t);
    }
    return true;   // (other keys: swallowed while typing)
}

void keyboardOpen(const char* title, const char* initial, int maxlen,
                  void (*on_done)(const char* text, void* user), void* user)
{
    kbOpen(title, initial, maxlen, on_done, user, false);
}

void keyboardOpenNumeric(const char* title, const char* initial, int maxlen,
                         void (*on_done)(const char* text, void* user), void* user)
{
    kbOpen(title, initial, maxlen, on_done, user, true);
}

// ================================================================ text field
namespace {

struct TextField {
    lv_obj_t* obj;
    lv_obj_t* lbl;
    char* text;
    char* placeholder;
    int maxlen;
    void (*on_done)(const char*, void*);
    void* user;
};

void tfShow(TextField* t)
{
    const bool empty = t->text[0] == 0;
    wk::setTextIfChanged(t->lbl, empty && t->placeholder ? t->placeholder : t->text);
    lv_obj_set_style_text_color(t->lbl, c(empty ? col::muted : col::ink), 0);
}

void tfDone(const char* text, void* user)
{
    TextField* t = (TextField*)user;
    strncpy(t->text, text ? text : "", (size_t)t->maxlen);
    t->text[t->maxlen] = 0;
    tfShow(t);
    if (t->on_done) t->on_done(t->text, t->user);
}

void tfCb(lv_event_t* e)
{
    TextField* t = (TextField*)lv_event_get_user_data(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_CLICKED:
        keyboardOpen(t->placeholder ? t->placeholder : "TEXT", t->text, t->maxlen, tfDone, t);
        break;
    case LV_EVENT_DELETE:
        if (s_kb && s_kb->user == t) {   // never call back into a deleted field
            s_kb->on_done = nullptr;
            kbClose(false);
        }
        lv_free(t->text);
        lv_free(t->placeholder);
        lv_free(t);
        break;
    default:
        break;
    }
}

}  // namespace

lv_obj_t* textFieldCreate(lv_obj_t* parent, int w, const char* text, int maxlen,
                          void (*on_done)(const char* text, void* user), void* user, const char* placeholder)
{
    TextField* t = (TextField*)lv_malloc(sizeof(TextField));
    if (!t) return nullptr;
    t->maxlen = maxlen > 0 ? maxlen : 32;
    t->text = (char*)lv_malloc((size_t)t->maxlen + 1);
    strncpy(t->text, text ? text : "", (size_t)t->maxlen);
    t->text[t->maxlen] = 0;
    t->placeholder = placeholder ? dupStr(placeholder) : nullptr;
    t->on_done = on_done;
    t->user = user;
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_add_style(o, &styles().input, 0);
    lv_obj_add_style(o, &styles().focus, LV_STATE_PRESSED);
    lv_obj_set_size(o, w, 36);
    lv_obj_set_style_pad_ver(o, 0, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(o, 4 + hitBonus());
    t->lbl = lv_label_create(o);
    lv_label_set_long_mode(t->lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(t->lbl, LV_PCT(100));
    lv_obj_align(t->lbl, LV_ALIGN_LEFT_MID, 0, 0);
    t->obj = o;
    tfShow(t);
    lv_obj_add_event_cb(o, tfCb, LV_EVENT_ALL, t);
    return o;
}

void textFieldSet(lv_obj_t* field, const char* text)
{
    TextField* t = (TextField*)wk::findState(field, tfCb);
    if (!t) return;
    strncpy(t->text, text ? text : "", (size_t)t->maxlen);
    t->text[t->maxlen] = 0;
    tfShow(t);
}

}  // namespace orbit::ui
