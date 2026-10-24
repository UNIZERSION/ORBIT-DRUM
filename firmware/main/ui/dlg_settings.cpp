// SETTINGS dialog (prototype "SETTINGS / TAB5": 175 px section list + settings body, footer with
// a note and APPLY). Every control edits app().settingsEdit() (a copy); APPLY calls
// App::applySettings() (hardware + settings file/NVS) plus the project-level items (name, launch
// quantization, the project's limiter) and closes; any other way of closing discards the copy.
// Brightness and master volume preview live while dragging and are restored on discard.
//
// Unsaved edits are guarded: a tap on the scrim never discards them (a toast points to APPLY / X),
// and X, ESC and OPEN UI GUIDE need a second tap within 3 s. Renaming the project to the name of
// another saved project needs a second APPLY (its file would be overwritten by the next SAVE).
//
// The Bluetooth section is the exception: it acts at once through App::bt* (on / off, connect, scan, output,
// volume, AirPods mode), keeps nothing in the APPLY copy and redraws on revs.bt only.
//
// Sections are built lazily the first time they are shown and then only shown/hidden. The
// window has one fixed height for every section (the modal maximum), so it never moves or resizes
// under the finger when the section changes; longer sections scroll.
//
// Built for size (-Os): a dialog of LVGL calls, nothing here is timing-critical (the 4 MB image limit).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("Os")
#endif
#include <stdio.h>
#include <string.h>

#include "core/config.h"
#include "core/project.h"
#include "core/settings.h"
#include "hal/hal.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/widgets.h"

#ifndef ORBIT_VERSION
#define ORBIT_VERSION "0.1"
#endif
#ifdef ORBIT_BUILD_DATE
static const char kBuildDate[] = ORBIT_BUILD_DATE;
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdate-time"
static const char kBuildDate[] = __DATE__;
#pragma GCC diagnostic pop
#endif

namespace orbit::ui {

namespace dlg_settings {
namespace {

constexpr int kTabs = 8;
const char* const kTabNames[kTabs] = {"General", "Audio", "MIDI", "Bluetooth", "Display", "Touch", "Storage", "About"};
static_assert(kTabBluetooth == 3, "Bluetooth tab index (ui.h)");
constexpr int kWinW = 824, kWinMaxH = 650, kHeadH = 56, kFootH = 58;
constexpr int kNavW = 175;
constexpr int kPaneW = kWinW - 2 - kNavW;                    // 647
constexpr int kShellMax = kWinMaxH - kHeadH - kFootH - 2;    // 534: every section (prototype min 370)
constexpr int kCtrlW = 174;                                  // .ui-input / .ui-select
// --settings-nav #dadcde rounds to a visibly magenta RGB565 grey (222,219,222); LIGHT's nav_fill #deddde
// gives the neutral (222,223,222) on the panel.
const uint32_t& kNavBg = col::nav_fill;

// The colours a Settings copy asks for (CUSTOM: + hue / saturation), comparable with the ones on screen.
struct Look {
    int base = 0, theme = 0, hue = 0, sat = 0;
    bool accentLike(const Look& o) const { return theme == o.theme && hue == o.hue && sat == o.sat; }
    bool operator==(const Look& o) const { return base == o.base && accentLike(o); }
};
// CUSTOM hue / saturation as themeSelect() uses them (saturation 0 = never set: the default).
int customHue(int raw) { return raw < 360 ? raw : 359; }
int customSat(int raw) { return raw == 0 ? kAccentSatDefault : (raw < kAccentSatMin ? kAccentSatMin : (raw > 100 ? 100 : raw)); }
Look lookOf(const Settings& s)
{
    Look l;
    l.base = s.base < kBaseCount ? s.base : 0;
    l.theme = s.theme <= kThemeCustom ? s.theme : 0;
    if (l.theme == kThemeCustom) {
        l.hue = customHue(s.accent_hue);
        l.sat = customSat(s.accent_sat);
    }
    return l;
}
Look lookOnScreen()
{
    Look l;
    l.base = baseIndex();
    l.theme = themeIndex();
    l.hue = themeHue();
    l.sat = themeSat();
    return l;
}

// ---------------------------------------------------------------- state
struct Dlg {
    bool open = false;
    bool applied = false;
    bool live_bright = false;
    bool edited = false;          // any control changed the edit copy / name / quantization / limiter
    bool discard_armed = false;   // X / ESC / OPEN UI GUIDE tapped once with unsaved edits
    uint32_t discard_ms = 0;
    bool rename_armed = false;    // APPLY tapped once with a name that another project already has
    uint32_t rename_ms = 0;
    char rename_name[32] = {};
    int limiter = -1;             // Audio > Master limiter: the project's limiter (-1: tab not built)
    int limiter0 = -1;
    int tab = 0;
    lv_obj_t* shell = nullptr;
    lv_obj_t* nav = nullptr;
    lv_obj_t* links[kTabs] = {};
    lv_obj_t* panes[kTabs] = {};
    // General (project-level, applied with APPLY)
    char name[32] = {};
    char name0[32] = {};
    int quant = 0, quant0 = 0;
    // live parts
    lv_obj_t* buf_note = nullptr;
    lv_obj_t* rate_note = nullptr;   // Audio > Sample rate: says when Bluetooth holds 44.1 kHz
    lv_obj_t* midi_status = nullptr;
    lv_obj_t* learn_btn = nullptr;
    lv_obj_t* map_note = nullptr;
    uint32_t rev_midi = 0;
    bool clear_armed = false;
    uint32_t clear_ms = 0;
    lv_obj_t* vel_note = nullptr;
    lv_obj_t* sd_status = nullptr;
    lv_obj_t* live_cpu = nullptr;
    lv_obj_t* live_mem = nullptr;
    lv_obj_t* live_diag = nullptr;   // About: worst-case timings of the device (hal::diagnostics)
    lv_obj_t* voice_note = nullptr;  // Audio > Voice limit: what the CPU governor allows now
    // Bluetooth (acts at once through the App, like START LEARN; refreshed on revs.bt)
    lv_obj_t* bt_toggle = nullptr;
    lv_obj_t* bt_module = nullptr;   // status value (right)
    lv_obj_t* bt_note = nullptr;     // notice / hint under "Module"
    lv_obj_t* bt_scan = nullptr;
    lv_obj_t* bt_list = nullptr;
    lv_obj_t* bt_pods = nullptr;     // AirPods row (hidden unless BtStatus::airpods)
    lv_obj_t* bt_pods_note = nullptr;
    lv_obj_t* bt_pods_sel = nullptr;
    uint32_t rev_bt = 0;
    // Display > Base color / Accent color (+ the CUSTOM hue / saturation row)
    lv_obj_t* base_sw[kBaseCount] = {};
    lv_obj_t* base_note = nullptr;
    lv_obj_t* theme_sw[kThemeCount + 1] = {};   // the presets, then CUSTOM
    lv_obj_t* theme_note = nullptr;
    lv_obj_t* custom_row = nullptr;   // shown while CUSTOM is chosen
    lv_obj_t* custom_note = nullptr;
    lv_obj_t* hue_sl = nullptr;
    lv_obj_t* sat_sl = nullptr;
    lv_obj_t* preview = nullptr;      // the CUSTOM accent as a fill with its text colour
    lv_obj_t* preview_txt = nullptr;
    Look look0;                       // the Settings colours when the dialog opened
    uint32_t bt_list_key = 0;        // hash of what the device list shows (rebuilt only when it changes)
    lv_obj_t* foot_note = nullptr;   // footer note: says that Bluetooth acts at once on its tab
    bool learn_shown = false;
    uint32_t live_ms = 0;
    bool live_init = false;
    lv_timer_t* timer = nullptr;
};
Dlg d;

lv_style_t s_link, s_link_on, s_link_pr, s_row, s_mono_box, s_dev, s_dev_pr, s_dev_on, s_tag, s_sb;
bool s_styles = false;

void initStyles()
{
    if (s_styles) return;
    s_styles = true;
    // .settings-link: 39 px, padding 0 17, radius 4, muted; .on accent / on-accent; :hover hover.
    lv_style_init(&s_link);
    lv_style_set_radius(&s_link, 4);
    lv_style_set_bg_opa(&s_link, LV_OPA_TRANSP);
    lv_style_set_bg_color(&s_link, c(col::hover));
    lv_style_set_text_color(&s_link, c(col::muted));
    lv_style_set_text_font(&s_link, f_m16);
    lv_style_set_pad_left(&s_link, 17);
    lv_style_init(&s_link_on);
    lv_style_set_bg_opa(&s_link_on, LV_OPA_COVER);
    lv_style_set_bg_color(&s_link_on, c(col::accent));
    lv_style_set_text_color(&s_link_on, c(col::on_accent));
    lv_style_init(&s_link_pr);
    lv_style_set_bg_opa(&s_link_pr, LV_OPA_COVER);
    lv_style_set_text_color(&s_link_pr, c(col::ink));
    // .setting-row: bottom line, padding 12 0 (10 here: taller 1.2x text), min-height 55, gap 20.
    lv_style_init(&s_row);
    lv_style_set_border_side(&s_row, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_width(&s_row, 1);
    lv_style_set_border_color(&s_row, c(col::line));
    lv_style_set_pad_ver(&s_row, 10);
    lv_style_set_pad_column(&s_row, 20);
    lv_style_set_min_height(&s_row, 55);
    // .about-copy: border line, radius 6, padding 16, line-height 1.6.
    lv_style_init(&s_mono_box);
    lv_style_set_border_width(&s_mono_box, 1);
    lv_style_set_border_color(&s_mono_box, c(col::line));
    lv_style_set_radius(&s_mono_box, 6);
    lv_style_set_pad_all(&s_mono_box, 16);
    lv_style_set_pad_row(&s_mono_box, 6);
    lv_style_set_text_line_space(&s_mono_box, 6);
    // Bluetooth device row (.file-item look): 40 px, bottom line, pressed hover, connected active-bg.
    lv_style_init(&s_dev);
    lv_style_set_border_side(&s_dev, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_width(&s_dev, 1);
    lv_style_set_border_color(&s_dev, c(col::cat_line));
    lv_style_set_pad_hor(&s_dev, 10);
    lv_style_set_pad_column(&s_dev, 8);
    lv_style_set_bg_color(&s_dev, c(col::hover));
    lv_style_init(&s_dev_pr);
    lv_style_set_bg_opa(&s_dev_pr, LV_OPA_COVER);
    lv_style_init(&s_dev_on);
    lv_style_set_bg_opa(&s_dev_on, LV_OPA_COVER);
    lv_style_set_bg_color(&s_dev_on, c(col::active_bg));
    // SAVED / SELECTED / CONNECTED tags: 11 px caps in a 1 px box.
    lv_style_init(&s_tag);
    lv_style_set_text_font(&s_tag, f_m12);
    lv_style_set_text_letter_space(&s_tag, 1);
    lv_style_set_border_width(&s_tag, 1);
    lv_style_set_radius(&s_tag, 4);
    lv_style_set_pad_hor(&s_tag, 6);
    lv_style_set_pad_ver(&s_tag, 2);
    // Section scrollbar (only while a section is taller than the window, e.g. Bluetooth with a long device
    // list): 4 px, muted, in the right padding.
    lv_style_init(&s_sb);
    lv_style_set_width(&s_sb, 4);
    lv_style_set_radius(&s_sb, 2);
    lv_style_set_bg_opa(&s_sb, LV_OPA_40);
    lv_style_set_bg_color(&s_sb, c(col::muted));
    lv_style_set_pad_right(&s_sb, 8);
    lv_style_set_pad_ver(&s_sb, 10);
}

// ---------------------------------------------------------------- helpers
void setText(lv_obj_t* l, const char* t)
{
    if (!l || !t) return;
    const char* cur = lv_label_get_text(l);
    if (cur && strcmp(cur, t) == 0) return;
    lv_label_set_text(l, t);
}

lv_obj_t* plain(lv_obj_t* parent)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

Settings& E() { return app().settingsEdit(); }

// Section body: h3 + muted intro + rows (flex column).
lv_obj_t* paneCreate(int tab, const char* title, const char* intro)
{
    lv_obj_t* p = lv_obj_create(d.shell);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, kNavW, 0);
    lv_obj_set_size(p, kPaneW, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(p, 26, 0);
    lv_obj_set_style_pad_top(p, 18, 0);
    lv_obj_set_style_pad_bottom(p, 18, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_style(p, &s_sb, LV_PART_SCROLLBAR);   // remove_style_all took the theme's: shows that it scrolls
    lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* h = label(p, title, f_m24, col::ink);
    lv_obj_set_style_pad_top(h, 2, 0);
    lv_obj_set_style_pad_bottom(h, 5, 0);
    lv_obj_t* in = label(p, intro, f_r14, col::muted);
    lv_obj_set_width(in, LV_PCT(100));
    lv_label_set_long_mode(in, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_bottom(in, 15, 0);
    d.panes[tab] = p;
    return p;
}

lv_obj_t* g_last_note = nullptr;

// .setting-row: name + note on the left, the control (added by the caller) on the right.
lv_obj_t* row(lv_obj_t* pane, const char* name, const char* note)
{
    lv_obj_t* r = plain(pane);
    lv_obj_add_style(r, &s_row, 0);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* left = plain(r);
    lv_obj_set_height(left, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 3, 0);
    lv_obj_t* n = label(left, name, f_m16, col::ink);
    lv_obj_set_width(n, LV_PCT(100));
    lv_obj_t* t = label(left, note ? note : "", f_r12, col::muted);
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    g_last_note = t;
    return r;
}

lv_obj_t* valueText(lv_obj_t* r, const char* text, const lv_font_t* font)
{
    lv_obj_t* l = label(r, text, font, col::ink);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(l, 300, 0);
    return l;
}

// ---------------------------------------------------------------- control callbacks
// Every control marks the dialog edited when it changes something (explicit flag: the Settings
// struct is not compared with memcmp, its padding is undefined).
void edited()
{
    d.edited = true;
    d.discard_armed = false;
}

void onFlag(bool on, void* user)
{
    uint8_t* f = (uint8_t*)user;
    if ((*f != 0) != on) edited();
    *f = on ? 1 : 0;
}

lv_obj_t* flagToggle(lv_obj_t* r, uint8_t& field) { return toggleCreate(r, field != 0, onFlag, &field); }

void onName(const char* text, void*)
{
    if (strcmp(d.name, text ? text : "") != 0) edited();
    snprintf(d.name, sizeof d.name, "%s", text ? text : "");
}
void onQuant(int i, void*)
{
    if (i != d.quant) edited();
    d.quant = i;
}
void onOutput(int i, void*)
{
    if ((int)E().output != i) edited();
    E().output = (AudioOut)i;
}
// Master limiter: the project's limiter (the same switch as mixer L and FX LIMITER), applied with
// APPLY; the setting also becomes the default for new projects.
void onLimiter(bool on, void*)
{
    if ((d.limiter != 0) != on) edited();
    d.limiter = on ? 1 : 0;
    E().master_limiter = on ? 1 : 0;
}

void bufNote()
{
    if (!d.buf_note) return;
    const Settings& e = E();
    // DMA buffers + the block being rendered: 4 + 1 at 64 frames, 3 + 1 at 128 / 256 (hal/audio.cpp)
    const float blocks = e.buffer_frames <= 64 ? 5.0f : 4.0f;
    const float ms = blocks * (float)e.buffer_frames * 1000.0f / (float)(e.sample_rate ? e.sample_rate : kRate48);
    char t[64];
    snprintf(t, sizeof t, "Bufor audio \xE2\x80\xA2 opóźnienie ~%.1f ms", (double)ms);
    setText(d.buf_note, t);
}
void rateNote()
{
    if (d.rate_note)
        setText(d.rate_note, app().btEnabled() ? "Bluetooth włączony: silnik pracuje z 44,1 kHz, ten wybór wróci po jego wyłączeniu"
                                               : "Zmiana restartuje silnik audio");
}
void onRate(int i, void*)
{
    const uint32_t r = i == 0 ? (uint32_t)kRate44 : (uint32_t)kRate48;
    if (E().sample_rate != r) edited();
    E().sample_rate = r;
    bufNote();
}
void onBuffer(int i, void*)
{
    static const uint16_t kFrames[3] = {64, 128, 256};
    const uint16_t f = kFrames[i < 0 ? 0 : (i > 2 ? 2 : i)];
    if (E().buffer_frames != f) edited();
    E().buffer_frames = f;
    bufNote();
}
const uint8_t kVoiceLimits[5] = {32, 24, 16, 12, 8};   // Engine::kMinVoiceLimit..kVoices
void onVoices(int i, void*)
{
    const uint8_t v = kVoiceLimits[i < 0 ? 0 : (i > 4 ? 4 : i)];
    if (E().voice_limit != v) edited();
    E().voice_limit = v;
}
void onMidiIn(int i, void*)
{
    const uint8_t on = i == 0 ? 1 : 0;
    if (E().midi_usb != on) edited();
    E().midi_usb = on;
}
void onChannel(int i, void*)
{
    const uint8_t ch = (uint8_t)(i < 0 ? 0 : (i > 16 ? 16 : i));
    if (E().midi_channel != ch) edited();
    E().midi_channel = ch;
}

void learnLook(bool on)
{
    if (!d.learn_btn) return;
    d.learn_shown = on;
    btnSetText(d.learn_btn, on ? "LEARNING\xE2\x80\xA6" : "START LEARN");
    btnSetActive(d.learn_btn, on);
}
void onLearn(lv_event_t*)
{
    const bool on = !app().midiLearning();
    app().midiLearn(on);
    learnLook(app().midiLearning());   // the App explains the next step in a toast
}

void mapNote()
{
    if (!d.map_note) return;
    char t[64];
    snprintf(t, sizeof t, "Powiązania CC: %d / 32 (zapisane na karcie SD)", app().midiMapCount());
    setText(d.map_note, t);
}

// Clearing the learned CC map needs a second tap within 3 s (it is saved on the card at once).
void onClearMap(lv_event_t*)
{
    if (app().midiMapCount() == 0) return;
    if (!d.clear_armed || lv_tick_elaps(d.clear_ms) >= 3000) {
        d.clear_armed = true;
        d.clear_ms = lv_tick_get();
        app().toast("Dotknij ponownie, aby usunąć powiązania MIDI");
        return;
    }
    d.clear_armed = false;
    app().midiClearMap();
    mapNote();
}

void onBright(int v, void*)
{
    if (E().brightness != (uint8_t)v) edited();
    E().brightness = (uint8_t)v;
    hal::setBrightness(v);   // live preview, restored on discard
    d.live_bright = true;
}
void onSens(int v, void*)
{
    if (E().knob_sensitivity != (uint8_t)v) edited();
    E().knob_sensitivity = (uint8_t)v;
}
void velNote()
{
    if (!d.vel_note) return;
    char t[64];
    snprintf(t, sizeof t, "Velocity w trybie Fixed: %d", (int)E().fixed_velocity);
    setText(d.vel_note, t);
}
void onVelMode(int i, void*)
{
    const PadVelocity pv = i == 0 ? PadVelocity::Position : PadVelocity::Fixed;
    if (E().pad_velocity != pv) edited();
    E().pad_velocity = pv;
}
void onVelFixed(int v, void*)
{
    if (E().fixed_velocity != (uint8_t)v) edited();
    E().fixed_velocity = (uint8_t)v;
    velNote();
}

// ---------------------------------------------------------------- unsaved edits guard
constexpr uint32_t kConfirmMs = 3000;

// X / ESC / OPEN UI GUIDE: with unsaved edits the first tap only warns; a second one within 3 s
// discards them. true = go ahead and close.
bool confirmDiscard()
{
    if (!d.edited || d.applied) return true;
    if (d.discard_armed && lv_tick_elaps(d.discard_ms) < kConfirmMs) {
        d.discard_armed = false;
        return true;
    }
    d.discard_armed = true;
    d.discard_ms = lv_tick_get();
    app().toast("Niezapisane ustawienia \xE2\x80\x94 dotknij ponownie, aby odrzucić (APPLY zapisuje)");
    return false;
}

// Runs before the modal's own handlers (LV_EVENT_PREPROCESS): a scrim tap never discards edits,
// X and ESC need the second tap.
void scrimGuard(lv_event_t* e)
{
    if (!d.open) return;
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED && lv_event_get_target(e) == lv_event_get_current_target(e)) {
        if (d.edited && !d.applied) {
            lv_event_stop_processing(e);
            app().toast("Niezapisane ustawienia \xE2\x80\x94 APPLY zapisuje, X odrzuca");
        }
    } else if (code == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC) {
        if (!confirmDiscard()) lv_event_stop_processing(e);
    }
}
void closeGuard(lv_event_t* e)
{
    if (d.open && !confirmDiscard()) lv_event_stop_processing(e);
}

// The modal's scrim and close button (widgets_overlay.cpp layout: scrim > window > head > [title, X]).
void installGuards(lv_obj_t* body)
{
    lv_obj_t* win = body ? lv_obj_get_parent(body) : nullptr;
    lv_obj_t* scrim = win ? lv_obj_get_parent(win) : nullptr;
    if (scrim) lv_obj_add_event_cb(scrim, scrimGuard, (lv_event_code_t)(LV_EVENT_ALL | LV_EVENT_PREPROCESS), nullptr);
    lv_obj_t* head = win ? lv_obj_get_child(win, 0) : nullptr;
    const int32_t n = head ? (int32_t)lv_obj_get_child_count(head) : 0;
    lv_obj_t* x = n > 0 ? lv_obj_get_child(head, n - 1) : nullptr;
    if (x && x != body && lv_obj_has_flag(x, LV_OBJ_FLAG_CLICKABLE))
        lv_obj_add_event_cb(x, closeGuard, (lv_event_code_t)(LV_EVENT_CLICKED | LV_EVENT_PREPROCESS), nullptr);
}

void onScan(lv_event_t*) { app().rescan(); }
void onExport(lv_event_t*) { app().exportJson(); }
void onGuide(lv_event_t*)   // replaces this modal: unsaved edits need the second tap
{
    if (confirmDiscard()) dlg_help::open();
}
void onLauncher(lv_event_t*) { app().exitToLauncher(); }   // false + toast when not possible

// ---------------------------------------------------------------- live values
void updateLive(bool force)
{
    if (!d.open) return;
    if (!force && d.live_init && lv_tick_elaps(d.live_ms) < 500) return;
    d.live_ms = lv_tick_get();
    d.live_init = true;
    char t[320];   // the diagnostics line is the longest
    if (d.midi_status) {
        if (app().midiConnected()) {   // App wraps hal::midiConnected / midiDeviceName
            const char* n = app().midiDeviceName();
            const float bpm = app().midiClockBpm();
            if (bpm > 0.0f)
                snprintf(t, sizeof t, "\xE2\x97\x8F %s \xE2\x80\xA2 %.1f BPM", (n && n[0]) ? n : "USB MIDI", (double)bpm);
            else
                snprintf(t, sizeof t, "\xE2\x97\x8F %s", (n && n[0]) ? n : "USB MIDI");
            lv_obj_set_style_text_color(d.midi_status, c(col::accent_dark), 0);
        } else {
            snprintf(t, sizeof t, "Brak urządzenia");
            lv_obj_set_style_text_color(d.midi_status, c(col::muted), 0);
        }
        setText(d.midi_status, t);
    }
    if (d.sd_status) setText(d.sd_status, app().sdStatus());
    const Telemetry& tel = app().engine().telemetry();
    const int cap = (int)tel.voice_cap.load(std::memory_order_relaxed);   // voice limit, lowered by the CPU governor
    if (d.live_cpu) {
        const unsigned xr = (unsigned)tel.xruns.load(std::memory_order_relaxed);   // driver underruns (App bridges them)
        snprintf(t, sizeof t, "CPU audio %d %%  \xE2\x80\xA2  głosy %d / %d  \xE2\x80\xA2  opóźnienie %.1f ms  \xE2\x80\xA2  xruns %u",
                 (int)(app().cpuLoad() * 100.0f + 0.5f), app().activeVoices(), cap, (double)app().latencyMs(), xr);
        setText(d.live_cpu, t);
    }
    if (d.voice_note) {
        const int lim = app().engine().voiceLimit();
        if (cap < lim)
            snprintf(t, sizeof t, "Teraz %d: przeciążenie CPU obniżyło limit \xE2\x80\x94 zwiększ bufor", cap);
        else
            snprintf(t, sizeof t, "Maksymalna polifonia (przy przeciążeniu CPU silnik obniża ją sam)");
        setText(d.voice_note, t);
    }
    if (d.live_diag) {
        hal::Diagnostics dg;
        hal::diagnostics(dg);
        char hp[48];
        if (dg.ioe_inputs < 0) snprintf(hp, sizeof hp, "HP_DET: brak odczytu");
        else
            snprintf(hp, sizeof hp, "HP_DET 0x%02X (bit 7 %d)%s", (unsigned)(dg.ioe_inputs & 0xFF), (dg.ioe_inputs >> 7) & 1,
                     dg.hp_det_trusted ? "" : ", niezweryfikowany");
        snprintf(t, sizeof t,
                 "Max: render %u / zapis %u / okres %u \xC2\xB5s, przeciążone bloki %u, DMA %u  \xE2\x80\xA2  obraz: obrót %u / "
                 "naprawa %u / publikacja %u \xC2\xB5s, bufor %u wierszy %s  \xE2\x80\xA2  %s",
                 (unsigned)dg.audio_render_max_us, (unsigned)dg.audio_write_max_us, (unsigned)dg.audio_period_max_us,
                 (unsigned)dg.audio_overload_blocks, (unsigned)dg.audio_dma_buffers, (unsigned)dg.rotate_max_us,
                 (unsigned)dg.repair_max_us, (unsigned)dg.publish_max_us, (unsigned)dg.draw_rows,
                 dg.draw_sram ? "SRAM" : "PSRAM", hp);
        setText(d.live_diag, t);
    }
    if (d.live_mem) {
        snprintf(t, sizeof t, "Wolna pamięć: RAM %u KB  \xE2\x80\xA2  PSRAM %.1f MB", (unsigned)(hal::freeInternal() / 1024u),
                 (double)hal::freePsram() / (1024.0 * 1024.0));
        setText(d.live_mem, t);
    }
}

// ---------------------------------------------------------------- sections
void buildGeneral()
{
    lv_obj_t* p = paneCreate(0, "General", "Projekt i zachowanie aplikacji.");
    lv_obj_t* r = row(p, "Project name", "Nazwa bieżącego projektu (plik na karcie SD)");
    lv_obj_t* f = textFieldCreate(r, kCtrlW, d.name, (int)sizeof(d.name) - 1, onName, nullptr, "PROJECT NAME");
    lv_obj_set_height(f, 35);
    r = row(p, "Auto-save", "Zapis co 5 minut, gdy są niezapisane zmiany");
    flagToggle(r, E().autosave);
    r = row(p, "Launch quantization", "Moment zmiany sceny podczas grania");
    static const char* const kQuant[] = {"1 Bar", "1/4", "1/8", "Off"};
    selectCreate(r, kCtrlW, 37, kQuant, 4, d.quant, onQuant, nullptr);
    r = row(p, "Pad audition", "Wybór padu odtwarza jego dźwięk");
    flagToggle(r, E().pad_audition);
}

void buildAudio()
{
    Settings& e = E();
    lv_obj_t* p = paneCreate(1, "Audio engine", "Kodek ES8388, 32 głosy, wewnętrznie float 32-bit.");
    lv_obj_t* r = row(p, "Output", "Wyjście audio");
    static const char* const kOut[] = {"Auto (ES8388)", "Speaker", "Headphones 3.5 mm"};
    selectCreate(r, kCtrlW, 37, kOut, 3, (int)e.output < 3 ? (int)e.output : 0, onOutput, nullptr);
    r = row(p, "Sample rate", "");
    d.rate_note = g_last_note;
    rateNote();
    static const char* const kRate[] = {"44.1 kHz", "48 kHz"};
    selectCreate(r, kCtrlW, 37, kRate, 2, e.sample_rate == (uint32_t)kRate44 ? 0 : 1, onRate, nullptr);
    r = row(p, "Buffer", "");
    d.buf_note = g_last_note;
    static const char* const kBuf[] = {"64 frames", "128 frames", "256 frames"};
    selectCreate(r, kCtrlW, 37, kBuf, 3, e.buffer_frames >= 256 ? 2 : (e.buffer_frames >= 128 ? 1 : 0), onBuffer, nullptr);
    bufNote();
    // Polyphony ceiling; under CPU overload the engine's governor lowers it further for a while.
    r = row(p, "Voice limit", "");
    d.voice_note = g_last_note;
    static const char* const kVoiceItems[] = {"32 voices", "24 voices", "16 voices", "12 voices", "8 voices"};
    int vi = 0;
    for (int i = 0; i < 5; ++i)
        if (kVoiceLimits[i] == e.voice_limit) vi = i;
    selectCreate(r, kCtrlW, 37, kVoiceItems, 5, vi, onVoices, nullptr);
    // The project's limiter (one switch with mixer L / FX LIMITER), not a second copy of it.
    r = row(p, "Master limiter", "Ogranicznik na wyjściu (jak L w mikserze); też domyślny dla nowych projektów");
    if (d.limiter < 0) d.limiter = d.limiter0 = app().project().limiter_on ? 1 : 0;
    toggleCreate(r, d.limiter != 0, onLimiter, nullptr);
    // Output levels live on the VOLUME knob (0.3): each output keeps its own, switched by itself.
    r = row(p, "Volume", "Pokrętło VOLUME: głośnik, słuchawki i Bluetooth pamiętają własną głośność");
    char lv[64];
    snprintf(lv, sizeof lv, "SPK %d%% · HP %d%% · BT %d%%", app().outputVolume(OutKind::Speaker),
             app().outputVolume(OutKind::Headphones), app().outputVolume(OutKind::Bluetooth));
    valueText(r, lv, f_r13);
}

void buildMidi()
{
    Settings& e = E();
    lv_obj_t* p = paneCreate(2, "MIDI", "USB MIDI (host, gniazdo USB-A): nuty padów, transport i zegar.");
    lv_obj_t* r = row(p, "Input device", "Urządzenie wejściowe");
    static const char* const kIn[] = {"USB MIDI \xE2\x80\x94 Auto", "None"};
    selectCreate(r, kCtrlW, 37, kIn, 2, e.midi_usb ? 0 : 1, onMidiIn, nullptr);
    r = row(p, "Status", "Podłączony kontroler");
    d.midi_status = valueText(r, "", f_r14);
    r = row(p, "MIDI clock sync", "Tempo z zewnętrznego zegara MIDI");
    flagToggle(r, e.midi_clock_in);
    r = row(p, "Clock out", "Wysyłanie zegara 24 PPQN przez USB");
    flagToggle(r, e.midi_clock_out);
    r = row(p, "Channel", "Kanał nut padów (domyślnie 10)");
    static const char* const kCh[] = {"Omni", "1", "2", "3", "4", "5", "6", "7", "8",
                                      "9", "10", "11", "12", "13", "14", "15", "16"};
    selectCreate(r, kCtrlW, 37, kCh, 17, e.midi_channel <= 16 ? e.midi_channel : 10, onChannel, nullptr);
    r = row(p, "Learn mode", "Rusz pokrętło, potem kontroler: ten CC steruje pokrętłem");
    d.learn_btn = actionBtn(r, "START LEARN", false, 0, 34);
    lv_obj_add_event_cb(d.learn_btn, onLearn, LV_EVENT_CLICKED, nullptr);
    learnLook(app().midiLearning());
    r = row(p, "CC map", "");
    d.map_note = g_last_note;
    lv_obj_t* clr = actionBtn(r, "CLEAR MAP", false, 0, 34);
    lv_obj_add_event_cb(clr, onClearMap, LV_EVENT_CLICKED, nullptr);
    mapNote();
    updateLive(true);
}

// ---------------------------------------------------------------- Bluetooth (Aurora Pico bridge)
// Acts at once through the App (not the APPLY copy): the toggle starts / stops the bridge (the engine moves
// to 44.1 kHz and back), a device row connects. The headphone volume is the VOLUME knob (0.3). Redrawn on revs.bt only.
void onBt(bool on, void*) { app().btSetEnabled(on); }
void onBtLocal(bool on, void*) { app().btSetLocal(on); }
void onBtScan(lv_event_t*) { app().btScan(); }
void onBtReconnect(lv_event_t*) { app().btReconnect(); }
void onBtTest(lv_event_t*) { app().btTest(); }
void onPodsMode(int i, void*) { app().btAirpodsMode(i + 1); }
void onDev(lv_event_t* e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < app().btDeviceCount()) app().btSelect(app().btDevice(i).mac);
}

// Signal bars from the RSSI of the last scan: none for a saved device the scan did not see (0 / -127).
int btBars(int rssi) { return (rssi == 0 || rssi <= -100) ? 0 : rssi >= -60 ? 4 : rssi >= -70 ? 3 : rssi >= -80 ? 2 : 1; }

void btTag(lv_obj_t* parent, const char* text, uint32_t fg, uint32_t box, bool filled)
{
    lv_obj_t* t = lv_label_create(parent);
    lv_obj_add_style(t, &s_tag, 0);
    lv_label_set_text(t, text);
    lv_obj_set_style_text_color(t, c(fg), 0);
    lv_obj_set_style_border_color(t, c(box), 0);
    if (filled) {
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(t, c(box), 0);
    }
}

// Device row: name, address, CONNECTED / SELECTED and SAVED tags, 4 signal bars; a tap connects.
void btDevRow(int i, const hal::BtDevice& dv, bool connected)
{
    lv_obj_t* r = plain(d.bt_list);
    lv_obj_add_style(r, &s_dev, 0);
    lv_obj_add_style(r, &s_dev_pr, LV_STATE_PRESSED);
    if (connected) lv_obj_add_style(r, &s_dev_on, 0);
    lv_obj_set_size(r, LV_PCT(100), 40);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(r, onDev, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_t* n = label(r, dv.name[0] ? dv.name : dv.mac, f_m14, col::ink);
    lv_obj_set_flex_grow(n, 1);
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
    if (dv.name[0]) label(r, dv.mac, f_r12, col::muted);
    if (connected) btTag(r, "CONNECTED", col::on_accent, col::accent, true);
    else if (dv.selected) btTag(r, "SELECTED", col::accent_dark, col::accent, false);
    if (dv.saved) btTag(r, "SAVED", col::muted, col::line, false);
    lv_obj_t* sig = plain(r);
    lv_obj_set_size(sig, 18, 13);
    const int lit = btBars(dv.rssi);
    for (int b = 0; b < 4; ++b) {
        lv_obj_t* bar = plain(sig);
        lv_obj_set_size(bar, 3, 4 + b * 3);
        lv_obj_set_pos(bar, b * 5, 9 - b * 3);
        lv_obj_set_style_radius(bar, 1, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, c(b < lit ? col::ink : col::line), 0);
    }
}

uint32_t fnv(uint32_t h, const void* p, size_t n)
{
    for (const uint8_t* b = (const uint8_t*)p; n--; ++b) h = (h ^ *b) * 16777619u;
    return h;
}

void podsPct(char* out, int cap, int v)
{
    if (v < 0) snprintf(out, (size_t)cap, "\xE2\x80\x94");
    else snprintf(out, (size_t)cap, "%d %%", v);
}

void btRefresh()
{
    rateNote();
    if (!d.bt_list) return;   // the tab was not built yet
    App& a = app();
    const hal::BtStatus& st = a.btStatus();
    const bool on = a.btEnabled();
    toggleSet(d.bt_toggle, on);
    char t[160];
    uint32_t color = col::muted;
    const char* ver = st.version[0] ? st.version : "?";
    if (!on) {
        snprintf(t, sizeof t, "Wyłączony");
    } else if (!st.module) {
        snprintf(t, sizeof t, "szukam modułu\xE2\x80\xA6");
    } else if (st.connected) {
        snprintf(t, sizeof t, "Pico %s \xC2\xB7 połączono: %s", ver, st.connected_name[0] ? st.connected_name : "?");
        color = col::accent_dark;
    } else {
        snprintf(t, sizeof t, st.scanning ? "Pico %s \xC2\xB7 skanowanie\xE2\x80\xA6" : "Pico %s \xC2\xB7 brak połączenia", ver);
        color = col::ink;
    }
    setText(d.bt_module, t);
    lv_obj_set_style_text_color(d.bt_module, c(color), 0);
    const char* note = !on                ? "Mostek: I2S G5 / G18 / G19, UART G6 \xE2\x86\x92 G32, G33 \xE2\x86\x92 G46"
                       : st.message[0]   ? st.message
                       : !st.module      ? "Budzenie modułu Pico (impuls na G6)"
                       : st.streaming    ? "Strumień audio 44,1 kHz \xC2\xB7 SBC"
                                         : "Moduł gotowy";
    // 0.4.4: while streaming, how much audio waits in the Pico (its ring between the 30 ms pulls, on average)
    char nb[120];
    if (on && st.streaming && st.pico_floor >= 0) {
        const int ms = (int)((st.pico_floor + 661) * 10 / 441);
        if (st.resyncs)
            snprintf(nb, sizeof nb, "Strumień 44,1 kHz \xC2\xB7 SBC \xC2\xB7 bufor Pico ok. %d ms \xC2\xB7 wyrównania: %lu", ms,
                     (unsigned long)st.resyncs);
        else snprintf(nb, sizeof nb, "Strumień 44,1 kHz \xC2\xB7 SBC \xC2\xB7 bufor Pico ok. %d ms", ms);
        note = nb;
    }
    setText(d.bt_note, note);
    btnSetText(d.bt_scan, st.scanning ? "SCANNING\xE2\x80\xA6" : "SCAN");
    btnSetActive(d.bt_scan, st.scanning != 0);

    if (on && st.airpods) {
        char l[12], r[12], k[12];
        podsPct(l, sizeof l, st.airpods_left);
        podsPct(r, sizeof r, st.airpods_right);
        podsPct(k, sizeof k, st.airpods_case);
        snprintf(t, sizeof t, "L %s \xC2\xB7 R %s \xC2\xB7 etui %s \xE2\x80\xA2 tryb słuchania", l, r, k);
        setText(d.bt_pods_note, t);
        if (st.airpods_mode >= 1 && st.airpods_mode <= 4) selectSet(d.bt_pods_sel, st.airpods_mode - 1);
        lv_obj_remove_flag(d.bt_pods, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(d.bt_pods, LV_OBJ_FLAG_HIDDEN);
    }

    // Device list: rebuilt only when what it shows changed (a tap is never lost to a status refresh); off: none.
    const int n = on ? a.btDeviceCount() : 0;
    uint32_t key = fnv(2166136261u, &st.connected, 1);
    key = fnv(key, &on, sizeof on);
    for (int i = 0; i < n; ++i) {
        const hal::BtDevice& dv = a.btDevice(i);
        const uint8_t f[3] = {(uint8_t)btBars(dv.rssi), dv.saved, dv.selected};
        key = fnv(fnv(fnv(key, dv.mac, strlen(dv.mac) + 1), dv.name, strlen(dv.name) + 1), f, 3);
    }
    key = fnv(key, st.connected_name, strlen(st.connected_name)) | 1u;
    if (key == d.bt_list_key) return;
    d.bt_list_key = key;
    lv_obj_clean(d.bt_list);
    for (int i = 0; i < n; ++i) {
        const hal::BtDevice& dv = a.btDevice(i);
        btDevRow(i, dv, st.connected && dv.selected);   // the Pico connects its selected device (never by name)
    }
    if (n == 0) {
        lv_obj_t* e = label(d.bt_list,
                            on ? "Brak urządzeń \xE2\x80\x94 włącz parowanie w słuchawkach i dotknij SCAN"
                               : "Włącz Bluetooth, aby zobaczyć zapisane urządzenia",
                            f_r14, col::muted);
        lv_obj_set_style_pad_ver(e, 12, 0);
        lv_obj_set_style_pad_left(e, 10, 0);   // in line with the device names
    }
}

void buildBluetooth()
{
    const Settings& s = app().settings();
    lv_obj_t* p = paneCreate(3, "Bluetooth",
                             "Słuchawki i głośniki Bluetooth (A2DP, SBC) przez mostek Pico. Zmiany działają od razu. Głośność: pokrętło VOLUME.");
    lv_obj_t* r = row(p, "Bluetooth", "Moduł Pico z Aurory, złącze M5-Bus");
    d.bt_toggle = toggleCreate(r, app().btEnabled(), onBt, nullptr);
    r = row(p, "Module", "");
    d.bt_note = g_last_note;
    d.bt_module = valueText(r, "", f_r14);
    // Output and AirPods above the device list (up to 32 rows), so they stay in view.
    r = row(p, "Also speaker / jack", "Wyjście: dźwięk także z głośnika lub gniazda 3,5 mm (ok. 0,2 s przed Bluetooth)");
    toggleCreate(r, s.bt_local != 0, onBtLocal, nullptr);
    d.bt_pods = row(p, "AirPods", "");
    d.bt_pods_note = g_last_note;
    static const char* const kModes[] = {"OFF", "ANC", "TRANSPARENCY", "ADAPTIVE"};
    d.bt_pods_sel = selectCreate(d.bt_pods, kCtrlW, 37, kModes, 4, 0, onPodsMode, nullptr);
    r = row(p, "Devices", "Dotknij urządzenie, aby połączyć (Pico zapamiętuje do 8)");
    lv_obj_t* btns = plain(r);
    lv_obj_set_size(btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 8, 0);
    d.bt_scan = actionBtn(btns, "SCAN", false, 0, 34);
    lv_obj_add_event_cb(d.bt_scan, onBtScan, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(actionBtn(btns, "RECONNECT", false, 0, 34), onBtReconnect, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(actionBtn(btns, "TEST", false, 0, 34), onBtTest, LV_EVENT_CLICKED, nullptr);
    d.bt_list = plain(p);
    lv_obj_set_size(d.bt_list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(d.bt_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_t* n = label(p,
                        "Bluetooth dodaje ok. 0,15\xE2\x80\x93" "0,25 s opóźnienia \xE2\x80\x94 do grania na padach na żywo "
                        "używaj głośnika lub słuchawek przewodowych. Gdy Bluetooth jest włączony, silnik audio "
                        "pracuje z 44,1 kHz.",
                        f_r12, col::muted);
    lv_obj_set_width(n, LV_PCT(100));
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_top(n, 12, 0);
    d.rev_bt = app().revs().bt;
    d.bt_list_key = 0;
    btRefresh();
}

// ---- Base color / Accent color: one swatch per base (its panel with "Aa" in its ink) and per accent, plus
// CUSTOM (a hue wheel around the custom colour, which the hue / saturation row below sets); an ink ring marks
// the chosen ones. Both apply after APPLY and a quick restart (themePrompt), because every style and screen
// takes its colours when it is built.
// The CUSTOM accent on the base chosen in the dialog (what the restart will show); before the first CUSTOM, the
// colour it would start with (the preset chosen so far).
uint32_t customColor()
{
    const Settings& e = E();
    int hue = e.accent_hue, sat = e.accent_sat;
    if (sat == 0 && e.theme < kThemeCount) presetHueSat(e.theme, hue, sat);
    return customAccent(hue, sat, e.base < kBaseCount ? e.base : 0);   // sat 0 with CUSTOM: the default, like themeSelect
}

void customShow()
{
    if (!d.custom_row) return;
    const Settings& e = E();
    const int hue = customHue(e.accent_hue), shown_sat = customSat(e.accent_sat);
    const uint32_t a = customColor();
    lv_obj_set_style_bg_color(d.preview, c(a), 0);
    lv_obj_set_style_text_color(d.preview_txt, c(onAccentFor(a)), 0);
    char t[64];
    snprintf(t, sizeof t, "Odcień %d\xC2\xB0 \xC2\xB7 nasycenie %d %%", hue, shown_sat);
    setText(d.custom_note, t);
    // the knobs show the colour they pick; the saturation track follows the hue
    lv_obj_set_style_bg_color(d.hue_sl, c(hueColor((float)hue, 1.f)), LV_PART_KNOB);
    lv_obj_set_style_bg_color(d.sat_sl, c(hueColor((float)hue, (float)shown_sat / 100.f)), LV_PART_KNOB);
    lv_obj_invalidate(d.sat_sl);
    if (d.theme_sw[kThemeCount]) lv_obj_invalidate(d.theme_sw[kThemeCount]);
}

void colorsShow()
{
    const Look want = lookOf(E()), now = lookOnScreen();
    // the chosen swatch: an ink ring 2 px outside it, visible on a swatch of any colour
    for (int i = 0; i < kBaseCount; ++i)
        if (d.base_sw[i]) lv_obj_set_style_outline_width(d.base_sw[i], i == want.base ? 2 : 0, 0);
    for (int i = 0; i <= kThemeCount; ++i)
        if (d.theme_sw[i]) lv_obj_set_style_outline_width(d.theme_sw[i], i == want.theme ? 2 : 0, 0);
    char t[96];
    if (want.base == now.base) snprintf(t, sizeof t, "Tło, panele, pady i pokrętła: %s", baseName(want.base));
    else snprintf(t, sizeof t, "%s \xE2\x80\x94 po APPLY i szybkim restarcie ORBIT", baseName(want.base));
    setText(d.base_note, t);
    const char* an = want.theme == kThemeCustom ? "CUSTOM" : kThemes[want.theme].name;
    if (want.accentLike(now)) snprintf(t, sizeof t, "Kolor akcentu interfejsu: %s", an);
    else snprintf(t, sizeof t, "%s \xE2\x80\x94 po APPLY i restarcie", an);
    setText(d.theme_note, t);
    if (d.custom_row) {
        if (want.theme == kThemeCustom) lv_obj_remove_flag(d.custom_row, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(d.custom_row, LV_OBJ_FLAG_HIDDEN);
    }
    customShow();
}

void onBase(lv_event_t* e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= kBaseCount) return;
    if (E().base != (uint8_t)i) edited();
    E().base = (uint8_t)i;
    colorsShow();
}

void onTheme(lv_event_t* e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i > kThemeCustom) return;
    Settings& s = E();
    if (i == kThemeCustom && s.accent_sat == 0 && s.theme < kThemeCount) {   // the first CUSTOM: from the preset chosen so far
        int hue = 0, sat = 0;
        presetHueSat(s.theme, hue, sat);
        s.accent_hue = (uint16_t)hue;
        s.accent_sat = (uint8_t)sat;
        if (d.hue_sl) lv_slider_set_value(d.hue_sl, hue, LV_ANIM_OFF);
        if (d.sat_sl) lv_slider_set_value(d.sat_sl, sat, LV_ANIM_OFF);
    }
    if (s.theme != (uint8_t)i) edited();
    s.theme = (uint8_t)i;
    colorsShow();
}

void onCustomSlider(lv_event_t* e)
{
    lv_obj_t* sl = (lv_obj_t*)lv_event_get_target(e);
    const int v = (int)lv_slider_get_value(sl);
    Settings& s = E();
    if (sl == d.hue_sl) {
        if (s.accent_hue != (uint16_t)v) edited();
        s.accent_hue = (uint16_t)v;
        if (s.accent_sat == 0) s.accent_sat = (uint8_t)kAccentSatDefault;
    } else {
        if (s.accent_sat != (uint8_t)v) edited();
        s.accent_sat = (uint8_t)v;
    }
    customShow();
}

// Hue / saturation tracks (the theme's slider minus its track and fill): the colour scale itself, with round
// ends in the end colours. Drawn before the knob (DRAW_MAIN_BEGIN).
void trackDraw(lv_event_t* e)
{
    lv_obj_t* o = (lv_obj_t*)lv_event_get_current_target(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer) return;
    const bool hue = o == d.hue_sl;
    const float h = (float)customHue(E().accent_hue);
    const float s0 = (float)kAccentSatMin / 100.f;
    auto at = [&](float f) { return hue ? hueColor(360.f * f, 1.f) : hueColor(h, s0 + (1.f - s0) * f); };
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const int32_t th = lv_area_get_height(&a), r = th / 2;
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_opa = LV_OPA_COVER;
    rd.radius = LV_RADIUS_CIRCLE;
    lv_area_t cap = {a.x1, a.y1, a.x1 + th - 1, a.y2};
    rd.bg_color = c(at(0.f));
    lv_draw_rect(layer, &rd, &cap);
    cap = {a.x2 - th + 1, a.y1, a.x2, a.y2};
    rd.bg_color = c(at(1.f));
    lv_draw_rect(layer, &rd, &cap);
    rd.radius = 0;
    rd.bg_grad.dir = LV_GRAD_DIR_HOR;
    rd.bg_grad.stops_count = 2;
    rd.bg_grad.stops[0].opa = LV_OPA_COVER;
    rd.bg_grad.stops[0].frac = 0;
    rd.bg_grad.stops[1].opa = LV_OPA_COVER;
    rd.bg_grad.stops[1].frac = 255;
    const int segs = hue ? 6 : 1;   // HSV is linear in RGB between the six primaries / secondaries
    const int32_t x0 = a.x1 + r, w = a.x2 - r - x0;
    for (int i = 0; i < segs; ++i) {
        lv_area_t sa = {x0 + w * i / segs, a.y1, x0 + w * (i + 1) / segs, a.y2};
        rd.bg_grad.stops[0].color = c(at((float)i / (float)segs));
        rd.bg_grad.stops[1].color = c(at((float)(i + 1) / (float)segs));
        lv_draw_rect(layer, &rd, &sa);
    }
}

// The CUSTOM swatch: a 12-step hue wheel around a dot of the custom colour (inside the swatch's ring).
void wheelDraw(lv_event_t* e)
{
    lv_obj_t* o = (lv_obj_t*)lv_event_get_current_target(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer) return;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const int32_t cx = (a.x1 + a.x2 + 1) / 2, cy = (a.y1 + a.y2 + 1) / 2;
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.center.x = cx;
    ad.center.y = cy;
    ad.radius = 12;
    ad.width = 12;
    for (int i = 0; i < 12; ++i) {
        ad.start_angle = (lv_value_precise_t)(i * 30);
        ad.end_angle = (lv_value_precise_t)(i * 30 + 31);
        ad.color = c(hueColor((float)(i * 30), 1.f));
        lv_draw_arc(layer, &ad);
    }
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.radius = LV_RADIUS_CIRCLE;
    rd.bg_opa = LV_OPA_COVER;
    rd.bg_color = c(customColor());
    rd.border_width = 2;
    rd.border_color = c(col::modal_bg);
    rd.border_opa = LV_OPA_COVER;
    lv_area_t dot = {cx - 6, cy - 6, cx + 5, cy + 5};
    lv_draw_rect(layer, &rd, &dot);
}

void swatchRowExt(lv_event_t* e) { lv_event_set_ext_draw_size(e, 4); }   // the outline band around the swatches

lv_obj_t* swatchRow(lv_obj_t* r)
{
    lv_obj_t* sw = plain(r);
    lv_obj_set_size(sw, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sw, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(sw, 7, 0);
    // the chosen swatch's ring lies 2..4 px outside it: drawn (and redrawn) there too
    lv_obj_add_flag(sw, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(sw, swatchRowExt, LV_EVENT_REFR_EXT_DRAW_SIZE, nullptr);
    lv_obj_refresh_ext_draw_size(sw);
    return sw;
}

// A 30 px swatch: fill + ring (base: 1 px of its own line colour, accent: the 2 px light ring), outline = chosen.
lv_obj_t* swatch(lv_obj_t* sw, uint32_t fill, uint32_t ring, int ring_w, int radius, lv_event_cb_t cb, int index)
{
    lv_obj_t* b = lv_obj_create(sw);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 30, 30);
    lv_obj_set_style_radius(b, radius, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, c(fill), 0);
    lv_obj_set_style_border_width(b, ring_w, 0);
    lv_obj_set_style_border_color(b, c(ring), 0);
    lv_obj_set_style_border_opa(b, ring_w > 1 ? 170 : LV_OPA_COVER, 0);
    lv_obj_set_style_outline_color(b, c(col::ink), 0);
    lv_obj_set_style_outline_pad(b, 2, 0);
    lv_obj_set_style_outline_width(b, 0, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(b, 4);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void*)(intptr_t)index);
    return b;
}

// One labelled slider line of the CUSTOM row.
lv_obj_t* customSlider(lv_obj_t* parent, const char* caption, int min, int max, int value)
{
    lv_obj_t* ln = plain(parent);
    lv_obj_set_size(ln, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ln, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ln, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ln, 12, 0);
    lv_obj_t* cap = overline(ln, caption);
    lv_obj_set_width(cap, 82);
    lv_obj_t* s = lv_slider_create(ln);   // the ORBIT theme's slider: knob + press handling
    lv_obj_set_size(s, 168, 10);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, 0);   // trackDraw paints the colour scale instead
    lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_ext_click_area(s, 12);
    lv_obj_add_event_cb(s, trackDraw, LV_EVENT_DRAW_MAIN_BEGIN, nullptr);
    lv_obj_add_event_cb(s, onCustomSlider, LV_EVENT_VALUE_CHANGED, nullptr);
    return s;
}

void buildDisplay()
{
    Settings& e = E();
    lv_obj_t* p = paneCreate(4, "Display", "Ekran 5\" 1280 \xC3\x97 720 (294 ppi).");
    lv_obj_t* rb = row(p, "Base color", "");
    d.base_note = g_last_note;
    lv_obj_t* bs = swatchRow(rb);
    for (int i = 0; i < kBaseCount; ++i) {
        uint32_t panel, ink, line;
        baseSwatch(i, panel, ink, line);
        lv_obj_t* b = swatch(bs, panel, line, 1, 7, onBase, i);
        lv_obj_t* t = label(b, "Aa", f_m13, ink);
        lv_obj_center(t);
        d.base_sw[i] = b;
    }
    lv_obj_t* rt = row(p, "Accent color", "");
    d.theme_note = g_last_note;
    lv_obj_t* sw = swatchRow(rt);
    for (int i = 0; i < kThemeCount; ++i)
        d.theme_sw[i] = swatch(sw, kThemes[i].accent, col::swatch_ring, 2, LV_RADIUS_CIRCLE, onTheme, i);
    lv_obj_t* cu = swatch(sw, col::modal_bg, col::swatch_ring, 2, LV_RADIUS_CIRCLE, onTheme, kThemeCustom);
    lv_obj_set_style_bg_opa(cu, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(cu, wheelDraw, LV_EVENT_DRAW_MAIN, nullptr);
    d.theme_sw[kThemeCount] = cu;
    // CUSTOM: hue + saturation with a preview of the accent (hidden unless CUSTOM is chosen)
    d.custom_row = row(p, "Custom accent", "");
    d.custom_note = g_last_note;
    lv_obj_t* box = plain(d.custom_row);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box, 14, 0);
    d.preview = plain(box);
    lv_obj_set_size(d.preview, 52, 44);
    lv_obj_set_style_radius(d.preview, geo::small_radius, 0);
    lv_obj_set_style_bg_opa(d.preview, LV_OPA_COVER, 0);
    d.preview_txt = label(d.preview, "Aa", f_m16, col::ink);
    lv_obj_center(d.preview_txt);
    lv_obj_t* sliders = plain(box);
    lv_obj_set_size(sliders, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sliders, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sliders, 12, 0);
    d.hue_sl = customSlider(sliders, "HUE", 0, 359, customHue(e.accent_hue));
    d.sat_sl = customSlider(sliders, "SATURATION", kAccentSatMin, 100, customSat(e.accent_sat));
    colorsShow();
    lv_obj_t* r = row(p, "Brightness", "Jasność ekranu (podgląd na żywo)");
    hsliderCreate(r, 128, 10, 100, e.brightness, onBright, nullptr);
    r = row(p, "Animations", "Przejścia i mikroanimacje");
    flagToggle(r, e.animations);
    r = row(p, "Level meters", "Animowane wskaźniki poziomu");
    flagToggle(r, e.meters);
    r = row(p, "Low power mode", "30 fps, bez animacji i efektu ripple");
    flagToggle(r, e.low_power);
}

void buildTouch()
{
    Settings& e = E();
    lv_obj_t* p = paneCreate(5, "Touch", "Multi-touch (do 5 palców) i reakcja kontrolek.");
    lv_obj_t* r = row(p, "Touch feedback", "Animacja wciśnięcia i ripple");
    flagToggle(r, e.touch_feedback);
    r = row(p, "Haptic indicator", "Wizualne potwierdzenie dotyku padu");
    flagToggle(r, e.haptic_indicator);
    r = row(p, "Knob sensitivity", "Czułość przeciągania pokręteł");
    hsliderCreate(r, 128, 1, 100, e.knob_sensitivity, onSens, nullptr);
    r = row(p, "Double tap reset", "Podwójne dotknięcie przywraca wartość domyślną");
    flagToggle(r, e.double_tap_reset);
    r = row(p, "Pad velocity", "Siła z miejsca dotyku (góra = głośniej) lub stała");
    static const char* const kVel[] = {"Position", "Fixed"};
    selectCreate(r, kCtrlW, 37, kVel, 2, e.pad_velocity == PadVelocity::Fixed ? 1 : 0, onVelMode, nullptr);
    r = row(p, "Fixed velocity", "");
    d.vel_note = g_last_note;
    hsliderCreate(r, 128, 1, 127, e.fixed_velocity, onVelFixed, nullptr);
    velNote();
}

void buildStorage()
{
    lv_obj_t* p = paneCreate(6, "Storage", "Karta microSD (FAT32 / exFAT): biblioteka sampli i projekty.");
    lv_obj_t* r = row(p, "Library path", "Ścieżka biblioteki");
    valueText(r, "/sdcard/ORBIT/SAMPLES/", f_mono12);
    r = row(p, "Project path", "Ścieżka projektów");
    valueText(r, "/sdcard/ORBIT/PROJECTS/", f_mono12);
    r = row(p, "SD card", "Pojemność i wolne miejsce");
    d.sd_status = valueText(r, app().sdStatus(), f_r14);
    r = row(p, "Scan library", "Odświeżenie listy plików");
    lv_obj_t* b = actionBtn(r, "SCAN SD", false, 0, 34);
    lv_obj_add_event_cb(b, onScan, LV_EVENT_CLICKED, nullptr);
    r = row(p, "Export project", "Zapis bieżącego projektu do PROJECTS/<nazwa>.json");
    b = actionBtn(r, "EXPORT JSON", false, 0, 34);
    lv_obj_add_event_cb(b, onExport, LV_EVENT_CLICKED, nullptr);
}

lv_obj_t* copyBox(lv_obj_t* p)
{
    lv_obj_t* bx = plain(p);
    lv_obj_add_style(bx, &s_mono_box, 0);
    lv_obj_set_size(bx, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bx, LV_FLEX_FLOW_COLUMN);
    return bx;
}

lv_obj_t* copyLine(lv_obj_t* bx, const char* text, uint32_t color)
{
    lv_obj_t* l = label(bx, text, f_r14, color);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

void buildAbout()
{
    char sub[96];
    snprintf(sub, sizeof sub, "ORBIT DRUM %s \xE2\x80\xA2 build %s \xE2\x80\xA2 1280 \xC3\x97 720", ORBIT_VERSION, kBuildDate);
    lv_obj_t* p = paneCreate(7, "About ORBIT DRUM", sub);
    lv_obj_set_style_pad_row(p, 0, 0);
    lv_obj_t* bx = copyBox(p);
    copyLine(bx, hal::boardInfo(), col::ink);
    d.live_cpu = copyLine(bx, "", col::ink);
    d.live_mem = copyLine(bx, "", col::ink);
    d.live_diag = copyLine(bx, "", col::muted);   // device worst cases (zeros on the PC)
    lv_obj_t* gap = plain(p);
    lv_obj_set_size(gap, 1, 12);
    lv_obj_t* cr = copyBox(p);
    copyLine(cr,
             "Czcionki: Lato, Montserrat, Share Tech Mono (SIL OFL 1.1) i DejaVu Sans. "
             "Zestawy 808 STYLE i 909 STYLE zsyntetyzowaliśmy sami \xE2\x80\x94 bez sampli Roland "
             "(TR-808 i TR-909 to znaki towarowe Roland).",
             col::muted);
    gap = plain(p);
    lv_obj_set_size(gap, 1, 16);
    lv_obj_t* btns = plain(p);
    lv_obj_set_size(btns, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 12, 0);
    lv_obj_t* b = actionBtn(btns, "OPEN UI GUIDE", false, 0, 34);
    lv_obj_add_event_cb(b, onGuide, LV_EVENT_CLICKED, nullptr);
    if (app().hasLauncher()) {   // standalone flash / PC: there is no launcher to return to
        b = actionBtn(btns, "BACK TO LAUNCHER", false, 0, 34);
        lv_obj_add_event_cb(b, onLauncher, LV_EVENT_CLICKED, nullptr);
    }
    updateLive(true);
}

void buildPane(int i)
{
    switch (i) {
    case 0: buildGeneral(); break;
    case 1: buildAudio(); break;
    case 2: buildMidi(); break;
    case 3: buildBluetooth(); break;
    case 4: buildDisplay(); break;
    case 5: buildTouch(); break;
    case 6: buildStorage(); break;
    default: buildAbout(); break;
    }
}

// One height for every section (the modal maximum, MIDI needs about that much): the centred
// window keeps its size and position, so the section list and X never move under the finger
// when the section changes (the prototype's per-section height did that). Longer sections scroll.
void fitShell()
{
    lv_obj_t* p = d.panes[d.tab];
    if (!p) return;
    lv_obj_set_height(p, kShellMax);
    lv_obj_set_height(d.shell, kShellMax);
    lv_obj_set_height(d.nav, kShellMax);
    lv_obj_scroll_to_y(p, 0, LV_ANIM_OFF);
}

void showTab(int i)
{
    if (i < 0 || i >= kTabs) i = 0;
    d.tab = i;
    if (!d.panes[i]) buildPane(i);
    for (int j = 0; j < kTabs; ++j) {
        if (d.panes[j]) {
            if (j == i) lv_obj_remove_flag(d.panes[j], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(d.panes[j], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_state(d.links[j], LV_STATE_CHECKED, j == i);
    }
    app().ui().settings_tab = i;   // remembered for the next open (UI-only state)
    if (d.foot_note && lv_obj_check_type(d.foot_note, &lv_label_class))
        setText(d.foot_note, i == kTabBluetooth ? "Bluetooth działa od razu \xE2\x80\x94 pozostałe zmiany po APPLY"
                                                : "Zmiany obowiązują po APPLY");
    fitShell();
}

void onLink(lv_event_t* e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i != d.tab) showTab(i);
}

// The file-system name App::renameProject() makes of `src` (same rules as its sanitizeName).
void cleanName(char* dst, int cap, const char* src)
{
    while (src && *src == ' ') ++src;
    int o = 0;
    for (int i = 0; src && src[i] && o < cap - 1; ++i) {
        const unsigned char ch = (unsigned char)src[i];
        const bool bad = ch < 32 || ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' ||
                         ch == '<' || ch == '>' || ch == '|';
        dst[o++] = bad ? '_' : (char)ch;
    }
    while (o > 0 && (dst[o - 1] == ' ' || dst[o - 1] == '.')) --o;
    dst[o] = 0;
}

bool sameNameCi(const char* a, const char* b)   // FAT names ignore ASCII case
{
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return false;
        if (!x) return true;
    }
}

// Another saved project already has this name (its file would be overwritten by the next SAVE).
bool nameTaken(const char* clean)
{
    if (!clean[0] || sameNameCi(clean, app().project().name)) return false;
    static char names[64][32];
    const int n = app().listProjects(names, 64);
    for (int i = 0; i < n; ++i)
        if (sameNameCi(names[i], clean)) return true;
    return false;
}

// After APPLY with new colours (base and / or accent): restart now (the project is saved and opened again) or later.
void onThemeLater(lv_event_t*) { modalClose(); }
void onThemeRestart(lv_event_t*) { app().restartApp(); }   // returns only when it did not restart (toast says why)

void themePrompt(void*)
{
    lv_obj_t* body = modalOpen("COLORS", 600, 250, 320);
    if (!body) return;
    const Look lk = lookOf(app().settings());
    char acc[24];
    if (lk.theme == kThemeCustom) snprintf(acc, sizeof acc, "CUSTOM %d\xC2\xB0", lk.hue);
    else snprintf(acc, sizeof acc, "%s", kThemes[lk.theme].name);
    char t[240];
    snprintf(t, sizeof t,
             "Kolory (tło %s, akcent %s) pojawią się po ponownym uruchomieniu ORBIT (ok. 2 s). Projekt zostanie "
             "zapisany i otwarty ponownie.",
             baseName(lk.base), acc);
    lv_obj_t* l = label(body, t, f_r14, col::ink);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_hor(l, 26, 0);
    lv_obj_set_style_pad_ver(l, 22, 0);
    lv_obj_t* foot = modalFooter("");
    lv_obj_add_event_cb(actionBtn(foot, "LATER", false, 0, 34), onThemeLater, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(actionBtn(foot, "RESTART NOW", true, 0, 34), onThemeRestart, LV_EVENT_CLICKED, nullptr);
}

void onApply(lv_event_t*)
{
    const bool rename = strcmp(d.name, d.name0) != 0 && d.name[0];
    char clean[32] = "";
    bool taken = false;
    if (rename) {
        cleanName(clean, sizeof clean, d.name);
        taken = nameTaken(clean);
        // The App's own warning would be replaced by "Zapisano ustawienia" in the same frame:
        // the first APPLY only warns, a second one within 3 s keeps the name.
        if (taken && !(d.rename_armed && strcmp(d.rename_name, clean) == 0 && lv_tick_elaps(d.rename_ms) < kConfirmMs)) {
            d.rename_armed = true;
            d.rename_ms = lv_tick_get();
            snprintf(d.rename_name, sizeof d.rename_name, "%s", clean);
            app().toast("Projekt %s już istnieje \xE2\x80\x94 APPLY ponownie: SAVE go nadpisze", clean);
            return;
        }
    }
    d.rename_armed = false;
    if (rename) app().renameProject(d.name);
    if (d.quant != d.quant0) app().setLaunchQuant((LaunchQuant)d.quant);
    app().applySettings();
    // After applySettings(): it copies a changed default into the project; the switch shown here wins.
    if (d.limiter >= 0 && d.limiter != d.limiter0 && (app().project().limiter_on != 0) != (d.limiter != 0))
        app().setLimiter(d.limiter != 0);
    if (taken) app().toast("Zapisano ustawienia \xE2\x80\x94 SAVE nadpisze projekt %s", clean);
    d.applied = true;
    // Asked only when this dialog chose other colours than the ones on screen (not again after LATER).
    const Look applied = lookOf(app().settings());
    const bool theme_new = !(applied == d.look0) && !(applied == lookOnScreen());
    modalClose();
    if (theme_new) lv_async_call(themePrompt, nullptr);   // after this event: the settings modal is gone
}

void onTimer(lv_timer_t*) { refresh(); }

void onClosed(void*)
{
    if (d.timer) lv_timer_delete(d.timer);
    d.timer = nullptr;
    if (!d.applied) {
        app().settingsDiscard();
        const Settings& s = app().settings();
        if (d.live_bright) hal::setBrightness(s.brightness);
    }
    d = Dlg{};
    g_last_note = nullptr;
}

}  // namespace

// ---------------------------------------------------------------- public
void open()
{
    initStyles();
    lv_obj_t* body = modalOpen("SETTINGS / TAB5", kWinW, kWinMaxH, kWinMaxH);   // closes any other modal first
    if (!body) return;
    d = Dlg{};
    d.open = true;
    app().settingsDiscard();   // start from the active settings
    d.look0 = lookOf(app().settings());
    snprintf(d.name, sizeof d.name, "%s", app().project().name);
    snprintf(d.name0, sizeof d.name0, "%s", d.name);
    d.quant = d.quant0 = (int)app().project().quant < 4 ? (int)app().project().quant : 0;
    modalOnClose(onClosed, nullptr);
    installGuards(body);

    // .settings-shell: 175 px list | body (fixed height, see fitShell()).
    d.shell = plain(body);
    lv_obj_set_size(d.shell, LV_PCT(100), kShellMax);
    d.nav = plain(d.shell);
    lv_obj_set_pos(d.nav, 0, 0);
    lv_obj_set_size(d.nav, kNavW, kShellMax);
    lv_obj_set_style_bg_opa(d.nav, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(d.nav, c(kNavBg), 0);
    lv_obj_set_style_border_side(d.nav, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_width(d.nav, 1, 0);
    lv_obj_set_style_border_color(d.nav, c(col::line), 0);
    lv_obj_set_style_radius(d.nav, 0, 0);
    lv_obj_set_style_pad_ver(d.nav, 14, 0);
    lv_obj_set_style_pad_hor(d.nav, 9, 0);
    lv_obj_set_style_pad_row(d.nav, 4, 0);
    lv_obj_set_flex_flow(d.nav, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < kTabs; ++i) {
        lv_obj_t* l = lv_obj_create(d.nav);
        lv_obj_remove_style_all(l);
        lv_obj_add_style(l, &s_link, 0);
        lv_obj_add_style(l, &s_link_pr, LV_STATE_PRESSED);
        lv_obj_add_style(l, &s_link_on, LV_STATE_CHECKED);
        lv_obj_set_size(l, LV_PCT(100), 39);
        lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_ext_click_area(l, 2);
        lv_obj_t* t = lv_label_create(l);
        lv_label_set_text(t, kTabNames[i]);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_event_cb(l, onLink, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        d.links[i] = l;
    }

    lv_obj_t* foot = modalFooter("Zmiany obowiązują po APPLY");
    d.foot_note = foot ? lv_obj_get_child(foot, 0) : nullptr;   // the note label (Bluetooth acts at once)
    lv_obj_t* apply = actionBtn(foot, "APPLY", true, 0, 34);
    lv_obj_add_event_cb(apply, onApply, LV_EVENT_CLICKED, nullptr);

    showTab(app().ui().settings_tab);
    d.timer = lv_timer_create(onTimer, 250, nullptr);
}

void refresh()
{
    if (!d.open) return;
    const uint32_t rb = app().revs().bt;
    if (rb != d.rev_bt) {   // Bluetooth on / off, bridge status, device list
        d.rev_bt = rb;
        btRefresh();
    }
    const uint32_t rm = app().revs().midi;
    if (rm != d.rev_midi) {   // device plugged / gone, learn finished, map changed
        d.rev_midi = rm;
        if (d.learn_btn && app().midiLearning() != d.learn_shown) learnLook(app().midiLearning());
        mapNote();
        updateLive(true);
        return;
    }
    updateLive(false);
}

}  // namespace dlg_settings
}  // namespace orbit::ui
