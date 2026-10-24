// Build + run (Git Bash, project root; needs host/lib from host/build_lvgl.sh and host/bin/{cJSON,kits_808,kits_909}.o
// from tools/run_host_tests.sh):
// export ZIG_GLOBAL_CACHE_DIR=$PWD/host/.zig-cache/global ZIG_LOCAL_CACHE_DIR=$PWD/host/.zig-cache/local; J=${IDF_PATH:-$HOME/esp/v5.5.2/esp-idf}/components/json/cJSON; U=firmware/main/ui; zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -Wall -Wextra -DLV_CONF_INCLUDE_SIMPLE -Ifirmware/main -Ihost -Ifirmware/managed_components/lvgl__lvgl -I$J host/tests/shot_sample.cpp host/lvgl_host.cpp host/hal_host.cpp host/platform_host.cpp $U/view_sample.cpp $U/wave_view.cpp $U/dlg_sample.cpp $U/app.cpp $U/theme.cpp $U/widgets.cpp $U/widgets_knob.cpp $U/widgets_input.cpp $U/widgets_overlay.cpp firmware/main/core/*.cpp host/bin/cJSON.o host/bin/kits_808.o host/bin/kits_909.o host/lib/liborbit_assets.a host/lib/liblvgl.a -o host/bin/shot_sample.exe && host/bin/shot_sample.exe
//
// SAMPLE editor view + waveform widget + SAMPLE DETAILS dialog on the headless 1280x720 display,
// with the REAL App (app().init(<repo>/sdcard, 48000)), the host HAL and audio pulled through
// halHostRenderAudio. Writes host/shots/sample_*.png (compare with reference/shots/view-sample.png,
// editor-*.png, modal-sample-details.png) and simulates taps / drags / long presses asserting App
// state. The one WAV written by SAVE WAV into sdcard/ORBIT/USER is removed again.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "core/platform.h"
#include "core/sampleio.h"
#include "hal/hal.h"
#include "hal_host.h"
#include "lvgl_host.h"
#include "src/display/lv_display_private.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/wave_view.h"
#include "ui/widgets.h"

using namespace orbit;
using namespace orbit::ui;

static int g_fails = 0;
static int g_checks = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                      \
            ++g_fails;                                                              \
        }                                                                           \
    } while (0)
#define CHECKF(c, ...)                                                              \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c);                      \
            printf(__VA_ARGS__);                                                    \
            printf("\n");                                                           \
            ++g_fails;                                                              \
        }                                                                           \
    } while (0)

static lv_obj_t* g_view = nullptr;
static char g_user_wav[kPathLen] = {};
static bool g_audio = false;
static int16_t g_audio_buf[2 * 4096];

// ---------------------------------------------------------------- harness
static void uiTimer(lv_timer_t*)
{
    app().tick();
    view_sample::refresh();
    dlg_sample::refresh();
}

static void pump(int ms)
{
    while (ms > 0) {
        const int step = ms < 16 ? ms : 16;
        if (g_audio) halHostRenderAudio(48 * step, g_audio_buf);
        lvhost_run(step);
        ms -= step;
    }
}

static bool waitLoader(int timeout_ms = 8000)
{
    const uint64_t t0 = nowUs();
    while (nowUs() - t0 < (uint64_t)timeout_ms * 1000u) {
        pump(16);
        if (app().loadingCount() == 0) {
            pump(64);
            return true;
        }
        sleepMs(2);
    }
    printf("timeout waiting for the loader (%d jobs)\n", app().loadingCount());
    return false;
}

static void shot(const char* name)
{
    char path[160];
    snprintf(path, sizeof path, "host/shots/sample_%s.png", name);
    CHECK(lvhost_screenshot(path));
}

static lv_obj_t* findText(lv_obj_t* root, const char* text, bool visible_only = true)
{
    if (!root) return nullptr;
    if (visible_only && lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return nullptr;
    if (lv_obj_check_type(root, &lv_label_class)) {
        const char* t = lv_label_get_text(root);
        if (t && strcmp(t, text) == 0) return root;
    }
    const uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; ++i)
        if (lv_obj_t* f = findText(lv_obj_get_child(root, (int32_t)i), text, visible_only)) return f;
    return nullptr;
}

static lv_obj_t* findPrefix(lv_obj_t* root, const char* prefix)
{
    if (!root || lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return nullptr;
    if (lv_obj_check_type(root, &lv_label_class)) {
        const char* t = lv_label_get_text(root);
        if (t && strncmp(t, prefix, strlen(prefix)) == 0) return root;
    }
    const uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; ++i)
        if (lv_obj_t* f = findPrefix(lv_obj_get_child(root, (int32_t)i), prefix)) return f;
    return nullptr;
}

static void centerOf(lv_obj_t* o, int* x, int* y)
{
    lv_obj_update_layout(o);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    *x = (a.x1 + a.x2) / 2;
    *y = (a.y1 + a.y2) / 2;
}

// Taps the object holding the label `text` (searching root, default: the screen + top layer).
static bool tapText(const char* text, lv_obj_t* root = nullptr)
{
    lv_obj_t* l = root ? findText(root, text) : findText(lv_layer_top(), text);
    if (!l && !root) l = findText(lv_screen_active(), text);
    if (!l) {
        printf("  (no label \"%s\")\n", text);
        return false;
    }
    int x, y;
    centerOf(l, &x, &y);
    lvhost_tap(x, y);
    pump(60);
    return true;
}

static void longPress(int x, int y)
{
    lvhost_pointer(x, y, true);
    pump(650);
    lvhost_pointer(x, y, false);
    pump(120);
}

static bool popupOpen() { return lv_obj_get_child_count(lv_layer_top()) > 0 && !modalIsOpen(); }

static const Pad& pad0() { return app().project().pad[app().ui().pad]; }

// Wave of the SAMPLE view: inner area x 325..1042 (718 px), y 121..331.
constexpr int kWaveX = 325, kWaveW = 718, kWaveMidY = 226;
static int waveX(float pos) { return kWaveX + (int)lroundf(pos * (float)(kWaveW - 1)); }

static lv_obj_t* waveObj()
{
    lv_point_t p = {700, 300};
    return lv_indev_search_obj(lv_screen_active(), &p);
}

static uint32_t invalidatedArea()
{
    lv_display_t* d = lv_display_get_default();
    uint32_t sum = 0;
    for (uint32_t i = 0; i < d->inv_p; ++i)
        if (!d->inv_area_joined[i]) sum += (uint32_t)lv_area_get_size(&d->inv_areas[i]);
    return sum;
}
static int32_t invalidatedMaxW()
{
    lv_display_t* d = lv_display_get_default();
    int32_t w = 0;
    for (uint32_t i = 0; i < d->inv_p; ++i)
        if (!d->inv_area_joined[i] && lv_area_get_width(&d->inv_areas[i]) > w) w = lv_area_get_width(&d->inv_areas[i]);
    return w;
}

// ---------------------------------------------------------------- stage
static void buildStage()
{
    lv_obj_t* scr = lv_screen_active();
    // Context only: empty panels where the other modules live.
    lv_obj_t* top = lv_obj_create(scr);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, 1280, 62);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(top, c(col::top_a), 0);
    lv_obj_set_style_border_side(top, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(top, 1, 0);
    lv_obj_set_style_border_color(top, c(col::top_line), 0);
    panelCreate(scr, geo::pads_x, geo::pads_y, geo::pads_w, geo::pads_h);
    panelCreate(scr, geo::browser_x, geo::browser_y, geo::browser_w, geo::browser_h);
    panelCreate(scr, geo::macros_x, geo::macros_y, geo::macros_w, geo::macros_h);
    panelCreate(scr, geo::timeline_x, geo::timeline_y, geo::timeline_w, geo::timeline_h);
    lv_obj_t* work = panelCreate(scr, geo::work_x, geo::work_y, geo::work_w, geo::work_h);
    g_view = view_sample::create(work);
    view_sample::onShow();
}

static void referenceParams()
{
    App& a = app();
    a.selectPad(0, false);
    a.selectLayer(0);
    a.setEditor(EditorTab::Trim);
    a.paramSet(ParamRef::pad(PP_ATTACK), 50.f);
    a.paramSet(ParamRef::pad(PP_DECAY), 256.f);
    a.paramSet(ParamRef::pad(PP_SUSTAIN), 0.72f);
    a.paramSet(ParamRef::pad(PP_RELEASE), 202.f);
    a.paramSet(ParamRef::pad(PP_CUTOFF), 4200.f);
    a.paramSet(ParamRef::pad(PP_RESO), 0.29f);
    a.paramSet(ParamRef::pad(PP_END), 0.86f);
    a.paramSet(ParamRef::pad(PP_START), 0.05f);
}

// ---------------------------------------------------------------- tests
static void testTabs()
{
    App& a = app();
    static const char* const names[6] = {"TRIM", "SLICE", "ENVELOPE", "FILTER", "PITCH", "LFO"};
    static const char* const first_caption[6] = {"PAD VOL", "TUNE", "ATTACK", "CUTOFF", "TUNE", "RATE"};
    static const char* const last_caption[6] = {"RESONANCE", nullptr, "VELOCITY", "ENV DECAY", "DOWNSAMPLE", "FADE"};
    for (int t = 0; t < 6; ++t) {
        CHECK(tapText(names[t], g_view));
        pump(50);
        CHECKF((int)a.ui().editor == t, "editor %d want %d", (int)a.ui().editor, t);
        CHECK(findText(g_view, first_caption[t]) != nullptr);
        if (last_caption[t]) CHECK(findText(g_view, last_caption[t]) != nullptr);
        char ov[40];
        snprintf(ov, sizeof ov, "%s \xC2\xB7 EDIT MODE", names[t]);
        CHECK(findText(g_view, ov) != nullptr);
    }
    // SLICE swaps the 2-knob group for the button column
    CHECK(tapText("SLICE", g_view));
    CHECK(findText(g_view, "AUTO \xE2\x96\xBE") != nullptr);
    CHECK(findText(g_view, "TO PADS") != nullptr);
    CHECK(findText(g_view, "CUTOFF") == nullptr);
    CHECK(findText(g_view, "START") != nullptr && findText(g_view, "END") != nullptr && findText(g_view, "GAIN") != nullptr);
    CHECK(tapText("TRIM", g_view));
    CHECK(findText(g_view, "AUTO \xE2\x96\xBE") == nullptr);
    CHECK(findText(g_view, "CUTOFF") != nullptr);

    // the same knob objects are re-bound: knob 2 edits ATTACK on TRIM and HOLD on ENVELOPE
    a.setEditor(EditorTab::Envelope);
    pump(50);
    const float hold0 = pad0().p[PP_HOLD], decay0 = pad0().p[PP_DECAY], attack0 = pad0().p[PP_ATTACK];
    lvhost_drag(650, 444, 650, 394, 300);
    pump(50);
    CHECKF(pad0().p[PP_HOLD] > hold0, "HOLD %.2f -> %.2f", (double)hold0, (double)pad0().p[PP_HOLD]);
    CHECK(pad0().p[PP_DECAY] == decay0);
    CHECK(findText(g_view, "0.0 ms") == nullptr || pad0().p[PP_HOLD] > 0.f);
    a.setEditor(EditorTab::Trim);
    pump(50);
    lvhost_drag(650, 444, 650, 424, 200);
    pump(50);
    CHECK(pad0().p[PP_ATTACK] != attack0);
    a.paramSet(ParamRef::pad(PP_HOLD), hold0);
    a.paramSet(ParamRef::pad(PP_ATTACK), attack0);
    pump(50);

    // tool buttons (wave / envelope / slice)
    const int tool_x[3] = {367, 411, 455};
    const EditorTab want[3] = {EditorTab::Trim, EditorTab::Envelope, EditorTab::Slice};
    for (int i = 2; i >= 0; --i) {
        lvhost_tap(tool_x[i], 416);
        pump(40);
        CHECK(a.ui().editor == want[i]);
    }
}

static void tabShots()
{
    App& a = app();
    static const char* const names[6] = {"trim", "slice", "envelope", "filter", "pitch", "lfo"};
    for (int t = 0; t < 6; ++t) {
        if (t == 1) continue;   // SLICE is shot with markers in testSlice()
        a.setEditor((EditorTab)t);
        pump(400);
        shot(names[t]);
    }
    a.setEditor(EditorTab::Trim);
    pump(100);
}

static void testSlice()
{
    App& a = app();
    a.setEditor(EditorTab::Slice);
    pump(100);
    // AUTO ▾ -> HIGH: sensitivity + transient detection (async)
    CHECK(tapText("AUTO \xE2\x96\xBE", g_view));
    CHECK(popupOpen());
    CHECK(tapText("HIGH"));
    CHECK(fabsf(a.sliceSens() - 0.8f) < 1e-4f);
    CHECK(waitLoader());
    printf("  AUTO HIGH on the kick: %d slices\n", pad0().slice_count);
    // GRID ▾ -> 8
    CHECK(tapText("GRID \xE2\x96\xBE", g_view));
    CHECK(popupOpen());
    CHECK(tapText("8"));
    pump(50);
    CHECK(pad0().slice_count == 8);
    // tap inside slice 3 selects it (region = that slice)
    const float s3 = pad0().slices[3], s4 = pad0().slices[4];
    lvhost_tap(waveX((s3 + s4) * 0.5f), kWaveMidY + 40);
    pump(60);
    CHECK(pad0().slice_sel == 3);
    CHECK(fabsf(pad0().p[PP_START] - s3) < 1e-4f && fabsf(pad0().p[PP_END] - s4) < 1e-4f);
    pump(400);
    shot("slice");
    // a slice further right
    const float s6 = pad0().slices[6], s7 = pad0().slices[7];
    lvhost_tap(waveX((s6 + s7) * 0.5f), kWaveMidY);
    pump(60);
    CHECK(pad0().slice_sel == 6);
}

// The time readout of the editor wave ("0.044s — 0.754s   48 000 Hz") is never cut by the START
// or END handle: it moves to the right, the left or between the markers.
static lv_obj_t* findContains(lv_obj_t* root, const char* part)
{
    if (!root || lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return nullptr;
    if (lv_obj_check_type(root, &lv_label_class)) {
        const char* t = lv_label_get_text(root);
        if (t && strstr(t, part)) return root;
    }
    const uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; ++i)
        if (lv_obj_t* f = findContains(lv_obj_get_child(root, (int32_t)i), part)) return f;
    return nullptr;
}

static void testReadout()
{
    App& a = app();
    a.setEditor(EditorTab::Trim);
    // full range (prototype: top right), END in the last third (the reported overlap), START far
    // right, a narrow region in the middle, END near the right edge with START mid-way
    static const float kRegions[][2] = {{0.f, 1.f}, {0.05f, 0.86f}, {0.06f, 0.754f}, {0.35f, 0.95f}, {0.02f, 0.4f}, {0.45f, 0.55f}};
    for (const auto& r : kRegions) {
        a.paramSet(ParamRef::pad(PP_END), 1.f);
        a.paramSet(ParamRef::pad(PP_START), r[0]);
        a.paramSet(ParamRef::pad(PP_END), r[1]);
        pump(60);
        lv_obj_t* t = findContains(waveObj(), " Hz");
        CHECKF(t != nullptr, "readout for %.2f..%.2f", (double)r[0], (double)r[1]);
        if (!t) continue;
        lv_obj_update_layout(t);
        lv_area_t ta;
        lv_obj_get_coords(t, &ta);
        const int hs = waveX(r[0]), he = waveX(r[1]);
        auto hits = [&](int hx) { return hx + 4 >= ta.x1 && hx - 4 <= ta.x2; };
        CHECKF(!hits(hs) && !hits(he), "readout %d..%d clear of START %d / END %d (region %.2f..%.2f)",
               (int)ta.x1, (int)ta.x2, hs, he, (double)r[0], (double)r[1]);
        CHECK(ta.x1 >= kWaveX && ta.x2 < kWaveX + kWaveW);
        CHECK(lv_obj_get_style_bg_opa(t, LV_PART_MAIN) >= LV_OPA_50);   // dark plate behind the text
    }
    a.paramSet(ParamRef::pad(PP_END), 1.f);
    a.paramSet(ParamRef::pad(PP_START), 0.f);
    pump(60);
}

static void testDrag()
{
    App& a = app();
    a.setEditor(EditorTab::Trim);
    a.paramSet(ParamRef::pad(PP_END), 0.86f);
    a.paramSet(ParamRef::pad(PP_START), 0.05f);
    pump(100);
    const int xs = waveX(0.05f);
    // press 5 px right of the START marker, drag 120 px right, screenshot while held
    lvhost_pointer(xs + 5, kWaveMidY + 60, true);
    pump(40);
    for (int i = 1; i <= 12; ++i) {
        lvhost_pointer(xs + 5 + i * 10, kWaveMidY + 60, true);
        pump(16);
    }
    pump(100);
    CHECK(wave::dragging(waveObj()));
    const float expect = (float)(xs + 120 - kWaveX) / (float)(kWaveW - 1);
    CHECKF(fabsf(pad0().p[PP_START] - expect) < 0.003f, "START %.4f want %.4f", (double)pad0().p[PP_START], (double)expect);
    shot("drag_start");
    lvhost_pointer(xs + 125, kWaveMidY + 60, false);
    pump(100);
    CHECK(!wave::dragging(waveObj()));
    CHECKF(fabsf(pad0().p[PP_START] - expect) < 0.003f, "START %.4f want %.4f", (double)pad0().p[PP_START], (double)expect);

    // END dragged far left stops 1 % after START
    const int xe = waveX(pad0().p[PP_END]);
    lvhost_drag(xe - 3, kWaveMidY - 50, kWaveX + 2, kWaveMidY - 50, 300);
    pump(50);
    CHECKF(pad0().p[PP_END] >= pad0().p[PP_START] + 0.0099f, "END %.4f START %.4f", (double)pad0().p[PP_END],
           (double)pad0().p[PP_START]);
    CHECK(pad0().p[PP_END] < pad0().p[PP_START] + 0.02f);
    // START dragged past END stops 1 % before it
    a.paramSet(ParamRef::pad(PP_END), 0.6f);
    a.paramSet(ParamRef::pad(PP_START), 0.2f);
    pump(50);
    lvhost_drag(waveX(0.2f), kWaveMidY, waveX(0.95f), kWaveMidY, 300);
    CHECKF(pad0().p[PP_START] <= 0.6f - 0.0099f && pad0().p[PP_START] > 0.57f, "START %.4f", (double)pad0().p[PP_START]);
    CHECK(fabsf(pad0().p[PP_END] - 0.6f) < 1e-5f);

    // double tap on the START handle snaps it to a zero crossing
    a.paramSet(ParamRef::pad(PP_START), 0.2137f);
    pump(50);
    const Sample* s = app().padSample(0, 0);
    CHECK(s != nullptr);
    if (s) {
        const float want = snapToZeroCrossing(s, pad0().p[PP_START]);
        lvhost_double_tap(waveX(pad0().p[PP_START]) + 2, kWaveMidY + 70);
        pump(50);
        CHECKF(fabsf(pad0().p[PP_START] - want) < 1e-6f, "START %.5f want %.5f", (double)pad0().p[PP_START], (double)want);
        printf("  snap: 0.2137 -> %.5f\n", (double)want);
    }
    // the double tap must not have started a preview (single tap on a handle is deferred)
    a.paramSet(ParamRef::pad(PP_END), 0.86f);
    a.paramSet(ParamRef::pad(PP_START), 0.05f);
    pump(100);
}

static void testPreviewPlayhead()
{
    App& a = app();
    a.setEditor(EditorTab::Trim);
    pump(400);   // let earlier voices end
    lv_obj_t* w = waveObj();
    CHECK(w != nullptr);
    lv_obj_t* head = w ? lv_obj_get_child(w, 0) : nullptr;
    // tap in the middle (away from the handles) -> previewPad -> a voice plays -> playhead shown
    lvhost_tap(700, kWaveMidY);
    pump(32);
    const float pp = a.engine().telemetry().play_pos.load();
    CHECKF(pp >= 0.f, "play_pos %.3f", (double)pp);
    pump(40);
    CHECK(head && !lv_obj_has_flag(head, LV_OBJ_FLAG_HIDDEN));
    // stop the audio clock so the playhead stays put for the screenshot
    g_audio = false;
    a.engine().telemetry().play_pos.store(0.17f);
    pump(80);
    shot("playhead");
    if (head) {
        lv_obj_update_layout(head);
        lv_area_t ha;
        lv_obj_get_coords(head, &ha);
        CHECKF(abs((ha.x1 + ha.x2) / 2 - waveX(0.17f)) <= 1, "playhead x %d want %d", (int)((ha.x1 + ha.x2) / 2),
               waveX(0.17f));
    }

    // ---- refresh cost: nothing changed -> nothing invalidated
    lv_display_t* d = lv_display_get_default();
    lv_refr_now(d);
    view_sample::refresh();
    CHECKF(d->inv_p == 0, "inv_p %u", (unsigned)d->inv_p);
    // playhead move invalidates only two narrow strips
    lv_refr_now(d);
    a.engine().telemetry().play_pos.store(0.30f);
    lvhost_run(40);   // ui timer -> refresh -> playhead (no refresh of the display yet? run includes it)
    lv_refr_now(d);
    a.engine().telemetry().play_pos.store(0.31f);
    // run only the ui timer: call refresh directly
    lv_tick_inc(40);
    view_sample::refresh();
    CHECKF(invalidatedMaxW() <= 12, "playhead invalidates %d px wide", (int)invalidatedMaxW());
    lv_refr_now(d);
    // region change invalidates strips, not the whole waveform
    a.paramSet(ParamRef::pad(PP_START), 0.06f);
    view_sample::refresh();
    CHECKF(invalidatedMaxW() < 300 && invalidatedArea() < 718u * 211u / 2u, "region invalidates w %d area %u",
           (int)invalidatedMaxW(), (unsigned)invalidatedArea());
    lv_refr_now(d);
    view_sample::refresh();
    CHECK(d->inv_p == 0);
    a.engine().telemetry().play_pos.store(-1.f);
    a.paramSet(ParamRef::pad(PP_START), 0.05f);
    g_audio = true;
    pump(1600);   // the preview voice ends -> playhead hidden
    CHECKF(head && lv_obj_has_flag(head, LV_OBJ_FLAG_HIDDEN), "play_pos %.3f",
           (double)a.engine().telemetry().play_pos.load());
}

static int libIndexOf(const char* suffix)
{
    Library& lib = app().library();
    if (getenv("SHOT_VERBOSE"))
        for (int i = 0; i < lib.count() && i < 8; ++i) printf("  lib[%d] %s\n", i, lib.at(i).path);
    const size_t n = strlen(suffix);
    for (int i = 0; i < lib.count(); ++i) {
        const char* p = lib.at(i).path;
        const size_t m = strlen(p);
        if (m >= n && strcmp(p + m - n, suffix) == 0) return i;
    }
    return -1;
}

static void testLayers()
{
    App& a = app();
    a.setEditor(EditorTab::Trim);
    pump(50);
    // tap LAYER 2 -> selected, empty -> "—"
    CHECK(tapText("LAYER 2", g_view));
    CHECK(a.ui().layer == 1);
    CHECK(findText(g_view, "\xE2\x80\x94") != nullptr);
    // a WAV in USER (the snare of pad 2, removed at the end), scanned, loaded into layer 2 (async)
    CHECK(a.library().userPath("ZZ_SHOT_SAMPLE.wav", g_user_wav, sizeof g_user_wav));
    const Sample* sn = a.padSample(1, 0);
    CHECK(sn && wavSave(g_user_wav, sn) == WavError::Ok);
    a.rescan();
    CHECK(waitLoader());
    const int li = libIndexOf("ZZ_SHOT_SAMPLE.wav");
    CHECK(li >= 0);
    if (li >= 0) {
        CHECK(a.loadFileToPad(li, 0, 1));
        CHECK(waitLoader());
        CHECK(a.padSample(0, 1) != nullptr);
        pump(100);
        if (const Sample* s = a.padSample(0, 1)) CHECK(findText(g_view, s->name) != nullptr);
        // SAMPLE DETAILS of a file-backed layer: file name + "48 kHz • WAV • 16-bit • ..."
        dlg_sample::open();
        pump(400);
        CHECK(findText(lv_layer_top(), "ZZ_SHOT_SAMPLE.wav") != nullptr);
        lv_obj_t* info = findPrefix(lv_layer_top(), "48 kHz \xE2\x80\xA2 WAV \xE2\x80\xA2 16-bit \xE2\x80\xA2 ");
        CHECK(info != nullptr);
        if (info) printf("  file info: %s\n", lv_label_get_text(info));
        shot("dialog_file");
        modalClose();
        pump(300);
    }
    // long press LAYER 2 -> menu
    lv_obj_t* l2 = findText(g_view, "LAYER 2");
    int x = 0, y = 0;
    if (l2) centerOf(lv_obj_get_parent(l2), &x, &y);
    longPress(x, y);
    CHECK(popupOpen());
    CHECK(findText(lv_layer_top(), "CLEAR LAYER") != nullptr);
    pump(200);
    shot("layer_menu");
    CHECK(tapText("VELOCITY"));
    CHECK(pad0().layer_mode == LayerMode::Velocity);
    // CLEAR LAYER
    longPress(x, y);
    CHECK(tapText("CLEAR LAYER"));
    CHECK(a.padSample(0, 1) == nullptr);
    // LOAD FROM BROWSER -> browser view, layer 2 stays selected
    longPress(x, y);
    CHECK(tapText("LOAD FROM BROWSER"));
    CHECK(a.ui().view == View::Browser);
    CHECK(a.ui().layer == 1);
    a.setView(View::Sample);
    a.setLayerMode(LayerMode::Single);
    CHECK(tapText("LAYER 1", g_view));
    CHECK(a.ui().layer == 0);
    pump(100);
}

static void testMode()
{
    CHECK(tapText("ONE SHOT \xE2\x96\xBE", g_view));
    CHECK(popupOpen());
    pump(150);
    shot("mode_menu");
    CHECK(tapText("GATE"));
    CHECK(pad0().mode == PlayMode::Gate);
    pump(30);
    CHECK(findText(g_view, "GATE \xE2\x96\xBE") != nullptr);
    CHECK(tapText("GATE \xE2\x96\xBE", g_view));
    CHECK(tapText("REVERSE"));
    CHECK(pad0().reverse == 1);
    pump(30);
    CHECK(findText(g_view, "GATE \xC2\xB7 REV \xE2\x96\xBE") != nullptr);
    CHECK(tapText("GATE \xC2\xB7 REV \xE2\x96\xBE", g_view));
    CHECK(tapText("\xE2\x97\x8F REVERSE"));
    CHECK(pad0().reverse == 0);
    CHECK(tapText("GATE \xE2\x96\xBE", g_view));
    CHECK(tapText("ONE SHOT"));
    CHECK(pad0().mode == PlayMode::OneShot);
}

static lv_obj_t* selectAfterCaption(const char* caption)
{
    lv_obj_t* cap = findText(lv_layer_top(), caption);
    if (!cap) return nullptr;
    lv_obj_t* parent = lv_obj_get_parent(cap);
    const int32_t i = lv_obj_get_index(cap);
    return lv_obj_get_child(parent, i + 1);
}

static bool tapObj(lv_obj_t* o)
{
    if (!o) return false;
    int x, y;
    centerOf(o, &x, &y);
    lvhost_tap(x, y);
    pump(60);
    return true;
}

static int listWavs(const char* dir, char names[][64], int max)
{
    int n = 0;
    DIR* d = opendir(dir);
    if (!d) return 0;
    while (struct dirent* e = readdir(d)) {
        const size_t l = strlen(e->d_name);
        if (l > 4 && l < 64 && strcmp(e->d_name + l - 4, ".wav") == 0 && n < max)
            snprintf(names[n++], 64, "%s", e->d_name);
    }
    closedir(d);
    return n;
}

static void testDialog()
{
    App& a = app();
    a.setEditor(EditorTab::Trim);
    a.selectPad(0, false);
    a.selectLayer(0);
    a.paramSet(ParamRef::pad(PP_END), 0.86f);
    a.paramSet(ParamRef::pad(PP_START), 0.05f);
    pump(100);
    // pencil = the sibling after the name label
    const Sample* s0 = a.padSample(0, 0);
    lv_obj_t* name = s0 ? findText(g_view, s0->name) : nullptr;
    CHECK(name != nullptr);
    if (name) CHECK(tapObj(lv_obj_get_child(lv_obj_get_parent(name), 1)));
    CHECK(modalIsOpen());
    pump(400);
    shot("dialog");
    CHECK(findText(lv_layer_top(), "SAMPLE DETAILS") != nullptr);
    CHECK(findText(lv_layer_top(), "Actions") != nullptr);

    // selects
    CHECK(tapObj(selectAfterCaption("CHOKE")));
    CHECK(tapText("3"));
    CHECK(pad0().choke == 3);
    CHECK(tapObj(selectAfterCaption("POLY")));
    CHECK(tapText("8"));
    CHECK(pad0().poly == 8);
    CHECK(tapObj(selectAfterCaption("LAYERS")));
    CHECK(tapText("Stack"));
    CHECK(pad0().layer_mode == LayerMode::Stack);
    const int bus_before = pad0().bus;
    CHECK(tapObj(selectAfterCaption("OUTPUT")));
    char bn[12];
    snprintf(bn, sizeof bn, "%.8s", a.project().bus[(bus_before + 1) % kBuses].name);
    CHECK(tapText(bn));
    CHECK(pad0().bus == (bus_before + 1) % kBuses);
    a.setChoke(0);
    a.setPoly(4);
    a.setLayerMode(LayerMode::Single);
    a.setBus(bus_before);
    pump(50);
    CHECK(modalIsOpen());

    // NORMALIZE -> undo available -> UNDO
    CHECK(!a.canUndo());
    lv_obj_t* undo_lbl = findText(lv_layer_top(), "UNDO");
    lv_obj_t* undo = undo_lbl ? lv_obj_get_parent(undo_lbl) : nullptr;
    CHECK(undo && lv_obj_has_state(undo, LV_STATE_DISABLED));
    const uint32_t id0 = a.padSample(0, 0)->id;
    CHECK(tapText("NORMALIZE"));
    CHECK(waitLoader());
    CHECK(a.canUndo());
    CHECK(a.padSample(0, 0)->id != id0);
    CHECK(undo && !lv_obj_has_state(undo, LV_STATE_DISABLED));
    pump(300);
    shot("dialog_edited");
    CHECK(tapText("UNDO"));
    CHECK(waitLoader());
    CHECK(!a.canUndo());
    CHECK(a.padSample(0, 0)->id == id0);
    CHECK(undo && lv_obj_has_state(undo, LV_STATE_DISABLED));

    // drag the START handle in the dialog wave (552 x 180 at body x 253, y 0+...)
    lv_obj_t* dw = nullptr;
    {
        // the dialog wave is the clickable object under its centre
        lv_point_t pt = {530, 380};
        dw = lv_indev_search_obj(lv_layer_top(), &pt);
    }
    CHECK(dw != nullptr);
    if (dw) {
        lv_area_t wa;
        lv_obj_get_coords(dw, &wa);
        const int wi = lv_area_get_width(&wa) - 2;
        const int xs = wa.x1 + 1 + (int)lroundf(0.05f * (float)(wi - 1));
        lvhost_drag(xs + 2, (wa.y1 + wa.y2) / 2, xs + 2 + 100, (wa.y1 + wa.y2) / 2, 300);
        const float expect = 0.05f + 100.f / (float)(wi - 1);
        CHECKF(fabsf(pad0().p[PP_START] - expect) < 0.004f, "dialog START %.4f want %.4f", (double)pad0().p[PP_START],
               (double)expect);
    }

    // knobs bound to FADE IN / FADE OUT / GAIN / LOOP
    CHECK(findText(lv_layer_top(), "FADE IN") != nullptr && findText(lv_layer_top(), "LOOP") != nullptr);

    // DUPLICATE -> first empty pad after the selected one
    a.clearPadLayer(15, 0);
    for (int l = 1; l < kLayers; ++l) a.clearPadLayer(15, l);
    pump(50);
    CHECK(a.padSample(15, -1) == nullptr);
    CHECK(tapText("DUPLICATE"));
    pump(50);
    CHECK(a.padSample(15, -1) != nullptr);

    // TRIM crops to START..END
    a.paramSet(ParamRef::pad(PP_START), 0.1f);
    a.paramSet(ParamRef::pad(PP_END), 0.6f);
    pump(50);
    const uint32_t frames0 = a.padSample(0, 0)->frames;
    CHECK(tapText("TRIM"));
    CHECK(waitLoader());
    const uint32_t frames1 = a.padSample(0, 0)->frames;
    CHECKF(frames1 < frames0 && frames1 > frames0 / 2 - 64 && frames1 < frames0 / 2 + 64, "trim %u -> %u",
           (unsigned)frames0, (unsigned)frames1);
    CHECK(pad0().p[PP_START] == 0.f && pad0().p[PP_END] == 1.f);
    pump(50);
    CHECK(findText(lv_layer_top(), "UNDO") != nullptr);
    CHECK(tapText("UNDO"));
    CHECK(waitLoader());
    CHECK(a.padSample(0, 0)->frames == frames0);

    // SAVE WAV -> USER/<name>.wav (removed again)
    if (a.sdReady()) {
        char dir[kPathLen];
        snprintf(dir, sizeof dir, "%s/USER", a.library().root());
        char before[64][64];
        const int nb = listWavs(dir, before, 64);
        CHECK(tapText("SAVE WAV"));   // App::saveSampleWav (async) -> USER/<name>.wav + rescan
        CHECK(waitLoader());
        char after[64][64];
        const int na = listWavs(dir, after, 64);
        int added = 0;
        for (int i = 0; i < na; ++i) {
            bool old = false;
            for (int k = 0; k < nb && !old; ++k) old = strcmp(after[i], before[k]) == 0;
            if (old) continue;
            ++added;
            char path[kPathLen + 64];
            snprintf(path, sizeof path, "%s/%s", dir, after[i]);
            WavInfo wi;
            CHECKF(wavProbe(path, wi) == WavError::Ok && wi.frames == a.padSample(0, 0)->frames, "%s", path);
            printf("  SAVE WAV wrote USER/%s (removed)\n", after[i]);
            remove(path);
        }
        CHECK(added == 1);
        a.rescan();
        CHECK(waitLoader());
    }

    // SLICE -> editor SLICE in the SAMPLE view, dialog closed
    a.setEditor(EditorTab::Trim);
    pump(30);
    CHECK(tapText("SLICE"));
    pump(300);
    CHECK(!modalIsOpen());
    CHECK(a.ui().editor == EditorTab::Slice && a.ui().view == View::Sample);

    // reopen + close via the close button clears the pointers (refresh after close is safe)
    dlg_sample::open();
    pump(300);
    modalClose();
    pump(300);
    dlg_sample::refresh();
    CHECK(!modalIsOpen());
}

static void testToPads()
{
    App& a = app();
    a.selectPad(0, false);
    a.setEditor(EditorTab::Slice);
    a.sliceGrid(4);
    pump(50);
    // A2..A4 hold kit sounds: the first tap only names them, the second replaces them.
    char old1[kNameLen];
    snprintf(old1, sizeof old1, "%s", a.project().pad[1].name);
    const Sample* old1s = a.padSample(1, -1);
    CHECK(old1s != nullptr && old1s != a.padSample(0, -1));
    CHECK(tapText("TO PADS", g_view));
    pump(50);
    CHECK(strcmp(a.project().pad[1].name, old1) == 0 && a.padSample(1, -1) == old1s);   // not yet
    CHECK(tapText("TO PADS", g_view));
    pump(50);
    printf("  TO PADS: pad 1 = \"%s\"\n", a.project().pad[1].name);
    CHECK(strncmp(a.project().pad[1].name, a.project().pad[0].name, strlen(a.project().pad[0].name) - 2) == 0);
    CHECK(a.padSample(1, -1) == a.padSample(0, -1));
    // UNDO TO PADS (8 s) brings A2 back, then the button reads TO PADS again
    CHECK(findText(g_view, "UNDO TO PADS") != nullptr);
    CHECK(tapText("UNDO TO PADS", g_view));
    pump(50);
    CHECK(strcmp(a.project().pad[1].name, old1) == 0 && a.padSample(1, -1) == old1s);
    CHECK(findText(g_view, "TO PADS") != nullptr && findText(g_view, "UNDO TO PADS") == nullptr);
    // and again, for the state the rest of the file expects
    CHECK(tapText("TO PADS", g_view));
    pump(50);
    CHECK(tapText("TO PADS", g_view));
    pump(50);
    CHECK(a.padSample(1, -1) == a.padSample(0, -1));
}

int main()
{
    lvhost_init(1280, 720);
    themeInit();
    char cwd[512];
    if (!getcwd(cwd, sizeof cwd)) return 2;
    char sd[600];
    snprintf(sd, sizeof sd, "%s/sdcard", cwd);
    remove("host/out/shot_sample_settings.bin");
    halHostSetSdRoot(sd);
    halHostSetSettingsPath("host/out/shot_sample_settings.bin");
    CHECK(app().init(sd, 48000));
    CHECK(hal::audioStart(48000, 128, &App::render, &app().engine()));
    g_audio = true;
    buildStage();
    lv_timer_create(uiTimer, 16, nullptr);
    CHECK(waitLoader());   // SD scan
    app().selectPad(0, false);
    pump(400);
    shot("default");   // kit just loaded: region 0..1, markers on the frame edges
    referenceParams();
    pump(500);
    if (const Sample* s = app().padSample(0, 0))
        printf("pad 0: name \"%s\" sample \"%s\" %u frames @ %u Hz\n", app().project().pad[0].name, s->name,
               (unsigned)s->frames, (unsigned)s->rate);

    tabShots();
    testTabs();
    referenceParams();
    testSlice();
    referenceParams();
    testReadout();
    testDrag();
    testPreviewPlayhead();
    testLayers();
    testMode();
    referenceParams();
    testDialog();
    testToPads();

    if (g_user_wav[0]) {
        remove(g_user_wav);
        app().rescan();
        waitLoader();
    }
    printf("shot_sample: %d checks, %d failed -> %s\n", g_checks, g_fails, g_fails ? "FAILED" : "passed");
    return g_fails ? 1 : 0;
}
