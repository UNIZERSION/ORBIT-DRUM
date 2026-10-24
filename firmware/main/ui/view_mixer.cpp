// MIXER view (work area): 8 bus strips + MASTER, as the prototype's .l-work-mixer.
//   head: mixer icon "8-CHANNEL MIXER", overline "TOUCH & DRAG FADERS".
//   strip: bus name, segmented meter (Engine telemetry, ~30 Hz) + fader (bus level, double tap:
//          default level), dB readout, M / S buttons (lit when on; 28x30 with a 3 px gap and 1 px
//          hit margins, so neither steals the other's taps), bus PAN slider (horizontal drag,
//          centre detent, double tap: centre; the readout shows the pan while dragging).
//          MASTER (#d4d7d8): master level, L = limiter, C = metronome click, no pan.
// Geometry from reference/geometry.json (view-mixer); positions below are screen px.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace orbit::ui::view_mixer {

namespace {

constexpr int kStrips = kBuses + 1;   // + MASTER
constexpr int kStripY = 118, kStripW = 73, kStripH = 401;
constexpr int kPairY = 151, kPairH = 282;
// M / S: 28x30 at +7 / +38 (gap 3, ext 1 each: the hit areas never overlap), y 455..484.
constexpr int kBtnW = 28, kBtnH = 30, kBtnY = 455, kBtn1X = 7, kBtn2X = 38, kBtnHit = 1;
// Bus PAN: 58x24 at +7, y 489..512 (ext 2 -> 487..514, inside the strip that ends at 518).
constexpr int kPanX = 7, kPanY = 489, kPanW = 58, kPanH = 24, kPanHit = 2;
constexpr int kPanThumbW = 8, kPanThumbH = 14, kPanTrackH = 3;
constexpr int kPanTravelPx = 2 * (kPanW - kPanThumbW);   // drag px for the full L..R range
constexpr float kPanDetent = 0.04f;                       // |pan| below this snaps to centre
constexpr int kTapMovePx = 5;                             // more movement: not a tap
constexpr uint32_t kTapMs = 350, kDoubleTapMs = 400;

inline int stripX(int i) { return 328 + (i * 719 + 4) / 9; }   // 9 strips over 712 px, gap ~7

struct Strip {
    lv_obj_t* title;
    lv_obj_t* meter;
    lv_obj_t* fader;
    lv_obj_t* db;
    lv_obj_t* b1;   // M (master: L)
    lv_obj_t* b2;   // S (master: C)
    lv_obj_t* pan;  // bus pan slider (nullptr on MASTER)
    lv_obj_t* pan_fill;
    lv_obj_t* pan_thumb;
    int pan_drawn;  // thumb x drawn (-1 unknown)
};

// Tap timing of a fader / pan slider (double tap resets, Settings > Touch > Double tap reset).
struct Taps {
    uint32_t press_ms;
    uint32_t last_tap_ms;
    int32_t travel;   // |movement| px of this press
};

struct View {
    lv_obj_t* root = nullptr;
    int ox = geo::work_x + 1, oy = geo::work_y + 1;
    Strip s[kStrips] = {};
    Taps fader_taps[kStrips] = {};
    Taps pan_taps[kBuses] = {};
    float pan_drag = 0.f;   // raw drag position 0..1 (before the centre detent)
    int pan_active = -1;    // strip whose pan is being dragged (its readout shows the pan)
    Revs drawn{};
    bool drawn_valid = false;
    uint32_t last_meter_ms = 0;
};
View v;

void place(lv_obj_t* o, int x, int y) { lv_obj_set_pos(o, x - v.ox, y - v.oy); }
void updateStrips(bool names);

// Press / move / release bookkeeping shared by faders and pan sliders; true on the second short
// tap within kDoubleTapMs.
bool tapTrack(Taps& t, lv_event_code_t code)
{
    const uint32_t now = lv_tick_get();
    if (code == LV_EVENT_PRESSED) {
        t.press_ms = now;
        t.travel = 0;
    } else if (code == LV_EVENT_PRESSING) {
        lv_indev_t* indev = lv_indev_active();
        if (!indev) return false;
        lv_point_t d;
        lv_indev_get_vect(indev, &d);
        t.travel += (d.x < 0 ? -d.x : d.x) + (d.y < 0 ? -d.y : d.y);
    } else if (code == LV_EVENT_RELEASED) {
        if (t.travel <= kTapMovePx && now - t.press_ms < kTapMs) {
            if (t.last_tap_ms && now - t.last_tap_ms < kDoubleTapMs) {
                t.last_tap_ms = 0;
                return app().settings().double_tap_reset != 0;
            }
            t.last_tap_ms = now;
        } else {
            t.last_tap_ms = 0;
        }
    } else if (code == LV_EVENT_PRESS_LOST) {
        t.last_tap_ms = 0;
    }
    return false;
}

// Fader: double tap -> default level (bus 0 dB, master default). The widget's own handler drags.
void onFaderTap(lv_event_t* e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= kStrips) return;
    if (tapTrack(v.fader_taps[i], lv_event_get_code(e))) {
        app().paramReset(ParamRef::busLevel(i));
        faderRefresh(v.s[i].fader);
    }
}

// ---- bus pan slider
void panPlace(int i)
{
    Strip& st = v.s[i];
    if (!st.pan) return;
    const float n = app().paramNorm(ParamRef::busPan(i));
    const int span = kPanW - kPanThumbW;
    int tx = (int)(n * (float)span + 0.5f);
    tx = tx < 0 ? 0 : (tx > span ? span : tx);
    if (tx == st.pan_drawn) return;
    st.pan_drawn = tx;
    lv_obj_set_x(st.pan_thumb, uiSx(st.pan_thumb, tx));   // design px -> the (maybe maximized) panel
    const int mid = span / 2 + kPanThumbW / 2, cx = tx + kPanThumbW / 2;
    const int x1 = cx < mid ? cx : mid, x2 = cx < mid ? mid : cx;
    lv_obj_set_x(st.pan_fill, uiSx(st.pan_fill, x1));
    const int fw = uiSx(st.pan_fill, x2 - x1 + 1);
    lv_obj_set_width(st.pan_fill, fw > 0 ? fw : 1);
}

void onPan(lv_event_t* e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= kBuses) return;
    const lv_event_code_t code = lv_event_get_code(e);
    const ParamRef ref = ParamRef::busPan(i);
    const bool dbl = tapTrack(v.pan_taps[i], code);
    if (code == LV_EVENT_PRESSED) {
        v.pan_drag = app().paramNorm(ref);
        v.pan_active = i;
        updateStrips(false);   // readout -> pan
    } else if (code == LV_EVENT_PRESSING) {
        lv_indev_t* indev = lv_indev_active();
        if (!indev) return;
        lv_point_t d;
        lv_indev_get_vect(indev, &d);
        if (d.x == 0) return;
        v.pan_drag += (float)d.x / ((float)kPanTravelPx * uiScaleOf(v.root).sx);   // travel as drawn (maximize)
        v.pan_drag = v.pan_drag < 0.f ? 0.f : (v.pan_drag > 1.f ? 1.f : v.pan_drag);
        float pan = v.pan_drag * 2.f - 1.f;
        if (pan > -kPanDetent && pan < kPanDetent) pan = 0.f;
        app().paramSet(ref, pan);
        panPlace(i);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (dbl) app().paramReset(ref);
        v.pan_active = -1;
        panPlace(i);
        updateStrips(false);   // readout back to dB
    }
}

lv_obj_t* panBox(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, c(color), 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

void buildPan(int i, int x)
{
    Strip& st = v.s[i];
    lv_obj_t* p = lv_obj_create(v.root);
    lv_obj_remove_style_all(p);
    place(p, x + kPanX, kPanY);
    lv_obj_set_size(p, kPanW, kPanH);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_ext_click_area(p, kPanHit);
    const int ty = (kPanH - kPanTrackH) / 2;
    panBox(p, 0, ty, kPanW, kPanTrackH, col::meter_bg, 1);                         // track
    st.pan_fill = panBox(p, kPanW / 2, ty, 1, kPanTrackH, col::accent, 1);          // centre -> pan
    panBox(p, (kPanW - kPanThumbW) / 2 + kPanThumbW / 2, ty - 4, 1, kPanTrackH + 8, col::muted, 0);   // centre tick
    st.pan_thumb = panBox(p, (kPanW - kPanThumbW) / 2, (kPanH - kPanThumbH) / 2, kPanThumbW, kPanThumbH, col::pan_thumb, 3);
    st.pan = p;
    st.pan_drawn = -1;
    lv_obj_add_event_cb(p, onPan, LV_EVENT_ALL, (void*)(intptr_t)i);
    panPlace(i);
}

void onMute(lv_event_t* e) { app().toggleBusMute((int)(intptr_t)lv_event_get_user_data(e)); }
void onSolo(lv_event_t* e) { app().toggleBusSolo((int)(intptr_t)lv_event_get_user_data(e)); }
void onLimiter(lv_event_t*)
{
    const bool on = !app().project().limiter_on;
    app().setLimiter(on);
    toast(on ? "Limiter: on" : "Limiter: off");
}
void onClick(lv_event_t*)
{
    const bool on = !app().project().metronome;
    app().setMetronome(on);
    toast(on ? "Metronome: on" : "Metronome: off");
}

// Centred fixed-width label inside a strip.
lv_obj_t* stripLabel(int x, int y, const char* text, const lv_font_t* f, uint32_t color)
{
    lv_obj_t* l = label(v.root, text, f, color);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_size(l, kStripW - 4, lv_font_get_line_height(f));   // fixed box: updates never relayout
    place(l, x + 2, y);
    return l;
}

void buildStrip(int i)
{
    const bool master = i == kBuses;
    const int x = stripX(i);
    Strip& st = v.s[i];
    lv_obj_t* bg = lv_obj_create(v.root);
    lv_obj_remove_style_all(bg);
    place(bg, x, kStripY);
    lv_obj_set_size(bg, kStripW, kStripH);
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bg, c(master ? col::master_bg : col::mixer_bg), 0);
    lv_obj_set_style_border_width(bg, 1, 0);
    lv_obj_set_style_border_color(bg, c(col::line), 0);
    lv_obj_set_style_radius(bg, 6, 0);
    lv_obj_remove_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

    // .strip-title 10 px / 400 -> 13 px regular, top 132
    st.title = stripLabel(x, 130, master ? "MASTER" : app().project().bus[i].name, f_r13, col::ink);
    // .strip-pair: meter 8 px at +17.5, fader 24 px at +31.5
    st.meter = meterCreate(v.root, 8, kPairH);
    place(st.meter, x + 17, kPairY);
    st.fader = faderCreate(v.root, 24, kPairH, ParamRef::busLevel(i));
    place(st.fader, x + 31, kPairY);
    if (st.fader) lv_obj_add_event_cb(st.fader, onFaderTap, LV_EVENT_ALL, (void*)(intptr_t)i);
    // .strip-num 10 px muted -> 12 px, top 438
    st.db = stripLabel(x, 437, "", f_r12, col::muted);
    // .strip-btns: the prototype's 25x24 micro buttons at +10 / +38 had a 3 px gap under 8 px hit
    // margins (S took the right 5 px of M); bigger buttons with 1 px margins instead.
    st.b1 = microBtn(v.root, master ? "L" : "M", kBtnW, kBtnH);
    place(st.b1, x + kBtn1X, kBtnY);
    setHit(st.b1, kBtnHit);
    st.b2 = microBtn(v.root, master ? "C" : "S", kBtnW, kBtnH);
    place(st.b2, x + kBtn2X, kBtnY);
    setHit(st.b2, kBtnHit);
    if (!master) buildPan(i, x);
    if (master) {
        lv_obj_add_event_cb(st.b1, onLimiter, LV_EVENT_CLICKED, nullptr);
        lv_obj_add_event_cb(st.b2, onClick, LV_EVENT_CLICKED, nullptr);
    } else {
        lv_obj_add_event_cb(st.b1, onMute, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_add_event_cb(st.b2, onSolo, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
}

void updateStrips(bool names)
{
    const Project& p = app().project();
    char buf[24];
    for (int i = 0; i < kStrips; ++i) {
        Strip& st = v.s[i];
        const bool master = i == kBuses;
        if (names && !master) {
            if (strcmp(lv_label_get_text(st.title), p.bus[i].name) != 0) lv_label_set_text(st.title, p.bus[i].name);
        }
        faderRefresh(st.fader);
        if (i == v.pan_active) app().paramText(ParamRef::busPan(i), buf, sizeof buf);   // while dragging PAN
        else app().paramText(ParamRef::busLevel(i), buf, sizeof buf);
        if (strcmp(lv_label_get_text(st.db), buf) != 0) lv_label_set_text(st.db, buf);
        if (!master) panPlace(i);
        if (master) {
            btnSetActive(st.b1, p.limiter_on != 0);
            btnSetActive(st.b2, p.metronome != 0);
        } else {
            btnSetActive(st.b1, p.bus[i].mute != 0);
            btnSetActive(st.b2, p.bus[i].solo != 0);
        }
    }
}

void updateMeters()
{
    const Telemetry& t = app().engine().telemetry();
    const bool on = app().settings().meters != 0;
    for (int i = 0; i < kBuses; ++i) meterSet(v.s[i].meter, on ? t.bus_peak[i].load(std::memory_order_relaxed) : 0.f);
    const float l = t.master_peak[0].load(std::memory_order_relaxed), r = t.master_peak[1].load(std::memory_order_relaxed);
    meterSet(v.s[kBuses].meter, on ? (l > r ? l : r) : 0.f);
}

}  // namespace

lv_obj_t* create(lv_obj_t* work)
{
    v = View{};
    v.root = lv_obj_create(work);
    lv_obj_remove_style_all(v.root);
    lv_obj_set_pos(v.root, 0, 0);
    lv_obj_set_size(v.root, geo::work_w - 2, geo::work_h - 2);
    lv_obj_remove_flag(v.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(v.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_update_layout(v.root);
    lv_area_t ra;
    lv_obj_get_coords(v.root, &ra);
    if (ra.x1 > 0 || ra.y1 > 0) {
        v.ox = ra.x1;
        v.oy = ra.y1;
    }

    // ---- head (.panel-head inside .l-work-mixer: padding 8 16, title at 328,87)
    lv_obj_t* ic = iconCreate(v.root, Icon::Mixer, 18, col::ink);
    place(ic, 328, 87);
    lv_obj_t* t = lv_label_create(v.root);
    lv_label_set_text(t, "8-CHANNEL MIXER");
    lv_obj_add_style(t, &styles().panel_title, 0);
    place(t, 356, 88);
    lv_obj_t* ov = overline(v.root, "TOUCH & DRAG FADERS");
    lv_obj_set_width(ov, 200);
    lv_obj_set_style_text_font(ov, f_s13, 0);   // 13 px caption (1.2x rule): the box has room
    lv_obj_set_style_text_letter_space(ov, ls(13, .13f), 0);
    lv_obj_set_style_text_align(ov, LV_TEXT_ALIGN_RIGHT, 0);
    place(ov, 1040 - 200, 89);

    for (int i = 0; i < kStrips; ++i) buildStrip(i);
    v.drawn_valid = false;
    refresh();
    return v.root;
}

void refresh()
{
    if (!v.root) return;
    const Revs& r = app().revs();
    const bool all = !v.drawn_valid;
    const bool names = all || r.project != v.drawn.project;
    if (names || r.mixer != v.drawn.mixer || r.params != v.drawn.params || r.fx != v.drawn.fx ||
        r.transport != v.drawn.transport)
        updateStrips(names);
    v.drawn = r;
    v.drawn_valid = true;
    const uint32_t now = lv_tick_get();
    if (now - v.last_meter_ms >= 33) {   // meters ~30 Hz
        v.last_meter_ms = now;
        updateMeters();
    }
}

void onShow()
{
    v.drawn_valid = false;
    refresh();
}

}  // namespace orbit::ui::view_mixer
