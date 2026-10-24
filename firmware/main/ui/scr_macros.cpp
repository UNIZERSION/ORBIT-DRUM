// Macro panel (#l-macros, right): VOLUME of the output that plays now (speaker / headphones / Bluetooth, each
// remembers its own level: App::setOutputVolume) and FX SEND / PAN of the selected pad as large white knobs
// with an 8-dot level column each, the transport row (REWIND = stop / again rewind, PLAY/PAUSE,
// REC) and the tempo footer (TAP, -, BPM, +; hold -/+ to repeat, long press the BPM to type it on
// the numeric pad).
// Refresh: knobs on revs.selection / params / pads, transport + tempo on revs.transport / project.
#include "ui/ui.h"

#include <stdlib.h>

#include "ui/theme.h"
#include "ui/ui_internal.h"
#include "ui/widgets.h"

namespace orbit::ui::macros {

namespace {

constexpr int kKnobD = 69;
constexpr int kKnobX = 64;                      // knob body, relative to the panel content
constexpr int kKnobY[3] = {34, 161, 287};
constexpr int kDots = 8, kDotD = 5, kDotPitch = 9;
constexpr int kDotsX = 167, kDotsW = 7;         // dot column right of the knob ring
// VOLUME is the device output level (0.3); the pad's own volume is the TRIM tab's PAD VOL knob.
ParamRef knobRef3(int k) { return k == 0 ? ParamRef::outVolume() : ParamRef::pad(k == 1 ? PP_SEND : PP_PAN); }
const char* const kCaps[3] = {"VOLUME", "FX SEND", "PAN"};
const uint32_t &kBtnBg = col::transport_btn, &kBtnBorder = col::transport_border, &kBtnInk = col::macro_ink;

lv_obj_t* s_panel = nullptr;
lv_obj_t* s_knob[3] = {};
lv_obj_t* s_dots[3] = {};
int s_lit[3] = {-1, -1, -1};
lv_obj_t* s_play = nullptr;
lv_obj_t* s_play_icon = nullptr;
lv_obj_t* s_rec = nullptr;
lv_obj_t* s_tap = nullptr;
lv_obj_t* s_bpm = nullptr;
lv_timer_t* s_tap_timer = nullptr;
uint32_t s_rev_sel = 0, s_rev_params = 0, s_rev_pads = 0, s_rev_transport = 0, s_rev_project = 0;
int s_drawn_playing = -1, s_drawn_rec = -1;
bool s_first = true;

// ---------------------------------------------------------------- LED dot columns
void dotsDrawCb(lv_event_t* e)
{
    lv_obj_t* o = (lv_obj_t*)lv_event_get_current_target(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    const int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (!layer || k < 0 || k > 2) return;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const Xf t = xfOf(o);   // maximized: larger dots, pitch along the scaled column
    const int lit = s_lit[k] < 0 ? 0 : s_lit[k];
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_opa = LV_OPA_COVER;
    const int32_t dot = t.F(kDotD);
    for (int i = 0; i < kDots; ++i) {   // i = 0 is the top dot; lit from the bottom up
        const bool on = (kDots - 1 - i) < lit;
        d.bg_color = c(on ? col::accent : col::macro_dot);
        const int32_t x1 = a.x1 + (lv_area_get_width(&a) - dot) / 2, y1 = t.Y((float)(i * kDotPitch));
        lv_area_t r = {x1, y1, x1 + dot - 1, y1 + dot - 1};
        lv_draw_rect(layer, &d, &r);
    }
}

// ---------------------------------------------------------------- buttons
// .l-transport button / .l-transport-footer button: #e3e5e6, 1 px #aeb3b4, radius 5, #3d3d3f;
// .active: accent background + border.
lv_obj_t* styledBtn(lv_obj_t* parent, const char* text, int x, int y, int w, int h)
{
    lv_obj_t* b = actionBtn(parent, text, false, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_pad_hor(b, 0, 0);
    lv_obj_set_style_bg_color(b, c(kBtnBg), 0);
    lv_obj_set_style_border_color(b, c(kBtnBorder), 0);
    lv_obj_set_style_text_color(b, c(kBtnInk), 0);
    lv_obj_set_style_text_font(b, f_m12, 0);
    lv_obj_set_style_text_letter_space(b, 0, 0);
    lv_obj_set_style_radius(b, 5, 0);
    lv_obj_set_style_bg_color(b, c(col::accent), LV_STATE_CHECKED);
    lv_obj_set_style_border_color(b, c(col::accent), LV_STATE_CHECKED);
    lv_obj_set_style_text_color(b, c(col::macro_ink_on), LV_STATE_CHECKED);
    return b;
}

// Icon ink of a transport button for its state (an active button is the accent: col::macro_ink_on).
void iconInk(lv_obj_t* b, bool on)
{
    const uint32_t n = b ? lv_obj_get_child_count(b) : 0;
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* ch = lv_obj_get_child(b, (int32_t)i);
        if (lv_obj_check_type(ch, &lv_image_class)) lv_obj_set_style_image_recolor(ch, c(on ? col::macro_ink_on : kBtnInk), 0);
    }
}

lv_obj_t* iconButton(lv_obj_t* parent, Icon icon, int x, int y, int w, int h, lv_obj_t** icon_out)
{
    lv_obj_t* b = styledBtn(parent, "", x, y, w, h);
    lv_obj_t* ic = iconCreate(b, icon, 20, kBtnInk);
    lv_obj_center(ic);
    if (icon_out) *icon_out = ic;
    return b;
}

void rewindCb(lv_event_t*) { app().stop(); }
void playCb(lv_event_t*) { app().togglePlay(); }
void recCb(lv_event_t*) { app().toggleRecord(); }
void minusRep(void*) { app().bpmStep(-1); }
void plusRep(void*) { app().bpmStep(+1); }

void tapOff(lv_timer_t*)
{
    s_tap_timer = nullptr;
    btnSetActive(s_tap, false);
}

void tapCb(lv_event_t*)
{
    app().tap();   // on PRESSED: the tempo follows the finger, not the release
    btnSetActive(s_tap, true);
    if (s_tap_timer) lv_timer_delete(s_tap_timer);
    s_tap_timer = lv_timer_create(tapOff, 130, nullptr);
    lv_timer_set_repeat_count(s_tap_timer, 1);
}

void bpmTyped(const char* text, void*)
{
    if (!text) return;
    char* end = nullptr;
    const double v = strtod(text, &end);
    while (end && (*end == ' ')) ++end;
    if (!end || end == text || *end != 0 || !(v >= 30.0 && v <= 300.0)) {
        app().toast("Invalid tempo: enter 30–300 BPM");
        return;
    }
    app().setBpm((float)v);
}

void bpmLongCb(lv_event_t*)
{
    char cur[16];
    formatBpm(cur, sizeof cur, app().project().bpm, false);
    keyboardOpenNumeric("TEMPO (BPM)", cur, 6, bpmTyped, nullptr);
}

lv_obj_t* placeKnob(lv_obj_t* panel, int k)
{
    lv_obj_t* w = knobCreate(panel, KnobLook::White, kKnobD, knobRef3(k), kCaps[k]);
    if (!w) return nullptr;
    lv_obj_update_layout(w);
    lv_obj_t* body = lv_obj_get_child(w, 1);
    const int32_t bx = body ? lv_obj_get_x(body) : 0, by = body ? lv_obj_get_y(body) : 0;
    lv_obj_set_pos(w, kKnobX - bx, kKnobY[k] - by);
    return w;
}

int litCount(int k)
{
    const float n = app().paramNorm(knobRef3(k));
    int lit = (int)(n * (float)kDots + 0.5f);
    return lit < 0 ? 0 : (lit > kDots ? kDots : lit);
}

}  // namespace

lv_obj_t* create(lv_obj_t* scr)
{
    lv_obj_t* panel = panelCreate(scr, geo::macros_x, geo::macros_y, geo::macros_w, geo::macros_h);
    if (bg::shadowsBaked()) lv_obj_set_style_shadow_width(panel, 0, 0);
    s_panel = panel;
    panelTopHighlight(panel, geo::macros_w);
    lv_obj_set_pos(panelMaxButton(panel, Panel::Macros), geo::macros_w - 32, 4);   // 0.3: maximize, above the dots

    for (int k = 0; k < 3; ++k) {
        s_knob[k] = placeKnob(panel, k);
        lv_obj_t* d = lv_obj_create(panel);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, kDotsW, kDots * kDotPitch - (kDotPitch - kDotD));
        lv_obj_set_pos(d, kDotsX, kKnobY[k] + kKnobD / 2 - (kDots * kDotPitch - (kDotPitch - kDotD)) / 2);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(d, dotsDrawCb, LV_EVENT_DRAW_MAIN, (void*)(intptr_t)k);
        s_dots[k] = d;
        s_lit[k] = -1;
    }

    // .l-transport: top line #c2c4c6, margin 0 11
    lv_obj_t* line = lv_obj_create(panel);
    lv_obj_remove_style_all(line);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(line, c(col::macro_line), 0);
    lv_obj_set_pos(line, 11, 393);
    lv_obj_set_size(line, 176, 1);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* rew = iconButton(panel, Icon::Rewind, 11, 400, 54, 33, nullptr);
    lv_obj_add_event_cb(rew, rewindCb, LV_EVENT_CLICKED, nullptr);
    s_play = iconButton(panel, Icon::Play, 72, 400, 54, 33, &s_play_icon);
    lv_obj_add_event_cb(s_play, playCb, LV_EVENT_CLICKED, nullptr);
    s_rec = iconButton(panel, Icon::Record, 133, 400, 54, 33, nullptr);
    lv_obj_add_event_cb(s_rec, recCb, LV_EVENT_CLICKED, nullptr);

    // .l-transport-footer: TAP, -, BPM (bold), +. 30 px tall (the prototype's 23 px is ~2 mm on
    // the 294 ppi panel); the panel content ends at y 483, so the row fits below the transport.
    // Touch targets are trimmed per side (setHitBox) so nothing overlaps: the BPM value (long
    // press: tempo pad) sits flush between - and +, whose hold-to-repeat must win up to its edge.
    constexpr int kFootY = 436, kFootH = 30, kFootW = 47, kBpmH = 18;
    // BPM label first, so even an overlapping later sibling (-/+) would win the shared edge.
    s_bpm = label(panel, "120", f_s13, col::ink);
    lv_obj_set_style_text_align(s_bpm, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_bpm, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_size(s_bpm, 29, kBpmH);
    lv_obj_set_pos(s_bpm, 111, kFootY + (kFootH - kBpmH) / 2);
    lv_obj_add_flag(s_bpm, LV_OBJ_FLAG_CLICKABLE);
    setHitBox(s_bpm, 0, 8, 0, 16);   // the footer row's height (434..475), not wider than the label
    lv_obj_add_event_cb(s_bpm, bpmLongCb, LV_EVENT_LONG_PRESSED, nullptr);
    s_tap = styledBtn(panel, "TAP", 11, kFootY, kFootW, kFootH);
    setHitBox(s_tap, 6, 2, 3, 10);
    lv_obj_add_event_cb(s_tap, tapCb, LV_EVENT_PRESSED, nullptr);
    lv_obj_t* minus = styledBtn(panel, "\xE2\x80\x93", 64, kFootY, kFootW, kFootH);
    setHitBox(minus, 3, 2, 0, 10);
    btnRepeat(minus, minusRep, nullptr);
    lv_obj_t* plus = styledBtn(panel, "+", 140, kFootY, kFootW, kFootH);
    setHitBox(plus, 0, 2, 6, 10);
    btnRepeat(plus, plusRep, nullptr);

    s_first = true;
    s_drawn_playing = s_drawn_rec = -1;
    refresh();
    return panel;
}

void refresh()
{
    if (!s_panel) return;
    App& a = app();
    const Revs& r = a.revs();
    if (s_first || r.selection != s_rev_sel || r.params != s_rev_params || r.pads != s_rev_pads) {
        s_rev_sel = r.selection;
        s_rev_params = r.params;
        s_rev_pads = r.pads;
        for (int k = 0; k < 3; ++k) {
            if (s_knob[k]) knobRefresh(s_knob[k]);
            const int lit = litCount(k);
            if (lit != s_lit[k]) {
                s_lit[k] = lit;
                lv_obj_invalidate(s_dots[k]);
            }
        }
    }
    if (s_first || r.transport != s_rev_transport || r.project != s_rev_project) {
        s_rev_transport = r.transport;
        s_rev_project = r.project;
        char buf[16];
        formatBpm(buf, sizeof buf, a.project().bpm, false);
        // the 29 px column holds "300"; a fractional tempo shows rounded here (exact in the top bar)
        if (strchr(buf, '.')) snprintf(buf, sizeof buf, "%d", (int)floorf(a.project().bpm + 0.5f));
        labelSet(s_bpm, buf);
        const int playing = a.playing() ? 1 : 0;
        if (playing != s_drawn_playing) {
            s_drawn_playing = playing;
            btnSetActive(s_play, playing != 0);
            iconInk(s_play, playing != 0);
            if (s_play_icon) iconSet(s_play_icon, playing ? Icon::Pause : Icon::Play, 20);
        }
        const int rec = a.recording() ? 1 : 0;
        if (rec != s_drawn_rec) {
            s_drawn_rec = rec;
            btnSetActive(s_rec, rec != 0);
            iconInk(s_rec, rec != 0);
        }
    }
    s_first = false;
}

}  // namespace orbit::ui::macros
