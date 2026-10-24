// Build (Git Bash, project root; first: bash host/build_lvgl.sh and bash tools/run_host_tests.sh app
// once for host/bin/cJSON.o + kits_*.o):
// export ZIG_GLOBAL_CACHE_DIR=$PWD/host/.zig-cache/global ZIG_LOCAL_CACHE_DIR=$PWD/host/.zig-cache/local; zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -Wall -Wextra -DLV_CONF_INCLUDE_SIMPLE -DORBIT_HOST -Ifirmware/main -Ihost -Ifirmware/managed_components/lvgl__lvgl -I$HOME/esp/v5.5.2/esp-idf/components/json/cJSON host/tests/shot_chrome.cpp firmware/main/ui/ui.cpp firmware/main/ui/ui_bg.cpp firmware/main/ui/scr_top.cpp firmware/main/ui/scr_pads.cpp firmware/main/ui/scr_catpanel.cpp firmware/main/ui/scr_macros.cpp firmware/main/ui/scr_timeline.cpp firmware/main/ui/app.cpp firmware/main/ui/theme.cpp firmware/main/ui/widgets.cpp firmware/main/ui/widgets_knob.cpp firmware/main/ui/widgets_input.cpp firmware/main/ui/widgets_overlay.cpp firmware/main/core/*.cpp host/hal_host.cpp host/platform_host.cpp host/lvgl_host.cpp host/bin/cJSON.o host/bin/kits_808.o host/bin/kits_909.o host/lib/liborbit_assets.a host/lib/liblvgl.a -o host/bin/shot_chrome.exe && host/bin/shot_chrome.exe
//
// Screen chrome test: ui::build() with the REAL App (host HAL, sdcard/ folder) on the headless
// 1280x720 display. The five views and the dialogs are other modules: weak no-op stubs below
// stand in for them (each stub view is an empty container with its name, so view switching is
// visible). Writes host/shots/chrome_*.png (default, playing, mute, queued, popup) and checks
// the interactions of the top bar, PADS (LVGL + multi-touch hook), BROWSER categories, macros,
// and the SCENES timeline against App state. Also checks that an idle UI invalidates nothing.
//
// Integration run: build the same line with firmware/main/ui/*.cpp instead of the listed ui files;
// the real views / dialogs then replace the weak stubs, stub-only checks are skipped and opened
// dialogs are closed again. CHROME_SHOT_PREFIX=<path/prefix_> keeps those shots out of host/shots.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "core/platform.h"
#include "hal/hal.h"
#include "hal_host.h"
#include "lvgl_host.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/widgets.h"

using namespace orbit;
using namespace orbit::ui;

// ------------------------------------------------------------------ stubs of other modules
namespace {
int g_focus_search = 0, g_dlg_settings = 0, g_dlg_project = 0, g_dlg_help = 0;
int g_on_show[(int)View::Count] = {};
lv_obj_t* g_view_obj[(int)View::Count] = {};

lv_obj_t* stubView(lv_obj_t* work, int v, const char* name)
{
    lv_obj_t* o = lv_obj_create(work);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* l = label(o, name, f_s11, col::muted);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 18, 14);
    g_view_obj[v] = o;
    return o;
}
}  // namespace

#define WEAK __attribute__((weak))
namespace orbit::ui {
namespace view_sample {
WEAK lv_obj_t* create(lv_obj_t* w) { return stubView(w, 0, "SAMPLE VIEW (stub)"); }
WEAK void refresh() {}
WEAK void onShow() { ++g_on_show[0]; }
}  // namespace view_sample
namespace view_sequence {
WEAK lv_obj_t* create(lv_obj_t* w) { return stubView(w, 1, "SEQUENCE VIEW (stub)"); }
WEAK void refresh() {}
WEAK void onShow() { ++g_on_show[1]; }
}  // namespace view_sequence
namespace view_mixer {
WEAK lv_obj_t* create(lv_obj_t* w) { return stubView(w, 2, "MIXER VIEW (stub)"); }
WEAK void refresh() {}
WEAK void onShow() { ++g_on_show[2]; }
}  // namespace view_mixer
namespace view_browser {
WEAK lv_obj_t* create(lv_obj_t* w) { return stubView(w, 3, "BROWSER VIEW (stub)"); }
WEAK void refresh() {}
WEAK void onShow() { ++g_on_show[3]; }
WEAK void focusSearch() { ++g_focus_search; }
}  // namespace view_browser
namespace view_fx {
WEAK lv_obj_t* create(lv_obj_t* w) { return stubView(w, 4, "FX VIEW (stub)"); }
WEAK void refresh() {}
WEAK void onShow() { ++g_on_show[4]; }
}  // namespace view_fx
namespace view_bass {
WEAK lv_obj_t* create(lv_obj_t* w) { return stubView(w, 5, "BASS VIEW (stub)"); }
WEAK void refresh() {}
WEAK void onShow() { ++g_on_show[5]; }
WEAK void onHide() {}
WEAK void touchInput(const hal::TouchPoint*, int) {}
WEAK bool keysOn() { return false; }
WEAK int liveBase() { return 36; }
WEAK bool liveAccent() { return false; }
WEAK bool liveSlide() { return true; }
WEAK void setLiveBase(int) {}
}  // namespace view_bass
namespace keys {
WEAK void poll() {}
WEAK void handle(const hal::KeyEvent&) {}
}  // namespace keys
namespace dlg_settings {
WEAK void open() { ++g_dlg_settings; }
WEAK void refresh() {}
}  // namespace dlg_settings
namespace dlg_project {
WEAK void open() { ++g_dlg_project; }
}  // namespace dlg_project
namespace dlg_help {
WEAK void open() { ++g_dlg_help; }
}  // namespace dlg_help
namespace dlg_sample {
WEAK void open() {}
WEAK void refresh() {}
}  // namespace dlg_sample
}  // namespace orbit::ui

// ------------------------------------------------------------------ test helpers
static int g_checks = 0, g_fails = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        ++g_checks;                                                      \
        if (!(c)) {                                                      \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);           \
            ++g_fails;                                                   \
        }                                                                \
    } while (0)

static lv_display_t* g_disp = nullptr;
// true when the real views / dialogs are linked instead of the weak stubs (integration run):
// stub-only checks are skipped and opened dialogs are closed again.
static bool g_real = false;
static void closeOverlays()
{
    for (int i = 0; i < 3 && (modalIsOpen() || overlayOpen()); ++i) {
        if (modalIsOpen()) modalClose();
        else lvhost_tap(640, 40);   // popup / keyboard scrim
        lvhost_run(350);
    }
}
static int16_t g_audio[2 * 256];

// Runs LVGL and the audio engine side by side (128-frame blocks = 2.67 ms at 48 kHz).
static void runAudio(int ms)
{
    int acc = 0;   // audio time in 1/48 ms
    for (int t = 0; t < ms; t += 5) {
        acc += 5 * 48;
        while (acc >= 128) {
            halHostRenderAudio(128, g_audio);
            acc -= 128;
        }
        lvhost_run(5);
    }
}

static void shot(const char* name)
{
    char path[160];
    const char* pre = getenv("CHROME_SHOT_PREFIX");   // integration runs write elsewhere
    snprintf(path, sizeof path, "%s%s.png", pre && pre[0] ? pre : "host/shots/chrome_", name);
    CHECK(lvhost_screenshot(path));
    printf("  wrote %s\n", path);
}

static uint32_t pixel(int x, int y)   // RGB888 of the last rendered frame
{
    lv_draw_buf_t* b = lv_display_get_buf_active(g_disp);
    if (!b || !b->data) return 0;
    const uint16_t p = *(const uint16_t*)(b->data + (size_t)y * b->header.stride + (size_t)x * 2);
    const uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, bl = p & 31;
    return (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((bl << 3) | (bl >> 2));
}
static bool near(uint32_t a, uint32_t b, int tol)
{
    for (int s = 0; s <= 16; s += 8) {
        const int d = (int)((a >> s) & 255) - (int)((b >> s) & 255);
        if (d > tol || d < -tol) return false;
    }
    return true;
}
static bool isAccentish(uint32_t p)   // orange: strong red, medium green, low blue
{
    const int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
    return r > 200 && g > 60 && g < 170 && b < 120;
}

static int g_inv = 0;
static int64_t g_inv_px = 0;
// Invalidations outside a watched rectangle (g_watch_on): "only this object redrew".
static bool g_watch_on = false;
static lv_area_t g_watch = {0, 0, 0, 0};
static int g_inv_out = 0;
static void invCb(lv_event_t* e)
{
    const lv_area_t* a = (const lv_area_t*)lv_event_get_param(e);
    ++g_inv;
    if (a) g_inv_px += (int64_t)lv_area_get_width(a) * lv_area_get_height(a);
    if (a && g_watch_on && !(a->x1 >= g_watch.x1 && a->y1 >= g_watch.y1 && a->x2 <= g_watch.x2 && a->y2 <= g_watch.y2))
        ++g_inv_out;
}

// pad centres (prototype geometry)
static const int kPadCx[4] = {25 + 30, 93 + 30, 162 + 30, 230 + 30};
static const int kPadCy[4] = {118 + 31, 187 + 31, 255 + 31, 324 + 31};
static int padX(int i) { return kPadCx[i & 3]; }
static int padY(int i) { return kPadCy[i >> 2]; }
// timeline cell centre (row r, slot i)
static int cellX(int i) { return 552 + 8 + (i * 589 + 8) / 16 + 17; }
static int cellY(int r) { return 576 + 16 + r * 22 + 10; }
static int cellLeft(int i) { return 552 + 8 + (i * 589 + 8) / 16; }   // first column of the cell

static void touch(const hal::TouchPoint* pts, int n) { halHostTouch(pts, n); }

// The toast: the non-clickable child of the top layer whose first child is a label ("" if none).
static const char* toastText()
{
    lv_obj_t* top = lv_layer_top();
    for (uint32_t i = 0; i < lv_obj_get_child_count(top); ++i) {
        lv_obj_t* o = lv_obj_get_child(top, (int32_t)i);
        lv_obj_t* l = lv_obj_get_child(o, 0);
        if (!lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) && l &&
            lv_obj_check_type(l, &lv_label_class))
            return lv_label_get_text(l);
    }
    return "";
}

// Topmost popup menu box (last overlay on the top layer), or nullptr.
static lv_obj_t* popupBox()
{
    lv_obj_t* top = lv_layer_top();
    const uint32_t n = lv_obj_get_child_count(top);
    for (int32_t i = (int32_t)n - 1; i >= 0; --i) {
        lv_obj_t* ov = lv_obj_get_child(top, i);
        if (lv_obj_has_flag(ov, LV_OBJ_FLAG_CLICKABLE)) return lv_obj_get_child(ov, 0);
    }
    return nullptr;
}

// ------------------------------------------------------------------ tests
static void setupSong()
{
    App& a = app();
    const int song[16] = {0, 0, 1, 0, 0, 2, 1, 3, 0, 0, 1, 0, 0, 2, 1, 3};
    for (int i = 0; i < 16; ++i) {
        if (a.project().song.slot[i] != song[i]) a.setSongSlot(i, song[i]);   // the lit cell would clear
        if (a.project().song.fx[i] != (i == 7 || i == 13 || i == 15)) a.toggleSongFx(i);
    }
    for (int i = 0; i < 16; ++i) CHECK(a.project().song.slot[i] == song[i]);
}

static void testTopbar()
{
    App& a = app();
    printf("top bar\n");
    CHECK(a.ui().view == View::Sample);
    if (!g_real) {
        CHECK(g_view_obj[0] && !lv_obj_has_flag(g_view_obj[0], LV_OBJ_FLAG_HIDDEN));
        // views are created lazily after the first frame: not there yet, or hidden
        CHECK(!g_view_obj[1] || lv_obj_has_flag(g_view_obj[1], LV_OBJ_FLAG_HIDDEN));
    }
    const int show1 = g_on_show[1];
    // perf-12: the very first press after boot must not restyle every button (ui::build synced
    // the shared press style): only the pressed nav button (+ its ripple / shadow) redraws.
    g_inv = 0;
    g_inv_out = 0;
    // six tabs since 0.3: 84 px wide, 8 apart (SAMPLE SEQUENCE BASS MIXER BROWSER FX)
    g_watch = {377 + 92 - 12, 13 - 12, 377 + 92 + 84 + 12, 13 + 35 + 12};
    g_watch_on = true;
    lvhost_pointer(377 + 92 + 42, 30, true);   // SEQUENCE
    lvhost_run(10);
    g_watch_on = false;
    const int first_press_inv = g_inv, first_press_out = g_inv_out;
    lvhost_run(50);
    lvhost_pointer(377 + 92 + 42, 30, false);
    lvhost_run(100);
    printf("  first press after boot: %d invalidations, %d outside the button\n", first_press_inv, first_press_out);
    CHECK(first_press_out == 0);
    lvhost_run(50);
    CHECK(a.ui().view == View::Sequence);
    if (!g_real) {
        CHECK(lv_obj_has_flag(g_view_obj[0], LV_OBJ_FLAG_HIDDEN));
        CHECK(!lv_obj_has_flag(g_view_obj[1], LV_OBJ_FLAG_HIDDEN));
        CHECK(g_on_show[1] == show1 + 1);
    }
    lvhost_tap(377 + 5 * 92 + 42, 30);   // FX
    lvhost_run(50);
    CHECK(a.ui().view == View::Fx);
    if (!g_real) CHECK(!lv_obj_has_flag(g_view_obj[4], LV_OBJ_FLAG_HIDDEN));
    lvhost_tap(377 + 42, 30);   // SAMPLE
    lvhost_run(400);
    CHECK(a.ui().view == View::Sample);
    // folder / gear / help
    lvhost_tap(978 + 15, 31);
    if (g_real) {
        CHECK(modalIsOpen());
        closeOverlays();
    }
    lvhost_tap(1020 + 15, 31);
    if (g_real) {
        CHECK(modalIsOpen());
        closeOverlays();
    }
    lvhost_tap(1227 + 16, 30);
    if (g_real) {
        CHECK(modalIsOpen());
        closeOverlays();
    } else {
        CHECK(g_dlg_project == 1 && g_dlg_settings == 1 && g_dlg_help == 1);
    }
    // play / rec in the top bar
    lvhost_tap(1153 + 16, 30);
    lvhost_run(40);
    CHECK(a.playing());
    lvhost_tap(1153 + 16, 30);
    lvhost_run(40);
    CHECK(!a.playing());
    lvhost_tap(1190 + 16, 30);
    lvhost_run(40);
    CHECK(a.recording());
    lvhost_tap(1190 + 16, 30);
    lvhost_run(40);
    CHECK(!a.recording());
    a.stop();   // a second stop rewinds
    runAudio(30);

    // ux-12: while MIDI learn is on, an accent chip "MIDI LEARN ✕" takes the place of the logo's subtitle (0.4.6:
    // the six tabs and the maximize button reach x 957, the old place covered FX); a tap on it ends learn.
    CHECK(!a.midiLearning());
    CHECK(!isAccentish(pixel(150, 20)));
    a.midiLearn(true);
    runAudio(60);
    CHECK(a.midiLearning());
    CHECK(isAccentish(pixel(150, 20)) && isAccentish(pixel(190, 20)));   // (above the text)
    CHECK(!isAccentish(pixel(874, 31)) && !isAccentish(pixel(966, 31)));   // FX tab and the maximize button free
    shot("midi_learn_chip");
    lvhost_tap(170, 31);
    runAudio(60);
    CHECK(!a.midiLearning());
    CHECK(!isAccentish(pixel(150, 20)));
}

static void testPads()
{
    App& a = app();
    printf("pads\n");
    // LVGL tap selects (and on the host plays) the pad; only the touched pads are redrawn
    lvhost_run(800);   // earlier ripples / transitions settle
    g_inv = 0;
    g_inv_px = 0;
    lvhost_tap(padX(5), padY(5));
    lvhost_run(400);   // includes the hit flash of the auditioned pad
    CHECK(a.ui().pad == 5);
    printf("  select a pad: %d invalidations, %lld px\n", g_inv, (long long)g_inv_px);
    CHECK(g_inv_px <= 6 * 69 * 72);   // a few pad rectangles (press tint, old + new selection)
    lvhost_tap(padX(0), padY(0));
    lvhost_run(40);
    CHECK(a.ui().pad == 0);
    // gaps belong to the nearest pad
    lvhost_tap(90, 150);   // between A1 (25..85) and A2 (93..)
    lvhost_run(40);
    CHECK(a.ui().pad == 1);
    lvhost_tap(padX(0), padY(0));
    lvhost_run(40);

    // multi-touch hook: two fingers at once trigger both pads at once
    runAudio(20);
    a.engine().telemetry().pad_hits.store(0);
    hal::TouchPoint two[2] = {{(int16_t)padX(9), (int16_t)(kPadCy[2] - 25), 0, 0},
                              {(int16_t)padX(14), (int16_t)(kPadCy[3] + 20), 1, 0}};
    touch(two, 2);
    halHostRenderAudio(128, g_audio);
    const uint32_t hits = a.engine().telemetry().pad_hits.load();
    CHECK((hits & (1u << 9)) && (hits & (1u << 14)));
    // refresh: selects the last finger's pad (no audition); it came < 180 ms after the last tap,
    // so the App selection waits for the pause (perf-4)
    lvhost_run(250);
    CHECK(a.ui().pad == 14);
    touch(two, 2);   // same fingers moving: no new hits
    a.engine().telemetry().pad_hits.store(0);
    halHostRenderAudio(128, g_audio);
    CHECK(a.engine().telemetry().pad_hits.load() == 0);
    touch(two, 0);   // all released
    halHostRenderAudio(128, g_audio);
    lvhost_run(40);
    // a finger starting in a gap never plays
    hal::TouchPoint gap = {89, 300, 3, 0};
    a.engine().telemetry().pad_hits.store(0);
    touch(&gap, 1);
    halHostRenderAudio(128, g_audio);
    CHECK(a.engine().telemetry().pad_hits.load() == 0);
    touch(&gap, 0);

    // perf-4: finger drumming over 4 pads at 8 hits/s. The orange "selected" look follows every
    // hit, but App::selectPad (SAMPLE view waveform re-render) runs for the first hit and once
    // after the burst: 2 selection revisions instead of 8.
    lvhost_run(400);
    {
        const uint32_t sel0 = a.revs().selection;
        const int seq[8] = {4, 5, 6, 7, 4, 5, 6, 7};
        bool look_follows = true;
        for (int k = 0; k < 8; ++k) {
            hal::TouchPoint f = {(int16_t)(padX(seq[k]) + 20), (int16_t)padY(seq[k]), (uint8_t)(10 + (k & 1)), 0};
            touch(&f, 1);
            halHostRenderAudio(128, g_audio);
            lvhost_run(60);
            if (k == 3) {   // pad 7 just hit: drawn selected while the App still has pad 4
                look_follows = isAccentish(pixel(padX(7) + 22, padY(7))) && !isAccentish(pixel(padX(4) + 22, padY(4)));
                CHECK(a.ui().pad == 4);
            }
            touch(&f, 0);
            halHostRenderAudio(128, g_audio);
            lvhost_run(65);
        }
        CHECK(look_follows);
        const uint32_t during = a.revs().selection - sel0;
        lvhost_run(250);
        const uint32_t total = a.revs().selection - sel0;
        printf("  drum burst, 8 hits: %u selection revisions during, %u total\n", (unsigned)during, (unsigned)total);
        CHECK(a.ui().pad == 7);
        CHECK(during == 1 && total == 2);
    }

    // MUTE mode: touch is silent, a tap toggles mute
    lvhost_tap(197 + 21, 92);
    lvhost_run(40);
    CHECK(a.ui().pad_mode == PadMode::Mute);
    hal::TouchPoint f = {(int16_t)padX(2), (int16_t)padY(2), 0, 0};
    a.engine().telemetry().pad_hits.store(0);
    touch(&f, 1);
    halHostRenderAudio(128, g_audio);
    touch(&f, 0);
    CHECK(a.engine().telemetry().pad_hits.load() == 0);
    lvhost_tap(padX(2), padY(2));
    lvhost_run(40);
    CHECK(a.project().pad[2].mute == 1);
    lvhost_tap(padX(6), padY(6));
    lvhost_run(40);
    CHECK(a.project().pad[6].mute == 1);
    lvhost_run(100);
    shot("mute");
    lvhost_tap(padX(6), padY(6));
    lvhost_run(40);
    CHECK(a.project().pad[6].mute == 0);
    // SOLO mode
    lvhost_tap(245 + 21, 92);
    lvhost_run(40);
    CHECK(a.ui().pad_mode == PadMode::Solo);
    lvhost_tap(padX(4), padY(4));
    lvhost_run(40);
    CHECK(a.project().pad[4].solo == 1);
    lvhost_tap(padX(4), padY(4));
    lvhost_run(40);
    CHECK(a.project().pad[4].solo == 0);
    lvhost_tap(245 + 21, 92);   // SOLO again -> PLAY
    lvhost_run(40);
    CHECK(a.ui().pad_mode == PadMode::Play);
    // MUTE (197..239) and SOLO (245..287) are 5 px apart: 1 px inside MUTE's right edge is MUTE (SOLO's
    // uniform 8 px margin used to take x 237..239), 1 px inside SOLO's left edge is SOLO.
    lvhost_tap(197 + 41, 92);
    lvhost_run(40);
    CHECK(a.ui().pad_mode == PadMode::Mute);
    lvhost_tap(197 + 41, 92);
    lvhost_run(40);
    CHECK(a.ui().pad_mode == PadMode::Play);
    lvhost_tap(245 + 1, 92);
    lvhost_run(40);
    CHECK(a.ui().pad_mode == PadMode::Solo);
    lvhost_tap(245 + 1, 92);
    lvhost_run(40);
    CHECK(a.ui().pad_mode == PadMode::Play);
    a.toggleMute(2);   // back to normal for the next shots
    lvhost_run(40);

    // overlay open: the touch hook stays silent
    lvhost_tap(1252 - 50, 669 + 12);   // LENGTH select opens a popup list
    lvhost_run(60);
    CHECK(overlayOpen());
    a.engine().telemetry().pad_hits.store(0);
    hal::TouchPoint g2 = {(int16_t)padX(3), (int16_t)padY(3), 0, 0};
    touch(&g2, 1);
    halHostRenderAudio(128, g_audio);
    touch(&g2, 0);
    CHECK(a.engine().telemetry().pad_hits.load() == 0);
    lvhost_tap(640, 300);   // close the popup (tap outside)
    lvhost_run(60);
    CHECK(!overlayOpen());
    a.selectPad(0, false);
    lvhost_run(40);
}

static void testCategories()
{
    App& a = app();
    printf("browser categories\n");
    lvhost_tap(150, 450 + 2 * 30 + 15);   // Drums
    lvhost_run(40);
    CHECK(a.ui().category == Category::Drums);
    CHECK(a.ui().view == View::Browser);
    lvhost_tap(150, 450 + 0 * 30 + 15);   // Kits
    lvhost_run(40);
    CHECK(a.ui().category == Category::Kits);
    // perf-14: a pressed 270x30 category row only tints, it never scales (no transform layer)
    {
        lvhost_pointer(150, 450 + 3 * 30 + 15, true);   // Bass
        lvhost_run(80);
        lv_point_t pt = {150, 450 + 3 * 30 + 15};
        lv_obj_t* r = lv_indev_search_obj(lv_screen_active(), &pt);
        CHECK(r && lv_obj_has_state(r, LV_STATE_PRESSED));
        CHECK(r && lv_obj_get_style_transform_scale_x(r, 0) == 256 && lv_obj_get_style_transform_scale_y(r, 0) == 256);
        lvhost_pointer(150, 450 + 3 * 30 + 15, false);
        lvhost_run(100);
        CHECK(a.ui().category == Category::Bass);
    }
    a.setView(View::Sample);
    lvhost_run(40);
    const int fs = g_focus_search;
    lvhost_tap(273, 432);   // search icon
    lvhost_run(40);
    CHECK(a.ui().view == View::Browser);
    if (!g_real) CHECK(g_focus_search == fs + 1);
    closeOverlays();
    lvhost_tap(150, 450 + 1 * 30 + 15);   // All Samples
    lvhost_run(40);
    CHECK(a.ui().category == Category::All);
    a.setView(View::Sample);
    lvhost_run(400);
}

static void testMacros()
{
    App& a = app();
    printf("macros\n");
    a.setBpm(120.f);
    lvhost_run(40);
    lvhost_tap(1207 + 23, 512 + 11);   // +
    lvhost_run(40);
    CHECK(fabsf(a.project().bpm - 121.f) < 0.01f);
    lvhost_tap(1131 + 23, 512 + 11);   // -
    lvhost_run(40);
    CHECK(fabsf(a.project().bpm - 120.f) < 0.01f);
    lvhost_pointer(1207 + 23, 512 + 11, true);   // hold + : accelerating repeat
    lvhost_run(1500);
    lvhost_pointer(1207 + 23, 512 + 11, false);
    lvhost_run(40);
    CHECK(a.project().bpm > 125.f);
    a.setBpm(120.f);
    // TAP: two taps 250 ms apart -> ~240 BPM; the button flashes
    lvhost_tap(1078 + 23, 512 + 11);
    sleepMs(250);
    lvhost_pointer(1078 + 23, 512 + 11, true);
    lvhost_run(20);
    CHECK(fabsf(a.project().bpm - 240.f) < 25.f);
    lvhost_pointer(1078 + 23, 512 + 11, false);
    lvhost_run(300);
    a.setBpm(120.f);
    lvhost_run(40);
    // ux-9: the inner edges of - and + (next to the BPM value) belong to the buttons: holding
    // there repeats, it never opens the tempo pad; a tap there steps once.
    {
        const int fy = 74 + 436 + 15;           // footer row centre (panel content y 436..465)
        const int plus_in = 1067 + 141, minus_in = 1067 + 109;
        lvhost_pointer(plus_in, fy, true);
        lvhost_run(1000);
        CHECK(!overlayOpen());
        lvhost_pointer(plus_in, fy, false);
        lvhost_run(100);
        printf("  hold + at its inner edge: %.1f BPM\n", (double)a.project().bpm);
        CHECK(a.project().bpm > 122.5f);
        CHECK(!overlayOpen());
        a.setBpm(120.f);
        lvhost_run(40);
        lvhost_tap(minus_in, fy);
        lvhost_run(40);
        CHECK(fabsf(a.project().bpm - 119.f) < 0.01f);
        CHECK(!overlayOpen());
        lvhost_tap(1067 + 175, 74 + 436 + 29 + 8);   // + below its bottom edge (taller target)
        lvhost_run(40);
        CHECK(fabsf(a.project().bpm - 120.f) < 0.01f);
        a.setBpm(120.f);
        lvhost_run(40);
    }
    // BPM long press -> numeric tempo pad; DEL DEL DEL 9 5 OK -> 95 BPM
    lvhost_pointer(1192, 524, true);
    lvhost_run(700);
    lvhost_pointer(1192, 524, false);
    lvhost_run(300);
    CHECK(overlayOpen());
    {
        const int nx = (1280 - 560) / 2, ny = 720 - 352 + 70, nh = 352 - 86;
        const float kw = (560.f - 3 * 7.f) / 4.f, kh = (nh - 2 * 7.f) / 3.f;
        auto nkey = [&](int r, int col) {
            lvhost_tap(nx + (int)(col * (kw + 7.f) + kw / 2.f), ny + (int)(r * (kh + 7.f) + kh / 2.f));
        };
        for (int i = 0; i < 3; ++i) nkey(0, 3);   // DEL
        nkey(2, 2);                                 // "9"
        nkey(1, 1);                                 // "5"
    }
    lvhost_tap(1280 - 24 - 60, 720 - 352 + 14 + 21);   // OK
    lvhost_run(300);
    CHECK(fabsf(a.project().bpm - 95.f) < 0.01f);
    CHECK(!overlayOpen());
    a.setBpm(120.f);
    // knob drag on VOLUME changes the level of the output that plays now (0.3); the dots follow
    const int v0 = a.outputVolume();
    lvhost_drag(1166, 143, 1166, 190, 300);
    lvhost_run(60);
    CHECK(a.outputVolume() < v0 - 5);
    // double tap resets it (Settings > Touch > Double tap reset): 70 % (headphones 50 %)
    lvhost_run(400);
    lvhost_double_tap(1166, 143);
    lvhost_run(60);
    CHECK(a.outputVolume() == (a.outputKind() == OutKind::Headphones ? 50 : 70));
    a.setOutputVolume(v0);
    lvhost_run(60);
    // transport: play / pause / rewind / rec
    lvhost_tap(1138 + 27, 474 + 16);
    lvhost_run(40);
    CHECK(a.playing());
    lvhost_tap(1078 + 27, 474 + 16);   // REWIND: stop
    lvhost_run(40);
    CHECK(!a.playing());
    lvhost_tap(1199 + 27, 474 + 16);
    lvhost_run(40);
    CHECK(a.recording());
    lvhost_tap(1199 + 27, 474 + 16);
    lvhost_run(40);
    CHECK(!a.recording());
}

static void testTimeline()
{
    App& a = app();
    printf("timeline\n");
    // scene buttons
    lvhost_tap(320 + 29 + 12, 604 + 17);   // B
    lvhost_run(40);
    CHECK(a.project().scene == 1);
    // "+" menu: COPY B -> A (item 0), CLEAR SCENE B (item 3)
    Scene& sb = a.project().scene_data[1];
    sb.track[3].step[5].on = 1;
    sb.track[3].step[5].vel = 77;
    lvhost_tap(320 + 4 * 29 + 12, 604 + 17);
    lvhost_run(80);
    CHECK(overlayOpen());
    lvhost_run(300);
    shot("popup");
    // the menu opens above the button: rows of 36 px + 2 px, 4 px padding
    lv_obj_t* top = lv_layer_top();
    lv_obj_t* ov = lv_obj_get_child(top, (int32_t)lv_obj_get_child_count(top) - 1);
    lv_obj_t* box = ov ? lv_obj_get_child(ov, 0) : nullptr;
    CHECK(box != nullptr);
    if (box) {
        lv_area_t ba;
        lv_obj_get_coords(box, &ba);
        lvhost_tap(ba.x1 + 40, ba.y1 + 4 + 18);   // item 0
        lvhost_run(80);
        CHECK(a.project().scene_data[0].track[3].step[5].on == 1 && a.project().scene_data[0].track[3].step[5].vel == 77);
    }
    lvhost_tap(320 + 4 * 29 + 12, 604 + 17);
    lvhost_run(80);
    ov = lv_obj_get_child(top, (int32_t)lv_obj_get_child_count(top) - 1);
    box = ov ? lv_obj_get_child(ov, 0) : nullptr;
    if (box) {
        lv_area_t ba;
        lv_obj_get_coords(box, &ba);
        lvhost_tap(ba.x1 + 40, ba.y1 + 4 + 3 * 38 + 18);   // item 3: CLEAR SCENE B
        lvhost_run(80);
        CHECK(a.project().scene_data[1].track[3].step[5].on == 0);
    }
    a.project().scene_data[0].track[3].step[5] = Step{};
    lvhost_tap(320 + 12, 604 + 17);   // A
    lvhost_run(40);
    CHECK(a.project().scene == 0);

    // matrix: tap an empty cell lights it, tap again clears; FX row toggles
    const int before = a.project().song.slot[3];
    lvhost_tap(cellX(3), cellY(2));
    lvhost_run(40);
    CHECK(a.project().song.slot[3] == 2);
    lvhost_tap(cellX(3), cellY(2));
    lvhost_run(40);
    CHECK(a.project().song.slot[3] == -1);
    a.setSongSlot(3, before);
    const int fx0 = a.project().song.fx[2];
    lvhost_tap(cellX(2), cellY(4));
    lvhost_run(40);
    CHECK(a.project().song.fx[2] == (fx0 ? 0 : 1));
    a.toggleSongFx(2);
    lvhost_run(40);

    // ux-13: scene buttons take taps a few px below their bottom edge (free panel space)
    lvhost_tap(320 + 2 * 29 + 12, 604 + 34 + 7);   // C, 7 px under the button
    lvhost_run(40);
    CHECK(a.project().scene == 2);
    lvhost_tap(320 + 12, 604 - 6);                  // A, 6 px above it
    lvhost_run(40);
    CHECK(a.project().scene == 0);

    // ux-13: slide-to-correct. Press slot 9 on the Scene D row, slide up to Scene C, release:
    // C is committed (not D); the cell under the finger is outlined while pressed.
    {
        CHECK(a.project().song.slot[8] == 0);   // setupSong: A
        lvhost_pointer(cellX(8), cellY(3), true);
        lvhost_run(40);
        const bool ring_d = near(pixel(cellLeft(8) + 1, cellY(3)), col::ink, 40);
        lvhost_pointer(cellX(8), cellY(2), true);
        lvhost_run(40);
        const bool ring_c = near(pixel(cellLeft(8) + 1, cellY(2)), col::ink, 40);
        const bool d_clear = !near(pixel(cellLeft(8) + 1, cellY(3)), col::ink, 40);
        CHECK(a.project().song.slot[8] == 0);   // nothing committed while pressed
        lvhost_pointer(cellX(8), cellY(2), false);
        lvhost_run(60);
        CHECK(ring_d && ring_c && d_clear);
        CHECK(a.project().song.slot[8] == 2);
        // replacing another scene is toasted
        printf("  toast: %s\n", toastText());
        CHECK(strncmp(toastText(), "Slot 9: Scene A", 15) == 0);
        // released outside the matrix: cancelled
        CHECK(a.project().song.slot[9] == 0);
        lvhost_pointer(cellX(9), cellY(1), true);
        lvhost_run(40);
        lvhost_pointer(cellX(9), 540, true);   // up into the work panel
        lvhost_run(40);
        lvhost_pointer(cellX(9), 540, false);
        lvhost_run(60);
        CHECK(a.project().song.slot[9] == 0);
        CHECK(!near(pixel(cellLeft(9) + 1, cellY(1)), col::ink, 40));   // outline gone
        // "+" menu: item 4 undoes the last cell edit (slot 9 back to A); item 5 is the pattern undo of the
        // CLEAR SCENE B above ("UNDO B", from any view)
        lvhost_tap(320 + 4 * 29 + 12, 604 + 17);
        lvhost_run(80);
        lv_obj_t* box = popupBox();
        CHECK(box && lv_obj_get_child_count(box) == 6);
        if (box) {
            lv_area_t ba;
            lv_obj_get_coords(box, &ba);
            lvhost_tap(ba.x1 + 40, ba.y1 + 4 + 4 * 38 + 18);   // item 4: UNDO SLOT 9
            lvhost_run(80);
        }
        CHECK(a.project().song.slot[8] == 0);
        // one level only: UNDO SLOT is gone, UNDO B is item 4 now
        lvhost_tap(320 + 4 * 29 + 12, 604 + 17);
        lvhost_run(80);
        box = popupBox();
        CHECK(box && lv_obj_get_child_count(box) == 5);
        CHECK(a.canUndoPattern() && a.undoPatternScene() == 1);
        const Scene cleared_b = a.project().scene_data[1];
        if (box) {
            lv_area_t ba;
            lv_obj_get_coords(box, &ba);
            lvhost_tap(ba.x1 + 40, ba.y1 + 4 + 4 * 38 + 18);   // item 4: UNDO B
            lvhost_run(80);
        }
        CHECK(a.project().scene_data[1].track[3].step[5].on == 1 && a.project().scene_data[1].track[3].step[5].vel == 77);
        CHECK(!a.canUndoPattern());
        a.project().scene_data[1] = cleared_b;   // the rest of the test expects the cleared scene B
        // nothing left to undo: the menu has 4 items again
        lvhost_tap(320 + 4 * 29 + 12, 604 + 17);
        lvhost_run(80);
        box = popupBox();
        CHECK(box && lv_obj_get_child_count(box) == 4);
        lvhost_tap(640, 300);   // close the menu
        lvhost_run(80);
        CHECK(!overlayOpen());
    }

    // FOLLOW / loop
    CHECK(a.project().song.follow == 0);
    lvhost_tap(1162 + 30, 577 + 15);
    lvhost_run(40);
    CHECK(a.project().song.follow == 1);
    lvhost_tap(1227 + 15, 577 + 15);   // link: loop the current scene
    lvhost_run(40);
    CHECK(a.project().song.follow == 0);

    // GRID select -> 1/32 (index 2), LENGTH -> 2 Bars (index 1)
    lvhost_tap(1162 + 47, 627 + 12);
    lvhost_run(60);
    ov = lv_obj_get_child(top, (int32_t)lv_obj_get_child_count(top) - 1);
    box = ov ? lv_obj_get_child(ov, 0) : nullptr;
    CHECK(box != nullptr);
    if (box) {
        lv_area_t ba;
        lv_obj_get_coords(box, &ba);
        lvhost_tap(ba.x1 + 40, ba.y1 + 4 + 2 * 38 + 18);
        lvhost_run(60);
        CHECK(a.project().scene_data[0].div == Division::D1_32);
    }
    lvhost_tap(1162 + 47, 670 + 12);
    lvhost_run(60);
    ov = lv_obj_get_child(top, (int32_t)lv_obj_get_child_count(top) - 1);
    box = ov ? lv_obj_get_child(ov, 0) : nullptr;
    if (box) {
        lv_area_t ba;
        lv_obj_get_coords(box, &ba);
        lvhost_tap(ba.x1 + 40, ba.y1 + 4 + 1 * 38 + 18);
        lvhost_run(60);
        CHECK(a.project().scene_data[0].length == 32);
    }
    a.setDivision(Division::D1_16);
    a.setSceneLength(16);
    lvhost_run(60);
}

static void testIdle()
{
    printf("idle: no invalidation\n");
    lvhost_run(3200);   // toasts fade out
    g_inv = 0;
    g_inv_px = 0;
    lvhost_run(1000);
    printf("  idle 1 s: %d invalidations\n", g_inv);
    CHECK(g_inv == 0);
}

static void testPlaying()
{
    App& a = app();
    printf("playing + FOLLOW\n");
    a.setFollow(true);
    a.play();
    g_inv = 0;
    g_inv_px = 0;
    runAudio(2400);   // slot 0 (A) -> slot 1 (A)
    printf("  playing 2.4 s: %d invalidations, %lld px (%.1f%% of the screen per frame at 60 Hz)\n", g_inv,
           (long long)g_inv_px, 100.0 * (double)g_inv_px / (2.4 * 60.0 * 1280.0 * 720.0));
    CHECK(a.playing());
    CHECK(a.songSlot() >= 1);
    // stop rendering right after a step with hits: the flash is visible
    for (int i = 0; i < 200; ++i) {
        halHostRenderAudio(128, g_audio);
        if (a.engine().telemetry().pad_hits.load() & 1u) break;   // the kick
        lvhost_run(3);
    }
    lvhost_run(40);
    shot("playing");
    // pad A1 hit ring: accent pixels in the gap left of the pad / the pad tinted
    int ring = 0;
    for (int y = 120; y < 178; ++y) ring += isAccentish(pixel(23, y)) ? 1 : 0;
    CHECK(ring > 20);
    // playhead line inside the playing slot's column + its triangle in the header
    const int slot = a.songSlot();
    bool head = false;
    for (int x = cellX(slot) - 18; x <= cellX(slot) + 18; ++x) head = head || isAccentish(pixel(x, 586));
    CHECK(head);
    // ux-16: in FOLLOW playback a scene button only selects for editing: it says so, the lane
    // of the playing scene (A) stays accent and the edited scene's lane (B) is outlined
    {
        CHECK(a.playScene() == 0);
        lvhost_tap(320 + 29 + 12, 604 + 17);   // B (no audio rendered: the song does not move)
        lvhost_run(40);
        CHECK(a.project().scene == 1);
        printf("  toast: %s\n", toastText());
        CHECK(strncmp(toastText(), "FOLLOW:", 7) == 0);
        CHECK(isAccentish(pixel(cellX(3), cellY(0))));                // A lane (playing): accent
        CHECK(!isAccentish(pixel(cellX(2), cellY(1))));               // B lane (edited): grey...
        CHECK(isAccentish(pixel(cellLeft(2) + 1, cellY(1))));         // ...with an accent outline
        CHECK(!isAccentish(pixel(cellLeft(5) + 1, cellY(2))));        // C lane: plain clip
        lvhost_tap(320 + 12, 604 + 17);        // A again
        lvhost_run(40);
        CHECK(a.project().scene == 0);
        CHECK(isAccentish(pixel(cellX(2), cellY(1))) == false && isAccentish(pixel(cellX(3), cellY(0))));
    }
    a.stop();
    a.stop();
    runAudio(100);
    a.setFollow(false);
}

static void testQueued()
{
    App& a = app();
    printf("queued scene\n");
    a.selectScene(0);
    a.play();
    runAudio(300);
    a.selectScene(2);   // C: queued until the next bar
    runAudio(40);
    CHECK(a.queuedScene() == 2);
    CHECK(a.playScene() == 0);
    // wait for the blink "on" phase (C lit) and take the shot
    for (int i = 0; i < 60 && ((lv_tick_get() / 250u) & 1u); ++i) runAudio(10);
    runAudio(20);
    shot("queued");
    CHECK(isAccentish(pixel(320 + 2 * 29 + 4, 604 + 6)));   // C lit
    CHECK(near(pixel(320 + 12, 604 + 34 - 6), 0xFF6531, 40));   // A (playing) shows its accent dot...
    CHECK(!isAccentish(pixel(320 + 12, 604 + 8)));               // ...but is not lit
    // 250 ms later C is in its "off" phase
    runAudio(250);
    lvhost_run(5);
    shot("queued_off");
    CHECK(!isAccentish(pixel(320 + 2 * 29 + 4, 604 + 6)));
    runAudio(2200);   // past the bar: C plays
    CHECK(a.queuedScene() == -1);
    CHECK(a.playScene() == 2);
    a.stop();
    a.stop();
    a.selectScene(0);
    runAudio(100);
}

int main()
{
    const char* root_env = getenv("ORBIT_ROOT");
    std::string root = root_env && root_env[0] ? root_env : ".";
    std::string sd = root + "/sdcard";
    halHostSetSdRoot(sd.c_str());
    halHostSetSettingsPath("host/out/shot_chrome_settings.bin");
    remove(halHostSettingsPath());
    CHECK(hal::boardInit());
    CHECK(hal::sdMount());
    g_disp = hal::displayStart();
    CHECK(g_disp != nullptr);
    themeInit();
    CHECK(app().init(hal::sdRoot(), 48000));
    CHECK(hal::audioStart(48000, 128, &App::render, &app().engine()));
    lv_display_add_event_cb(g_disp, invCb, LV_EVENT_INVALIDATE_AREA, nullptr);

    const uint64_t t0 = nowUs();
    ui::build(g_disp);
    g_real = g_view_obj[0] == nullptr;
    if (g_real) printf("integration run: real views and dialogs linked\n");
    printf("ui::build: %u ms (includes baking the background + pads)\n", (unsigned)((nowUs() - t0) / 1000u));
    setupSong();
    runAudio(400);
    shot("default");
    // ux-21: unsaved changes show as an accent dot on the folder icon (top-right of 978,16 31x30)
    CHECK(app().dirty());
    CHECK(isAccentish(pixel(978 + 25 + 3, 16 + 1 + 3)));

    testTopbar();
    testPads();
    testCategories();
    testMacros();
    testTimeline();
    testIdle();
    testPlaying();
    testQueued();

    hal::audioStop();
    remove(halHostSettingsPath());
    printf("shot_chrome: %d checks, %d failed -> %s\n", g_checks, g_fails, g_fails ? "FAILED" : "passed");
    return g_fails ? 1 : 0;
}
