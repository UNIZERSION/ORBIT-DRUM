// Build (Git Bash, project root; first: bash tools/run_host_tests.sh once so host/bin/cJSON.o,
// host/bin/kits_*.o and host/lib/*.a exist):
// export ZIG_GLOBAL_CACHE_DIR=$PWD/host/.zig-cache/global ZIG_LOCAL_CACHE_DIR=$PWD/host/.zig-cache/local; U=firmware/main/ui; zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -Wall -Wextra -DLV_CONF_INCLUDE_SIMPLE -Ifirmware/main -Ihost -Ifirmware/managed_components/lvgl__lvgl -I$HOME/esp/v5.5.2/esp-idf/components/json/cJSON host/tests/shot_browser_dlg.cpp $U/view_browser.cpp $U/dlg_settings.cpp $U/dlg_project.cpp $U/dlg_help.cpp $U/wave_view.cpp $U/app.cpp $U/theme.cpp $U/widgets.cpp $U/widgets_knob.cpp $U/widgets_input.cpp $U/widgets_overlay.cpp $(ls firmware/main/core/*.cpp) host/hal_host.cpp host/platform_host.cpp host/lvgl_host.cpp host/bin/cJSON.o host/bin/kits_808.o host/bin/kits_909.o host/lib/liborbit_assets.a host/lib/liblvgl.a -o host/bin/shot_browser_dlg.exe && host/bin/shot_browser_dlg.exe
//
// BROWSER view + SETTINGS / PROJECT MANAGER / UI GUIDE dialogs on the headless 1280x720 LVGL host
// with the REAL App (app().init on a generated SD folder, loader thread, engine) and the host HAL.
// Writes host/shots/browser_*.png and host/shots/dlg_*.png (compare with reference/shots/
// view-browser.png, modal-settings-*.png, modal-project.png, modal-help.png) and checks the
// interactions (selection, preview, LOAD TO / OPEN IN SAMPLER / LOAD KIT, search + keyboard,
// virtualized scrolling of 2000+ entries, settings APPLY / discard, project open / new / save).
// The no-SD-card state runs in a child process (the App is a process-wide singleton).
// Stubs: only dlg_sample::open() (another module) as a weak no-op that counts calls.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "core/kits.h"
#include "hal/hal.h"
#include "hal_host.h"
#include "lvgl_host.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace fs = std::filesystem;
using namespace orbit;
using namespace orbit::ui;

// ------------------------------------------------------------------ stubs (other modules)
static int g_details_opened = 0;
namespace orbit::ui::dlg_sample {
__attribute__((weak)) void open() { ++g_details_opened; }
__attribute__((weak)) void refresh() {}
}  // namespace orbit::ui::dlg_sample

// Fallback for ui/wave_view.cpp (another module, built when it compiles): a plain dark frame
// with a min/max outline so the browser layout can be checked without it.
#include "ui/wave_view.h"
namespace orbit::ui::wave {
namespace {
void phDraw(lv_event_t* e)
{
    lv_obj_t* o = (lv_obj_t*)lv_event_get_current_target(e);
    const Sample* s = (const Sample*)lv_obj_get_user_data(o);
    if (!s) return;
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const int w = lv_area_get_width(&a), h = lv_area_get_height(&a), mid = a.y1 + h / 2;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c(col::accent);
    for (int x = 0; x < w; ++x) {
        const int b = x * kPeakBins / w;
        lv_area_t r = {a.x1 + x, mid - s->pk_max[b] * (h / 2 - 4) / 127, a.x1 + x, mid - s->pk_min[b] * (h / 2 - 4) / 127};
        lv_draw_rect(layer, &d, &r);
    }
}
}  // namespace
__attribute__((weak)) lv_obj_t* create(lv_obj_t* parent, int w, int h)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, c(col::wave_bg), 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(o, phDraw, LV_EVENT_DRAW_MAIN, nullptr);
    return o;
}
__attribute__((weak)) void setSample(lv_obj_t* obj, const Sample* s)
{
    if (lv_obj_get_user_data(obj) == (void*)s) return;
    lv_obj_set_user_data(obj, (void*)s);
    lv_obj_invalidate(obj);
}
__attribute__((weak)) void setShowTime(lv_obj_t*, bool) {}
__attribute__((weak)) void setHandles(lv_obj_t*, bool) {}
__attribute__((weak)) void setPlayhead(lv_obj_t*, float) {}
}  // namespace orbit::ui::wave

// ------------------------------------------------------------------ checks
static int g_checks = 0, g_fails = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        ++g_checks;                                          \
        if (!(cond)) {                                       \
            ++g_fails;                                       \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);    \
            printf(__VA_ARGS__);                             \
            printf("\n");                                    \
        }                                                    \
    } while (0)
#define INFO(...)              \
    do {                       \
        printf("  info: ");    \
        printf(__VA_ARGS__);   \
        printf("\n");          \
    } while (0)

// ------------------------------------------------------------------ test SD card
static void putLe(FILE* f, uint32_t v, int n)
{
    for (int i = 0; i < n; ++i) fputc((int)((v >> (8 * i)) & 0xFF), f);
}

// 16-bit PCM WAV, mono or stereo (same signal on both channels).
static bool writeWav(const std::string& path, int rate, int channels, int frames, float (*gen)(int, int, int, int))
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
    for (int i = 0; i < frames; ++i) {
        float v = gen(i, frames, rate, 0);
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        const int16_t s = (int16_t)lrintf(v * 30000.0f);
        for (int c = 0; c < channels; ++c) putLe(f, (uint16_t)s, 2);
    }
    fclose(f);
    return true;
}

static uint32_t g_noise = 12345;
static float noise()
{
    g_noise = g_noise * 1664525u + 1013904223u;
    return (float)((int32_t)g_noise) / 2147483648.0f;
}
static const float kTau = 6.2831853f;
static float genKick(int i, int n, int r, int)
{
    const float t = (float)i / (float)r;
    const float f = 50.0f + 110.0f * expf(-t * 18.0f);
    static float ph = 0;
    if (i == 0) ph = 0;
    ph += kTau * f / (float)r;
    return sinf(ph) * expf(-t * 6.5f) * (i < n - 64 ? 1.0f : (float)(n - i) / 64.0f);
}
static float genSnare(int i, int, int r, int)
{
    const float t = (float)i / (float)r;
    return (0.6f * noise() + 0.5f * sinf(kTau * 185.0f * t)) * expf(-t * 16.0f);
}
static float genHat(int i, int, int r, int)
{
    static float prev = 0;
    const float t = (float)i / (float)r;
    const float x = noise();
    const float y = x - prev;
    prev = x;
    return 0.5f * y * expf(-t * 40.0f);
}
static float genPerc(int i, int, int r, int)
{
    const float t = (float)i / (float)r;
    return sinf(kTau * 410.0f * t + 2.0f * sinf(kTau * 90.0f * t)) * expf(-t * 11.0f);
}
static float genSub(int i, int, int r, int)
{
    const float t = (float)i / (float)r;
    return 0.9f * sinf(kTau * 65.4f * t) * (t < 0.01f ? t / 0.01f : expf(-(t - 0.01f) * 1.6f));
}
static float genChord(int i, int n, int r, int)
{
    const float t = (float)i / (float)r, T = (float)n / (float)r;
    const float env = (t < 0.25f ? t / 0.25f : 1.0f) * (t > T - 0.6f ? (T - t) / 0.6f : 1.0f);
    return 0.28f * env * (sinf(kTau * 261.6f * t) + sinf(kTau * 329.6f * t) + sinf(kTau * 392.0f * t));
}
static float genLead(int i, int, int r, int)
{
    const float t = (float)i / (float)r;
    const float ph = fmodf(440.0f * t * (1.0f + 0.01f * sinf(kTau * 5.0f * t)), 1.0f);
    return 0.5f * (2.0f * ph - 1.0f) * expf(-t * 2.0f);
}
static float genVocal(int i, int, int r, int)
{
    const float t = (float)i / (float)r;
    const float f0 = 220.0f * (1.0f + 0.02f * sinf(kTau * 6.0f * t));
    float v = 0;
    for (int h = 1; h < 12; ++h) {
        const float fh = f0 * (float)h;
        const float g = expf(-powf((fh - 700.0f) / 300.0f, 2.0f)) + 0.6f * expf(-powf((fh - 1200.0f) / 400.0f, 2.0f));
        v += g * sinf(kTau * fh * t);
    }
    return 0.35f * v * sinf(3.14159f * t / 0.6f);
}
static float genSpace(int i, int n, int r, int)
{
    const float t = (float)i / (float)r, T = (float)n / (float)r;
    return 0.5f * noise() * sinf(3.14159f * t / T) * (0.5f + 0.5f * sinf(kTau * 0.8f * t));
}
static float genRise(int i, int n, int r, int)
{
    const float t = (float)i / (float)r, T = (float)n / (float)r;
    const float f = 80.0f + 900.0f * (t / T) * (t / T);
    return 0.45f * (sinf(kTau * f * t) > 0 ? 1.0f : -1.0f) * (t / T);
}
static float genTake(int i, int n, int r, int)
{
    const float t = (float)i / (float)r;
    return 0.6f * sinf(kTau * (200.0f + (float)(n % 700)) * t) * expf(-t * 30.0f);
}

static const int kTakes = 2000;

static std::string g_sd;   // <root>/host/out/shot_browser_sd

static bool prepareSd(const std::string& root)
{
    const std::string orbit = root + "/ORBIT";
    std::error_code ec;
    fs::create_directories(orbit + "/KITS", ec);
    // Kits: copied from the repository card (read-only use of sdcard/).
    for (const char* kit : {"808_STYLE", "909_STYLE"}) {
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
        float (*gen)(int, int, int, int);
    };
    const W files[] = {
        {"Drums", "KICK_01", 0.420f, 48000, 1, genKick},       {"Drums", "SNARE_02", 0.280f, 48000, 1, genSnare},
        {"Drums", "HAT_CLOSED", 0.118f, 48000, 1, genHat},     {"Drums", "PERC_04", 0.355f, 48000, 1, genPerc},
        {"Bass", "SUB_BASS_C2", 1.840f, 48000, 1, genSub},     {"Synth", "SOFT_CHORD", 2.10f, 44100, 2, genChord},
        {"Synth", "NEON_LEAD", 0.92f, 48000, 1, genLead},      {"Vocals", "VOCAL_CHOP", 0.60f, 48000, 1, genVocal},
        {"FX", "SPACE_FX", 2.52f, 48000, 1, genSpace},         {"FX", "GLITCH_RISE", 1.22f, 48000, 1, genRise},
    };
    for (const W& w : files) {
        const std::string dir = orbit + "/SAMPLES/" + w.dir;
        fs::create_directories(dir, ec);
        const std::string p = dir + "/" + w.name + ".wav";
        if (!fs::exists(p) && !writeWav(p, w.rate, w.ch, (int)(w.sec * (float)w.rate + 0.5f), w.gen)) return false;
    }
    // 2000 short takes in USER/TAKES (virtualized list test; listed after SAMPLES).
    const std::string takes = orbit + "/USER/TAKES";
    fs::create_directories(takes, ec);
    char last[64];
    snprintf(last, sizeof last, "/TAKE_%04d.wav", kTakes);
    if (!fs::exists(takes + last)) {
        for (int i = 1; i <= kTakes; ++i) {
            char nm[64];
            snprintf(nm, sizeof nm, "/TAKE_%04d.wav", i);
            if (!writeWav(takes + nm, 48000, 1, 240 + (i * 37) % 2400, genTake)) return false;
        }
    }
    // Fresh PROJECTS folder every run (test output only).
    fs::remove_all(orbit + "/PROJECTS", ec);
    fs::create_directories(orbit + "/PROJECTS", ec);
    return true;
}

// ------------------------------------------------------------------ LVGL helpers
static void uiTimer(lv_timer_t*)
{
    app().tick();
    view_browser::refresh();
    dlg_settings::refresh();
}

static bool visible(lv_obj_t* o)
{
    for (; o; o = lv_obj_get_parent(o))
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    return true;
}

static lv_obj_t* findLabelIn(lv_obj_t* root, const char* text, bool prefix)
{
    if (!root) return nullptr;
    const uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* ch = lv_obj_get_child(root, (int32_t)i);
        if (lv_obj_check_type(ch, &lv_label_class) && visible(ch)) {
            const char* t = lv_label_get_text(ch);
            if (t && (prefix ? strncmp(t, text, strlen(text)) == 0 : strcmp(t, text) == 0)) return ch;
        }
        if (lv_obj_t* f = findLabelIn(ch, text, prefix)) return f;
    }
    return nullptr;
}

// Search the overlays first (newest on top), then the screen.
static lv_obj_t* findLabel(const char* text, bool prefix = false)
{
    lv_obj_t* top = lv_layer_top();
    for (int32_t i = (int32_t)lv_obj_get_child_count(top) - 1; i >= 0; --i)
        if (lv_obj_t* f = findLabelIn(lv_obj_get_child(top, i), text, prefix)) return f;
    return findLabelIn(lv_screen_active(), text, prefix);
}

static void center(lv_obj_t* o, int& x, int& y)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    x = (a.x1 + a.x2) / 2;
    y = (a.y1 + a.y2) / 2;
}

static bool tapObj(lv_obj_t* o)
{
    if (!o) return false;
    int x, y;
    center(o, x, y);
    lvhost_tap(x, y);
    return true;
}

static bool tapLabel(const char* text, bool prefix = false)
{
    lv_obj_t* l = findLabel(text, prefix);
    if (!l) {
        printf("  (label not found: %s)\n", text);
        return false;
    }
    // The clickable owner (button / row): the label itself is not clickable.
    lv_obj_t* o = l;
    while (o && !lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) o = lv_obj_get_parent(o);
    return tapObj(o ? o : l);
}

// Settings row control: the last child of the row holding the name label.
static lv_obj_t* rowControl(const char* name)
{
    lv_obj_t* l = findLabel(name);
    if (!l) return nullptr;
    lv_obj_t* row = lv_obj_get_parent(lv_obj_get_parent(l));
    const int32_t n = (int32_t)lv_obj_get_child_count(row);
    return n > 0 ? lv_obj_get_child(row, n - 1) : nullptr;
}

static lv_obj_t* rowNote(const char* name)
{
    lv_obj_t* l = findLabel(name);
    if (!l) return nullptr;
    lv_obj_t* left = lv_obj_get_parent(l);
    return lv_obj_get_child_count(left) > 1 ? lv_obj_get_child(left, 1) : nullptr;
}

static lv_obj_t* findTextarea(lv_obj_t* root)
{
    const uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* ch = lv_obj_get_child(root, (int32_t)i);
        if (lv_obj_check_type(ch, &lv_textarea_class)) return ch;
        if (lv_obj_t* f = findTextarea(ch)) return f;
    }
    return nullptr;
}

// Keyboard: type text into its field and press OK.
static bool keyboardEnter(const char* text)
{
    lv_obj_t* ta = findTextarea(lv_layer_top());
    if (!ta) return false;
    lv_textarea_set_text(ta, text);
    lvhost_run(50);
    return tapLabel("OK");
}

// Runs the UI until the loader queue is empty (real time: the loader is a thread).
static bool waitIdle(int max_real_ms = 8000)
{
    const auto t0 = std::chrono::steady_clock::now();
    int idle = 0;
    for (;;) {
        lvhost_run(16);
        if (app().loadingCount() == 0) {
            if (++idle > 3) return true;
        } else {
            idle = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (ms > max_real_ms) return false;
    }
}

static void settle(int ms = 700) { lvhost_run(ms); }

static void shot(const char* name)
{
    char p[160];
    snprintf(p, sizeof p, "host/shots/%s.png", name);
    lvhost_screenshot(p);
    printf("  shot %s\n", p);
}

// Screen chrome stand-in: the persistent panels as empty .panel boxes so the dialogs are seen in
// context (the real ones are other modules).
static lv_obj_t* buildScreen()
{
    lv_obj_t* scr = lv_screen_active();
    lv_obj_t* top = lv_obj_create(scr);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, 1280, geo::top_h);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(top, c(col::top_a), 0);
    lv_obj_set_style_bg_grad_color(top, c(col::top_b), 0);
    lv_obj_set_style_bg_grad_dir(top, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_side(top, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(top, 1, 0);
    lv_obj_set_style_border_color(top, c(col::top_line), 0);
    lv_obj_t* logo = label(top, "TAB5", f_b34, col::ink);
    lv_obj_set_pos(logo, 21, 10);
    lv_obj_t* pads = panelCreate(scr, geo::pads_x, geo::pads_y, geo::pads_w, geo::pads_h);
    panelHead(pads, Icon::Grid, "PADS");
    lv_obj_t* br = panelCreate(scr, geo::browser_x, geo::browser_y, geo::browser_w, geo::browser_h);
    panelHead(br, Icon::Grid, "BROWSER");
    panelCreate(scr, geo::macros_x, geo::macros_y, geo::macros_w, geo::macros_h);
    lv_obj_t* tl = panelCreate(scr, geo::timeline_x, geo::timeline_y, geo::timeline_w, geo::timeline_h);
    panelHead(tl, Icon::Grid, "SCENES");
    return panelCreate(scr, geo::work_x, geo::work_y, geo::work_w, geo::work_h);
}

static int countChildren(lv_obj_t* o)
{
    int n = 1;
    const uint32_t k = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < k; ++i) n += countChildren(lv_obj_get_child(o, (int32_t)i));
    return n;
}

// Window of the open modal (false when none).
static bool modalBox(lv_area_t* out)
{
    lv_obj_t* top = lv_layer_top();
    for (int32_t i = (int32_t)lv_obj_get_child_count(top) - 1; i >= 0; --i) {
        lv_obj_t* scrim = lv_obj_get_child(top, i);
        if (lv_obj_get_child_count(scrim) == 0 || lv_obj_get_width(scrim) < 1280) continue;
        lv_obj_update_layout(scrim);
        lv_obj_get_coords(lv_obj_get_child(scrim, 0), out);
        return true;
    }
    return false;
}

static void printModalBox()
{
    lv_obj_t* top = lv_layer_top();
    for (int32_t i = (int32_t)lv_obj_get_child_count(top) - 1; i >= 0; --i) {
        lv_obj_t* scrim = lv_obj_get_child(top, i);
        if (lv_obj_get_child_count(scrim) == 0 || lv_obj_get_width(scrim) < 1280) continue;   // not the toast
        lv_area_t a;
        lv_obj_get_coords(lv_obj_get_child(scrim, 0), &a);
        INFO("modal window at %d,%d %dx%d", (int)a.x1, (int)a.y1, (int)lv_area_get_width(&a), (int)lv_area_get_height(&a));
        return;
    }
}

// ------------------------------------------------------------------ browser tests
static int posOf(const char* stem)
{
    for (int i = 0; i < app().resultCount(); ++i)
        if (strcmp(app().library().at(app().result(i)).name, stem) == 0) return i;
    return -1;
}

static int rowY(int pos) { return 143 + pos * 43 + 21; }   // list scrolled to the top

static void browserTests()
{
    printf("browser: All Samples\n");
    app().setCategory(Category::All);
    app().selectPad(0, false);
    settle(300);
    const int total = app().resultCount();
    CHECK(total >= kTakes + 10, "All Samples lists %d entries (want >= %d)", total, kTakes + 10);
    CHECK(findLabel("SUB_BASS_C2.wav") != nullptr, "first file row visible");
    CHECK(findLabel("Wybierz plik z listy") != nullptr, "preview hint without a selection");

    const int kick = posOf("KICK_01");
    CHECK(kick >= 0 && kick < 8, "KICK_01 at list position %d", kick);
    lvhost_tap(500, rowY(kick));
    CHECK(app().ui().lib_sel == app().result(kick), "tap selects KICK_01 (lib_sel %d)", app().ui().lib_sel);
    CHECK(waitIdle(), "probe/preview job finished");
    settle(400);
    const Sample* bp = app().previewSample();
    CHECK(bp && strstr(bp->path, "KICK_01") != nullptr, "preview sample decoded for the waveform");
    CHECK(findLabel("KICK_01") != nullptr, "preview name shows KICK_01");
    CHECK(findLabel("48 KHZ \xE2\x80\xA2 16-BIT WAV \xE2\x80\xA2 0.420S") != nullptr, "file info line");
    CHECK(findLabel("0.420s") != nullptr, "duration column of KICK_01 (lazy probe)");
    CHECK(findLabel("LOAD TO A1") != nullptr, "LOAD TO <pad> label");
    settle(2600);   // let the toast (if any) go away
    shot("browser_all");

    // ▶ PREVIEW: the engine plays the decoded file (rendered here, there is no audio device).
    {
        int16_t buf[2 * 256];
        int before = 0, after = 0;
        for (int i = 0; i < 8; ++i) {
            App::render(buf, 256, nullptr);
            for (int k = 0; k < 512; ++k) before = abs(buf[k]) > before ? abs(buf[k]) : before;
        }
        tapLabel("\xE2\x96\xB6 PREVIEW");
        for (int i = 0; i < 16; ++i) {
            App::render(buf, 256, nullptr);
            for (int k = 0; k < 512; ++k) after = abs(buf[k]) > after ? abs(buf[k]) : after;
        }
        CHECK(before < 64 && after > 1000, "PREVIEW plays the file (peak %d -> %d)", before, after);
        for (int i = 0; i < 400; ++i) App::render(buf, 256, nullptr);   // let it finish
        lvhost_run(50);
    }

    // LOAD TO A1.
    tapLabel("LOAD TO A1");
    CHECK(waitIdle(), "load job finished");
    CHECK(strcmp(app().padSampleName(0), "KICK_01") == 0, "LOAD TO A1 -> pad A1 = %s", app().padSampleName(0));

    // Pad selection follows into the labels. The third button says that it loads (LOAD & EDIT
    // <pad>), replaces another sample only after a second tap, and reads OPEN IN SAMPLER (view
    // switch only) when the pad already holds the file.
    app().selectPad(1, false);
    lvhost_run(50);
    CHECK(findLabel("LOAD TO A2") != nullptr, "label follows the selected pad");
    CHECK(findLabel("LOAD & EDIT A2") != nullptr, "LOAD & EDIT names the pad");
    app().selectLayer(1);
    lvhost_run(50);
    CHECK(findLabel("LOAD TO A2 \xC2\xB7 L2") != nullptr, "label shows the layer");
    CHECK(findLabel("LOAD & EDIT A2 \xC2\xB7 L2") != nullptr, "LOAD & EDIT shows the layer");
    app().selectLayer(0);
    const int snare = posOf("SNARE_02");
    lvhost_tap(500, rowY(snare));
    CHECK(app().ui().lib_sel == app().result(snare), "SNARE_02 selected");
    waitIdle();
    settle(2700);   // earlier toasts gone
    char a2[64];
    snprintf(a2, sizeof a2, "%s", app().padSampleName(1));
    tapLabel("LOAD & EDIT A2");
    settle(100);
    CHECK(app().ui().view == View::Browser && strcmp(app().padSampleName(1), a2) == 0,
          "first LOAD & EDIT tap over %s only warns (view %d, pad A2 = %s)", a2, (int)app().ui().view, app().padSampleName(1));
    CHECK(findLabel("A2 gra ", true) != nullptr, "the warning names the sample that would be replaced");
    tapLabel("LOAD & EDIT A2");
    CHECK(waitIdle(), "load-and-edit load finished");
    CHECK(app().ui().view == View::Sample, "second LOAD & EDIT tap switches to the SAMPLE view");
    CHECK(strcmp(app().padSampleName(1), "SNARE_02") == 0, "pad A2 = %s", app().padSampleName(1));
    app().setView(View::Browser);
    settle(100);
    CHECK(findLabel("OPEN IN SAMPLER") != nullptr, "pad A2 holds SNARE_02: OPEN IN SAMPLER");
    {
        const uint32_t samples = app().revs().samples;
        tapLabel("OPEN IN SAMPLER");
        settle(100);
        CHECK(app().ui().view == View::Sample && app().revs().samples == samples && app().loadingCount() == 0,
              "OPEN IN SAMPLER only switches the view (no reload)");
        app().setView(View::Browser);
    }
    // An empty layer is loaded at once.
    app().selectLayer(3);
    settle(100);
    if (!app().padSample(1, 3)) {
        tapLabel("LOAD & EDIT A2 \xC2\xB7 L4");
        CHECK(waitIdle(), "layer 4 load finished");
        const Sample* l4 = app().padSample(1, 3);
        CHECK(app().ui().view == View::Sample && l4 && strstr(l4->path, "SNARE_02") != nullptr,
              "empty layer: one tap loads + opens the sampler");
        app().clearPadLayer(1, 3);
    }
    app().selectLayer(0);
    app().setView(View::Browser);
    settle(100);

    tapLabel("SAMPLE DETAILS");
    CHECK(g_details_opened == 1, "SAMPLE DETAILS opens dlg_sample (%d)", g_details_opened);

    // Second tap on the selected row = preview (no crash, selection kept).
    lvhost_tap(500, rowY(snare));
    CHECK(app().ui().lib_sel == app().result(snare), "second tap keeps the selection");
    waitIdle();

    // Virtualized list: object count stays constant while scrolling through 2000+ entries.
    lv_obj_t* first = findLabel("SUB_BASS_C2.wav");
    lv_obj_t* list = first ? lv_obj_get_parent(lv_obj_get_parent(first)) : nullptr;
    CHECK(list != nullptr, "list container found");
    if (list) {
        const int objs = countChildren(list);
        CHECK(objs < 80, "list uses %d objects for %d entries", objs, total);
        lvhost_drag(550, 500, 550, 170, 250);
        settle(1500);
        CHECK(findLabel("TAKE_", true) != nullptr, "TAKE_ rows appear after a drag");
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 200; ++i) {
            lv_obj_scroll_by(list, 0, -43, LV_ANIM_OFF);
            lv_refr_now(nullptr);
        }
        const double us = (double)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count() / 200.0;
        INFO("scroll by one row + full refresh: %.0f us per step on the PC", us);
        lv_obj_scroll_to_y(list, 1000000, LV_ANIM_OFF);
        settle(600);
        char lastName[32];
        snprintf(lastName, sizeof lastName, "TAKE_%04d.wav", kTakes);
        CHECK(findLabel(lastName) != nullptr, "last entry %s reachable", lastName);
        CHECK(countChildren(list) == objs, "object count constant after scrolling (%d)", countChildren(list));
        lv_obj_t* lastLbl = findLabel(lastName);
        if (lastLbl) {
            lv_obj_t* dur = lv_obj_get_child(lv_obj_get_parent(lastLbl), 2);
            CHECK(dur && lv_label_get_text(dur)[0] != 0, "duration probed lazily for the last rows (%s)",
                  dur ? lv_label_get_text(dur) : "-");
        }
        shot("browser_scrolled");
        lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
        settle(100);
    }

    // Kits category: SD kit folders + two built-ins.
    printf("browser: Kits\n");
    app().setCategory(Category::Kits);
    settle(300);
    CHECK(findLabel("808_STYLE") && findLabel("909_STYLE"), "SD kit folders listed");
    CHECK(findLabel("808 STYLE \xC2\xB7 wbudowany") && findLabel("909 STYLE \xC2\xB7 wbudowany"), "built-in kits listed");
    lvhost_tap(500, rowY(2));   // 808 STYLE (built-in)
    settle(300);
    CHECK(findLabel("SELECTED KIT") != nullptr, "kit preview card");
    CHECK(findLabel("LOAD KIT") != nullptr, "LOAD KIT button");
    CHECK(findLabel("OPEN IN SAMPLER") == nullptr, "file-only buttons hidden for kits");
    settle(2600);
    shot("browser_kits");
    // The pads hold the 909 kit: the first LOAD KIT only says what it replaces, the second loads.
    char a1_before[kNameLen];
    snprintf(a1_before, sizeof a1_before, "%s", app().project().pad[0].name);
    tapLabel("LOAD KIT");
    settle(100);
    CHECK(strcmp(app().project().pad[0].name, a1_before) == 0, "first LOAD KIT only asks (pad A1 = %s)",
          app().project().pad[0].name);
    tapLabel("LOAD KIT");
    settle(100);
    const BuiltinKit& k808 = builtinKit(0);
    CHECK(strcmp(app().project().pad[0].name, k808.sounds[0].name) == 0, "built-in 808 loaded (pad A1 = %s)",
          app().project().pad[0].name);
    // UNDO LOAD KIT (8 s) brings the 909 pads back; a second load needs the confirmation again.
    CHECK(findLabel("UNDO LOAD KIT") != nullptr, "LOAD KIT reads UNDO LOAD KIT after the load");
    tapLabel("UNDO LOAD KIT");
    settle(100);
    CHECK(strcmp(app().project().pad[0].name, a1_before) == 0, "UNDO LOAD KIT restored pad A1 (%s)",
          app().project().pad[0].name);
    CHECK(findLabel("LOAD KIT") != nullptr && findLabel("UNDO LOAD KIT") == nullptr, "button reads LOAD KIT again");
    tapLabel("LOAD KIT");
    settle(100);
    tapLabel("LOAD KIT");
    settle(100);
    CHECK(strcmp(app().project().pad[0].name, k808.sounds[0].name) == 0, "built-in 808 loaded again (pad A1 = %s)",
          app().project().pad[0].name);
    lvhost_tap(500, rowY(1));   // 909_STYLE (SD folder)
    settle(200);
    CHECK(findLabel("909_STYLE") != nullptr, "SD kit selected");
    // Loader activity tag: the job counts as pending until App::tick() takes its result.
    app().loadKit(app().ui().lib_sel);
    view_browser::refresh();
    CHECK(findLabel("WCZYTYWANIE…") != nullptr, "WCZYTYWANIE tag while the loader works (%d jobs)", app().loadingCount());
    CHECK(waitIdle(), "kit load (direct) finished");
    settle(100);
    CHECK(findLabel("WCZYTYWANIE…") == nullptr, "tag hidden when idle");
    app().loadBuiltinKit(0);
    tapLabel("LOAD KIT");   // replaces the 808 pads: confirm
    settle(100);
    tapLabel("LOAD KIT");
    CHECK(waitIdle(), "kit load finished");
    CHECK(strcmp(app().padSampleName(0), "01_Kick") == 0, "SD kit 909_STYLE -> pad A1 = %s", app().padSampleName(0));
    shot("browser_kits_sd");

    // Search with the on-screen keyboard.
    printf("browser: search\n");
    app().setCategory(Category::All);
    settle(200);
    lvhost_tap(555, 115);
    settle(400);
    lv_obj_t* ta = findTextarea(lv_layer_top());
    CHECK(ta != nullptr, "tapping the search field opens the keyboard");
    if (ta) {
        lv_textarea_set_text(ta, "kick");
        settle(300);
        shot("browser_search_kb");
        tapLabel("OK");
        settle(300);
    }
    CHECK(strcmp(app().ui().search, "kick") == 0, "search = '%s'", app().ui().search);
    CHECK(app().resultCount() == 1, "one result for 'kick' (%d)", app().resultCount());
    CHECK(findLabel("KICK_01.wav") != nullptr && findLabel("SUB_BASS_C2.wav") == nullptr, "list filtered");
    settle(2600);
    shot("browser_search");
    // Clear button.
    lvhost_tap(756, 115);
    settle(200);
    CHECK(app().ui().search[0] == 0 && app().resultCount() == total, "clear button resets the search (%d)", app().resultCount());
    // focusSearch() (magnifier of the BROWSER panel) opens the keyboard directly.
    view_browser::focusSearch();
    settle(300);
    CHECK(findTextarea(lv_layer_top()) != nullptr, "focusSearch opens the keyboard");
    tapLabel("CANCEL");
    settle(300);
    CHECK(findTextarea(lv_layer_top()) == nullptr, "keyboard closed");
    // No results message.
    app().setSearch("zzzz");
    settle(100);
    CHECK(findLabel("Brak wyników dla", true) != nullptr, "empty result message");
    app().setSearch("");
    settle(100);
}

// ------------------------------------------------------------------ settings tests
static void settingsTests()
{
    printf("settings\n");
    const Settings before = app().settings();
    dlg_settings::open();
    settle(500);
    printModalBox();
    shot("dlg_settings_general");
    CHECK(findLabel("SETTINGS / TAB5") != nullptr, "settings modal open");

    lv_area_t win0 = {};
    CHECK(modalBox(&win0), "settings window found");

    // Auto-save toggle edits the copy only.
    tapObj(rowControl("Auto-save"));
    CHECK(app().settingsEdit().autosave != before.autosave, "Auto-save toggled in the edit copy");
    CHECK(app().settings().autosave == before.autosave, "active settings unchanged before APPLY");
    // A tap on the scrim (beside the window) never throws unsaved edits away.
    settle(2700);
    lvhost_tap(100, 600);
    settle(200);
    CHECK(modalIsOpen() && app().settingsEdit().autosave != before.autosave, "scrim tap keeps the dialog and the edits");
    CHECK(findLabel("Niezapisane ustawienia", true) != nullptr, "scrim tap explains APPLY / X");
    // Project name via keyboard.
    tapObj(rowControl("Project name"));
    settle(300);
    CHECK(keyboardEnter("MY_SET"), "project name keyboard");
    settle(300);
    // Launch quantization popup -> 1/4.
    tapObj(rowControl("Launch quantization"));
    settle(200);
    tapLabel("1/4");
    settle(200);

    static const char* const kTabs[] = {"Audio", "MIDI", "Bluetooth", "Display", "Touch", "Storage", "About"};
    static const char* const kShots[] = {"dlg_settings_audio", "dlg_settings_midi", "dlg_settings_bluetooth_off",
                                         "dlg_settings_display", "dlg_settings_touch", "dlg_settings_storage",
                                         "dlg_settings_about"};
    for (int i = 0; i < 7; ++i) {
        tapLabel(kTabs[i]);
        settle(300);
        CHECK(app().ui().settings_tab == i + 1, "tab %s (%d)", kTabs[i], app().ui().settings_tab);
        if (i == 0) {
            // Buffer 128 -> latency note.
            tapObj(rowControl("Buffer"));
            settle(200);
            tapLabel("128 frames");
            settle(200);
            lv_obj_t* note = rowNote("Buffer");
            CHECK(note && strstr(lv_label_get_text(note), "10.7 ms"), "buffer latency note: %s",
                  note ? lv_label_get_text(note) : "-");
            CHECK(app().settingsEdit().buffer_frames == 128, "buffer 128 in the edit copy");
        }
        if (i == 1) {
            CHECK(findLabel("Brak urządzenia") != nullptr, "MIDI status line (no device on the PC)");
        }
        if (i == 2) {   // Bluetooth, off: the switch, the status and the empty list say so
            CHECK(findLabel("Moduł Pico z Aurory, złącze M5-Bus") != nullptr, "Bluetooth switch row");
            CHECK(findLabel("Wyłączony") != nullptr, "module status: off");
            CHECK(findLabel("Włącz Bluetooth, aby zobaczyć zapisane urządzenia") != nullptr, "device list: off");
            CHECK(findLabel("AirPods") == nullptr, "no AirPods row without AirPods");
        }
        if (i == 3) {
            lv_obj_t* sl = rowControl("Brightness");
            if (sl) {
                lv_area_t a;
                lv_obj_get_coords(sl, &a);
                lvhost_drag(a.x2 - 10, (a.y1 + a.y2) / 2, a.x1 + 20, (a.y1 + a.y2) / 2, 200);
                CHECK(halHostBrightness() < before.brightness, "brightness previews live while dragging (%d)", halHostBrightness());
            } else {
                CHECK(false, "brightness slider not found");
            }
        }
        if (i == 5) {
            CHECK(findLabel(app().sdStatus()) != nullptr, "SD status shown (%s)", app().sdStatus());
            tapLabel("EXPORT JSON");
            CHECK(waitIdle(), "export from settings finished");
            char jp[96];
            snprintf(jp, sizeof jp, "/ORBIT/PROJECTS/%s.json", app().project().name);
            CHECK(fs::exists(g_sd + jp), "Storage > EXPORT JSON wrote %s", jp);
            tapLabel("SCAN SD");
            CHECK(app().loadingCount() > 0 || waitIdle(), "SCAN SD started a rescan");
            CHECK(waitIdle(), "rescan finished");
            CHECK(app().library().count() >= kTakes + 12, "library after rescan: %d", app().library().count());
            CHECK(modalIsOpen(), "dialog stays open");
        }
        if (i == 6) {
            settle(600);
            CHECK(findLabel("CPU audio", true) != nullptr, "live CPU / voices line");
            CHECK(findLabel("Wolna pamięć", true) != nullptr, "live memory line");
            CHECK(findLabel("PC host (no Tab5 hardware)") != nullptr, "board info line");
        }
        settle(400);
        printModalBox();
        {
            lv_area_t w = {};
            CHECK(modalBox(&w) && w.x1 == win0.x1 && w.y1 == win0.y1 && w.x2 == win0.x2 && w.y2 == win0.y2,
                  "tab %s: window stays at %d,%d-%d,%d (now %d,%d-%d,%d)", kTabs[i], (int)win0.x1, (int)win0.y1,
                  (int)win0.x2, (int)win0.y2, (int)w.x1, (int)w.y1, (int)w.x2, (int)w.y2);
        }
        shot(kShots[i]);
        if (i == 1) {
            tapLabel("START LEARN");
            settle(200);
            CHECK(app().midiLearning() && findLabel("LEARNING\xE2\x80\xA6") != nullptr, "START LEARN arms MIDI learn");
            shot("dlg_settings_midi_learning");
            tapLabel("LEARNING\xE2\x80\xA6");
            settle(200);
            CHECK(!app().midiLearning() && findLabel("START LEARN") != nullptr, "second tap ends MIDI learn");
        }
    }
    // BACK TO LAUNCHER on the PC: no launcher -> the button is not shown (integration: App::hasLauncher()).
    CHECK(findLabel("BACK TO LAUNCHER") == nullptr, "no BACK TO LAUNCHER without a launcher");
    CHECK(modalIsOpen(), "dialog stays open");
    // Touch: pad velocity select + fixed velocity note.
    tapLabel("Touch");
    settle(300);
    // Fixed is the default (62 px pads are too small for a position curve): switch to Position.
    CHECK(app().settingsEdit().pad_velocity == PadVelocity::Fixed, "pad velocity Fixed by default");
    tapObj(rowControl("Pad velocity"));
    settle(200);
    tapLabel("Position");
    settle(200);
    CHECK(app().settingsEdit().pad_velocity == PadVelocity::Position, "pad velocity Position in the edit copy");
    // MIDI: channel select.
    tapLabel("MIDI");
    settle(300);
    tapObj(rowControl("Channel"));
    settle(200);
    tapLabel("12");   // the list opens scrolled to the current channel (10)
    settle(200);
    CHECK(app().settingsEdit().midi_channel == 12, "MIDI channel 12 (%d)", app().settingsEdit().midi_channel);
    lv_obj_t* map = rowNote("CC map");
    CHECK(map && strstr(lv_label_get_text(map), "0 / 32"), "CC map note (%s)", map ? lv_label_get_text(map) : "-");
    // APPLY.
    tapLabel("APPLY");
    settle(400);
    CHECK(!modalIsOpen(), "APPLY closes the dialog");
    CHECK(app().settings().autosave != before.autosave, "APPLY stored Auto-save");
    CHECK(app().settings().buffer_frames == 128, "APPLY stored buffer 128");
    CHECK(app().settings().brightness < before.brightness, "APPLY stored brightness %d", app().settings().brightness);
    CHECK(app().settings().pad_velocity == PadVelocity::Position && app().settings().midi_channel == 12,
          "APPLY stored pad velocity + MIDI channel");
    CHECK(halHostBrightness() == app().settings().brightness, "hardware brightness = applied value");
    CHECK(strcmp(app().project().name, "MY_SET") == 0, "APPLY renamed the project (%s)", app().project().name);
    CHECK(app().project().quant == LaunchQuant::Beat, "APPLY set launch quantization 1/4");

    // Discard: change brightness, close with X -> restored.
    const int bright = app().settings().brightness;
    dlg_settings::open();
    settle(400);
    tapLabel("Display");
    settle(300);
    lv_obj_t* sl = rowControl("Brightness");
    if (sl) {
        lv_area_t a;
        lv_obj_get_coords(sl, &a);
        lvhost_drag(a.x1 + 10, (a.y1 + a.y2) / 2, a.x2 - 5, (a.y1 + a.y2) / 2, 200);
    }
    CHECK(halHostBrightness() != bright, "live brightness changed (%d)", halHostBrightness());
    // X with unsaved edits: the first tap only warns, the second one (within 3 s) discards.
    {
        lv_obj_t* x = nullptr;
        lv_obj_t* top = lv_layer_top();
        for (int32_t i = (int32_t)lv_obj_get_child_count(top) - 1; i >= 0 && !x; --i) {
            lv_obj_t* scrim = lv_obj_get_child(top, i);
            if (lv_obj_get_child_count(scrim) == 0 || lv_obj_get_width(scrim) < 1280) continue;
            lv_obj_t* head = lv_obj_get_child(lv_obj_get_child(scrim, 0), 0);
            x = head ? lv_obj_get_child(head, -1) : nullptr;
        }
        CHECK(x != nullptr, "close button found");
        settle(2700);
        tapObj(x);
        settle(200);
        CHECK(modalIsOpen() && halHostBrightness() != bright, "first X tap with unsaved edits keeps the dialog");
        CHECK(findLabel("Niezapisane ustawienia", true) != nullptr, "X explains the second tap");
        tapObj(x);
        settle(300);
    }
    CHECK(!modalIsOpen(), "second X tap closes");
    CHECK(halHostBrightness() == bright, "closing without APPLY restores brightness (%d vs %d)", halHostBrightness(), bright);
    CHECK(app().settingsEdit().brightness == app().settings().brightness, "edit copy discarded");

    // No edits: X closes at once.
    dlg_settings::open();
    settle(400);
    {
        lv_area_t w = {};
        CHECK(modalBox(&w), "settings open again");
        lvhost_tap(w.x2 - 14 - 20, w.y1 + 28);
        settle(300);
        CHECK(!modalIsOpen(), "X without edits closes at once");
    }

    // Renaming to the name of another saved project needs a second APPLY (FAT ignores case).
    {
        const std::string taken = g_sd + "/ORBIT/PROJECTS/TAKEN_SET.orb";
        FILE* f = fopen(taken.c_str(), "wb");
        if (f) fclose(f);
        dlg_settings::open();
        settle(400);
        tapLabel("General");
        settle(300);
        tapObj(rowControl("Project name"));
        settle(300);
        CHECK(keyboardEnter("taken_set"), "project name keyboard (taken)");
        settle(2700);
        tapLabel("APPLY");
        settle(200);
        CHECK(modalIsOpen() && strcmp(app().project().name, "MY_SET") == 0, "first APPLY with a taken name only warns");
        CHECK(findLabel("Projekt taken_set już istnieje", true) != nullptr, "the warning stays visible");
        tapLabel("APPLY");
        settle(200);
        CHECK(!modalIsOpen() && strcmp(app().project().name, "taken_set") == 0, "second APPLY renames (%s)", app().project().name);
        CHECK(findLabel("Zapisano ustawienia \xE2\x80\x94 SAVE nadpisze projekt taken_set") != nullptr,
              "after APPLY the toast still names the overwrite");
        app().renameProject("MY_SET");
        std::error_code ec;
        fs::remove(taken, ec);
        settle(2700);
    }

    // Master limiter shows and sets the project's limiter (the switch of mixer L / FX LIMITER).
    {
        app().setLimiter(false);
        dlg_settings::open();
        settle(400);
        tapLabel("Audio");
        settle(300);
        tapObj(rowControl("Master limiter"));
        settle(100);
        tapLabel("APPLY");
        settle(200);
        CHECK(app().project().limiter_on == 1, "Settings > Audio limiter switched the project's limiter on");
        dlg_settings::open();
        settle(400);
        tapLabel("Audio");
        settle(300);
        tapObj(rowControl("Master limiter"));
        settle(100);
        tapLabel("APPLY");
        settle(200);
        CHECK(app().project().limiter_on == 0 && app().settings().master_limiter == 0,
              "and off again (the setting follows: new projects start the same way)");
        app().setLimiter(true);
        settle(2700);
    }

    // About -> OPEN UI GUIDE replaces the dialog with the guide.
    dlg_settings::open();
    settle(300);
    tapLabel("About");
    settle(300);
    tapLabel("OPEN UI GUIDE");
    settle(500);
    CHECK(findLabel("UI GUIDE / INTERACTIONS") != nullptr, "OPEN UI GUIDE opens the guide");
    modalClose();
    settle(300);
}

// ------------------------------------------------------------------ Bluetooth tab (fake Pico of hal_host)
// Control of the setting row whose note is `note` (row names like "Bluetooth" also name the tab and the title).
static lv_obj_t* rowControlByNote(const char* note)
{
    lv_obj_t* l = findLabel(note);
    lv_obj_t* row = l ? lv_obj_get_parent(lv_obj_get_parent(l)) : nullptr;
    const int32_t n = row ? (int32_t)lv_obj_get_child_count(row) : 0;
    return n > 0 ? lv_obj_get_child(row, n - 1) : nullptr;
}

// Every command ORBIT sent to the Pico since the last call, one per line.
static std::string btSent()
{
    std::string all;
    char line[128];
    while (halHostBtTakeSent(line, sizeof line) > 0) {
        all += line;
        all += '\n';
    }
    return all;
}

// Scrolls o into view first: the Bluetooth section is taller than the window (a tap goes to screen coordinates).
static lv_obj_t* inView(lv_obj_t* o)
{
    if (o) {
        lv_obj_scroll_to_view_recursive(o, LV_ANIM_OFF);
        lvhost_run(50);
    }
    return o;
}

static void btFeed(const char* line)
{
    halHostBtFeed(line);
    lvhost_run(20);
}

static void bluetoothTests()
{
    printf("bluetooth\n");
    CHECK(!app().btEnabled() && app().engine().sampleRate() == 48000, "Bluetooth off, engine at 48 kHz");
    dlg_settings::openAt(dlg_settings::kTabBluetooth);
    settle(400);
    CHECK(modalIsOpen() && app().ui().settings_tab == dlg_settings::kTabBluetooth, "openAt opens the Bluetooth tab (%d)",
          app().ui().settings_tab);
    lv_obj_t* sw = rowControlByNote("Moduł Pico z Aurory, złącze M5-Bus");
    CHECK(sw != nullptr, "Bluetooth switch found");
    tapObj(sw);
    settle(200);
    CHECK(app().btEnabled() && app().engine().sampleRate() == 44100, "switch on: Bluetooth on, engine at 44.1 kHz (%d)",
          app().engine().sampleRate());
    CHECK(findLabel("szukam modułu\xE2\x80\xA6") != nullptr, "module status while the Pico has not answered");
    halHostBtAdvance(500);
    btSent();

    // The Pico answers, lists two saved devices and a new one, connects the selected AirPods.
    btFeed("READY version=0.9.5 cap_airpods=1");
    btFeed("DEVICE 1 A4:C3:37:5E:91:0B -127 1 1 AirPods Pro");
    btFeed("DEVICE 2 00:42:79:B1:22:EC -127 1 0 JBL Flip 6");
    btFeed("DEVICE 3 F8:4E:17:3A:6D:50 -71 0 0 WH-1000XM4");
    btFeed("END_LIST");
    btFeed("STATE connection=2 audio=2");
    btFeed("AIRPODS connected=1 mode=2 left=80 right=75 case=40 ears=3 auto_pause=0");
    halHostBtAdvance(200);
    settle(400);
    const hal::BtStatus& st = app().btStatus();
    CHECK(st.module && st.connected && strcmp(st.version, "0.9.5") == 0, "status: module %d connected %d version %s",
          st.module, st.connected, st.version);
    CHECK(app().btDeviceCount() == 3, "three devices (%d)", app().btDeviceCount());
    CHECK(findLabel("Pico 0.9.5 \xC2\xB7 połączono: ", true) != nullptr, "module status: connected");
    CHECK(findLabel("AirPods Pro") && findLabel("JBL Flip 6") && findLabel("WH-1000XM4"), "device rows");
    CHECK(findLabel("CONNECTED") && findLabel("SAVED"), "device tags");
    CHECK(findLabel("AirPods") != nullptr && findLabel("L 80 % \xC2\xB7 R 75 % \xC2\xB7 etui 40 %", true) != nullptr,
          "AirPods row with the batteries");
    settle(2700);   // the connect toast fades
    shot("dlg_settings_bluetooth");
    // The lower half of the section (output, volume, AirPods, latency note).
    if (lv_obj_t* note = findLabel("Bluetooth dodaje ok.", true)) {
        lv_obj_scroll_to_view_recursive(note, LV_ANIM_OFF);
        settle(100);
        shot("dlg_settings_bluetooth_end");
    } else {
        CHECK(false, "latency note");
    }

    // A tap on a device row connects it, SCAN shows SCANNING…, the AirPods mode reaches the Pico.
    tapObj(inView(findLabel("JBL Flip 6")));
    settle(100);
    std::string sent = btSent();
    CHECK(sent.find("SELECTMAC 00:42:79:B1:22:EC") != std::string::npos, "row tap -> SELECTMAC (%s)", sent.c_str());
    tapObj(inView(rowControlByNote("Wyjście: dźwięk także z głośnika lub gniazda 3,5 mm (ok. 0,2 s przed Bluetooth)")));
    settle(100);
    CHECK(app().settings().bt_local == 1, "Also speaker / jack on (off by default)");
    tapObj(inView(rowControl("AirPods")));
    settle(200);
    tapLabel("TRANSPARENCY");
    settle(100);
    sent = btSent();
    CHECK(sent.find("AIRPODS MODE 3") != std::string::npos, "AirPods mode -> AIRPODS MODE 3 (%s)", sent.c_str());
    if (lv_obj_t* scan = findLabel("SCAN")) {
        lv_obj_scroll_to_view_recursive(scan, LV_ANIM_OFF);
        settle(100);
    }
    tapLabel("SCAN");
    settle(100);
    sent = btSent();
    CHECK(sent.find("SCAN") != std::string::npos, "SCAN sent (%s)", sent.c_str());
    btFeed("RESTART scanning; saved target preserved");
    halHostBtAdvance(100);
    settle(200);
    CHECK(findLabel("SCANNING\xE2\x80\xA6") != nullptr, "SCAN button reads SCANNING\xE2\x80\xA6 while scanning");
    btFeed("SCAN_DONE");
    halHostBtAdvance(100);
    settle(200);
    CHECK(findLabel("SCANNING\xE2\x80\xA6") == nullptr && findLabel("SCAN") != nullptr, "scan finished");

    // Closing without APPLY keeps Bluetooth (it is not part of the APPLY copy) and it is saved.
    modalClose();
    settle(300);
    Settings rd;
    CHECK(hal::settingsLoad(rd) && rd.bt_enabled == 1 && rd.bt_local == 1 && rd.sample_rate == 48000,
          "saved: bt_enabled %d bt_local %d rate %u", rd.bt_enabled, rd.bt_local, (unsigned)rd.sample_rate);
    app().btSetLocal(false);
    app().btSetEnabled(false);
    settle(300);
    CHECK(!app().btEnabled() && app().engine().sampleRate() == 48000, "off again: engine back at 48 kHz");
    settle(2700);
}

// ------------------------------------------------------------------ project manager tests
static void projectTests()
{
    printf("project manager\n");
    for (const char* n : {"NEON_DRAFT", "LIVE_SESSION_02", "EMPTY_PROJECT", "ORBIT_01"}) {
        CHECK(app().saveAs(n), "saveAs %s", n);
        CHECK(waitIdle(), "save %s finished", n);
    }
    settle(3000);
    CHECK(!app().dirty(), "saved project is clean");
    app().toggleStep(2, 3);   // unsaved change
    CHECK(app().dirty(), "edit makes the project dirty");
    dlg_project::open();
    settle(500);
    printModalBox();
    shot("dlg_project");
    CHECK(findLabel("PROJECT MANAGER") != nullptr, "project manager open");
    CHECK(findLabel("ORBIT_01") != nullptr, "current project card");
    CHECK(findLabel("16 pads \xE2\x80\xA2 4 scenes \xE2\x80\xA2 120 BPM", true) != nullptr, "card info line");
    CHECK(findLabel("\xE2\x97\x8F niezapisane zmiany") != nullptr, "dirty marker");
    CHECK(findLabel("NEON_DRAFT") && findLabel("LIVE_SESSION_02") && findLabel("EMPTY_PROJECT"), "saved projects listed");

    // Dirty: first tap warns, second within 3 s opens.
    tapLabel("NEON_DRAFT");
    settle(200);
    CHECK(modalIsOpen() && strcmp(app().project().name, "ORBIT_01") == 0, "first tap only warns");
    tapLabel("NEON_DRAFT");
    CHECK(waitIdle(), "project load finished");
    settle(200);
    CHECK(!modalIsOpen(), "second tap opens and closes the dialog");
    CHECK(strcmp(app().project().name, "NEON_DRAFT") == 0, "opened %s", app().project().name);

    // SAVE (clean afterwards), NEW PROJECT.
    app().toggleStep(2, 5);
    dlg_project::open();
    settle(400);
    CHECK(findLabel("ORBIT_01") != nullptr, "ORBIT_01 now in the list");
    tapLabel("SAVE");
    CHECK(waitIdle(), "save finished");
    settle(600);
    CHECK(!app().dirty(), "SAVE clears dirty");
    CHECK(findLabel("\xE2\x97\x8F niezapisane zmiany") == nullptr, "dirty marker hidden after save");
    tapLabel("EXPORT JSON");
    CHECK(waitIdle(), "export finished");
    CHECK(fs::exists(g_sd + "/ORBIT/PROJECTS/NEON_DRAFT.json"), "EXPORT JSON wrote NEON_DRAFT.json");
    app().toggleStep(2, 7);   // dirty again: NEW PROJECT needs a second tap
    tapLabel("NEW PROJECT");
    settle(200);
    CHECK(modalIsOpen() && strcmp(app().project().name, "NEON_DRAFT") == 0, "dirty: first NEW PROJECT tap only warns");
    tapLabel("NEW PROJECT");
    settle(300);
    CHECK(!modalIsOpen(), "second NEW PROJECT tap creates the project and closes the dialog");
    CHECK(strncmp(app().project().name, "ORBIT_", 6) == 0 && strcmp(app().project().name, "ORBIT_01") != 0,
          "new project %s", app().project().name);
    settle(3000);
}

static void helpTests()
{
    printf("help\n");
    dlg_help::open();
    settle(500);
    printModalBox();
    shot("dlg_help");
    CHECK(findLabel("1. Pads & velocity") && findLabel("8. Project, SD & settings"), "guide cards");
    {
        // all eight cards fit the window (nothing hidden below a scroll)
        lv_area_t w = {};
        CHECK(modalBox(&w), "guide window");
        lv_obj_t* top = lv_layer_top();
        lv_obj_t* body = nullptr;
        for (int32_t i = (int32_t)lv_obj_get_child_count(top) - 1; i >= 0 && !body; --i) {
            lv_obj_t* scrim = lv_obj_get_child(top, i);
            if (lv_obj_get_child_count(scrim) == 0 || lv_obj_get_width(scrim) < 1280) continue;
            body = lv_obj_get_child(lv_obj_get_child(scrim, 0), 1);
        }
        CHECK(body && lv_obj_get_scroll_bottom(body) <= 0, "guide fits without scrolling (%d px below)",
              body ? (int)lv_obj_get_scroll_bottom(body) : -1);
    }
    modalClose();
    settle(300);
}

// ------------------------------------------------------------------ no SD card (child process)
static int runNoSd()
{
    printf("no SD card\n");
    app().setView(View::Browser);
    settle(300);
    CHECK(!app().sdReady(), "no card");
    CHECK(findLabel("Brak karty SD \xE2\x80\x94 dostępne są wbudowane zestawy") != nullptr, "no-card notice");
    {
        // no search strip without a card: the list (notice first) starts at the top of the panel
        int nx = 0, ny = 0;
        lv_obj_t* n = findLabel("Brak karty SD \xE2\x80\x94 dostępne są wbudowane zestawy");
        if (n) center(n, nx, ny);
        CHECK(n && ny < 87 + 3 + 43, "notice row at the top of the list (y %d)", ny);
    }
    CHECK(findLabel("808 STYLE \xC2\xB7 wbudowany") && findLabel("909 STYLE \xC2\xB7 wbudowany"), "built-ins listed");
    CHECK(findLabel("Wybierz zestaw") != nullptr, "preview hint");
    tapLabel("909 STYLE \xC2\xB7 wbudowany");   // second built-in below the notice strip
    settle(300);
    CHECK(findLabel("909 STYLE") != nullptr && findLabel("LOAD KIT") != nullptr, "909 built-in selected");
    shot("browser_nosd");
    tapLabel("LOAD KIT");   // over the pads of the start-up kit: confirm
    settle(100);
    tapLabel("LOAD KIT");
    settle(100);
    CHECK(strcmp(app().project().pad[0].name, builtinKit(1).sounds[0].name) == 0, "909 built-in loaded (%s)",
          app().project().pad[0].name);
    dlg_project::open();
    settle(400);
    CHECK(findLabel("Brak karty SD. Projekty", true) != nullptr, "project manager explains the missing card");
    shot("dlg_project_nosd");
    modalClose();
    settle(300);
    dlg_settings::open();
    settle(300);
    tapLabel("Storage");
    settle(300);
    CHECK(findLabel("Brak karty SD") != nullptr, "storage tab: no card");
    modalClose();
    settle(300);
    printf("shot_browser_dlg --nosd: %d checks, %d failed -> %s\n", g_checks, g_fails, g_fails ? "FAILED" : "passed");
    return g_fails ? 1 : 0;
}

// The repository card as it is (kits only, empty SAMPLES folders); read-only: nothing is saved.
static int runRepoSd()
{
    printf("repository sdcard\n");
    for (int i = 0; i < 400 && (app().library().count() < 2 || app().loadingCount() > 0); ++i) {
        lvhost_run(16);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    waitIdle();
    app().setCategory(Category::All);
    settle(300);
    CHECK(app().resultCount() == 0, "repo card: no files in SAMPLES/USER (%d)", app().resultCount());
    CHECK(findLabel("Brak plików WAV w kategorii All Samples") != nullptr, "empty category message");
    settle(2600);
    shot("browser_repo_all");
    app().setCategory(Category::Kits);
    settle(300);
    CHECK(findLabel("808_STYLE") && findLabel("909_STYLE") && findLabel("808 STYLE \xC2\xB7 wbudowany"), "repo kits + built-ins");
    lvhost_tap(500, rowY(0));
    settle(400);
    CHECK(findLabel("SELECTED KIT") != nullptr && findLabel("16 SOUNDS \xE2\x80\xA2 SD KIT") != nullptr, "repo kit preview");
    shot("browser_repo_kits");
    printf("shot_browser_dlg --reposd: %d checks, %d failed -> %s\n", g_checks, g_fails, g_fails ? "FAILED" : "passed");
    return g_fails ? 1 : 0;
}

int main(int argc, char** argv)
{
    const bool nosd = argc > 1 && strcmp(argv[1], "--nosd") == 0;
    const bool reposd = argc > 1 && strcmp(argv[1], "--reposd") == 0;
    std::error_code ec;
    fs::create_directories("host/shots", ec);
    fs::create_directories("host/out", ec);
    const std::string cwd = fs::current_path().generic_string();
    const std::string settings =
        cwd + (nosd ? "/host/out/shot_browser_settings_nosd.bin"
                    : (reposd ? "/host/out/shot_browser_settings_repo.bin" : "/host/out/shot_browser_settings.bin"));
    fs::remove(settings, ec);
    halHostSetSettingsPath(settings.c_str());
    g_sd = reposd ? cwd + "/sdcard" : cwd + "/host/out/shot_browser_sd";
    if (!nosd && !reposd && !prepareSd(g_sd)) {
        printf("cannot prepare %s\nshot_browser_dlg: FAILED\n", g_sd.c_str());
        return 1;
    }
    halHostSetSdRoot(nosd ? "" : g_sd.c_str());
    hal::sdMount();
    hal::displayStart();
    themeInit();
    if (!app().init(nosd ? nullptr : g_sd.c_str(), 48000)) {
        printf("App init failed\nshot_browser_dlg: FAILED\n");
        return 1;
    }
    lv_obj_t* work = buildScreen();
    view_browser::create(work);
    lv_timer_create(uiTimer, 16, nullptr);
    app().setView(View::Browser);
    if (nosd) return runNoSd();
    if (reposd) return runRepoSd();

    // Wait for the initial SD scan.
    for (int i = 0; i < 400 && app().library().count() < kTakes + 12; ++i) {
        lvhost_run(16);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    waitIdle();
    printf("library: %d entries\n", app().library().count());
    CHECK(app().library().count() >= kTakes + 12, "scan found %d entries", app().library().count());

    browserTests();
    settingsTests();
    bluetoothTests();
    projectTests();
    helpTests();

    // No-card run in a child process.
    char cmd[512];
    snprintf(cmd, sizeof cmd, "\"%s\" --nosd", argv[0]);
    for (char* p = cmd; *p; ++p)
        if (*p == '/') *p = '\\';
    fflush(stdout);
    const int rc = system(cmd);
    CHECK(rc == 0, "no-SD child run (exit %d)", rc);
    snprintf(cmd, sizeof cmd, "\"%s\" --reposd", argv[0]);
    for (char* p = cmd; *p; ++p)
        if (*p == '/') *p = '\\';
    fflush(stdout);
    const int rc2 = system(cmd);
    CHECK(rc2 == 0, "repository-card child run (exit %d)", rc2);

    printf("shot_browser_dlg: %d checks, %d failed -> %s\n", g_checks, g_fails, g_fails ? "FAILED" : "passed");
    return g_fails ? 1 : 0;
}
