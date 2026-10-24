// Widget kit, part 4: modal window and toast (both on lv_layer_top()).
#include <string.h>

#include "ui/widgets.h"
#include "ui/widgets_internal.h"

namespace orbit::ui {

namespace {

// ================================================================ modal
struct Modal {
    lv_obj_t* scrim;
    lv_obj_t* win;
    lv_obj_t* body;
    lv_obj_t* footer;
    int min_h, max_h;
    void (*on_close)(void*);
    void* user;
};
Modal* s_modal = nullptr;
constexpr int kHeadH = 56, kFootH = 58;
constexpr lv_opa_t kScrimOpa = 153;   // #0009

lv_style_t s_win, s_head, s_body, s_foot;
bool s_modal_styles = false;

void modalStyles()
{
    if (s_modal_styles) return;
    s_modal_styles = true;
    // .modal-window: accent border, radius 9, modal-bg. The prototype's shadow 0 22px 80px #0009
    // is left out: the #0009 scrim hides almost all of it, and with no shadow cache in the
    // software renderer every strip redrawn under the window would re-blur it.
    lv_style_init(&s_win);
    lv_style_set_bg_opa(&s_win, LV_OPA_COVER);
    lv_style_set_bg_color(&s_win, c(col::modal_bg));
    lv_style_set_border_width(&s_win, 1);
    lv_style_set_border_color(&s_win, c(col::accent));
    lv_style_set_radius(&s_win, geo::radius);
    lv_style_set_text_color(&s_win, c(col::ink));
    lv_style_set_text_font(&s_win, f_r14);
    // .modal-head: 56 px, bottom line, padding 0 24, 17 px / .10em -> 20 px.
    lv_style_init(&s_head);
    lv_style_set_border_side(&s_head, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_width(&s_head, 1);
    lv_style_set_border_color(&s_head, c(col::line));
    lv_style_set_pad_left(&s_head, 24);
    lv_style_set_pad_right(&s_head, 14);
    lv_style_set_text_font(&s_head, f_m20);
    lv_style_set_text_letter_space(&s_head, 2);
    lv_style_init(&s_body);
    lv_style_set_pad_all(&s_body, 0);
    // .modal-footer: 58 px, top line, padding 0 24, gap 12, muted 11 px note -> 13 px.
    lv_style_init(&s_foot);
    lv_style_set_border_side(&s_foot, LV_BORDER_SIDE_TOP);
    lv_style_set_border_width(&s_foot, 1);
    lv_style_set_border_color(&s_foot, c(col::line));
    lv_style_set_pad_hor(&s_foot, 24);
    lv_style_set_pad_column(&s_foot, 12);
    lv_style_set_text_font(&s_foot, f_r13);
    lv_style_set_text_color(&s_foot, c(col::muted));
}

void modalScrimCb(lv_event_t* e)
{
    Modal* m = (Modal*)lv_event_get_user_data(e);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED && lv_event_get_target(e) == m->scrim) {
        if (s_modal == m) modalClose();
    } else if (code == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC) {
        if (s_modal == m) modalClose();
    } else if (code == LV_EVENT_DELETE) {
        if (s_modal == m) s_modal = nullptr;
        lv_free(m);
    }
}

void modalCloseBtnCb(lv_event_t*) { modalClose(); }

void bodyLimits(Modal* m)
{
    const int foot = m->footer ? kFootH : 0;
    int lo = m->min_h - kHeadH - foot - 2;
    int hi = m->max_h - kHeadH - foot - 2;
    if (lo < 0) lo = 0;
    if (hi < lo) hi = lo;
    lv_obj_set_style_min_height(m->body, lo, 0);
    lv_obj_set_style_max_height(m->body, hi, 0);
}

// ================================================================ toast
lv_obj_t* s_toast = nullptr;
lv_obj_t* s_toast_lbl = nullptr;
lv_timer_t* s_toast_timer = nullptr;

void toastExec(void* var, int32_t v)   // v: 0 hidden .. 1024 shown
{
    lv_obj_t* o = (lv_obj_t*)var;
    lv_obj_set_style_opa(o, (lv_opa_t)(255 * v / 1024), 0);
    lv_obj_set_style_translate_y(o, 15 - 15 * v / 1024, 0);
}

void toastHidden(lv_anim_t* a) { lv_obj_add_flag((lv_obj_t*)a->var, LV_OBJ_FLAG_HIDDEN); }

void toastAnim(bool show)
{
    lv_anim_delete(s_toast, toastExec);
    if (!wk::animOn()) {
        toastExec(s_toast, show ? 1024 : 0);
        if (!show) lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const int32_t cur = 1024 * lv_obj_get_style_opa(s_toast, 0) / 255;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_toast);
    lv_anim_set_exec_cb(&a, toastExec);
    lv_anim_set_values(&a, cur, show ? 1024 : 0);
    lv_anim_set_duration(&a, 240);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (!show) lv_anim_set_completed_cb(&a, toastHidden);
    lv_anim_start(&a);
}

void toastTimerCb(lv_timer_t*)
{
    s_toast_timer = nullptr;   // one-shot timers delete themselves
    if (s_toast) toastAnim(false);
}

// Toast "key": the text up to and including a colon in its first 24 bytes ("TAP TEMPO:"), else
// the whole text including its terminator (exact match). Returns the number of bytes to compare.
size_t toastKeyLen(const char* t)
{
    const char* colon = strchr(t, ':');
    if (colon && colon - t < 24) return (size_t)(colon - t) + 1;
    return strlen(t) + 1;
}

bool toastSameKey(const char* a, const char* b)
{
    const size_t n = toastKeyLen(a);
    return toastKeyLen(b) == n && strncmp(a, b, n) == 0;
}

constexpr size_t kToastShortLen = 24;           // bytes; shorter texts stay up for less time
constexpr uint32_t kToastMs = 2600, kToastShortMs = 1600;

void toastDeleted(lv_event_t*)
{
    s_toast = nullptr;
    s_toast_lbl = nullptr;
    if (s_toast_timer) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = nullptr;
    }
}

}  // namespace

lv_obj_t* modalOpen(const char* title, int w, int min_h, int max_h)
{
    modalStyles();
    if (s_modal) modalClose();
    Modal* m = (Modal*)lv_malloc(sizeof(Modal));
    if (!m) return nullptr;
    memset(m, 0, sizeof *m);
    m->min_h = min_h;
    m->max_h = max_h;
    m->scrim = wk::overlayCreate(0);
    lv_obj_set_style_bg_color(m->scrim, c(col::scrim), 0);
    lv_obj_set_style_bg_opa(m->scrim, kScrimOpa, 0);
    s_modal = m;
    lv_obj_add_event_cb(m->scrim, modalScrimCb, LV_EVENT_ALL, m);

    lv_obj_t* win = lv_obj_create(m->scrim);
    lv_obj_remove_style_all(win);
    lv_obj_add_style(win, &s_win, 0);
    lv_obj_set_size(win, w, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(win, min_h, 0);
    lv_obj_set_style_max_height(win, max_h, 0);
    lv_obj_add_flag(win, LV_OBJ_FLAG_CLICKABLE);   // taps inside never reach the scrim
    lv_obj_remove_flag(win, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(win, LV_FLEX_FLOW_COLUMN);
    lv_obj_center(win);
    m->win = win;

    lv_obj_t* head = lv_obj_create(win);
    lv_obj_remove_style_all(head);
    lv_obj_add_style(head, &s_head, 0);
    lv_obj_set_size(head, LV_PCT(100), kHeadH);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* t = lv_label_create(head);
    lv_label_set_text(t, title ? title : "");
    lv_obj_t* x = iconBtn(head, Icon::Close, 22, 40);
    lv_obj_add_event_cb(x, modalCloseBtnCb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* body = lv_obj_create(win);
    lv_obj_remove_style_all(body);
    lv_obj_add_style(body, &s_body, 0);
    lv_obj_set_size(body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);
    m->body = body;
    bodyLimits(m);

    // No open animation: the scrim is translucent and full screen, so each step of a fade (or of
    // a window slide above it) repaints all 1280x720 px from the background image up, ~100 ms
    // per frame on the Tab5. Scrim and window appear in one frame.
    if (s_toast) lv_obj_move_foreground(s_toast);
    return body;
}

lv_obj_t* modalFooter(const char* note)
{
    Modal* m = s_modal;
    if (!m) return nullptr;
    if (m->footer) return m->footer;
    lv_obj_t* f = lv_obj_create(m->win);
    lv_obj_remove_style_all(f);
    lv_obj_add_style(f, &s_foot, 0);
    lv_obj_set_size(f, LV_PCT(100), kFootH);
    lv_obj_remove_flag(f, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(f, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(f, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(f, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* n = lv_label_create(f);
    lv_label_set_text(n, note ? note : "");
    lv_obj_set_flex_grow(n, 1);   // .modal-footer span { margin-right: auto }
    m->footer = f;
    bodyLimits(m);
    return f;
}

void modalOnClose(void (*cb)(void* user), void* user)
{
    if (!s_modal) return;
    s_modal->on_close = cb;
    s_modal->user = user;
}

void modalClose()
{
    Modal* m = s_modal;
    if (!m) return;
    s_modal = nullptr;
    void (*cb)(void*) = m->on_close;
    void* user = m->user;
    m->on_close = nullptr;
    // Deleted asynchronously: modalClose() is usually called from an event of a child.
    lv_obj_delete_async(m->scrim);
    if (cb) cb(user);
}

bool modalIsOpen() { return s_modal != nullptr; }

bool overlayOpen()
{
    if (s_modal) return true;
    // Popup menus (selects), the keyboard sheet and modal scrims are full-screen clickable
    // catchers on the top layer (wk::overlayCreate); the toast is not clickable.
    lv_obj_t* top = lv_layer_top();
    if (!top) return false;
    const uint32_t n = lv_obj_get_child_count(top);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* ch = lv_obj_get_child(top, (int32_t)i);
        if (lv_obj_has_flag(ch, LV_OBJ_FLAG_CLICKABLE) && !lv_obj_has_flag(ch, LV_OBJ_FLAG_HIDDEN)) return true;
    }
    return false;
}

void toast(const char* text)
{
    if (!text) text = "";
    // Visible (not fading out) with the same key: new text only, the timer keeps running.
    if (s_toast && s_toast_timer && !lv_obj_has_flag(s_toast, LV_OBJ_FLAG_HIDDEN)) {
        const char* cur = lv_label_get_text(s_toast_lbl);
        if (cur && toastSameKey(cur, text)) {
            if (strcmp(cur, text) != 0) lv_label_set_text(s_toast_lbl, text);
            return;
        }
    }
    if (!s_toast) {
        // #toast: bottom 23 px, toast-bg, accent border, radius 8, padding 11 24, 180..660 px.
        s_toast = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_toast);
        lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_toast, c(col::toast_bg), 0);
        lv_obj_set_style_border_width(s_toast, 1, 0);
        lv_obj_set_style_border_color(s_toast, c(col::accent), 0);
        lv_obj_set_style_radius(s_toast, 8, 0);
        lv_obj_set_style_pad_hor(s_toast, 24, 0);
        lv_obj_set_style_pad_ver(s_toast, 11, 0);
        lv_obj_set_style_min_width(s_toast, 180, 0);
        lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -23);
        s_toast_lbl = lv_label_create(s_toast);
        lv_obj_set_style_text_font(s_toast_lbl, f_m16, 0);
        lv_obj_set_style_text_color(s_toast_lbl, c(col::toast_fg), 0);
        lv_obj_set_style_text_align(s_toast_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_max_width(s_toast_lbl, 660 - 48, 0);
        lv_obj_set_width(s_toast_lbl, LV_SIZE_CONTENT);
        lv_obj_center(s_toast_lbl);
        lv_obj_add_event_cb(s_toast, toastDeleted, LV_EVENT_DELETE, nullptr);
        lv_obj_set_style_opa(s_toast, 0, 0);
    }
    lv_label_set_text(s_toast_lbl, text);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    toastAnim(true);
    if (s_toast_timer) lv_timer_delete(s_toast_timer);
    s_toast_timer = lv_timer_create(toastTimerCb, strlen(text) < kToastShortLen ? kToastShortMs : kToastMs, nullptr);
    lv_timer_set_repeat_count(s_toast_timer, 1);
}

}  // namespace orbit::ui
