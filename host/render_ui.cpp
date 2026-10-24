// Full-UI renderer + interaction smoke test (PC). Build + run through tools/run_host_tests.sh ui_shots, or:
// export ZIG_GLOBAL_CACHE_DIR=$PWD/host/.zig-cache/global ZIG_LOCAL_CACHE_DIR=$PWD/host/.zig-cache/local; zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -Wall -Wextra -DLV_CONF_INCLUDE_SIMPLE -DORBIT_HOST -Ifirmware/main -Ihost -Ifirmware/managed_components/lvgl__lvgl -I$HOME/esp/v5.5.2/esp-idf/components/json/cJSON host/render_ui.cpp firmware/main/ui/*.cpp firmware/main/core/*.cpp host/hal_host.cpp host/platform_host.cpp host/lvgl_host.cpp host/bin/cJSON.o host/bin/kits_808.o host/bin/kits_909.o host/lib/liborbit_assets.a host/lib/liblvgl.a -o host/bin/render_ui.exe && host/bin/render_ui.exe
// (needs host/lib/*.a from host/build_lvgl.sh and host/bin/{cJSON,kits_808,kits_909}.o from run_host_tests.sh)
//
// The whole firmware UI — every ui/*.cpp module, the REAL App (engine, loader thread, library) and the host
// HAL — on the headless 1280x720 display, started in the firmware's order (app_main.cpp). Nothing is stubbed.
// The SD card is a generated folder, host/out/render_ui_sd: the repository's kits plus ten synthesized
// samples with the prototype's names (the repository card has empty SAMPLES folders), so the BROWSER looks
// like the reference. Writes host/shots/ui_<state>.png named after reference/shots/<state>.png:
//   view-sample, view-sequence, view-mixer, view-browser, editor-{slice,envelope,filter,pitch,lfo},
//   modal-settings-{General,Audio,MIDI,Bluetooth,Display,Touch,Storage,About}, modal-project, modal-help,
//   modal-sample-details, playing-sequence (1.3 s of virtual time with the audio pulled through
//   halHostRenderAudio so telemetry moves), plus view-fx, keyboard, mute-mode, lane-velocity, song-follow,
//   bt-chip and modal-settings-Bluetooth-connected (Bluetooth on with the host HAL's fake Pico).
// Then a smoke test of the interactions (nav, all pads, multi-touch hook, step toggles + drag paint,
// macro knob drag, every dialog open/close, settings tabs, browser LOAD TO pad, OPEN IN SAMPLER, scene
// select, song cells, transport) asserting App state, and an idle check (0 invalidations).
// Last line: "render_ui: N checks, M failed -> passed|FAILED".
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <filesystem>
#include <string>

#include "core/platform.h"
#include "hal/hal.h"
#include "hal_host.h"
#include "lvgl_host.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/step_grid.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace fs = std::filesystem;
using namespace orbit;
using namespace orbit::ui;

// ------------------------------------------------------------------ checks
static int g_checks = 0, g_fails = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        ++g_checks;                                          \
        if (!(cond)) {                                       \
            ++g_fails;                                       \
            printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            printf(__VA_ARGS__);                             \
            printf("\n");                                    \
        }                                                    \
    } while (0)

// ------------------------------------------------------------------ test SD card
static void putLe(FILE* f, uint32_t v, int n)
{
    for (int i = 0; i < n; ++i) fputc((int)((v >> (8 * i)) & 0xFF), f);
}

static uint32_t g_noise = 12345;
static float noise()
{
    g_noise = g_noise * 1664525u + 1013904223u;
    return (float)((int32_t)g_noise) / 2147483648.0f;
}
static const float kTau = 6.2831853f;

enum class Gen { Kick, Snare, Hat, Perc, Sub, Chord, Lead, Vocal, Space, Rise };

static float gen(Gen g, int i, int n, int r, float& ph, float& prev)
{
    const float t = (float)i / (float)r, T = (float)n / (float)r;
    switch (g) {
    case Gen::Kick: {
        const float f = 50.0f + 110.0f * expf(-t * 18.0f);
        ph += kTau * f / (float)r;
        return sinf(ph) * expf(-t * 6.5f) * (i < n - 64 ? 1.0f : (float)(n - i) / 64.0f);
    }
    case Gen::Snare: return (0.6f * noise() + 0.5f * sinf(kTau * 185.0f * t)) * expf(-t * 16.0f);
    case Gen::Hat: {
        const float x = noise(), y = x - prev;
        prev = x;
        return 0.5f * y * expf(-t * 40.0f);
    }
    case Gen::Perc: return sinf(kTau * 410.0f * t + 2.0f * sinf(kTau * 90.0f * t)) * expf(-t * 11.0f);
    case Gen::Sub: return 0.9f * sinf(kTau * 65.4f * t) * (t < 0.01f ? t / 0.01f : expf(-(t - 0.01f) * 1.6f));
    case Gen::Chord: {
        const float env = (t < 0.25f ? t / 0.25f : 1.0f) * (t > T - 0.6f ? (T - t) / 0.6f : 1.0f);
        return 0.28f * env * (sinf(kTau * 261.6f * t) + sinf(kTau * 329.6f * t) + sinf(kTau * 392.0f * t));
    }
    case Gen::Lead: {
        const float p = fmodf(440.0f * t * (1.0f + 0.01f * sinf(kTau * 5.0f * t)), 1.0f);
        return 0.5f * (2.0f * p - 1.0f) * expf(-t * 2.0f);
    }
    case Gen::Vocal: {
        const float f0 = 220.0f * (1.0f + 0.02f * sinf(kTau * 6.0f * t));
        float v = 0;
        for (int h = 1; h < 12; ++h) {
            const float fh = f0 * (float)h;
            const float gn = expf(-powf((fh - 700.0f) / 300.0f, 2.0f)) + 0.6f * expf(-powf((fh - 1200.0f) / 400.0f, 2.0f));
            v += gn * sinf(kTau * fh * t);
        }
        return 0.35f * v * sinf(3.14159f * t / 0.6f);
    }
    case Gen::Space: return 0.5f * noise() * sinf(3.14159f * t / T) * (0.5f + 0.5f * sinf(kTau * 0.8f * t));
    case Gen::Rise: {
        const float f = 80.0f + 900.0f * (t / T) * (t / T);
        return 0.45f * (sinf(kTau * f * t) > 0 ? 1.0f : -1.0f) * (t / T);
    }
    }
    return 0.0f;
}

static bool writeWav(const std::string& path, int rate, int channels, int frames, Gen g)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t data = (uint32_t)(frames * channels * 2);
    fwrite("RIFF", 1, 4, f);
    putLe(f, 36 + data, 4);
    fwrite("WAVEfmt ", 1, 8, f);
    putLe(f, 16, 4);
    putLe(f, 1, 2);
    putLe(f, (uint32_t)channels, 2);
    putLe(f, (uint32_t)rate, 4);
    putLe(f, (uint32_t)(rate * channels * 2), 4);
    putLe(f, (uint32_t)(channels * 2), 2);
    putLe(f, 16, 2);
    fwrite("data", 1, 4, f);
    putLe(f, data, 4);
    float ph = 0, prev = 0;
    for (int i = 0; i < frames; ++i) {
        float v = gen(g, i, frames, rate, ph, prev);
        v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
        const int16_t s = (int16_t)lrintf(v * 30000.0f);
        for (int c = 0; c < channels; ++c) putLe(f, (uint16_t)s, 2);
    }
    fclose(f);
    return true;
}

static bool prepareSd(const std::string& root)
{
    const std::string orbit = root + "/ORBIT";
    std::error_code ec;
    fs::create_directories(orbit + "/KITS", ec);
    for (const char* kit : {"808_STYLE", "909_STYLE"}) {   // read-only use of the repository card
        const std::string src = std::string("sdcard/ORBIT/KITS/") + kit, dst = orbit + "/KITS/" + kit;
        fs::create_directories(dst, ec);
        for (const auto& e : fs::directory_iterator(src, ec)) {
            const fs::path to = fs::path(dst) / e.path().filename();
            if (!fs::exists(to)) fs::copy_file(e.path(), to, ec);
        }
    }
    struct W {
        const char* dir;
        const char* name;
        float sec;
        int rate, ch;
        Gen g;
    };
    const W files[] = {
        {"Drums", "KICK_01", 0.420f, 48000, 1, Gen::Kick},     {"Drums", "SNARE_02", 0.280f, 48000, 1, Gen::Snare},
        {"Drums", "HAT_CLOSED", 0.118f, 48000, 1, Gen::Hat},   {"Drums", "PERC_04", 0.355f, 48000, 1, Gen::Perc},
        {"Bass", "SUB_BASS_C2", 1.840f, 48000, 1, Gen::Sub},   {"Synth", "SOFT_CHORD", 2.10f, 44100, 2, Gen::Chord},
        {"Synth", "NEON_LEAD", 0.92f, 48000, 1, Gen::Lead},    {"Vocals", "VOCAL_CHOP", 0.60f, 48000, 1, Gen::Vocal},
        {"FX", "SPACE_FX", 2.52f, 48000, 1, Gen::Space},       {"FX", "GLITCH_RISE", 1.22f, 48000, 1, Gen::Rise},
    };
    for (const W& w : files) {
        const std::string dir = orbit + "/SAMPLES/" + w.dir;
        fs::create_directories(dir, ec);
        const std::string p = dir + "/" + w.name + ".wav";
        if (!fs::exists(p) && !writeWav(p, w.rate, w.ch, (int)(w.sec * (float)w.rate + 0.5f), w.g)) return false;
    }
    fs::remove_all(orbit + "/USER", ec);       // test output of earlier runs
    fs::create_directories(orbit + "/USER", ec);
    fs::remove_all(orbit + "/PROJECTS", ec);
    fs::create_directories(orbit + "/PROJECTS", ec);
    fs::remove(orbit + "/midimap.bin", ec);
    return true;
}

// ------------------------------------------------------------------ LVGL helpers
static lv_display_t* g_disp = nullptr;
static int16_t g_audio[2 * 256];

// LVGL and the audio engine side by side (128-frame blocks = 2.67 ms at 48 kHz).
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
    char path[200];
    const char* pre = getenv("UI_SHOT_PREFIX");
    snprintf(path, sizeof path, "%s%s.png", pre && pre[0] ? pre : "host/shots/ui_", name);
    lvhost_run(5);
    CHECK(lvhost_screenshot(path), "%s", path);
    printf("  wrote %s\n", path);
}

static bool visible(lv_obj_t* o)
{
    for (; o; o = lv_obj_get_parent(o))
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    return true;
}

struct Box {
    int x1, y1, x2, y2;
};
static const Box kAll = {0, 0, 1279, 719};
static const Box kWork = {311, 73, 1056, 558};
static const Box kTimeline = {311, 568, 1265, 706};
static const Box kFooter = {329, 470, 1040, 548};   // SEQUENCE footer cards

static bool inside(lv_obj_t* o, const Box& b)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
    return cx >= b.x1 && cx <= b.x2 && cy >= b.y1 && cy <= b.y2;
}

static lv_obj_t* findLabelIn(lv_obj_t* root, const char* text, bool prefix, const Box& b)
{
    if (!root) return nullptr;
    const uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* ch = lv_obj_get_child(root, (int32_t)i);
        if (lv_obj_has_flag(ch, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_check_type(ch, &lv_label_class)) {
            const char* t = lv_label_get_text(ch);
            if (t && (prefix ? strncmp(t, text, strlen(text)) == 0 : strcmp(t, text) == 0) && inside(ch, b) &&
                visible(ch))
                return ch;
        }
        if (lv_obj_t* f = findLabelIn(ch, text, prefix, b)) return f;
    }
    return nullptr;
}

// Overlays first (newest on top), then the screen.
static lv_obj_t* findLabel(const char* text, bool prefix = false, const Box& b = kAll)
{
    lv_obj_t* top = lv_layer_top();
    for (int32_t i = (int32_t)lv_obj_get_child_count(top) - 1; i >= 0; --i)
        if (lv_obj_t* f = findLabelIn(lv_obj_get_child(top, i), text, prefix, b)) return f;
    return findLabelIn(lv_screen_active(), text, prefix, b);
}

static void center(lv_obj_t* o, int& x, int& y)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    x = (a.x1 + a.x2) / 2;
    y = (a.y1 + a.y2) / 2;
}

static bool tapObj(lv_obj_t* o, int settle = 120)
{
    if (!o) return false;
    int x, y;
    center(o, x, y);
    lvhost_tap(x, y);
    runAudio(settle);
    return true;
}

static bool tapLabel(const char* text, const Box& b = kAll, bool prefix = false, int settle = 120)
{
    lv_obj_t* l = findLabel(text, prefix, b);
    if (!l) printf("  (label '%s' not found)\n", text);
    return tapObj(l, settle);
}

static void longPress(int x, int y, int ms = 700)
{
    lvhost_pointer(x, y, true);
    runAudio(ms);
    lvhost_pointer(x, y, false);
    runAudio(150);
}

static void closeOverlays()
{
    for (int i = 0; i < 4 && (modalIsOpen() || overlayOpen()); ++i) {
        if (modalIsOpen()) modalClose();
        else if (!tapLabel("CANCEL")) lvhost_tap(640, 8);   // keyboard / popup scrim
        runAudio(350);
    }
}

static bool waitLoader(int max_ms = 4000)
{
    for (int t = 0; t < max_ms; t += 20) {
        runAudio(20);
        if (app().loadingCount() == 0) return true;
    }
    return app().loadingCount() == 0;
}

static int g_inv = 0;
static int64_t g_inv_px = 0;
static void invCb(lv_event_t* e)
{
    const lv_area_t* a = (const lv_area_t*)lv_event_get_param(e);
    ++g_inv;
    if (a) g_inv_px += (int64_t)lv_area_get_width(a) * lv_area_get_height(a);
}

// geometry of the chrome (prototype positions, see the chrome module)
static const int kPadCx[4] = {25 + 30, 93 + 30, 162 + 30, 230 + 30};
static const int kPadCy[4] = {118 + 31, 187 + 31, 255 + 31, 324 + 31};
static int padX(int i) { return kPadCx[i & 3]; }
static int padY(int i) { return kPadCy[i >> 2]; }
// Six tabs since 0.3 (84 px, 8 apart), left to right SAMPLE SEQUENCE BASS MIXER BROWSER FX.
static int navX(int v)
{
    static const int kSlot[(int)View::Count] = {0, 1, 3, 4, 5, 2};   // View -> tab position
    return 377 + kSlot[v] * 92 + 42;
}
static const int kNavY = 30;
static int sceneX(int s) { return 320 + s * 29 + 12; }
static const int kSceneY = 621;
static int cellX(int i) { return 552 + 8 + (i * 589 + 8) / 16 + 17; }
static int cellY(int r) { return 576 + 16 + r * 22 + 10; }
// SEQUENCE step grid (ui/step_grid.h): column x 432 + c * 37.5, rows 174 + r * 34, 29 px high
static int stepX(int c) { return 432 + (c * 75) / 2 + 16; }
static int stepY(int r) { return 174 + r * 34 + 14; }

static void setupSong()
{
    App& a = app();
    const int song[16] = {0, 0, 1, 0, 0, 2, 1, 3, 0, 0, 1, 0, 0, 2, 1, 3};
    for (int i = 0; i < 16; ++i) {
        if (a.project().song.slot[i] != song[i]) a.setSongSlot(i, song[i]);
        if (a.project().song.fx[i] != (i == 7 || i == 13 || i == 15)) a.toggleSongFx(i);
    }
}

// Saved projects on the test card (PROJECT MANAGER list as in the reference), current = ORBIT_01, clean.
static void seedProjects()
{
    App& a = app();
    for (const char* n : {"NEON_DRAFT", "LIVE_SESSION_02", "EMPTY_PROJECT", "ORBIT_01"}) {
        bool ok = false;
        for (int i = 0; i < 100 && !(ok = a.saveAs(n)); ++i) runAudio(20);   // "Saving in progress": wait
        CHECK(ok, "saveAs %s", n);
        for (int i = 0; i < 200 && a.loadingCount() > 0; ++i) runAudio(20);
        runAudio(100);
    }
    char names[8][32];
    const int n = a.listProjects(names, 8);
    CHECK(n >= 3, "%d saved projects", n);
    CHECK(strcmp(a.project().name, "ORBIT_01") == 0 && !a.dirty(), "current ORBIT_01 saved ('%s')", a.project().name);
    runAudio(3500);   // toasts fade out
}

static void nav(View v)
{
    lvhost_tap(navX((int)v), kNavY);
    runAudio(450);   // includes the screen-enter animation
    CHECK(app().ui().view == v, "view %d", (int)v);
}

// ------------------------------------------------------------------ screenshots of every state
static void shotsSample()
{
    App& a = app();
    printf("SAMPLE view + editor tabs\n");
    CHECK(a.ui().view == View::Sample, "start view");
    CHECK(a.ui().editor == EditorTab::Trim, "start tab");
    shot("view-sample");
    struct T {
        const char* label;
        EditorTab tab;
        const char* name;
    };
    const T tabs[] = {{"SLICE", EditorTab::Slice, "editor-slice"},
                      {"ENVELOPE", EditorTab::Envelope, "editor-envelope"},
                      {"FILTER", EditorTab::Filter, "editor-filter"},
                      {"PITCH", EditorTab::Pitch, "editor-pitch"},
                      {"LFO", EditorTab::Lfo, "editor-lfo"}};
    for (const T& t : tabs) {
        CHECK(tapLabel(t.label, kWork, false, 400), "tab %s", t.label);
        CHECK(a.ui().editor == t.tab, "editor %s", t.label);
        shot(t.name);
    }
    CHECK(tapLabel("TRIM", kWork, false, 300), "TRIM tab");
    CHECK(a.ui().editor == EditorTab::Trim, "back to TRIM");

    // SAMPLE DETAILS from the pencil next to the sample name
    lvhost_tap(406, 97);
    runAudio(450);
    CHECK(modalIsOpen(), "pencil opens SAMPLE DETAILS");
    if (!modalIsOpen()) {
        dlg_sample::open();
        runAudio(450);
    }
    shot("modal-sample-details");
    closeOverlays();
    CHECK(!modalIsOpen() && !overlayOpen(), "details closed");
}

static void shotsSequence()
{
    App& a = app();
    printf("SEQUENCE view, VELOCITY lane, playing\n");
    nav(View::Sequence);
    shot("view-sequence");
    CHECK(tapLabel("VELOCITY", kFooter, false, 300), "VELOCITY button");
    CHECK(a.ui().lane == StepLane::Velocity, "lane velocity");
    shot("lane-velocity");
    CHECK(tapLabel("VELOCITY", kFooter, false, 300), "VELOCITY again");
    CHECK(a.ui().lane == StepLane::Trig, "lane back to TRIG");

    lvhost_tap(1153 + 16, 30);   // top-bar PLAY
    runAudio(1300);
    CHECK(a.playing(), "playing");
    CHECK(a.playStep() >= 0, "play step %d", a.playStep());
    shot("playing-sequence");
    lvhost_tap(1153 + 16, 30);   // pause
    runAudio(60);
    CHECK(!a.playing(), "paused");
    a.stop();   // rewind
    runAudio(60);
}

static void shotsOtherViews()
{
    App& a = app();
    printf("MIXER / BROWSER / FX\n");
    nav(View::Mixer);
    shot("view-mixer");
    nav(View::Browser);
    waitLoader();
    runAudio(300);
    CHECK(a.resultCount() >= 10, "browser entries %d", a.resultCount());
    if (lv_obj_t* l = findLabel("KICK_01", true, kWork)) {
        tapObj(l, 300);
        CHECK(a.ui().lib_sel >= 0, "entry selected");
    } else {
        CHECK(false, "KICK_01 row missing");
    }
    waitLoader();
    runAudio(300);
    shot("view-browser");
    nav(View::Fx);
    shot("view-fx");
    // ACID BASS (0.3): a random line, then a step drawn on the line and a key of the keyboard
    nav(View::Bass);
    app().bassRandom();
    runAudio(100);
    shot("view-bass");
    CHECK(app().bassStep(0).gate == 1, "RANDOM puts a note on step 1");
}

static void shotsDialogs()
{
    App& a = app();
    printf("dialogs\n");
    nav(View::Sample);
    // SETTINGS: every tab
    lvhost_tap(1020 + 15, 31);
    runAudio(400);
    CHECK(modalIsOpen(), "settings open");
    const char* const tabs[8] = {"General", "Audio", "MIDI", "Bluetooth", "Display", "Touch", "Storage", "About"};
    for (int i = 0; i < 8; ++i) {
        CHECK(tapLabel(tabs[i], kAll, false, 350), "settings tab %s", tabs[i]);
        CHECK(a.ui().settings_tab == i, "settings_tab %d (want %d)", a.ui().settings_tab, i);
        char nm[48];
        snprintf(nm, sizeof nm, "modal-settings-%s", tabs[i]);
        shot(nm);
    }
    CHECK(tapLabel("General", kAll, false, 300), "General again");
    closeOverlays();
    CHECK(!modalIsOpen(), "settings closed");
    // PROJECT MANAGER
    lvhost_tap(978 + 15, 31);
    runAudio(400);
    CHECK(modalIsOpen(), "project open");
    shot("modal-project");
    closeOverlays();
    // UI GUIDE
    lvhost_tap(1227 + 16, 30);
    runAudio(400);
    CHECK(modalIsOpen(), "help open");
    shot("modal-help");
    closeOverlays();
    CHECK(!modalIsOpen() && !overlayOpen(), "all closed");
}

// The control of a settings row (dlg_settings row(): [name + note] [control]): the row's last child.
static lv_obj_t* rowControl(const char* name)
{
    lv_obj_t* l = findLabel(name);
    lv_obj_t* left = l ? lv_obj_get_parent(l) : nullptr;
    lv_obj_t* row = left ? lv_obj_get_parent(left) : nullptr;
    return row ? lv_obj_get_child(row, -1) : nullptr;
}

// Settings > Display colours: a base swatch, CUSTOM (starts at the preset's hue), the hue slider, APPLY -> the
// restart prompt, LATER. The session's palette never changes (it applies at the next start).
static void shotsColors()
{
    App& a = app();
    printf("colours (Settings > Display)\n");
    const int base0 = baseIndex(), theme0 = themeIndex(), hue0 = themeHue(), sat0 = themeSat();
    lvhost_tap(1020 + 15, 31);
    runAudio(400);
    CHECK(modalIsOpen() && tapLabel("Display", kAll, false, 350), "settings > Display");
    lv_obj_t* bases = rowControl("Base color");
    lv_obj_t* accents = rowControl("Accent color");
    CHECK(bases && lv_obj_get_child_count(bases) == (uint32_t)kBaseCount, "base swatches");
    CHECK(accents && lv_obj_get_child_count(accents) == (uint32_t)kThemeCount + 1, "accent swatches + CUSTOM");
    if (!bases || !accents) {
        closeOverlays();
        return;
    }
    const int pick = base0 == 4 ? 5 : 4;   // DARK (BLACK when DARK is on screen)
    tapObj(lv_obj_get_child(bases, pick), 200);
    CHECK(a.settingsEdit().base == pick, "base swatch picks base %d (%d)", pick, a.settingsEdit().base);
    CHECK(findLabel(baseName(pick), true), "base note names the new base");
    lv_obj_t* accent_row = lv_obj_get_parent(accents);   // the CUSTOM row (hidden unless CUSTOM) comes next
    lv_obj_t* custom_row = lv_obj_get_child(lv_obj_get_parent(accent_row), lv_obj_get_index(accent_row) + 1);
    CHECK(custom_row && visible(custom_row) == (theme0 == kThemeCustom), "CUSTOM row only with CUSTOM");
    int ph = 0, ps = 0;
    presetHueSat(theme0 < kThemeCount ? theme0 : 0, ph, ps);
    tapObj(lv_obj_get_child(accents, kThemeCount), 200);
    CHECK(a.settingsEdit().theme == kThemeCustom, "CUSTOM swatch");
    if (theme0 < kThemeCount && sat0 == 0)
        CHECK(a.settingsEdit().accent_hue == ph && a.settingsEdit().accent_sat == ps, "CUSTOM starts at the preset (%d/%d)",
              a.settingsEdit().accent_hue, a.settingsEdit().accent_sat);
    CHECK(custom_row && visible(custom_row), "CUSTOM row shown");
    lv_obj_t* hue = nullptr;   // the HUE line: [caption, slider]
    if (lv_obj_t* l = findLabel("HUE")) hue = lv_obj_get_child(lv_obj_get_parent(l), -1);
    CHECK(hue && lv_obj_check_type(hue, &lv_slider_class), "HUE slider");
    if (hue) {
        lv_area_t s;
        lv_obj_get_coords(hue, &s);
        lvhost_tap(s.x1 + (s.x2 - s.x1) * 3 / 4, (s.y1 + s.y2) / 2);
        runAudio(200);
        const int h = a.settingsEdit().accent_hue;
        CHECK(h > 200 && h < 330, "hue slider at 3/4 (%d)", h);
    }
    shot("modal-settings-Display-custom");
    CHECK(tapLabel("APPLY", kAll, false, 500), "APPLY");
    CHECK(a.settings().base == pick && a.settings().theme == kThemeCustom, "colours saved");
    CHECK(modalIsOpen() && findLabel("RESTART NOW") && findLabel("LATER"), "restart prompt");
    shot("modal-colors");
    CHECK(tapLabel("LATER", kAll, false, 400), "LATER");
    CHECK(!modalIsOpen(), "prompt closed");
    CHECK(baseIndex() == base0 && themeIndex() == theme0, "the session keeps its palette");
    // back to the colours on screen (no prompt: they are the session's own)
    Settings& e = a.settingsEdit();
    e.base = (uint8_t)base0;
    e.theme = (uint8_t)theme0;
    e.accent_hue = (uint16_t)hue0;
    e.accent_sat = (uint8_t)sat0;
    a.applySettings();
    a.ui().settings_tab = 0;
    runAudio(3500);   // toasts fade
}

// 0.4.6 Settings > Display > Interface size: the list, APPLY -> restart prompt, then every dialog and list larger
// (settings x1.25 x1.3, capped at the screen width), larger touch areas for widgets made from then on.
static void shotsUiSize()
{
    App& a = app();
    printf("interface size (Settings > Display)\n");
    lvhost_tap(1020 + 15, 31);
    runAudio(400);
    CHECK(modalIsOpen() && tapLabel("Display", kAll, false, 350), "settings > Display");
    CHECK(modalScaleOf() > 1.24f && modalScaleOf() < 1.26f, "settings drawn x1.25 (%.3f)", modalScaleOf());
    lv_obj_t* sel = rowControl("Interface size");
    CHECK(sel != nullptr, "Interface size select");
    if (!sel) {
        closeOverlays();
        return;
    }
    tapObj(sel, 250);
    CHECK(overlayOpen() && findLabel("Extra large"), "size list open");
    if (lv_obj_t* l = findLabel("Extra large")) {
        lv_area_t r;
        lv_obj_get_coords(lv_obj_get_parent(l), &r);
        CHECK(lv_area_get_height(&r) >= 44, "list rows as large as the dialog (%d px)", (int)lv_area_get_height(&r));
    }
    shot("popup-settings-size");
    CHECK(tapLabel("Extra large", kAll, false, 250), "pick Extra large");
    CHECK(a.settingsEdit().ui_size == 2, "edit copy ui_size 2 (%d)", a.settingsEdit().ui_size);
    CHECK(a.settings().ui_size == 0, "not active before APPLY");
    CHECK(tapLabel("APPLY", kAll, false, 500), "APPLY");
    CHECK(a.settings().ui_size == 2, "ui_size saved");
    CHECK(modalIsOpen() && findLabel("INTERFACE SIZE") && findLabel("RESTART NOW"), "restart prompt (interface size)");
    CHECK(modalScaleOf() > 1.29f, "the prompt itself is larger (%.3f)", modalScaleOf());
    shot("modal-uisize");
    CHECK(tapLabel("LATER", kAll, false, 400), "LATER");
    CHECK(hitBonus() == 8, "touch bonus 8 px (%d)", hitBonus());
    // the settings again: as wide as the screen allows
    lvhost_tap(1020 + 15, 31);
    runAudio(400);
    CHECK(modalIsOpen(), "settings open (XL)");
    const float kmax = 1264.0f / 824.0f;
    CHECK(modalScaleOf() > kmax - 0.01f && modalScaleOf() < kmax + 0.01f, "settings XL capped (%.3f)", modalScaleOf());
    if (lv_obj_t* l = findLabel("SETTINGS / TAB5")) {
        lv_obj_t* win = lv_obj_get_parent(lv_obj_get_parent(l));
        lv_area_t w;
        lv_obj_get_coords(win, &w);
        CHECK(w.x1 >= 0 && w.x2 <= 1279 && w.y1 >= 0 && w.y2 <= 719, "XL window on the screen (%d,%d-%d,%d)", (int)w.x1,
              (int)w.y1, (int)w.x2, (int)w.y2);
    }
    shot("modal-settings-xl");
    CHECK(tapLabel("Touch", kAll, false, 350), "Touch tab (built at XL)");
    shot("modal-settings-xl-touch");
    // no edits: X closes it at once
    closeOverlays();
    lvhost_tap(978 + 15, 31);
    runAudio(400);
    CHECK(modalIsOpen() && modalScaleOf() > 1.29f, "project manager x1.3 (%.3f)", modalScaleOf());
    shot("modal-project-xl");
    closeOverlays();
    lvhost_tap(1227 + 16, 30);
    runAudio(400);
    CHECK(modalIsOpen() && modalScaleOf() > 1.29f, "UI guide x1.3");
    shot("modal-help-xl");
    closeOverlays();
    Settings& e = a.settingsEdit();
    e.ui_size = 0;
    a.applySettings();
    CHECK(hitBonus() == 0 && uiSizeFactor() == 1.0f, "Standard again");
    a.ui().settings_tab = 0;
    runAudio(3500);
}

static void shotsExtras()
{
    App& a = app();
    printf("keyboard / mute mode / song follow\n");
    // long press on the BPM value of the macros panel: keyboard
    longPress(1192, 524, 800);
    CHECK(overlayOpen(), "BPM keyboard open");
    shot("keyboard");
    closeOverlays();
    CHECK(!overlayOpen(), "keyboard closed");

    // MUTE mode with two muted pads
    lvhost_tap(197 + 21, 92);
    runAudio(80);
    CHECK(a.ui().pad_mode == PadMode::Mute, "pad mode MUTE");
    lvhost_tap(padX(2), padY(2));
    runAudio(60);
    lvhost_tap(padX(6), padY(6));
    runAudio(300);
    CHECK(a.project().pad[2].mute && a.project().pad[6].mute, "pads 3, 7 muted");
    shot("mute-mode");
    lvhost_tap(padX(2), padY(2));
    runAudio(60);
    lvhost_tap(padX(6), padY(6));
    runAudio(60);
    CHECK(!a.project().pad[2].mute && !a.project().pad[6].mute, "unmuted");
    lvhost_tap(197 + 21, 92);   // MUTE again: back to PLAY
    runAudio(80);
    CHECK(a.ui().pad_mode == PadMode::Play, "pad mode PLAY");

    // FOLLOW + playing: the song chain runs, the timeline follows
    CHECK(tapLabel("FOLLOW", kTimeline, false, 100), "FOLLOW button");
    CHECK(a.project().song.follow == 1, "follow on");
    nav(View::Sequence);
    a.play();
    runAudio(4800);   // slot 0 (A, 2 bars at 111 BPM = 4.3 s) -> slot 1
    CHECK(a.playing() && a.songSlot() >= 1, "song slot %d", a.songSlot());
    shot("song-follow");
    a.stop();
    a.stop();
    runAudio(60);
    CHECK(tapLabel("FOLLOW", kTimeline, false, 100), "FOLLOW off");
    CHECK(a.project().song.follow == 0, "follow off");
    nav(View::Sample);
}

// Bluetooth through the host HAL's fake Pico: the top-bar BT chip (hidden while off, dim while it waits, accent
// when connected; a tap opens Settings > Bluetooth) and the Bluetooth tab with a device list + connected AirPods.
static void shotsBluetooth()
{
    App& a = app();
    printf("bluetooth\n");
    nav(View::Sample);
    const Box top = {0, 0, 1279, 61};
    CHECK(findLabel("BT", false, top) == nullptr, "no BT chip while Bluetooth is off");
    a.btSetEnabled(true);
    runAudio(300);
    CHECK(a.engine().sampleRate() == 44100 && halHostAudioRate() == 44100, "Bluetooth on: engine + stream at 44.1 kHz (%u)",
          (unsigned)halHostAudioRate());
    lv_obj_t* chip = findLabel("BT", false, top);
    CHECK(chip != nullptr && !lv_obj_has_state(lv_obj_get_parent(chip), LV_STATE_CHECKED),
          "dim BT chip while no device is connected");
    runAudio(2800);   // the "Bluetooth on" toast fades
    shot("bt-chip-waiting");
    halHostBtAdvance(300);
    halHostBtFeed("READY version=0.9.5 cap_airpods=1");
    halHostBtFeed("DEVICE 1 A4:C3:37:5E:91:0B -127 1 1 AirPods Pro");
    halHostBtFeed("DEVICE 2 00:42:79:B1:22:EC -127 1 0 JBL Flip 6");
    halHostBtFeed("DEVICE 3 F8:4E:17:3A:6D:50 -64 0 0 Marshall Emberton");
    halHostBtFeed("END_LIST");
    halHostBtFeed("STATE connection=2 audio=2");
    halHostBtFeed("AIRPODS connected=1 mode=2 left=80 right=75 case=40 ears=3 auto_pause=0");
    halHostBtAdvance(200);
    runAudio(400);
    CHECK(a.btStatus().connected && a.btDeviceCount() == 3, "connected, %d devices", a.btDeviceCount());
    chip = findLabel("BT", false, top);
    lv_obj_t* sub = findLabel("SAMPLER / SEQUENCER", false, top);
    if (chip && sub) {
        lv_obj_t* btn = lv_obj_get_parent(chip);
        lv_area_t c, s;
        lv_obj_get_coords(btn, &c);
        lv_obj_get_coords(sub, &s);
        CHECK(lv_obj_has_state(btn, LV_STATE_CHECKED), "BT chip lit while connected");
        CHECK(c.x1 > s.x2 + 8 && c.x2 < 377 - 4 && c.y1 >= 10 && c.y2 <= 52,
              "BT chip %d..%d between the subtitle (ends %d) and the navigation (377)", (int)c.x1, (int)c.x2, (int)s.x2);
    } else {
        CHECK(false, "BT chip / subtitle not found");
    }
    shot("bt-chip");
    CHECK(tapObj(chip, 400), "BT chip tap");
    CHECK(modalIsOpen() && a.ui().settings_tab == dlg_settings::kTabBluetooth, "chip opens Settings > Bluetooth (%d)",
          a.ui().settings_tab);
    CHECK(findLabel("AirPods Pro") && findLabel("CONNECTED") && findLabel("AirPods"), "device list + AirPods row");
    runAudio(2800);   // the connect toast fades
    shot("modal-settings-Bluetooth-connected");
    closeOverlays();
    a.btSetEnabled(false);
    runAudio(300);
    CHECK(findLabel("BT", false, top) == nullptr, "BT chip hidden again");
    CHECK(a.engine().sampleRate() == 48000 && halHostAudioRate() == 48000, "Bluetooth off: back at 48 kHz");
    runAudio(2800);
}

// ------------------------------------------------------------------ interaction smoke test
static void smokeNav()
{
    printf("smoke: nav buttons\n");
    const View order[] = {View::Sequence, View::Bass, View::Mixer, View::Browser, View::Fx, View::Sample};
    for (View v : order) nav(v);
}

static void smokePads()
{
    App& a = app();
    printf("smoke: every pad + multi-touch\n");
    for (int i = 0; i < 16; ++i) {
        a.engine().telemetry().pad_hits.store(0);
        lvhost_tap(padX(i), padY(i));
        halHostRenderAudio(128, g_audio);
        runAudio(60);
        CHECK(a.ui().pad == i, "pad %d selected (got %d)", i, a.ui().pad);
    }
    lvhost_tap(padX(0), padY(0));
    runAudio(300);
    // touch hook (touch task path): two fingers play two pads at once
    a.engine().telemetry().pad_hits.store(0);
    hal::TouchPoint two[2] = {{(int16_t)padX(5), (int16_t)padY(5), 0, 0}, {(int16_t)padX(10), (int16_t)padY(10), 1, 0}};
    halHostTouch(two, 2);
    halHostRenderAudio(128, g_audio);
    const uint32_t hits = a.engine().telemetry().pad_hits.load();
    CHECK((hits & (1u << 5)) && (hits & (1u << 10)), "touch hits 0x%x", (unsigned)hits);
    halHostTouch(two, 0);
    runAudio(100);
    lvhost_tap(padX(0), padY(0));
    runAudio(200);
    CHECK(a.ui().pad == 0, "pad 0 again");
}

// Screen point of design point (x, y) of the 1280x720 layout inside maximized panel p, whose design top-left
// corner is (px, py): the panel is laid out natively larger (ui_scale.h), sx / sy from its registration.
static lv_point_t maxPoint(Panel p, int x, int y, int px, int py)
{
    lv_obj_t* r = panelRoot(p);
    lv_area_t ar;
    lv_obj_get_coords(r, &ar);
    const UiScale s = uiScaleOf(r);
    return lv_point_t{ar.x1 + (int32_t)lroundf((float)(x - px) * s.sx), ar.y1 + (int32_t)lroundf((float)(y - py) * s.sy)};
}

// 0.3: maximize (natively larger since 0.3.2). LVGL taps, the touch task and the step grid's / keyboard's own
// hit testing must land on the right pad / cell / key; shots of every maximized panel and of the BASS view on
// the whole screen.
// ui_scale.h on its own: the font ladder and the subtree re-layout of a small hand-made tree.
static void testUiScale()
{
    printf("ui_scale: fonts + tree\n");
    CHECK(fontScaled(f_r13, 1.0f) == f_r13 && fontScaled(f_m20, 0.8f) == f_m20, "no larger font at scale <= 1");
    const lv_font_t* m = fontScaled(f_m20, 1.48f);   // 29.6 px -> the 28 px medium
    CHECK(m != f_m20 && lv_font_get_line_height(m) > lv_font_get_line_height(f_m20), "m20 x1.48 is larger");
    CHECK(lv_font_get_line_height(fontScaled(f_m24, 2.0f)) > lv_font_get_line_height(fontScaled(f_m24, 1.48f)),
          "the ladder keeps growing");
    lv_obj_t* root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, 10, 20);
    lv_obj_set_size(root, 200, 100);
    lv_obj_set_style_pad_left(root, 4, 0);
    lv_obj_set_style_radius(root, LV_RADIUS_CIRCLE, 0);
    lv_obj_t* l = label(root, "Ab", f_s13, col::ink);
    lv_obj_set_pos(l, 30, 40);
    lv_obj_t* b = actionBtn(root, "OK", false, 60, 34);   // shared styles: pad_hor 14, radius 5, font m13
    lv_obj_set_pos(b, 100, 50);
    const UiScale s{1.5f, 1.25f, 1.25f};
    uiScaleRegister(root, s);
    uiScaleTree(root, s);
    lv_obj_update_layout(root);
    CHECK(lv_obj_get_width(root) == 300 && lv_obj_get_height(root) == 125, "root size %dx%d", (int)lv_obj_get_width(root),
          (int)lv_obj_get_height(root));
    CHECK(lv_obj_get_style_pad_left(root, 0) == 6, "padding along x");
    CHECK(lv_obj_get_style_radius(root, 0) == LV_RADIUS_CIRCLE, "circle radius kept");
    CHECK(lv_obj_get_x(l) == 45 && lv_obj_get_y(l) == 50, "child position inside the padding (%d,%d)",
          (int)lv_obj_get_x(l), (int)lv_obj_get_y(l));
    CHECK(lv_obj_get_style_text_font(l, 0) == fontScaled(f_s13, 1.25f) && fontScaled(f_s13, 1.25f) != f_s13,
          "label font swapped");
    CHECK(lv_obj_get_style_pad_left(b, 0) == 21 && lv_obj_get_style_radius(b, 0) == 6, "shared style props scaled (%d, %d)",
          (int)lv_obj_get_style_pad_left(b, 0), (int)lv_obj_get_style_radius(b, 0));
    CHECK(lv_obj_get_style_text_font(b, 0) == fontScaled(f_m13, 1.25f), "button font from the shared style swapped");
    CHECK(uiScaleOf(l).sx == 1.5f && uiScaleOf(lv_screen_active()).unit(), "registry finds the panel of a child only");
    lv_obj_delete(root);
    lv_obj_t* probe = lv_obj_create(lv_screen_active());   // may reuse root's memory: must not inherit its scale
    CHECK(uiScaleOf(probe).unit(), "registry entry dropped with the root");
    lv_obj_delete(probe);
}

static void smokeMaximize()
{
    App& a = app();
    printf("smoke: maximize panels\n");
    testUiScale();
    panelToggleMax(Panel::Pads);
    runAudio(100);
    CHECK(panelMaxed() == Panel::Pads, "pads maximized");
    CHECK(uiScaleOf(panelRoot(Panel::Pads)).f > 1.5f, "pads laid out larger (f %.2f)", (double)uiScaleOf(panelRoot(Panel::Pads)).f);
    shot("max-pads");
    auto mx = [&](int i) { return (int)maxPoint(Panel::Pads, padX(i), padY(i), geo::pads_x, geo::pads_y).x; };
    auto my = [&](int i) { return (int)maxPoint(Panel::Pads, padX(i), padY(i), geo::pads_x, geo::pads_y).y; };
    lvhost_tap(mx(6), my(6));
    runAudio(300);
    CHECK(a.ui().pad == 6, "maximized pad tap selects pad 6 (got %d)", a.ui().pad);
    a.engine().telemetry().pad_hits.store(0);
    hal::TouchPoint one[1] = {{(int16_t)mx(9), (int16_t)my(9), 0, 0}};
    halHostTouch(one, 1);
    halHostRenderAudio(128, g_audio);
    CHECK(a.engine().telemetry().pad_hits.load() & (1u << 9), "touch task hits pad 9 while maximized");
    halHostTouch(one, 0);
    runAudio(100);
    panelToggleMax(Panel::Pads);
    runAudio(100);
    CHECK(panelMaxed() == Panel::Count, "pads restored");
    a.engine().telemetry().pad_hits.store(0);
    halHostTouch(one, 0);
    lvhost_tap(padX(0), padY(0));
    runAudio(300);
    CHECK(a.ui().pad == 0, "normal pads after restore");

    // work area: the button after the last view tab (top bar, not scaled)
    nav(View::Sequence);
    const int bx = 377 + (int)View::Count * 92 + 2 + 13, by = 13 + 4 + 13;
    lvhost_tap(bx, by);
    runAudio(150);
    CHECK(panelMaxed() == Panel::Work, "work maximized");
    shot("max-work");
    const int cx = step_grid::colX(2) + step_grid::colW(2) / 2, cy = step_grid::rowY(1) + step_grid::kCellH / 2;
    const int pad = a.ui().seq_bank * step_grid::kRows + 1, st = a.ui().seq_page * kStepsPerPage + 2;
    const int on0 = a.step(pad, st).on;
    const lv_point_t cp = maxPoint(Panel::Work, cx, cy, geo::work_x, geo::work_y);
    lvhost_tap(cp.x, cp.y);
    runAudio(100);
    CHECK(a.step(pad, st).on != on0, "scaled step tap toggles row 1 step 3");
    a.setStepOn(pad, st, on0 != 0);
    lvhost_tap(bx, by);
    runAudio(150);
    CHECK(panelMaxed() == Panel::Count, "work restored");

    // BASS on the whole screen (its own corner-marks button, right of PLAY), a key, and back
    nav(View::Bass);
    runAudio(100);
    const int kbx = 1039 - 26 + 13, kby = 87 + 4 + 13;   // the button's centre (design px)
    lvhost_tap(kbx, kby);
    runAudio(150);
    CHECK(panelMaxed() == Panel::Work && panelMaxedFull(), "BASS on the whole screen");
    lv_area_t wa;
    lv_obj_get_coords(panelRoot(Panel::Work), &wa);
    CHECK(wa.x1 <= 1 && wa.y1 <= 1 && wa.x2 >= 1278 && wa.y2 >= 718, "work panel covers the screen (%d,%d %d,%d)",
          (int)wa.x1, (int)wa.y1, (int)wa.x2, (int)wa.y2);
    shot("max-bass-full");
    {
        const int sel = a.ui().seq_page * kStepsPerPage;   // the view selects the page's first step on show
        BassStep b0 = a.bassStep(sel);
        b0.gate = 0;
        a.setBassStep(sel, b0);
        // white key E (index 2 of 8) of the keyboard 329..1039 x 348..424, low half (below the black keys)
        const int kx = 329 + 2 * 710 / 8 + 710 / 16, ky = 348 + 66;
        const lv_point_t kp = maxPoint(Panel::Work, kx, ky, geo::work_x, geo::work_y);
        lvhost_tap(kp.x, kp.y);
        runAudio(120);
        const BassStep& b1 = a.bassStep(sel);
        CHECK(b1.gate && ((b1.note % 12) + 12) % 12 == 4, "full-screen key E sets the step (gate %d note %d)", b1.gate,
              b1.note);
    }
    const lv_point_t rp = maxPoint(Panel::Work, kbx, kby, geo::work_x, geo::work_y);
    lvhost_tap(rp.x, rp.y);
    runAudio(150);
    CHECK(panelMaxed() == Panel::Count && !panelMaxedFull(), "BASS back from the whole screen");
    // every other view in the maximized work area (built natively larger in the stand-in panel, ui.cpp createView)
    const View views[] = {View::Sample, View::Mixer, View::Browser, View::Fx, View::Bass};
    const char* const vnames[] = {"max-work-sample", "max-work-mixer", "max-work-browser", "max-work-fx", "max-work-bass"};
    for (int i = 0; i < 5; ++i) {
        nav(views[i]);
        panelToggleMax(Panel::Work);
        runAudio(120);
        CHECK(panelMaxed() == Panel::Work, "%s maximized", vnames[i]);
        shot(vnames[i]);
        panelToggleMax(Panel::Work);
        runAudio(60);
    }
    nav(View::Sequence);
    const Panel others[] = {Panel::Timeline, Panel::Macros, Panel::Browser};
    const char* const names[] = {"max-timeline", "max-macros", "max-browser"};
    for (int i = 0; i < 3; ++i) {
        panelToggleMax(others[i]);
        runAudio(100);
        shot(names[i]);
        panelToggleMax(others[i]);
        runAudio(60);
    }
    CHECK(panelMaxed() == Panel::Count, "all restored");
    // 0.4.6: a category of the maximized BROWSER panel opens the BROWSER view maximized at once
    {
        const Category cat0 = a.ui().category;
        panelToggleMax(Panel::Browser);
        runAudio(100);
        CHECK(panelMaxed() == Panel::Browser, "BROWSER panel maximized");
        CHECK(tapLabel("Drums", kAll, false, 200), "Drums in the maximized panel");
        CHECK(panelMaxed() == Panel::Work && a.ui().view == View::Browser && a.ui().category == Category::Drums,
              "Drums opens the BROWSER view maximized (max %d view %d cat %d)", (int)panelMaxed(), (int)a.ui().view,
              (int)a.ui().category);
        shot("max-browser-drums");
        // the category list next to the search switches the category without leaving the large view
        const Box catBox = {540, 95, 800, 160};
        CHECK(tapLabel("Drums", catBox, false, 250), "category list shows Drums");
        CHECK(overlayOpen() && tapLabel("Bass", kAll, false, 250), "pick Bass from the list");
        CHECK(a.ui().category == Category::Bass && panelMaxed() == Panel::Work, "Bass in the large view (cat %d)",
              (int)a.ui().category);
        CHECK(findLabel("Bass", false, catBox) != nullptr, "category list shows Bass");
        panelToggleMax(Panel::Work);
        runAudio(60);
        CHECK(panelMaxed() == Panel::Count, "restored after Drums");
        a.setCategory(cat0);
        nav(View::Sequence);
    }
}

// 0.4.2: BASS KEYS. The head's KEYS toggle swaps the line / step row / step keyboard for a row (OCT - / +, ACCENT,
// SLIDE, SAW / SQR) and a two-octave keyboard that only plays: nothing is written into the line. Moving the finger
// to another key plays it (slides with SLIDE); OCT moves the range; the mode stays on the whole screen.
static void smokeBassKeys()
{
    App& a = app();
    printf("smoke: BASS KEYS\n");
    nav(View::Bass);
    if (a.playing()) a.togglePlay();
    runAudio(100);
    const Box head = {329, 84, 1039, 121}, row = {329, 134, 1039, 168};
    CHECK(tapLabel("KEYS", head), "KEYS toggle");
    CHECK(findLabel("LIVE \xC2\xB7 NOT RECORDED", false, row) != nullptr, "KEYS row shown");
    CHECK(findLabel("GATE", false, kWork) == nullptr, "step row hidden in KEYS");
    CHECK(findLabel("303 STYLE \xC2\xB7 LIVE KEYS", false, head) != nullptr, "KEYS title");
    CHECK(findLabel("C2 \xE2\x80\x93 C4", false, row) != nullptr, "range C2 - C4");
    // 0.4.3: the keyboard is the touch task's exclusive zone (LVGL follows the other fingers)
    int zx1 = 0, zy1 = 0, zx2 = 0, zy2 = 0;
    CHECK(halHostExclusive(&zx1, &zy1, &zx2, &zy2) && zx1 == 329 && zy1 == 176 && zx2 == 329 + 710 - 1 &&
              zy2 == 176 + 248 - 1,
          "KEYS: exclusive touch zone on the keyboard (%d,%d %d,%d)", zx1, zy1, zx2, zy2);
    BassStep line[kMaxSteps];
    for (int s = 0; s < kMaxSteps; ++s) line[s] = a.bassStep(s);
    const uint32_t rev0 = a.revs().pattern;
    const uint32_t n0 = a.bassNotes();
    // white keys (15 over 329..1039 x 176..424): low half, below the black keys
    auto wx = [](int i) { return 329 + i * 710 / 15 + 710 / 30; };
    const int ky = 176 + 210;
    lvhost_pointer(wx(2), ky, true);   // E2
    runAudio(80);
    CHECK(a.bassNotes() == n0 + 1, "a key plays a note (%u)", (unsigned)(a.bassNotes() - n0));
    shot("view-bass-keys");
    lvhost_pointer(wx(4), ky, true);   // on to G2 (slides)
    runAudio(80);
    CHECK(a.bassNotes() == n0 + 2, "moving on plays the next key (%u)", (unsigned)(a.bassNotes() - n0));
    shot("view-bass-keys-slide");   // only the two keys redrawn: E2 white again, G2 lit
    lvhost_pointer(wx(4), ky, false);
    runAudio(80);
    lvhost_tap(329 + 710 / 15 - 2, 176 + 60);   // the black key C#2
    runAudio(60);
    CHECK(a.bassNotes() == n0 + 3, "a black key plays (%u)", (unsigned)(a.bassNotes() - n0));
    // several fingers (the touch task's hook): the newest key sounds, lifting it goes back to the held one
    {
        const uint32_t m0 = a.bassNotes();
        const hal::TouchPoint e2 = {(int16_t)wx(2), (int16_t)ky, 0, 0}, g2 = {(int16_t)wx(4), (int16_t)ky, 1, 0};
        halHostTouch(&e2, 1);
        runAudio(40);
        CHECK(a.bassNotes() == m0 + 1, "finger 1 plays E2 (%u)", (unsigned)(a.bassNotes() - m0));
        const hal::TouchPoint both[2] = {e2, g2};
        halHostTouch(both, 2);
        runAudio(40);
        CHECK(a.bassNotes() == m0 + 2, "finger 2 adds G2: it sounds (%u)", (unsigned)(a.bassNotes() - m0));
        halHostTouch(both, 2);   // the same sample again: nothing new
        runAudio(40);
        CHECK(a.bassNotes() == m0 + 2, "held fingers start nothing new (%u)", (unsigned)(a.bassNotes() - m0));
        shot("view-bass-keys-two");   // G2 sounding, E2 held (dimmer)
        halHostTouch(&e2, 1);         // G2 lifted: back to E2
        runAudio(40);
        CHECK(a.bassNotes() == m0 + 3, "lifting G2 goes back to E2 (%u)", (unsigned)(a.bassNotes() - m0));
        halHostTouch(nullptr, 0);
        runAudio(120);
        CHECK(a.bassNotes() == m0 + 3, "all fingers up: no new note");
        // a finger that came down outside the keyboard never plays it, even when it slides onto it
        const hal::TouchPoint knob = {(int16_t)(329 + 40), (int16_t)460, 2, 0};
        const hal::TouchPoint slid = {(int16_t)wx(6), (int16_t)ky, 2, 0};
        halHostTouch(&knob, 1);
        halHostTouch(&slid, 1);
        runAudio(40);
        halHostTouch(nullptr, 0);
        runAudio(40);
        CHECK(a.bassNotes() == m0 + 3, "a finger from outside does not play (%u)", (unsigned)(a.bassNotes() - m0));
    }
    bool same = a.revs().pattern == rev0;
    for (int s = 0; s < kMaxSteps; ++s) {
        const BassStep& b = a.bassStep(s);
        same = same && b.gate == line[s].gate && b.note == line[s].note && b.accent == line[s].accent &&
               b.slide == line[s].slide;
    }
    CHECK(same, "KEYS writes nothing into the line");
    CHECK(tapLabel("OCT +", row) && findLabel("C3 \xE2\x80\x93 C5", false, row) != nullptr, "OCT + -> C3 - C5");
    CHECK(tapLabel("OCT -", row) && tapLabel("OCT -", row) && findLabel("C1 \xE2\x80\x93 C3", false, row) != nullptr,
          "OCT - twice -> C1 - C3");
    // 0.4.6: an octave more each way: C0 .. C5 for the lowest key
    CHECK(tapLabel("OCT -", row) && findLabel("C0 \xE2\x80\x93 C2", false, row) != nullptr, "OCT - -> C0 - C2");
    CHECK(tapLabel("OCT -", row) && findLabel("C0 \xE2\x80\x93 C2", false, row) != nullptr, "C0 is the lowest");
    for (int i = 0; i < 6; ++i) tapLabel("OCT +", row);
    CHECK(findLabel("C5 \xE2\x80\x93 C7", false, row) != nullptr, "C5 is the highest");
    CHECK(a.bassStep(0).note >= kBassNoteMin && kBassNoteMin == -24 && kBassNoteMax == 36, "line range C0..C5");
    for (int i = 0; i < 3; ++i) tapLabel("OCT -", row);
    CHECK(findLabel("C2 \xE2\x80\x93 C4", false, row) != nullptr, "back to C2 - C4");
    // the line plays on (its note lights its key)
    a.togglePlay();
    runAudio(260);
    shot("view-bass-keys-play");
    a.togglePlay();
    runAudio(100);
    // the zone belongs to the keyboard only while it is on screen: another view, a dialog
    nav(View::Sequence);
    CHECK(!halHostExclusive(nullptr, nullptr, nullptr, nullptr), "another view: no exclusive zone");
    nav(View::Bass);
    runAudio(60);
    CHECK(halHostExclusive(nullptr, nullptr, nullptr, nullptr), "BASS KEYS again: zone back");
    lvhost_tap(1020 + 15, 31);   // SETTINGS
    runAudio(400);
    CHECK(modalIsOpen() && !halHostExclusive(nullptr, nullptr, nullptr, nullptr), "a dialog: no exclusive zone");
    closeOverlays();
    runAudio(60);
    CHECK(halHostExclusive(nullptr, nullptr, nullptr, nullptr), "dialog closed: zone back");
    // the whole screen keeps KEYS; a key there plays
    const int kbx = 1039 - 26 + 13, kby = 87 + 4 + 13;
    lvhost_tap(kbx, kby);
    runAudio(150);
    CHECK(panelMaxedFull(), "BASS KEYS on the whole screen");
    CHECK(findLabel("LIVE \xC2\xB7 NOT RECORDED") != nullptr, "KEYS row on the whole screen");
    {
        const lv_point_t z1 = maxPoint(Panel::Work, 329, 176, geo::work_x, geo::work_y);
        CHECK(halHostExclusive(&zx1, &zy1, &zx2, &zy2) && abs(zx1 - z1.x) <= 2 && abs(zy1 - z1.y) <= 2 && zx2 > 1100,
              "full screen: the zone follows the larger keyboard (%d,%d %d,%d)", zx1, zy1, zx2, zy2);
    }
    const uint32_t n1 = a.bassNotes();
    const lv_point_t kp = maxPoint(Panel::Work, wx(9), ky, geo::work_x, geo::work_y);   // E3
    lvhost_pointer(kp.x, kp.y, true);
    runAudio(80);
    CHECK(a.bassNotes() == n1 + 1, "full-screen key plays (%u)", (unsigned)(a.bassNotes() - n1));
    shot("max-bass-keys");
    lvhost_pointer(kp.x, kp.y, false);
    runAudio(80);
    // 0.4.3: only the shown mode is built; KEYS off on the whole screen builds the step editor there, larger
    {
        const lv_point_t mp = maxPoint(Panel::Work, 589 + 35, 87 + 17, geo::work_x, geo::work_y);   // KEYS
        lvhost_tap(mp.x, mp.y);
        runAudio(150);
        lv_obj_t* gate = findLabel("GATE");
        lv_obj_t* gbtn = gate ? lv_obj_get_parent(gate) : nullptr;
        const lv_point_t g1 = maxPoint(Panel::Work, 329, 306, geo::work_x, geo::work_y);
        lv_area_t ga = {0, 0, 0, 0};
        if (gbtn) lv_obj_get_coords(gbtn, &ga);
        CHECK(gbtn && abs(ga.x1 - g1.x) <= 2 && abs(ga.y1 - g1.y) <= 2 && lv_area_get_width(&ga) > 76 + 20,
              "full screen: the step row built later is laid out larger (%d,%d w %d)", (int)ga.x1, (int)ga.y1,
              (int)lv_area_get_width(&ga));
        CHECK(!halHostExclusive(nullptr, nullptr, nullptr, nullptr), "full screen, KEYS off: no zone");
        shot("max-bass-steps-later");
        lvhost_tap(mp.x, mp.y);   // KEYS again
        runAudio(150);
        CHECK(findLabel("LIVE \xC2\xB7 NOT RECORDED") != nullptr && halHostExclusive(nullptr, nullptr, nullptr, nullptr),
              "full screen: KEYS back");
    }
    const lv_point_t rp = maxPoint(Panel::Work, kbx, kby, geo::work_x, geo::work_y);
    lvhost_tap(rp.x, rp.y);
    runAudio(150);
    CHECK(panelMaxed() == Panel::Count, "BASS KEYS back from the whole screen");
    CHECK(a.revs().pattern == rev0, "still nothing written");
    // KEYS off: the step editor again
    CHECK(tapLabel("KEYS", head) && findLabel("GATE", false, kWork) != nullptr, "KEYS off: step row again");
    CHECK(findLabel("LIVE \xC2\xB7 NOT RECORDED") == nullptr, "KEYS row hidden");
    CHECK(!halHostExclusive(nullptr, nullptr, nullptr, nullptr), "KEYS off: no exclusive zone");
}

// 0.4.4: the test console's commands (tools/device_test.py drives the Tab5 with them): ping / state / tap through
// the touch path (here the hook directly: no touch task on the PC).
// 0.4.6: the Tab5 Keyboard with Ableton Live's shortcuts (keys injected as the keyboard driver would queue them)
static hal::KeyEvent keyEv(hal::Key k, char ch = 0, bool ctrl = false, bool shift = false, bool alt = false)
{
    hal::KeyEvent e = {};
    e.key = k;
    e.ch = ch;
    e.ctrl = ctrl;
    e.shift = shift;
    e.alt = alt;
    return e;
}
static void key(const hal::KeyEvent& e, int ms = 80)
{
    hal::kbdInject(e);
    runAudio(ms);
}
static void keyChar(char c, bool ctrl = false, bool shift = false, bool alt = false)
{
    key(keyEv(hal::Key::Char, c, ctrl, shift, alt));
}

static char g_typed[64];
static void typedCb(const char* t, void*) { snprintf(g_typed, sizeof g_typed, "%s", t); }

static void smokeKeyboard()
{
    App& a = app();
    printf("smoke: Tab5 Keyboard (Ableton shortcuts)\n");
    nav(View::Sequence);
    // Tab / Shift+Tab: the views in tab order
    key(keyEv(hal::Key::Tab), 300);
    CHECK(a.ui().view == View::Bass, "Tab: SEQUENCE -> BASS (%d)", (int)a.ui().view);
    key(keyEv(hal::Key::Tab, 0, false, true), 300);
    CHECK(a.ui().view == View::Sequence, "Shift+Tab: back to SEQUENCE");
    // Space: play from the start / stop
    keyChar(' ');
    CHECK(a.playing(), "Space plays");
    keyChar(' ');
    CHECK(!a.playing(), "Space stops");
    // note keys: A = pad 1, W = pad 2 (computer MIDI keyboard)
    a.engine().telemetry().pad_hits.store(0);
    hal::kbdInject(keyEv(hal::Key::Char, 'w'));
    runAudio(10);
    halHostRenderAudio(128, g_audio);
    CHECK(a.engine().telemetry().pad_hits.load() & (1u << 1), "W plays pad 2");
    key(keyEv(hal::Key::Release));
    // C / V velocity, M off: W no longer plays
    keyChar('m');
    a.engine().telemetry().pad_hits.store(0);
    keyChar('w');
    halHostRenderAudio(128, g_audio);
    CHECK(!(a.engine().telemetry().pad_hits.load() & (1u << 1)), "M: note keys off");
    keyChar('m');
    // in BASS the note keys play the bass; Z / X move the range
    nav(View::Bass);
    const uint32_t n0 = a.bassNotes();
    keyChar('a');
    keyChar('d');   // a second key while A is held (no release between): slides
    key(keyEv(hal::Key::Release));
    CHECK(a.bassNotes() == n0 + 2, "BASS: A, D play two notes (%u)", (unsigned)(a.bassNotes() - n0));
    const int b0 = view_bass::liveBase();
    keyChar('x');
    CHECK(view_bass::liveBase() == b0 + 12, "X: octave up");
    keyChar('z');
    CHECK(view_bass::liveBase() == b0, "Z: octave down");
    // Up / Down: scenes
    nav(View::Sequence);
    key(keyEv(hal::Key::Down), 200);
    CHECK(a.project().scene == 1, "Down: scene B");
    key(keyEv(hal::Key::Up), 200);
    CHECK(a.project().scene == 0, "Up: scene A");
    // Left / Right: pads; F1: mute pad 1 of the bank; 0: mute the selected pad
    key(keyEv(hal::Key::Right));
    CHECK(a.ui().pad == 1, "Right: pad 2");
    key(keyEv(hal::Key::Left));
    CHECK(a.ui().pad == 0, "Left: pad 1");
    const int m0 = a.project().pad[0].mute;
    key(keyEv(hal::Key::F1));
    CHECK(a.project().pad[0].mute != m0, "F1 mutes pad 1");
    keyChar('0');
    CHECK(a.project().pad[0].mute == m0, "0 toggles the selected pad back");
    // Ctrl+, settings; Esc closes
    keyChar(',', true);
    runAudio(300);
    CHECK(modalIsOpen(), "Ctrl+, opens the settings");
    key(keyEv(hal::Key::Esc), 300);
    CHECK(!modalIsOpen(), "Esc closes them");
    // F11 / Esc: the work area maximized and back
    key(keyEv(hal::Key::F11), 200);
    CHECK(panelMaxed() == Panel::Work, "F11 maximizes the work area");
    key(keyEv(hal::Key::Esc), 200);
    CHECK(panelMaxed() == Panel::Count, "Esc brings it back");
    // typing into the on-screen keyboard
    g_typed[0] = 0;
    keyboardOpen("TEST", "", 20, typedCb, nullptr);
    runAudio(100);
    keyChar('a');
    keyChar('B', false, true);
    keyChar('c');
    key(keyEv(hal::Key::Backspace));
    key(keyEv(hal::Key::Enter), 200);
    CHECK(!strcmp(g_typed, "aB"), "typing: a, B, c, Backspace, Enter -> \"%s\"", g_typed);
    CHECK(!overlayOpen(), "the on-screen keyboard closed");
}

// 0.4.6: the top bar's touch targets reach the top edge and split the gaps (PLAY 1153..1188, REC 1193..1228)
static void smokeTopBar()
{
    App& a = app();
    printf("smoke: top bar touch targets\n");
    nav(View::Sequence);
    lvhost_tap(navX((int)View::Sample), 1);   // the very top of the screen above the SAMPLE tab
    runAudio(300);
    CHECK(a.ui().view == View::Sample, "tab tapped at y 1");
    nav(View::Sequence);
    const bool rec0 = a.recording();
    lvhost_tap(1190, 3);   // in the gap, PLAY's half, near the top
    runAudio(120);
    CHECK(a.playing() && a.recording() == rec0, "x 1190: PLAY, not REC");
    lvhost_tap(1190, 58);
    runAudio(120);
    CHECK(!a.playing(), "x 1190, y 58: PLAY again (stop)");
    lvhost_tap(1192, 30);   // REC's half of the gap
    runAudio(120);
    CHECK(a.recording() != rec0 && !a.playing(), "x 1192: REC");
    lvhost_tap(1192, 30);
    runAudio(120);
    CHECK(a.recording() == rec0, "REC off again");
    // a finger that slides a little while pressing still taps
    lvhost_drag(1020 + 15, 31, 1020 + 15 + 14, 31 + 12, 120);
    runAudio(400);
    CHECK(modalIsOpen(), "settings open after a sliding tap on the gear");
    closeOverlays();
}

static void smokeConsole()
{
    App& a = app();
    printf("smoke: test console\n");
    nav(View::Sequence);
    testcon::handleLine("ping");
    testcon::handleLine("state");
    testcon::handleLine("audio");
    testcon::handleLine("bogus");
    a.engine().telemetry().pad_hits.store(0);
    char cmd[48];
    snprintf(cmd, sizeof cmd, "tap %d %d", padX(3), padY(3));
    testcon::handleLine(cmd);   // touch + release through hal::touchInject -> the pads' hook
    halHostRenderAudio(128, g_audio);
    CHECK(a.engine().telemetry().pad_hits.load() & (1u << 3), "console tap plays pad 4");
    runAudio(100);
    lvhost_tap(padX(0), padY(0));   // pad 1 selected again for the tests after this one
    runAudio(200);
    CHECK(a.ui().pad == 0, "pad 1 again after the console");
}

static void smokeSteps()
{
    App& a = app();
    printf("smoke: step toggles + drag paint\n");
    nav(View::Sequence);
    const int scene = a.project().scene;
    CHECK(a.ui().seq_bank == 0 && a.ui().seq_page == 0, "bank 0 bar 1");
    // single toggles in rows 1 and 4
    const bool was = a.step(1, 2).on;
    lvhost_tap(stepX(2), stepY(1));
    runAudio(60);
    CHECK(a.step(1, 2).on != was, "pad 2 step 3 toggled");
    lvhost_tap(stepX(2), stepY(1));
    runAudio(60);
    CHECK(a.step(1, 2).on == was, "pad 2 step 3 back");
    const bool was4 = a.step(4, 9).on;
    lvhost_tap(stepX(9), stepY(4));
    runAudio(60);
    CHECK(a.step(4, 9).on != was4, "pad 5 step 10 toggled");
    lvhost_tap(stepX(9), stepY(4));
    runAudio(60);
    // drag paint across row 6 (pad 7): first cell decides
    Step saved[16];
    for (int s = 0; s < 16; ++s) saved[s] = a.step(6, s);
    if (a.step(6, 0).on) a.setStepOn(6, 0, false);
    runAudio(40);
    lvhost_drag(stepX(0), stepY(6), stepX(11), stepY(6), 300);
    runAudio(80);
    int on = 0;
    for (int s = 0; s <= 11; ++s) on += a.step(6, s).on ? 1 : 0;
    CHECK(on == 12, "drag painted %d of 12", on);
    for (int s = 0; s < 16; ++s) a.setStepOn(6, s, saved[s].on != 0);
    runAudio(60);
    CHECK(a.project().scene == scene, "scene unchanged");
    // RANDOM + UNDO (pattern undo of the App)
    Step before[16];
    for (int s = 0; s < 16; ++s) before[s] = a.step(0, s);
    CHECK(tapLabel("CLEAR", kWork, false, 150), "CLEAR");
    int any = 0;
    for (int s = 0; s < 16; ++s) any += a.step(0, s).on;
    CHECK(any == 0, "cleared");
    CHECK(a.canUndoPattern(), "undo available");
    if (!tapLabel("UNDO", kWork, false, 150)) a.undoPattern();
    runAudio(60);
    int same = 0;
    for (int s = 0; s < 16; ++s) same += a.step(0, s).on == before[s].on;
    CHECK(same == 16, "undo restored (%d/16)", same);
    nav(View::Sample);
}

static void smokeKnob()
{
    App& a = app();
    printf("smoke: macro knob drag\n");
    const ParamRef vol = ParamRef::outVolume();   // the device output level (0.3)
    const float v0 = a.paramNorm(vol);
    lvhost_drag(1165, 160, 1165, 100, 300);
    runAudio(100);
    const float v1 = a.paramNorm(vol);
    CHECK(v1 > v0 + 0.02f, "VOLUME up %.3f -> %.3f", v0, v1);
    lvhost_drag(1165, 100, 1165, 220, 300);
    runAudio(100);
    CHECK(a.paramNorm(vol) < v1 - 0.02f, "VOLUME down %.3f", a.paramNorm(vol));
    a.paramSetNorm(vol, v0);
    runAudio(60);
}

static void smokeDialogs()
{
    App& a = app();
    printf("smoke: dialogs open/close\n");
    struct D {
        int x, y;
        const char* what;
    };
    const D ds[] = {{978 + 15, 31, "project"}, {1020 + 15, 31, "settings"}, {1227 + 16, 30, "help"}};
    for (const D& d : ds) {
        lvhost_tap(d.x, d.y);
        runAudio(350);
        CHECK(modalIsOpen(), "%s open", d.what);
        modalClose();
        runAudio(350);
        CHECK(!modalIsOpen() && !overlayOpen(), "%s closed", d.what);
    }
    // settings: APPLY path (brightness change) and the X / discard path
    lvhost_tap(1020 + 15, 31);
    runAudio(350);
    CHECK(tapLabel("Display", kAll, false, 300), "Display tab");
    CHECK(tapLabel("APPLY", kAll, false, 400), "APPLY");
    CHECK(!modalIsOpen(), "APPLY closes");
    runAudio(300);
    // OPEN UI GUIDE from About replaces the settings modal
    lvhost_tap(1020 + 15, 31);
    runAudio(350);
    CHECK(tapLabel("About", kAll, false, 300), "About tab");
    if (tapLabel("OPEN UI GUIDE", kAll, false, 400)) CHECK(modalIsOpen(), "guide open from About");
    closeOverlays();
    // SAMPLE DETAILS from the SAMPLE view
    nav(View::Sample);
    lvhost_tap(406, 97);
    runAudio(400);
    CHECK(modalIsOpen(), "details open");
    closeOverlays();
    CHECK(!overlayOpen(), "details closed");
    (void)a;
}

static void smokeBrowser()
{
    App& a = app();
    printf("smoke: browser LOAD TO pad / OPEN IN SAMPLER / categories\n");
    lvhost_tap(padX(4), padY(4));   // B1
    runAudio(200);
    CHECK(a.ui().pad == 4, "pad B1");
    nav(View::Browser);
    lv_obj_t* row = findLabel("SNARE_02", true, kWork);
    CHECK(row != nullptr, "SNARE_02 row");
    tapObj(row, 300);
    CHECK(tapLabel("LOAD TO", kWork, true, 200), "LOAD TO button");
    waitLoader();
    runAudio(200);
    const char* nm = a.padSampleName(4);
    CHECK(nm && strstr(nm, "SNARE_02"), "pad B1 sample '%s'", nm ? nm : "(null)");
    // LOAD & EDIT B1: replaces SNARE_02 only after a second tap, then shows the SAMPLE view
    row = findLabel("NEON_LEAD", true, kWork);
    CHECK(row != nullptr, "NEON_LEAD row");
    tapObj(row, 300);
    CHECK(tapLabel("LOAD & EDIT B1", kWork, false, 300), "LOAD & EDIT B1");
    runAudio(100);
    CHECK(a.ui().view == View::Browser && strstr(a.padSampleName(4), "SNARE_02"), "first tap only warns");
    CHECK(tapLabel("LOAD & EDIT B1", kWork, false, 300), "LOAD & EDIT B1 again");
    waitLoader();
    runAudio(450);
    CHECK(a.ui().view == View::Sample, "SAMPLE view after LOAD & EDIT");
    nm = a.padSampleName(4);
    CHECK(nm && strstr(nm, "NEON_LEAD"), "pad B1 sample '%s'", nm ? nm : "(null)");
    // the pad holds the file now: OPEN IN SAMPLER only switches the view
    nav(View::Browser);
    CHECK(tapLabel("OPEN IN SAMPLER", kWork, false, 300), "OPEN IN SAMPLER");
    CHECK(a.ui().view == View::Sample, "SAMPLE view after OPEN IN SAMPLER");
    // categories panel: Drums row filters the browser
    CHECK(tapLabel("Drums", Box{14, 413, 301, 706}, false, 450), "Drums category");
    CHECK(a.ui().category == Category::Drums && a.ui().view == View::Browser, "category Drums");
    CHECK(a.resultCount() == 4, "4 drum samples (%d)", a.resultCount());
    // Kits: ▶ PREVIEW plays the kit's first sound and leaves the pads alone (App::previewKit / previewBuiltinKit)
    CHECK(tapLabel("Kits", Box{14, 413, 301, 706}, false, 450), "Kits category");
    CHECK(a.ui().category == Category::Kits, "category Kits");
    char pad0[64];
    snprintf(pad0, sizeof pad0, "%s", a.padSampleName(0) ? a.padSampleName(0) : "");
    for (const char* kit : {"808_STYLE", "909 STYLE"}) {
        CHECK(tapObj(findLabel(kit, true, kWork), 300), "kit row %s", kit);
        runAudio(1500);   // earlier sounds end
        int quiet = 0;
        for (int i = 0; i < 20; ++i) {
            halHostRenderAudio(128, g_audio);
            for (int k = 0; k < 256; ++k) quiet = abs(g_audio[k]) > quiet ? abs(g_audio[k]) : quiet;
        }
        CHECK(tapLabel("\xE2\x96\xB6 PREVIEW", kWork, false, 0), "PREVIEW %s", kit);
        int peak = 0;
        // The SD kit's first file comes through the loader thread (real time), so wait in real
        // time too (up to ~3 s) instead of only advancing the virtual clock.
        for (int i = 0; i < 1500 && peak < 2000; ++i) {
            halHostRenderAudio(128, g_audio);
            for (int k = 0; k < 256; ++k) peak = abs(g_audio[k]) > peak ? abs(g_audio[k]) : peak;
            lvhost_run(3);
            if (peak < 2000) orbit::sleepMs(2);
        }
        CHECK(quiet < 50 && peak >= 2000, "%s preview: silence %d, peak %d", kit, quiet, peak);
    }
    CHECK(strcmp(pad0, a.padSampleName(0) ? a.padSampleName(0) : "") == 0, "pads unchanged by kit preview");
    CHECK(tapLabel("All Samples", Box{14, 413, 301, 706}, false, 300), "All Samples");
    lvhost_tap(padX(0), padY(0));
    runAudio(200);
    nav(View::Sample);
}

static void smokeScenes()
{
    App& a = app();
    printf("smoke: scenes + song cells + transport\n");
    for (int s : {1, 2, 3, 0}) {
        lvhost_tap(sceneX(s), kSceneY);
        runAudio(80);
        CHECK(a.project().scene == s, "scene %d selected (got %d)", s, a.project().scene);
    }
    a.stop();
    a.stop();
    runAudio(60);
    // song cell: slot 4 row C sets the slot, tapping the lit cell clears it
    const int8_t old = a.project().song.slot[4];
    lvhost_tap(cellX(4), cellY(2));
    runAudio(60);
    CHECK(a.project().song.slot[4] == 2, "slot 5 = C (%d)", a.project().song.slot[4]);
    lvhost_tap(cellX(4), cellY(2));
    runAudio(60);
    CHECK(a.project().song.slot[4] == -1, "slot 5 cleared (%d)", a.project().song.slot[4]);
    a.setSongSlot(4, old);
    // macros transport: play / rec / rewind
    lvhost_tap(1066 + 72 + 27, 73 + 400 + 17);
    runAudio(200);
    CHECK(a.playing(), "macros PLAY");
    lvhost_tap(1066 + 72 + 27, 73 + 400 + 17);
    runAudio(60);
    CHECK(!a.playing(), "macros PAUSE");
    lvhost_tap(1066 + 11 + 27, 73 + 400 + 17);   // rewind = stop
    runAudio(60);
    CHECK(a.playStep() < 0, "rewound (%d)", a.playStep());
    // BPM +/-: one tap is one step
    const float bpm = a.project().bpm;
    lvhost_tap(1066 + 140 + 23, 73 + 438 + 11);
    runAudio(100);
    CHECK(a.project().bpm > bpm, "BPM + (%.1f)", a.project().bpm);
    lvhost_tap(1066 + 64 + 23, 73 + 438 + 11);
    runAudio(100);
    CHECK(fabsf(a.project().bpm - bpm) < 0.01f, "BPM - (%.1f)", a.project().bpm);
}

static void checkIdle()
{
    printf("idle: nothing redraws\n");
    runAudio(3500);   // toasts fade out, animations end
    g_inv = 0;
    g_inv_px = 0;
    lvhost_run(1000);
    printf("  idle 1 s: %d invalidations, %lld px\n", g_inv, (long long)g_inv_px);
    CHECK(g_inv == 0, "%d invalidations while idle", g_inv);
    // playing: share of the screen redrawn per frame
    nav(View::Sequence);
    app().play();
    runAudio(300);
    g_inv = 0;
    g_inv_px = 0;
    runAudio(2000);
    const double pct = 100.0 * (double)g_inv_px / (2.0 * 60.0 * 1280.0 * 720.0);
    printf("  playing SEQUENCE 2 s: %d invalidations, %.1f%% of the screen per 60 Hz frame\n", g_inv, pct);
    CHECK(pct < 15.0, "redraw %.1f%% per frame", pct);
    app().stop();
    app().stop();
    runAudio(100);
}

int main()
{
    const char* root_env = getenv("ORBIT_ROOT");
    const std::string root = root_env && root_env[0] ? root_env : ".";
    const std::string sd = root + "/host/out/render_ui_sd";
    std::error_code ec;
    fs::create_directories(root + "/host/out", ec);
    fs::create_directories(root + "/host/shots", ec);
    if (!prepareSd(sd)) {
        printf("cannot create %s\nrender_ui: 1 checks, 1 failed -> FAILED\n", sd.c_str());
        return 1;
    }
    halHostSetSdRoot(sd.c_str());
    halHostSetSettingsPath((root + "/host/out/render_ui_settings.bin").c_str());
    remove(halHostSettingsPath());

    // Firmware start order (firmware/main/app_main.cpp)
    CHECK(hal::boardInit(), "boardInit");
    const bool sdok = hal::sdMount();
    CHECK(sdok, "sdMount");
    Settings boot;
    settingsDefaults(boot);
    hal::settingsLoad(boot);
    const uint32_t rate = boot.sample_rate == 44100 ? 44100u : 48000u;
    App& a = app();
    CHECK(a.init(sdok ? hal::sdRoot() : nullptr, (int)rate), "App init");
    g_disp = hal::displayStart();
    CHECK(g_disp != nullptr, "displayStart");
    if (!g_disp) return 1;
    hal::setBrightness(a.settings().brightness);
    const uint64_t t0 = nowUs();
    CHECK(hal::lvLock(0), "lvLock");
    // Colours of every shot (Settings > Display): ORBIT_BASE=0..5 base palette (LIGHT WARM SILVER SLATE DARK
    // BLACK), ORBIT_THEME=0..7 accent preset, ORBIT_HUE=0..359 (+ ORBIT_SAT=20..100) the CUSTOM accent.
    const char* env_base = getenv("ORBIT_BASE");
    const char* env_theme = getenv("ORBIT_THEME");
    const char* env_hue = getenv("ORBIT_HUE");
    const char* env_sat = getenv("ORBIT_SAT");
    const int look_base = env_base ? atoi(env_base) : 0;
    const int look_theme = env_hue ? kThemeCustom : (env_theme ? atoi(env_theme) : 0);
    const int look_hue = env_hue ? atoi(env_hue) : 0, look_sat = env_sat ? atoi(env_sat) : 0;
    if (env_base) baseSelect(look_base);
    if (env_base || env_theme || env_hue) themeSelect(look_theme, look_hue, look_sat);
    themeInit();
    ui::build(g_disp);
    if (env_base || env_theme || env_hue) {   // the settings say the same (SETTINGS > Display shows it)
        Settings& e = a.settingsEdit();
        e.base = (uint8_t)baseIndex();
        e.theme = (uint8_t)themeIndex();
        e.accent_hue = (uint16_t)themeHue();
        e.accent_sat = (uint8_t)themeSat();
        a.applySettings();
    }
    hal::lvUnlock();
    printf("ui::build: %u ms\n", (unsigned)((nowUs() - t0) / 1000u));
    hal::audioSetVolume(a.settings().volume);
    hal::audioSetOutput(a.settings().output);
    CHECK(hal::audioStart(rate, a.settings().buffer_frames, &App::render, &a.engine()), "audioStart");
    lv_display_add_event_cb(g_disp, invCb, LV_EVENT_INVALIDATE_AREA, nullptr);

    waitLoader();
    runAudio(600);
    CHECK(halHostBrightness() == a.settings().brightness, "brightness %d", halHostBrightness());
    CHECK(halHostVolume() == a.settings().volume, "volume %d", halHostVolume());
    CHECK(a.padSampleName(0) && a.padSampleName(0)[0], "kit on the pads");
    setupSong();
    seedProjects();
    runAudio(300);

    shotsSample();
    shotsSequence();
    shotsOtherViews();
    shotsDialogs();
    shotsColors();
    shotsUiSize();
    shotsExtras();
    shotsBluetooth();

    smokeNav();
    smokePads();
    smokeSteps();
    smokeKnob();
    smokeMaximize();
    smokeBassKeys();
    smokeConsole();
    smokeKeyboard();
    smokeTopBar();
    smokeDialogs();
    smokeBrowser();
    smokeScenes();
    closeOverlays();
    checkIdle();

    hal::audioStop();
    remove(halHostSettingsPath());
    printf("render_ui: %d checks, %d failed -> %s\n", g_checks, g_fails, g_fails ? "FAILED" : "passed");
    return g_fails ? 1 : 0;
}
