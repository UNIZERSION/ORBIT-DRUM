// Build + run (Git Bash, project root). Links every core/*.cpp (+ cJSON from ESP-IDF, read-only, for persist.cpp); stubs_for_app.cpp only replaces ui::toast (no LVGL here). ORBIT_TEST_VERBOSE=1 prints the toasts:
// export ZIG_GLOBAL_CACHE_DIR=$PWD/host/.zig-cache/global ZIG_LOCAL_CACHE_DIR=$PWD/host/.zig-cache/local; J=${IDF_PATH:-$HOME/esp/v5.5.2/esp-idf}/components/json/cJSON; C=firmware/main/core; mkdir -p host/bin && zig cc -target x86_64-windows-gnu -O2 -c $J/cJSON.c -o host/bin/cJSON.o && zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -Wall -Wextra -Ifirmware/main -Ihost -I$J -DHAL_HOST_NO_LVGL host/tests/test_app.cpp host/tests/stubs_for_app.cpp firmware/main/ui/app.cpp $C/*.cpp host/hal_host.cpp host/platform_host.cpp host/bin/cJSON.o -x c++ $C/kits_808.c $C/kits_909.c -o host/bin/test_app.exe && host/bin/test_app.exe
//
// App controller (ui/app.cpp) + PC HAL (host/hal_host.cpp): init on the repo sdcard, pads,
// ParamRefs, pattern editing, random/copy/clear scenes, song, tap tempo math, library filter,
// loads, sample ops + undo (copy-on-write), slices to pads, settings, save -> open round trip,
// live record, and 2 s of rendered audio with the transport playing.
// Wave 2: renameProject, setLaunchQuant (scene switch timing), pattern undo, SAVE WAV, MIDI through
// halHostMidi (GM/909 note map, channel filter, Start/Continue/Stop, clock in with jitter and late
// ticks, clock out via the service timer, learn + CC + midimap.bin), exit to the (simulated) launcher.
// Bluetooth through hal_host's fake Pico (halHostBtFeed / halHostBtTakeSent / halHostBtAdvance): on / off with
// the engine rate switch 48 -> 44.1 -> 48 kHz, status + device list, connect / disconnect toasts, headphone
// buttons (REMOTE PLAY / PAUSE / NEXT / PREVIOUS), volume (deferred save), output, AirPods, persistence.
// Files written into sdcard/ORBIT (one synthetic WAV in USER, project ZZ_TEST_APP_RT, SAVE WAV files,
// midimap.bin) are removed at the end (and the synth/project at the start, after an interrupted run).
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <direct.h>
#endif

#include "core/kits.h"
#include "core/platform.h"
#include "core/sampleio.h"
#include "hal/hal.h"
#include "hal_host.h"
#include "ui/app.h"

using namespace orbit;
using namespace orbit::ui;

namespace testhooks {   // stubs_for_app.cpp (ui::toast capture)
const char* lastToast();
int toastCount();
}  // namespace testhooks

static int checks = 0, fails = 0;
#define CHECK(c)                                                             \
    do {                                                                     \
        ++checks;                                                            \
        if (!(c)) {                                                          \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);          \
            ++fails;                                                         \
        }                                                                    \
    } while (0)
#define NEAR(a, b, eps) CHECK(std::fabs((double)(a) - (double)(b)) <= (double)(eps))

static const char* kSynth = "__test_app_synth";
static const char* kProj = "ZZ_TEST_APP_RT";
static constexpr int kSynthFrames = 19200;   // 0.4 s at 48 kHz, 4 bursts

// ------------------------------------------------------------------ files
static bool isDir(const std::string& p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && (st.st_mode & S_IFDIR);
}
static bool isFile(const std::string& p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && !(st.st_mode & S_IFDIR);
}
static void mkdirOne(const std::string& p)
{
#if defined(_WIN32)
    _mkdir(p.c_str());
#else
    mkdir(p.c_str(), 0775);
#endif
}
static void removeTree(const std::string& dir)
{
    if (!isDir(dir)) return;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            const std::string p = dir + "/" + e->d_name;
            if (isDir(p)) removeTree(p);
            else remove(p.c_str());
        }
        closedir(d);
    }
#if defined(_WIN32)
    _rmdir(dir.c_str());
#else
    rmdir(dir.c_str());
#endif
}
static int countWavs(const std::string& dir)
{
    int n = 0;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const size_t L = strlen(e->d_name);
            if (L > 4 && (!strcmp(e->d_name + L - 4, ".wav") || !strcmp(e->d_name + L - 4, ".WAV"))) ++n;
        }
        closedir(d);
    }
    return n;
}

// Review-fix tests: an 8 s sample (a browser preview that is still playing), scratch projects.
static const char* kLong = "__test_app_long";
static constexpr int kLongFrames = 8 * 48000;
static const char* kPart = "ZZ_TEST_APP_PART";   // save with an edited sample that cannot be written
static const char* kOvr = "ZZ_TEST_APP_OVR";     // a .orb of "another project" (SAVE asks first)
static const char* kCopy = "ZZ_TEST_APP_COPY";   // kProj.orb copied on a PC under another name

static std::string g_sd;
static std::string synthPath() { return g_sd + "/ORBIT/USER/" + kSynth + ".wav"; }
static std::string longPath() { return g_sd + "/ORBIT/USER/" + kLong + ".wav"; }
static std::string projPath() { return g_sd + "/ORBIT/PROJECTS/" + kProj + ".orb"; }
static std::string projJson() { return g_sd + "/ORBIT/PROJECTS/" + kProj + ".json"; }
static std::string projDir() { return g_sd + "/ORBIT/PROJECTS/" + kProj; }
static std::string projFile(const char* name) { return g_sd + "/ORBIT/PROJECTS/" + name + ".orb"; }
static void cleanupFiles()
{
    remove(synthPath().c_str());
    remove(longPath().c_str());
    remove(projPath().c_str());
    remove(projJson().c_str());
    removeTree(projDir());
    for (const char* n : {kPart, kOvr, kCopy}) {
        const std::string d = g_sd + "/ORBIT/PROJECTS/" + n;
        remove(projFile(n).c_str());
        if (isDir(d)) removeTree(d);
        else remove(d.c_str());   // kPart: a plain file blocks the sample folder
    }
}

// Four decaying 440 Hz bursts of falling level (asymmetric, so REVERSE is visible).
static bool writeSynth()
{
    mkdirOne(g_sd + "/ORBIT");
    mkdirOne(g_sd + "/ORBIT/USER");
    Sample* s = sampleCreate(kSynthFrames, 1, 48000);
    if (!s) return false;
    const float amp[4] = {0.9f, 0.6f, 0.4f, 0.2f};
    for (int b = 0; b < 4; ++b)
        for (int i = 0; i < 1200; ++i) {
            const float env = amp[b] * expf(-(float)i / 300.0f);
            s->data[b * 4800 + i] = (int16_t)(32767.0f * env * sinf(6.2831853f * 440.0f * (float)i / 48000.0f));
        }
    sampleAnalyze(s);
    const bool ok = wavSave(synthPath().c_str(), s) == WavError::Ok;
    sampleFree(s);
    return ok;
}

// 8 s of a quiet 220 Hz tone (a preview that keeps playing while the test works).
static bool writeLongSynth()
{
    Sample* s = sampleCreate(kLongFrames, 1, 48000);
    if (!s) return false;
    for (int i = 0; i < kLongFrames; ++i) s->data[i] = (int16_t)(6000.0f * sinf(6.2831853f * 220.0f * (float)i / 48000.0f));
    sampleAnalyze(s);
    const bool ok = wavSave(longPath().c_str(), s) == WavError::Ok;
    sampleFree(s);
    return ok;
}

static bool writeText(const std::string& path, const char* text)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fputs(text, f);
    return fclose(f) == 0;
}

static bool copyFile(const std::string& from, const std::string& to)
{
    FILE* a = fopen(from.c_str(), "rb");
    if (!a) return false;
    FILE* b = fopen(to.c_str(), "wb");
    if (!b) {
        fclose(a);
        return false;
    }
    char buf[4096];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) ok = ok && fwrite(buf, 1, n, b) == n;
    fclose(a);
    return fclose(b) == 0 && ok;
}

// ------------------------------------------------------------------ pumping
static int16_t g_buf[2 * 256];
static void pump(int blocks = 1)
{
    for (int i = 0; i < blocks; ++i) {
        halHostRenderAudio(128, g_buf);
        app().tick();
    }
}
static bool waitLoader(int timeout_ms = 8000)
{
    const uint64_t t0 = nowUs();
    while (nowUs() - t0 < (uint64_t)timeout_ms * 1000u) {
        pump();
        if (app().loadingCount() == 0) {
            pump(3);   // engine applies the SetSample commands posted by the result
            return true;
        }
        sleepMs(1);
    }
    std::printf("timeout waiting for the loader (%d jobs)\n", app().loadingCount());
    return false;
}
// Renders `frames` (ticking the App every block); returns peak and RMS of both channels.
static void renderMeasure(int frames, float& peak, float& rms)
{
    double sum = 0.0;
    int pk = 0;
    for (int done = 0; done < frames; done += 128) {
        halHostRenderAudio(128, g_buf);
        for (int i = 0; i < 256; ++i) {
            const int v = g_buf[i] < 0 ? -g_buf[i] : g_buf[i];
            if (v > pk) pk = v;
            sum += (double)g_buf[i] * (double)g_buf[i];
        }
        app().tick();
    }
    peak = (float)pk / 32768.0f;
    rms = (float)std::sqrt(sum / (double)(frames * 2)) / 32768.0f;
}

static bool containsCi(const char* hay, const char* needle)
{
    std::string h(hay), n(needle);
    for (auto& c : h) c = (char)tolower((unsigned char)c);
    for (auto& c : n) c = (char)tolower((unsigned char)c);
    return h.find(n) != std::string::npos;
}

static int trackHits(const Scene& sc, int t, int len)
{
    int n = 0;
    for (int i = 0; i < len; ++i) n += sc.track[t].step[i].on ? 1 : 0;
    return n;
}

// ================================================================== tests
static void testInitAndKits(App& a)
{
    CHECK(a.sdReady());
    CHECK(a.sdStatus() && a.sdStatus()[0]);
    CHECK(a.project().bpm >= 30.0f && a.project().bpm <= 300.0f);
    CHECK(!a.dirty());
    if (builtinKitCount() >= 2) {
        const BuiltinKit& k = builtinKit(1);
        CHECK(strstr(k.name, "909") != nullptr);
        for (int i = 0; i < kPads && i < k.count; ++i) {
            const Sample* s = a.padSample(i);
            CHECK(s && s->embedded && s->frames == k.sounds[i].frames);
            CHECK(strcmp(a.project().pad[i].name, k.sounds[i].name) == 0);
            CHECK(a.project().pad[i].choke == k.sounds[i].choke);
            CHECK(a.project().pad[i].bus == k.sounds[i].bus);
        }
        CHECK(strcmp(a.padSampleName(0), k.sounds[0].name) == 0);
    } else {
        std::printf("note: no built-in kits linked, kit checks skipped\n");
    }
    CHECK(a.padSample(-1) == nullptr && a.padSample(16) == nullptr);
}

static void testPads(App& a)
{
    a.padFlashBits();
    a.padDown(0, 127, Producer::Touch);
    std::thread t([] { app().padDown(1, 90, Producer::Midi); });   // [any task]
    t.join();
    pump(4);
    const uint32_t bits = a.padFlashBits();
    CHECK(bits & 1u);
    CHECK(bits & 2u);
    a.padUp(0, Producer::Touch);
    a.padUp(1, Producer::Midi);
    a.padDown(-1, 100, Producer::Touch);   // ignored
    a.padDown(16, 100, Producer::Touch);

    const uint32_t r0 = a.revs().selection;
    a.selectPad(5, false);
    CHECK(a.ui().pad == 5);
    CHECK(a.revs().selection != r0);
    CHECK(a.engine().telemetry().watch_pad.load() == 5);
    a.selectLayer(9);
    CHECK(a.ui().layer == kLayers - 1);
    a.selectLayer(0);

    a.setPadMode(PadMode::Mute);
    a.padFlashBits();
    a.padDown(7, 100, Producer::Touch);   // MUTE mode: a touch toggles, it does not play
    pump(3);
    CHECK((a.padFlashBits() & (1u << 7)) == 0);
    a.padTap(3);
    CHECK(a.project().pad[3].mute == 1);
    a.padTap(3);
    CHECK(a.project().pad[3].mute == 0);
    a.setPadMode(PadMode::Solo);
    a.padTap(4);
    CHECK(a.project().pad[4].solo == 1);
    a.toggleSolo(4);
    CHECK(a.project().pad[4].solo == 0);
    a.setPadMode(PadMode::Play);
    a.padTap(6);
    CHECK(a.ui().pad == 6);
    pump(2);
    CHECK(a.padFlashBits() & (1u << 6));
    a.selectPad(5, false);

    a.setPlayMode(PlayMode::Gate);
    CHECK(a.project().pad[5].mode == PlayMode::Gate);
    a.setPlayMode(PlayMode::OneShot);
    a.setChoke(12);
    CHECK(a.project().pad[5].choke == 8);
    a.setChoke(0);
    a.setBus(99);
    CHECK(a.project().pad[5].bus == kBuses - 1);
    a.setBus(4);
    a.setPoly(0);
    CHECK(a.project().pad[5].poly == 1);
    a.setPoly(4);
    a.setLayerMode(LayerMode::RoundRobin);
    CHECK(a.project().pad[5].layer_mode == LayerMode::RoundRobin);
    a.setLayerMode(LayerMode::Single);
    a.setReverse(true);
    CHECK(a.project().pad[5].reverse == 1);
    a.setReverse(false);
    a.renamePad(5, "MY RIM");
    CHECK(strcmp(a.project().pad[5].name, "MY RIM") == 0);
    CHECK(a.dirty());   // App member edits mark the project dirty
}

static void testParams(App& a)
{
    char txt[32];
    a.selectPad(5, false);
    const ParamRef cut = ParamRef::pad(PP_CUTOFF);
    const uint32_t rp = a.revs().params;
    a.paramSet(cut, 1000.0f);
    NEAR(a.project().pad[5].p[PP_CUTOFF], 1000.0f, 0.01f);
    NEAR(a.paramGet(cut), 1000.0f, 0.01f);
    CHECK(a.revs().params != rp);
    a.paramSetNorm(cut, 0.0f);
    NEAR(a.project().pad[5].p[PP_CUTOFF], padSpec(PP_CUTOFF).min, 0.01f);
    a.paramSetNorm(cut, 1.0f);
    NEAR(a.project().pad[5].p[PP_CUTOFF], padSpec(PP_CUTOFF).max, 0.5f);
    NEAR(a.paramNorm(cut), 1.0f, 1e-4f);
    a.paramReset(cut);
    NEAR(a.project().pad[5].p[PP_CUTOFF], padSpec(PP_CUTOFF).def, 0.01f);
    CHECK(a.paramText(cut, txt, sizeof txt) > 0 && strstr(txt, "Hz"));
    CHECK(a.paramSpec(cut) == &padSpec(PP_CUTOFF));
    CHECK(strcmp(a.paramCaption(cut), padSpec(PP_CUTOFF).label) == 0);

    // explicit pad index + clamping
    const ParamRef tune2 = ParamRef::pad(PP_TUNE, 2);
    a.paramSet(tune2, 7.0f);
    NEAR(a.project().pad[2].p[PP_TUNE], 7.0f, 1e-5f);
    a.paramSet(tune2, 100.0f);
    NEAR(a.project().pad[2].p[PP_TUNE], 24.0f, 1e-5f);
    a.paramReset(tune2);
    NEAR(a.project().pad[2].p[PP_TUNE], 0.0f, 1e-5f);
    NEAR(a.project().pad[5].p[PP_TUNE], 0.0f, 1e-5f);

    // the play region stays valid
    a.paramSet(ParamRef::pad(PP_END), 0.3f);
    a.paramSet(ParamRef::pad(PP_START), 0.5f);
    CHECK(a.project().pad[5].p[PP_START] < a.project().pad[5].p[PP_END]);
    a.paramReset(ParamRef::pad(PP_START));
    a.paramReset(ParamRef::pad(PP_END));
    NEAR(a.project().pad[5].p[PP_START], 0.0f, 1e-6f);
    NEAR(a.project().pad[5].p[PP_END], 1.0f, 1e-6f);

    // global
    const ParamRef rv = ParamRef::global(GP_REV_SIZE);
    const uint32_t rf = a.revs().fx;
    a.paramSet(rv, 0.4f);
    NEAR(a.project().g[GP_REV_SIZE], 0.4f, 1e-5f);
    CHECK(a.revs().fx != rf);
    CHECK(strcmp(a.paramCaption(rv), globalSpec(GP_REV_SIZE).label) == 0);

    // bus faders (busGain law: 0.7071 = 0 dB)
    const ParamRef b2 = ParamRef::busLevel(2);
    const uint32_t rm = a.revs().mixer;
    a.paramSet(b2, 0.5f);
    NEAR(a.project().bus[2].level, 0.5f, 1e-6f);
    CHECK(a.revs().mixer != rm);
    CHECK(a.paramText(b2, txt, sizeof txt) > 0 && strstr(txt, "-6.0 dB"));
    a.paramSetNorm(b2, 0.0f);
    a.paramText(b2, txt, sizeof txt);
    CHECK(strstr(txt, "inf") != nullptr);
    a.paramReset(b2);
    NEAR(a.project().bus[2].level, 0.7071f, 1e-3f);
    CHECK(a.paramSpec(b2) == nullptr);
    CHECK(strcmp(a.paramCaption(b2), a.project().bus[2].name) == 0);

    // master = BusLevel(kBuses) -> GP_MASTER in dB with the same fader law
    const ParamRef m = ParamRef::busLevel(kBuses);
    a.paramSetNorm(m, 0.70710678f);
    NEAR(a.project().g[GP_MASTER], 0.0f, 0.01f);
    NEAR(a.paramNorm(m), 0.7071f, 1e-3f);
    a.paramSetNorm(m, 0.5f);
    NEAR(a.project().g[GP_MASTER], -6.02f, 0.02f);
    a.paramSetNorm(m, 1.0f);
    NEAR(a.project().g[GP_MASTER], 6.0f, 0.01f);
    a.paramSetNorm(m, 0.0f);
    NEAR(a.project().g[GP_MASTER], globalSpec(GP_MASTER).min, 0.01f);
    CHECK(strcmp(a.paramCaption(m), "MASTER") == 0);
    a.paramReset(m);
    NEAR(a.project().g[GP_MASTER], globalSpec(GP_MASTER).def, 0.01f);
    CHECK(a.paramText(m, txt, sizeof txt) > 0 && strstr(txt, "dB"));

    const ParamRef p1 = ParamRef::busPan(1);
    a.paramSetNorm(p1, 0.75f);
    NEAR(a.project().bus[1].pan, 0.5f, 1e-5f);
    CHECK(a.paramText(p1, txt, sizeof txt) > 0);
    a.paramReset(p1);
    NEAR(a.project().bus[1].pan, 0.0f, 1e-6f);

    // layers (selected layer of the selected pad)
    a.selectLayer(1);
    a.paramSet(ParamRef::layer(ParamRef::Kind::LayerTune), 3.0f);
    NEAR(a.project().pad[5].layer[1].tune, 3.0f, 1e-5f);
    a.paramSet(ParamRef::layer(ParamRef::Kind::LayerGain, 0), -6.0f);
    NEAR(a.project().pad[5].layer[0].gain_db, -6.0f, 1e-5f);
    a.paramSet(ParamRef::layer(ParamRef::Kind::LayerPan), 0.25f);
    NEAR(a.project().pad[5].layer[1].pan, 0.25f, 1e-5f);
    a.paramReset(ParamRef::layer(ParamRef::Kind::LayerTune));
    NEAR(a.project().pad[5].layer[1].tune, 0.0f, 1e-6f);
    a.selectLayer(0);

    // LFO rate shows the sync division when SYNC is on
    a.paramSet(ParamRef::pad(PP_LFO_SYNC), 1.0f);
    const ParamRef rate = ParamRef::pad(PP_LFO_RATE);
    a.paramSetNorm(rate, 5.0f / 11.0f);
    a.paramText(rate, txt, sizeof txt);
    CHECK(strcmp(txt, syncDivName(SyncDiv::D1_4)) == 0);
    a.paramSet(ParamRef::pad(PP_LFO_SYNC), 0.0f);
    a.paramText(rate, txt, sizeof txt);
    CHECK(strstr(txt, "Hz") != nullptr);
}

static void testTransportAndTap(App& a)
{
    a.setBpm(500.0f);
    NEAR(a.project().bpm, 300.0f, 1e-4f);
    a.setBpm(10.0f);
    NEAR(a.project().bpm, 30.0f, 1e-4f);
    a.setBpm(120.0f);
    a.bpmStep(1);
    NEAR(a.project().bpm, 121.0f, 1e-4f);
    a.bpmStep(-2);
    NEAR(a.project().bpm, 119.0f, 1e-4f);

    uint64_t t = 1000000000ull;
    const int tap_toasts = testhooks::toastCount();
    for (int k = 0; k < 5; ++k) a.tapAt(t + (uint64_t)k * 500000u);
    NEAR(a.project().bpm, 120.0f, 0.05f);
    CHECK(testhooks::toastCount() == tap_toasts);   // TAP shows the tempo live (top bar, macros): no toast
    t += 10000000ull;   // > 2 s: starts again; 6 taps at 400 ms
    for (int k = 0; k < 6; ++k) a.tapAt(t + (uint64_t)k * 400000u);
    NEAR(a.project().bpm, 150.0f, 0.05f);
    t += 10000000ull;   // last 4 intervals 500, 400, 400, 500 ms -> 450 ms
    const uint64_t at[6] = {0, 500000, 1000000, 1400000, 1800000, 2300000};
    for (uint64_t x : at) a.tapAt(t + x);
    NEAR(a.project().bpm, 133.33f, 0.06f);
    t += 10000000ull;
    a.tapAt(t);   // a single tap after the reset changes nothing
    NEAR(a.project().bpm, 133.33f, 0.06f);
    a.tapAt(t + 100000u);   // 600 BPM -> clamp
    NEAR(a.project().bpm, 300.0f, 1e-3f);
    t += 10000000ull;
    a.tapAt(t);
    a.tapAt(t + 1900000u);  // 31.6 BPM (1.9 s is still inside the 2 s window)
    NEAR(a.project().bpm, 31.6f, 0.06f);
    a.setBpm(120.0f);

    a.setMetronome(true);
    CHECK(a.project().metronome == 1);
    a.setMetronome(false);
    a.setFill(true);
    CHECK(a.ui().fill == 1);
    a.setFill(false);
    CHECK(a.ui().fill == 0);
}

static void testPattern(App& a)
{
    a.selectScene(1);
    CHECK(a.project().scene == 1);
    a.clearScene();
    Scene& s1 = a.project().scene_data[1];
    for (int t = 0; t < kPads; ++t) CHECK(trackHits(s1, t, kMaxSteps) == 0);

    const uint32_t rp = a.revs().pattern;
    a.toggleStep(0, 3);
    CHECK(s1.track[0].step[3].on == 1 && a.step(0, 3).on == 1);
    CHECK(a.revs().pattern != rp);
    a.toggleStep(0, 3);
    CHECK(s1.track[0].step[3].on == 0);
    a.setStepOn(2, 7, true);
    a.setStepOn(2, 7, true);
    CHECK(s1.track[2].step[7].on == 1);
    a.toggleStep(-1, 0);
    a.toggleStep(0, 64);   // out of range: ignored
    CHECK(a.step(0, 64).on == 0);

    a.setStepLane(2, 7, StepLane::Velocity, 200);
    CHECK(s1.track[2].step[7].vel == 127);
    a.setStepLane(2, 7, StepLane::Velocity, 0);
    CHECK(s1.track[2].step[7].vel == 1);
    a.setStepLane(2, 7, StepLane::Velocity, 77);
    CHECK(a.stepLaneValue(2, 7, StepLane::Velocity) == 77);
    a.setStepLane(2, 7, StepLane::Probability, -5);
    CHECK(s1.track[2].step[7].prob == 0);
    a.setStepLane(2, 7, StepLane::Probability, 60);
    CHECK(a.stepLaneValue(2, 7, StepLane::Probability) == 60);
    a.setStepLane(2, 7, StepLane::Ratchet, 9);
    CHECK(s1.track[2].step[7].ratchet == 4);
    a.setStepLane(2, 7, StepLane::Nudge, -80);
    CHECK(s1.track[2].step[7].nudge == -50);
    a.setStepLane(2, 7, StepLane::Pitch, 30);
    CHECK(s1.track[2].step[7].pitch == 24);
    a.setStepLane(2, 7, StepLane::Cond, 99);
    CHECK((int)s1.track[2].step[7].cond == (int)TrigCond::Count - 1);
    a.setStepLane(2, 7, StepLane::Cond, (int)TrigCond::Fill);
    CHECK(a.stepLaneValue(2, 7, StepLane::Cond) == (int)TrigCond::Fill);
    a.clearTrack(2);
    CHECK(trackHits(s1, 2, kMaxSteps) == 0 && s1.track[2].step[7].vel == 100);

    a.setSceneLength(70);
    CHECK(s1.length == 64);
    a.setSceneLength(0);
    CHECK(s1.length == 1);
    a.setSceneLength(16);
    a.setDivision(Division::D1_8T);
    CHECK(s1.div == Division::D1_8T);
    a.setDivision(Division::D1_16);
    a.setSwing(90);
    CHECK(s1.swing == 75);
    a.setSwing(10);
    CHECK(s1.swing == 50);
    a.setSwing(54);
    a.setTrackLength(3, 12);
    CHECK(s1.track[3].length == 12);
    a.setTrackLength(3, 99);
    CHECK(s1.track[3].length == 64);
    a.setTrackLength(3, 0);

    // RANDOM: musically weighted per pad role (909 layout: 0 kick, 1 snare, 2 closed hat, 6..8 toms)
    const int runs = 40;
    int kick0 = 0, kick = 0, snareBack = 0, snareOff = 0, snare = 0, hat = 0, tom = 0;
    for (int r = 0; r < runs; ++r) {
        a.randomScene();
        const int len = s1.length;
        kick0 += s1.track[0].step[0].on;
        kick += trackHits(s1, 0, len);
        hat += trackHits(s1, 2, len);
        tom += trackHits(s1, 6, len) + trackHits(s1, 7, len) + trackHits(s1, 8, len);
        snare += trackHits(s1, 1, len);
        snareBack += s1.track[1].step[4].on + s1.track[1].step[8].on + s1.track[1].step[12].on;
        snareOff += s1.track[1].step[0].on + s1.track[1].step[2].on + s1.track[1].step[6].on + s1.track[1].step[10].on;
        for (int t = 0; t < kPads; ++t)
            for (int i = len; i < kMaxSteps; ++i) CHECK(!s1.track[t].step[i].on);
    }
    std::printf("random x%d: kick %.1f/bar, snare %.1f (backbeat %d, offbeat %d), hat %.1f, toms %.2f\n", runs,
                (double)kick / runs, (double)snare / runs, snareBack, snareOff, (double)hat / runs, (double)tom / runs);
    CHECK(kick0 == runs);                       // kick anchors the downbeat
    CHECK(kick > 2 * runs && kick < 7 * runs);  // a few kicks per bar
    CHECK(snareBack >= runs);                   // backbeat present
    CHECK(snareOff == 0);                       // never on the even non-backbeat steps
    CHECK(hat > 6 * runs);                      // hats are the densest
    CHECK(hat > kick && hat > snare);
    CHECK(tom < 3 * runs);                      // toms stay sparse (3 pads together)

    // copy / clear
    a.randomScene();
    a.copyScene(1, 2);
    CHECK(memcmp(&a.project().scene_data[1], &a.project().scene_data[2], sizeof(Scene)) == 0);
    CHECK(strstr(testhooks::lastToast(), "B") && strstr(testhooks::lastToast(), "C"));
    a.copyScene(1, 1);   // no-op
    a.copyScene(-1, 2);
    a.clearScene();
    for (int t = 0; t < kPads; ++t) CHECK(trackHits(s1, t, kMaxSteps) == 0);
    CHECK(trackHits(a.project().scene_data[2], 0, kMaxSteps) > 0);   // the copy is independent

    // song arranger + mixer + macros
    a.setSongSlot(0, 2);
    CHECK(a.project().song.slot[0] == 2);
    a.setSongSlot(0, 2);   // tapping the lit cell clears it
    CHECK(a.project().song.slot[0] == -1);
    a.setSongSlot(1, 3);
    a.setSongSlot(1, -1);
    CHECK(a.project().song.slot[1] == -1);
    a.setSongSlot(16, 1);  // ignored
    a.toggleSongFx(3);
    CHECK(a.project().song.fx[3] == 1);
    a.toggleSongFx(3);
    CHECK(a.project().song.fx[3] == 0);
    a.setFollow(true);
    CHECK(a.project().song.follow == 1);
    a.setFollow(false);
    {
        // FOLLOW off hands the scene back to the editor: the engine plays the edited scene again (queued
        // at once while stopped), not the song slot's scene (logic-4 cross-area).
        const int slot0 = a.project().song.slot[0];
        a.setSongSlot(0, 3);
        a.setFollow(true);
        a.stop();
        pump(2);
        a.stop();   // second STOP: rewind to the song's slot 0 (scene D)
        pump(2);
        a.selectScene(1);   // FOLLOW + song: selects B for editing only
        pump(2);
        const int playing_before = a.playScene();
        a.setFollow(false);
        pump(2);
        std::printf("  FOLLOW off: playing scene %d -> %d\n", playing_before, a.playScene());
        CHECK(a.playScene() == 1);
        a.setSongSlot(0, slot0);
        a.selectScene(0);
        pump(2);
        CHECK(a.playScene() == 0);
    }

    a.toggleBusMute(3);
    CHECK(a.project().bus[3].mute == 1);
    a.toggleBusMute(3);
    a.toggleBusSolo(3);
    CHECK(a.project().bus[3].solo == 1);
    a.toggleBusSolo(3);
    const bool w = a.macro(1);
    a.toggleMacro(1);
    CHECK(a.macro(1) != w && a.project().wide_on == (w ? 0 : 1));
    a.toggleMacro(1);
    CHECK(!a.macro(7));
    a.setLimiter(false);
    CHECK(a.project().limiter_on == 0);
    a.setLimiter(true);
    a.setPumpSource(20);
    CHECK(a.project().pump_src == 15);
    a.setPumpSource(0);

    a.selectScene(0);
    CHECK(a.project().scene == 0);
}

// Library filter, preview, load to pad, ops + undo, duplicate, slices.
static void testSamples(App& a, bool have_synth)
{
    a.setSearch("");
    a.setCategory(Category::Kits);
    const int kits = a.resultCount();
    int kit_index = -1;
    for (int i = 0; i < kits; ++i)
        if (a.library().at(a.result(i)).is_kit && kit_index < 0) kit_index = a.result(i);
    const bool kit_dirs = isDir(g_sd + "/ORBIT/KITS/909_STYLE") || isDir(g_sd + "/ORBIT/KITS/808_STYLE");
    std::printf("library: %d entries, %d in Kits\n", a.library().count(), kits);
    if (kit_dirs) CHECK(kit_index >= 0);

    a.setCategory(Category::User);
    a.setSearch("ZZQX_no_such_file");
    CHECK(a.resultCount() == 0);
    a.setSearch(kSynth);
    CHECK(a.resultCount() == (have_synth ? 1 : 0));
    const int idx = a.resultCount() > 0 ? a.result(0) : -1;
    CHECK(a.result(-1) == -1 && a.result(a.resultCount()) == -1);
    a.setCategory(Category::All);
    a.setSearch("kick");
    for (int i = 0; i < a.resultCount(); ++i) CHECK(containsCi(a.library().at(a.result(i)).name, "kick"));
    a.setSearch("");
    if (idx < 0) {
        std::printf("note: synthetic WAV not in the library, sample checks skipped\n");
        return;
    }
    CHECK(containsCi(a.library().at(idx).name, kSynth));

    // browser preview: probe + decoded sample for the waveform
    a.selectEntry(idx);
    CHECK(a.ui().lib_sel == idx);
    waitLoader();
    CHECK(a.previewSample() && a.previewSample()->frames == (uint32_t)kSynthFrames);
    a.previewEntry(idx);
    pump(8);
    a.stopPreview();
    pump(4);

    // load to D1 (uses the decoded preview), then D2 layer 2 through the loader
    a.loadFileToPad(idx, 12, 0);
    waitLoader();
    CHECK(a.padSample(12, 0) && a.padSample(12, 0)->frames == (uint32_t)kSynthFrames);
    CHECK(strcmp(a.padSampleName(12), kSynth) == 0);
    CHECK(strcmp(a.project().pad[12].name, kSynth) == 0);
    CHECK(strstr(testhooks::lastToast(), "\xE2\x86\x92 D1") != nullptr);   // "__test_app_synth.wav → D1"
    if (kit_index >= 0) {
        a.selectEntry(kit_index);   // a kit: no waveform preview
        pump(2);
        CHECK(a.previewSample() == nullptr);
    }
    char before[kNameLen];
    snprintf(before, sizeof before, "%s", a.project().pad[13].name);
    CHECK(a.loadFileToPad(idx, 13, 1));
    CHECK(a.loadingCount() >= 0);
    waitLoader();
    CHECK(a.padSample(13, 1) && a.padSample(13, 1)->frames == (uint32_t)kSynthFrames);
    CHECK(strcmp(a.project().pad[13].name, before) == 0);   // layer 2 keeps the pad name

    // ---- ops on D1 layer 1 (own slot)
    a.selectPad(12, false);
    a.selectLayer(0);
    const Sample* s0 = a.padSample(12, 0);
    const uint32_t id0 = s0->id;
    std::vector<int16_t> orig(s0->data, s0->data + s0->frames);
    CHECK(!a.canUndo());
    CHECK(a.applyOp(SampleOp::Reverse));
    waitLoader();
    const Sample* s1 = a.padSample(12, 0);
    CHECK(s1 && s1->id != id0 && s1->frames == (uint32_t)kSynthFrames);
    if (s1 && s1->frames == (uint32_t)kSynthFrames) {
        bool rev = true;
        for (int i = 0; i < kSynthFrames; i += 97) rev = rev && s1->data[i] == orig[(size_t)(kSynthFrames - 1 - i)];
        CHECK(rev);
    }
    CHECK(a.canUndo());
    CHECK(a.applyOp(SampleOp::Undo));
    pump(3);
    CHECK(a.padSample(12, 0) && a.padSample(12, 0)->id == id0);
    CHECK(!a.canUndo());
    CHECK(!a.applyOp(SampleOp::Undo));   // nothing left to undo

    // TRIM crops to START..END and resets the region; undo brings both back
    a.paramSet(ParamRef::pad(PP_START), 0.25f);
    a.paramSet(ParamRef::pad(PP_END), 0.75f);
    CHECK(a.applyOp(SampleOp::Trim));
    waitLoader();
    const Sample* st = a.padSample(12, 0);
    CHECK(st && std::abs((int)st->frames - kSynthFrames / 2) <= 2);
    NEAR(a.project().pad[12].p[PP_START], 0.0f, 1e-6f);
    NEAR(a.project().pad[12].p[PP_END], 1.0f, 1e-6f);
    CHECK(a.applyOp(SampleOp::Undo));
    pump(3);
    CHECK(a.padSample(12, 0)->frames == (uint32_t)kSynthFrames);
    NEAR(a.project().pad[12].p[PP_START], 0.25f, 1e-6f);
    NEAR(a.project().pad[12].p[PP_END], 0.75f, 1e-6f);
    a.paramReset(ParamRef::pad(PP_START));
    a.paramReset(ParamRef::pad(PP_END));

    CHECK(!a.applyOp(SampleOp::ToMono));   // already mono
    CHECK(a.applyOp(SampleOp::Normalize));
    waitLoader();
    CHECK(a.padSample(12, 0) && std::fabs(a.padSample(12, 0)->peak - 0.966f) < 0.02f);
    a.selectLayer(3);
    CHECK(!a.applyOp(SampleOp::Reverse));   // empty layer
    a.selectLayer(0);

    // ---- DUPLICATE to the next empty pad (D4 after emptying it), sharing the slot
    a.clearPadLayer(15, 0);
    for (int l = 0; l < kLayers; ++l) CHECK(a.padSample(15, l) == nullptr);
    CHECK(a.applyOp(SampleOp::Duplicate));
    CHECK(a.padSample(15, 0) == a.padSample(12, 0));
    CHECK(strcmp(a.project().pad[15].name, a.project().pad[12].name) == 0);
    CHECK(memcmp(a.project().pad[15].p, a.project().pad[12].p, sizeof a.project().pad[12].p) == 0);
    // an op on a shared slot is copy-on-write: D4 keeps the old content, undo shares again
    const Sample* shared = a.padSample(15, 0);
    const uint32_t shared_id = shared->id;
    CHECK(a.applyOp(SampleOp::Reverse));
    waitLoader();
    CHECK(a.padSample(15, 0) == shared && shared->id == shared_id);
    CHECK(a.padSample(12, 0) != shared);
    CHECK(a.applyOp(SampleOp::Undo));
    pump(3);
    CHECK(a.padSample(12, 0) == a.padSample(15, 0));

    // ---- slicing
    Pad& p12 = a.project().pad[12];
    a.sliceGrid(4);
    CHECK(p12.slice_count == 4);
    for (int i = 0; i < 4 && p12.slice_count == 4; ++i) NEAR(p12.slices[i], 0.25f * (float)i, 1e-4f);
    a.sliceSelect(2);
    NEAR(p12.p[PP_START], 0.5f, 1e-4f);
    NEAR(p12.p[PP_END], 0.75f, 1e-4f);
    a.sliceSelect(3);
    NEAR(p12.p[PP_END], 1.0f, 1e-4f);
    a.paramReset(ParamRef::pad(PP_START));
    a.paramReset(ParamRef::pad(PP_END));
    a.setSliceSens(0.5f);
    a.sliceAuto();   // async (loader)
    waitLoader();
    std::printf("auto slice: %d slices\n", p12.slice_count);
    CHECK(p12.slice_count >= 2 && p12.slice_count <= kMaxSlices);
    a.paramReset(ParamRef::pad(PP_START));
    a.paramReset(ParamRef::pad(PP_END));
    a.sliceGrid(4);
    a.slicesToPads();
    for (int i = 0; i < 4; ++i) {
        const Pad& d = a.project().pad[12 + i];
        CHECK(a.padSample(12 + i, 0) == a.padSample(12, 0));
        NEAR(d.p[PP_START], 0.25f * (float)i, 1e-4f);
        NEAR(d.p[PP_END], 0.25f * (float)(i + 1), 1e-4f);
        char want[kNameLen + 8];
        snprintf(want, sizeof want, " %d", i + 1);
        CHECK(strstr(d.name, want) != nullptr);
    }
    a.duplicatePadTo(9);
    CHECK(a.padSample(9, 0) == a.padSample(12, 0));
    a.loadBuiltinKit(1);   // restore A1..D4 for the audio part (D1..D4 slices are replaced)
    if (builtinKitCount() >= 2) CHECK(a.padSample(9, 0)->embedded);

    // ---- an SD kit folder to the pads
    if (kit_index >= 0) {
        // PREVIEW of a kit: the loader lists the folder and plays its first sound
        a.selectEntry(kit_index);
        const int toasts = testhooks::toastCount();
        a.previewKit(kit_index);
        waitLoader();
        CHECK(testhooks::toastCount() == toasts);   // no "Zestaw jest pusty" / read error
        a.stopPreview();
        pump(4);
        CHECK(a.loadKit(kit_index));
        waitLoader();
        CHECK(a.padSample(0) && !a.padSample(0)->embedded);
        CHECK(strlen(a.padSampleName(0)) > 0);
        CHECK(a.canUndoPads() && strcmp(a.undoPadsLabel(), "LOAD KIT") == 0);
        a.loadBuiltinKit(1);
    }
    if (builtinKitCount() >= 1) {
        a.loadBuiltinKit(0);
        CHECK(strcmp(a.padSampleName(10), builtinKit(0).sounds[10].name) == 0);
        a.loadBuiltinKit(1);
    }
}

static void testSaveOpen(App& a, bool have_synth)
{
    // An edited (normalized) synthetic sample on D1 is written as WAV into the project folder.
    int synth_idx = -1;
    if (have_synth) {
        a.setCategory(Category::User);
        a.setSearch(kSynth);
        synth_idx = a.resultCount() > 0 ? a.result(0) : -1;
        a.setSearch("");
        if (synth_idx >= 0) {
            a.loadFileToPad(synth_idx, 12, 0);
            waitLoader();
            a.selectPad(12, false);
            a.selectLayer(0);
            a.applyOp(SampleOp::Normalize);
            waitLoader();
        }
    }
    a.setBpm(133.0f);
    a.selectScene(1);
    a.clearScene();
    a.toggleStep(0, 0);
    a.toggleStep(2, 5);
    a.setStepLane(2, 5, StepLane::Velocity, 77);
    a.renamePad(3, "MY HAT");
    a.paramSet(ParamRef::pad(PP_CUTOFF, 3), 1234.0f);
    a.setSongSlot(0, 1);
    a.setSongSlot(1, 3);
    if (!a.macro(1)) a.toggleMacro(1);
    CHECK(a.dirty());

    CHECK(a.saveAs(kProj));
    CHECK(strcmp(a.project().name, kProj) == 0);
    waitLoader();
    CHECK(!a.dirty());
    CHECK(isFile(projPath()));
    if (synth_idx >= 0) CHECK(countWavs(projDir()) == 1);
    char names[64][32];
    const int n = a.listProjects(names, 64);
    bool listed = false;
    for (int i = 0; i < n; ++i) listed = listed || strcmp(names[i], kProj) == 0;
    CHECK(listed);
    CHECK(a.exportJson());
    waitLoader();
    CHECK(isFile(projJson()));

    // change everything, then open the saved file
    a.setBpm(90.0f);
    a.toggleStep(0, 0);
    a.renamePad(3, "X");
    a.setSongSlot(0, 2);
    a.toggleMacro(1);
    a.selectScene(0);
    CHECK(a.dirty());
    CHECK(a.open(kProj));
    waitLoader();
    pump(4);
    const Project& p = a.project();
    NEAR(p.bpm, 133.0f, 1e-3f);
    CHECK(p.scene == 1);
    CHECK(p.scene_data[1].track[0].step[0].on == 1);
    CHECK(p.scene_data[1].track[2].step[5].on == 1 && p.scene_data[1].track[2].step[5].vel == 77);
    CHECK(strcmp(p.pad[3].name, "MY HAT") == 0);
    NEAR(p.pad[3].p[PP_CUTOFF], 1234.0f, 0.5f);
    CHECK(p.song.slot[0] == 1 && p.song.slot[1] == 3);
    CHECK(p.wide_on == 1);
    CHECK(!a.dirty());
    if (builtinKitCount() >= 2) CHECK(a.padSample(0) && a.padSample(0)->embedded);   // "builtin:" refs
    if (synth_idx >= 0) {
        const Sample* s = a.padSample(12, 0);
        CHECK(s && s->frames == (uint32_t)kSynthFrames && std::fabs(s->peak - 0.966f) < 0.02f);
        CHECK(s && strstr(s->path, kProj) != nullptr);   // read back from the project folder
    }
    a.open("ZZ_no_such_project");
    waitLoader();
    NEAR(a.project().bpm, 133.0f, 1e-3f);   // a failed open keeps the project

    a.newProject();
    pump(4);
    CHECK(strncmp(a.project().name, "ORBIT_", 6) == 0);
    NEAR(a.project().bpm, 120.0f, 1e-3f);
    CHECK(!a.dirty());
    if (builtinKitCount() >= 2) CHECK(strcmp(a.padSampleName(0), builtinKit(1).sounds[0].name) == 0);
}

static void testSettings(App& a)
{
    Settings& e = a.settingsEdit();
    e.brightness = 55;
    e.low_power = 1;
    e.output = AudioOut::Headphones;
    a.applySettings();
    pump(2);   // volTick: the headphones' own level reaches the codec
    CHECK(halHostBrightness() == 55);
    CHECK(halHostFrameCap() == 30);
    CHECK(halHostOutput() == AudioOut::Headphones);
    CHECK(a.settings().brightness == 55);
    // output volume memory (0.3): the VOLUME knob sets the level of the output that plays now
    CHECK(a.outputKind() == OutKind::Headphones);
    a.setOutputVolume(40);
    CHECK(halHostVolume() == 40);
    CHECK(a.outputVolume(OutKind::Headphones) == 40);
    a.paramSetNorm(ParamRef::outVolume(), 0.35f);
    CHECK(halHostVolume() == 35 && a.outputVolume() == 35);
    // the speaker keeps its own level, applied by itself when the output changes
    a.settingsEdit().output = AudioOut::Speaker;
    a.applySettings();
    pump(2);
    CHECK(a.outputKind() == OutKind::Speaker);
    CHECK(halHostVolume() == a.outputVolume(OutKind::Speaker));
    a.setOutputVolume(66);
    CHECK(halHostVolume() == 66);
    a.settingsEdit().output = AudioOut::Headphones;
    a.applySettings();
    pump(2);
    CHECK(halHostVolume() == 35);
    // APPLY never overwrites the levels with the dialog's copy
    a.settingsEdit().vol_headphones = 1;
    a.applySettings();
    CHECK(a.outputVolume(OutKind::Headphones) == 35);
    a.setOutputVolume(40);
    halHostSetMicros(5000000000ull);   // the levels are saved 1.5 s after the last change
    pump(1);
    halHostSetMicros(0);
    Settings rd;
    CHECK(hal::settingsLoad(rd) && rd.brightness == 55 && rd.vol_headphones == 40 && rd.vol_init == 1);
    a.settingsEdit().brightness = 5;
    a.settingsDiscard();
    CHECK(a.settingsEdit().brightness == 55);
    a.settingsEdit().low_power = 0;
    a.settingsEdit().output = AudioOut::Auto;
    a.applySettings();
    CHECK(halHostFrameCap() == 60);

    // Audio > Voice limit: reaches the engine at once, is saved, and is clamped to 8..32
    CHECK(a.settings().voice_limit == kVoices && a.engine().voiceLimit() == kVoices);
    a.settingsEdit().voice_limit = 12;
    a.applySettings();
    CHECK(a.engine().voiceLimit() == 12);
    CHECK(hal::settingsLoad(rd) && rd.voice_limit == 12);
    a.settingsEdit().voice_limit = 3;
    a.applySettings();
    CHECK(a.engine().voiceLimit() == Engine::kMinVoiceLimit && a.settings().voice_limit == Engine::kMinVoiceLimit);
    a.settingsEdit().voice_limit = kVoices;
    a.applySettings();
    CHECK(a.engine().voiceLimit() == kVoices);

    // sample rate: audio stop -> engine re-init -> audio start; samples stay on the pads
    a.settingsEdit().sample_rate = 44100;
    a.applySettings();
    CHECK(a.engine().sampleRate() == 44100);
    CHECK(halHostAudioRate() == 44100);
    a.padDown(0, 120, Producer::Touch);
    float pk = 0.0f, rms = 0.0f;
    renderMeasure(22050, pk, rms);
    std::printf("44.1 kHz pad hit: peak %.3f rms %.4f\n", (double)pk, (double)rms);
    CHECK(pk > 0.02f);
    a.settingsEdit().sample_rate = 48000;
    a.settingsEdit().buffer_frames = 256;
    a.applySettings();
    CHECK(a.engine().sampleRate() == 48000 && halHostAudioRate() == 48000 && halHostAudioBlock() == 256);
    a.settingsEdit().buffer_frames = 128;
    a.applySettings();
    CHECK(halHostAudioBlock() == 128);

    // Display colours: base palette + accent (8 presets, CUSTOM = hue + saturation), applied at the next
    // start, saved with APPLY and clamped.
    static_assert(offsetof(Settings, base) == 76 && sizeof(Settings) == 80, "colours after the 0.3.0 / 0.3.1 payload");
    const Settings def;
    CHECK(def.base == 0 && def.theme == 0 && def.accent_hue == 0 && def.accent_sat == 0);
    // a 0.3.0 / 0.3.1 file is 76 bytes: decoded over the defaults (hal/storage.cpp), the colours stay LIGHT / ORANGE
    Settings older;
    older.theme = 5;
    memset(reinterpret_cast<uint8_t*>(&older) + 76, 0xAB, sizeof(Settings) - 76);   // bytes such a file never had
    Settings dec;
    settingsDefaults(dec);
    memcpy(&dec, &older, 76);
    CHECK(dec.theme == 5 && dec.base == 0 && dec.accent_hue == 0 && dec.accent_sat == 0);
    // a zero-filled tail is the default look as well (accent_sat 0 = never set)
    memset(reinterpret_cast<uint8_t*>(&dec) + 76, 0, sizeof(Settings) - 76);
    CHECK(dec.base == 0 && dec.accent_hue == 0 && dec.accent_sat == 0);
    // round trip: APPLY -> active settings -> settings file
    a.settingsEdit().base = 4;
    a.settingsEdit().theme = kAccentCustom;
    a.settingsEdit().accent_hue = 217;
    a.settingsEdit().accent_sat = 76;
    a.applySettings();
    CHECK(a.settings().base == 4 && a.settings().theme == kAccentCustom);
    CHECK(a.settings().accent_hue == 217 && a.settings().accent_sat == 76);
    CHECK(hal::settingsLoad(rd) && rd.base == 4 && rd.theme == kAccentCustom && rd.accent_hue == 217 && rd.accent_sat == 76);
    a.settingsEdit().base = 1;   // discarded edits never reach the active copy
    a.settingsDiscard();
    CHECK(a.settingsEdit().base == 4);
    // clamped: a base / accent out of range is the default, hue 0..359, saturation 0 (never set) or 20..100
    a.settingsEdit().base = 9;
    a.settingsEdit().theme = 12;
    a.settingsEdit().accent_hue = 500;
    a.settingsEdit().accent_sat = 5;
    a.applySettings();
    CHECK(a.settings().base == 0 && a.settings().theme == 0);
    CHECK(a.settings().accent_hue == 359 && a.settings().accent_sat == kAccentSatMin);
    a.settingsEdit().accent_sat = 250;
    a.applySettings();
    CHECK(a.settings().accent_sat == 100);
    a.settingsEdit().accent_sat = 0;
    a.applySettings();
    CHECK(a.settings().accent_sat == 0);
    for (int b = 0; b < kBaseCount; ++b) {   // every base and accent in range is kept
        a.settingsEdit().base = (uint8_t)b;
        a.settingsEdit().theme = (uint8_t)(b + 3);
        a.applySettings();
        CHECK(a.settings().base == b && a.settings().theme == b + 3);
    }
    CHECK(hal::settingsLoad(rd) && rd.base == kBaseCount - 1 && rd.theme == kAccentCustom);
    a.settingsEdit().base = 0;
    a.settingsEdit().theme = 0;
    a.settingsEdit().accent_hue = 0;
    a.settingsEdit().accent_sat = 0;
    a.applySettings();
    CHECK(a.settings().base == 0 && a.settings().theme == 0 && a.settings().accent_hue == 0);
}

// ------------------------------------------------------------------ Bluetooth (hal_host's fake Pico)
// Every command ORBIT sent to the Pico since the last call, as "\nCMD\nCMD\n".
static std::string btSent()
{
    std::string all = "\n";
    char line[128];
    while (halHostBtTakeSent(line, sizeof line) > 0) {
        all += line;
        all += '\n';
    }
    return all;
}
static bool sentHas(const std::string& all, const char* cmd) { return all.find("\n" + std::string(cmd) + "\n") != std::string::npos; }
static void btFeed(const char* line, int blocks = 2)
{
    halHostBtFeed(line);
    pump(blocks);
}

static void testBluetooth(App& a)
{
    Settings rd;
    float pk = 0.0f, rms = 0.0f;
    CHECK(!a.btEnabled() && !a.btStatus().enabled && a.settings().bt_local == 0 && a.settings().bt_volume == 70);
    CHECK(a.engine().sampleRate() == 48000 && halHostAudioRate() == 48000);
    btSent();

    // On: bridge + volume + route, engine 48 -> 44.1 kHz through the rate-change path, saved at once.
    const uint32_t rb0 = a.revs().bt;
    a.btSetEnabled(true);
    CHECK(a.btEnabled() && a.revs().bt != rb0);
    CHECK(a.engine().sampleRate() == 44100 && halHostAudioRate() == 44100 && halHostAudioBlock() == 128);
    CHECK(containsCi(testhooks::lastToast(), "Bluetooth włączony") && containsCi(testhooks::lastToast(), "44,1 kHz"));
    CHECK(hal::settingsLoad(rd) && rd.bt_enabled == 1 && rd.sample_rate == 48000);   // the user's rate stays
    CHECK(a.settings().sample_rate == 48000 && a.settingsEdit().bt_enabled == 1);
    pump(2);
    CHECK(a.btStatus().enabled && !a.btStatus().module);
    halHostBtAdvance(800);   // wake pulse + boot guard
    std::string sent = btSent();
    CHECK(sentHas(sent, "USE"));
    // the pads still play after the engine re-init
    a.padDown(0, 120, Producer::Touch);
    renderMeasure(22050, pk, rms);
    CHECK(pk > 0.02f);

    // The Pico answers, lists its devices and connects the selected one.
    btFeed("READY version=0.9.5 cap_airpods=1");
    halHostBtAdvance(100);
    sent = btSent();
    CHECK(sentHas(sent, "LIST") && sentHas(sent, "VOLUME 70"));
    btFeed("DEVICE 1 A4:C3:37:5E:91:0B -127 1 1 AirPods Pro");
    btFeed("DEVICE 2 00:42:79:B1:22:EC -58 0 0 JBL Flip 6");
    btFeed("END_LIST");
    CHECK(a.btStatus().module && strcmp(a.btStatus().version, "0.9.5") == 0 && !a.btStatus().connected);
    CHECK(a.btDeviceCount() == 2);
    CHECK(strcmp(a.btDevice(0).name, "AirPods Pro") == 0 && a.btDevice(0).saved && a.btDevice(0).selected &&
          a.btDevice(0).rssi == 0);
    CHECK(strcmp(a.btDevice(1).mac, "00:42:79:B1:22:EC") == 0 && a.btDevice(1).rssi == -58 && !a.btDevice(1).saved);
    CHECK(a.btDevice(2).mac[0] == 0 && a.btDevice(-1).name[0] == 0);   // out of range: an empty device
    const int toasts = testhooks::toastCount();
    const uint32_t rb1 = a.revs().bt;
    btFeed("STATE connection=2 audio=0");
    CHECK(a.btStatus().connected && a.revs().bt != rb1);
    CHECK(testhooks::toastCount() > toasts && strcmp(testhooks::lastToast(), "Bluetooth: połączono z AirPods Pro") == 0);
    // The connect sent LIST again: 4 saved + 8 found devices all reach the App (the Pico lists up to 32).
    CHECK(sentHas(btSent(), "LIST"));
    btFeed("DEVICE 1 A4:C3:37:5E:91:0B -127 1 1 AirPods Pro");
    btFeed("DEVICE 2 00:42:79:B1:22:EC -127 1 0 JBL Flip 6");
    btFeed("DEVICE 3 10:00:00:00:00:03 -127 1 0 Saved 3");
    btFeed("DEVICE 4 10:00:00:00:00:04 -127 1 0 Saved 4");
    for (int i = 0; i < 8; ++i) {
        char l[80];
        std::snprintf(l, sizeof l, "DEVICE %d 20:00:00:00:00:%02X -%d 0 0 Found %d", 5 + i, i, 50 + i, i);
        btFeed(l);
    }
    btFeed("END_LIST");
    CHECK(a.btDeviceCount() == 12 && strcmp(a.btDevice(11).name, "Found 7") == 0 && a.btDevice(11).rssi == -57);
    CHECK(strcmp(a.btDevice(1).name, "JBL Flip 6") == 0 && a.btDevice(1).saved && !a.btDevice(1).selected);

    // Headphone buttons: PLAY / PAUSE drive the transport, NEXT / PREVIOUS step through the scenes (wrapping).
    a.stop();
    pump(2);
    CHECK(!a.playing());
    btFeed("REMOTE PLAY");
    CHECK(a.playing());
    btFeed("REMOTE PLAY");   // already playing: keeps playing
    CHECK(a.playing());
    pump(300);               // ~0.9 s: a few steps in
    const int at = a.playStep();
    CHECK(at >= 3);
    btFeed("REMOTE PAUSE");
    CHECK(!a.playing());
    btFeed("REMOTE PLAY");   // resumes like the on-screen play button (Continue), not from step 0
    CHECK(a.playing() && a.playStep() >= at);
    btFeed("REMOTE PAUSE");
    CHECK(!a.playing());
    a.selectScene(3);
    btFeed("REMOTE NEXT");
    CHECK(a.project().scene == 0);
    btFeed("REMOTE NEXT");
    CHECK(a.project().scene == 1);
    btFeed("REMOTE PREVIOUS");
    CHECK(a.project().scene == 0);
    btFeed("REMOTE PREVIOUS");
    CHECK(a.project().scene == 3);
    a.selectScene(0);
    pump(2);

    // Volume: to the Pico at once, saved once ~1 s after the last change (a slider drag).
    const uint64_t t = 7000000000ull;
    halHostSetMicros(t);
    a.btSetVolume(40);
    a.btSetVolume(55);
    a.btSetVolume(140);   // clamped
    a.btSetVolume(55);
    halHostBtAdvance(100);
    sent = btSent();
    CHECK(sentHas(sent, "VOLUME 55") && a.settings().bt_volume == 55);
    pump(1);
    CHECK(hal::settingsLoad(rd) && rd.bt_volume == 70);   // not yet
    halHostSetMicros(t + 1500000ull);
    pump(1);
    CHECK(hal::settingsLoad(rd) && rd.bt_volume == 55);
    halHostSetMicros(0);

    // Also speaker / jack (off by default: the speaker would play ~0.2 s ahead of the headphones)
    a.btSetLocal(true);
    CHECK(a.settings().bt_local == 1 && hal::settingsLoad(rd) && rd.bt_local == 1);
    a.btSetLocal(false);
    CHECK(a.settings().bt_local == 0 && hal::settingsLoad(rd) && rd.bt_local == 0);

    // AirPods, test tone, connect another device
    btFeed("AIRPODS connected=1 mode=2 left=80 right=75 case=40 ears=3 auto_pause=0");
    const hal::BtStatus& st = a.btStatus();
    CHECK(st.airpods && st.airpods_left == 80 && st.airpods_right == 75 && st.airpods_case == 40 && st.airpods_mode == 2);
    a.btAirpodsMode(3);
    a.btAirpodsMode(9);   // ignored
    halHostBtAdvance(60);
    sent = btSent();
    CHECK(sentHas(sent, "AIRPODS MODE 3") && sent.find("AIRPODS MODE 9") == std::string::npos);
    a.btTest();
    halHostBtAdvance(60);
    CHECK(sentHas(btSent(), "TEST"));
    // SCAN restarts the Pico: the link drops on purpose and the pairing hint stays (no "rozłączono" over it).
    a.btScan();
    halHostBtAdvance(60);
    CHECK(sentHas(btSent(), "SCAN") && containsCi(testhooks::lastToast(), "włącz parowanie"));
    btFeed("RESTART scanning; saved target preserved");
    CHECK(!a.btStatus().connected && a.btStatus().scanning && containsCi(testhooks::lastToast(), "włącz parowanie"));
    btFeed("READY version=0.9.5 cap_airpods=1");
    btFeed("SCAN_DONE");
    btFeed("STATE connection=2 audio=0");
    CHECK(a.btStatus().connected && !a.btStatus().scanning);
    btSent();
    // A device row (SELECTMAC) restarts it too: "łączę z ..." stays up.
    a.btSelect("00:42:79:B1:22:EC");
    halHostBtAdvance(60);
    CHECK(sentHas(btSent(), "SELECTMAC 00:42:79:B1:22:EC"));
    CHECK(containsCi(testhooks::lastToast(), "łączę z JBL Flip 6"));
    btFeed("SELECTED JBL Flip 6; restarting to connect");
    CHECK(!a.btStatus().connected && containsCi(testhooks::lastToast(), "łączę z JBL Flip 6"));
    btFeed("READY version=0.9.5 cap_airpods=1");
    btFeed("STATE connection=2 audio=0");
    CHECK(a.btStatus().connected);

    // Audio > Sample rate while Bluetooth holds 44.1 kHz: only remembered; bt_* never come from the APPLY copy.
    a.settingsEdit().sample_rate = 44100;
    a.applySettings();
    CHECK(a.engine().sampleRate() == 44100 && a.settings().sample_rate == 44100);
    a.settingsEdit().sample_rate = 48000;
    a.applySettings();
    CHECK(a.engine().sampleRate() == 44100 && a.settings().sample_rate == 48000);
    CHECK(containsCi(testhooks::lastToast(), "po wyłączeniu Bluetooth"));
    a.settingsEdit().bt_enabled = 0;
    a.settingsEdit().bt_volume = 3;
    a.applySettings();
    CHECK(a.btEnabled() && a.settings().bt_enabled == 1 && a.settings().bt_volume == 55);

    // Disconnect (5 s after the last SCAN / SELECT / RECONNECT: a real one gets its toast)
    halHostSetMicros(hal::micros() + 6000000ull);
    btFeed("STATE connection=0 audio=0");
    CHECK(!a.btStatus().connected && strcmp(testhooks::lastToast(), "Bluetooth: rozłączono") == 0);
    halHostSetMicros(0);

    // Off: STOP + SLEEP, the engine back at the remembered 48 kHz, saved; buttons are ignored.
    a.btSetEnabled(false);
    CHECK(!a.btEnabled() && a.engine().sampleRate() == 48000 && halHostAudioRate() == 48000);
    CHECK(strcmp(testhooks::lastToast(), "Bluetooth wyłączony \xE2\x80\x94 silnik audio 48 kHz") == 0);
    CHECK(hal::settingsLoad(rd) && rd.bt_enabled == 0 && rd.bt_volume == 55);
    halHostBtAdvance(100);
    sent = btSent();
    CHECK(sentHas(sent, "SLEEP"));
    btFeed("STANDBY");
    CHECK(!a.btStatus().enabled);
    CHECK(a.btDeviceCount() == 0 && a.btDevice(0).mac[0] == 0);   // no stale rows while off
    btFeed("REMOTE PLAY");
    CHECK(!a.playing());
    a.padDown(0, 120, Producer::Touch);
    renderMeasure(24000, pk, rms);
    CHECK(pk > 0.02f);
    a.btScan();
    CHECK(containsCi(testhooks::lastToast(), "Najpierw włącz Bluetooth"));

    // Engine already at 44.1 kHz: no switch, nothing to restore.
    a.settingsEdit().sample_rate = 44100;
    a.applySettings();
    CHECK(a.engine().sampleRate() == 44100);
    a.btSetEnabled(true);
    CHECK(a.engine().sampleRate() == 44100 && strcmp(testhooks::lastToast(), "Bluetooth włączony") == 0);
    a.btSetEnabled(false);
    CHECK(a.engine().sampleRate() == 44100 && strcmp(testhooks::lastToast(), "Bluetooth wyłączony") == 0);
    a.settingsEdit().sample_rate = 48000;
    a.applySettings();
    CHECK(a.engine().sampleRate() == 48000 && halHostAudioRate() == 48000);
    halHostBtAdvance(3000);
    btSent();
    pump(4);
}

static void testRecordAndAudio(App& a)
{
    a.selectScene(0);
    a.clearTrack(9);
    a.play();
    CHECK(a.playing());
    pump(40);
    CHECK(a.playStep() >= 0);
    a.toggleRecord();
    CHECK(a.recording());
    pump(4);
    a.padDown(9, 100, Producer::Touch);
    pump(30);
    a.padUp(9, Producer::Touch);
    const Scene& sc = a.project().scene_data[a.project().scene];
    CHECK(trackHits(sc, 9, kMaxSteps) >= 1);   // RecordHit written by tick()

    // Polymeter track (3 steps in a 16-step scene): recorded hits wrap into the track's own steps.
    a.clearTrack(8);
    a.setTrackLength(8, 3);
    for (int k = 0; k < 6; ++k) {
        a.padDown(8, 90, Producer::Touch);
        pump(3);
        a.padUp(8, Producer::Touch);
        pump(70 + 13 * k);
    }
    CHECK(trackHits(sc, 8, 3) >= 1);
    CHECK(trackHits(sc, 8, kMaxSteps) == trackHits(sc, 8, 3));   // nothing written past the track length
    a.setTrackLength(8, 0);
    a.clearTrack(8);
    a.toggleRecord();
    CHECK(!a.recording());

    // 2 s of the demo pattern
    a.padFlashBits();
    float pk = 0.0f, rms = 0.0f;
    renderMeasure(96000, pk, rms);
    const uint32_t bits = a.padFlashBits();
    std::printf("2 s playing: peak %.3f rms %.4f, pads hit 0x%04x, cpu %.3f, voices %d\n", (double)pk, (double)rms,
                (unsigned)bits, (double)a.cpuLoad(), a.activeVoices());
    CHECK(pk > 0.05f);
    CHECK(rms > 0.005f);
    CHECK(bits != 0);
    CHECK(a.latencyMs() > 0.0f);
    CHECK(a.cpuLoad() > 0.0f);          // driver timing hook -> Engine::reportTiming
    CHECK(!halHostAudioIdle());         // playing: no deferred flash writes
    a.stop();
    pump(4);
    CHECK(!a.playing());
    a.stop();   // second stop = rewind
    // Idle (deferred NVS writes allowed) only after the voices, FX tails and a 3 s hold of silence.
    pump(2);
    CHECK(!halHostAudioIdle());
    const uint64_t t0 = nowUs();
    while (!halHostAudioIdle() && nowUs() - t0 < 15000000u) {   // voices ring out, then the hold
        pump();
        sleepMs(1);
    }
    CHECK(halHostAudioIdle());
    CHECK(a.activeVoices() == 0);
    CHECK(nowUs() - t0 >= 2900000u);
    // A live hit (touch / MIDI task) clears the flag at once, before the engine has even started the voice.
    a.padDown(0, 100, Producer::Touch);
    CHECK(!halHostAudioIdle());
    a.padUp(0, Producer::Touch);
    pump(4);
    CHECK(!halHostAudioIdle());
}

// host/hal_host.cpp itself: hooks reach the registered callbacks, settings file round trip.
static int g_touch_calls = 0, g_touch_last = -1, g_midi_note = -1;
static void testHalHost()
{
    hal::setTouchHook(
        [](const hal::TouchPoint* pts, int count, void*) {
            ++g_touch_calls;
            g_touch_last = count > 0 ? pts[0].x : -1;
        },
        nullptr);
    const hal::TouchPoint tp[2] = {{100, 200, 0, 0}, {300, 400, 1, 0}};
    halHostTouch(tp, 2);
    halHostTouch(nullptr, 0);
    CHECK(g_touch_calls == 2 && g_touch_last == -1);
    halHostTouch(tp, 1);
    CHECK(g_touch_last == 100);
    hal::setTouchHook(nullptr, nullptr);
    halHostTouch(tp, 1);
    CHECK(g_touch_calls == 3);
    CHECK(hal::midiStart([](const hal::MidiMsg& m, void*) { g_midi_note = m.d1; }, nullptr));
    halHostMidi(hal::MidiMsg{0x99, 36, 100, 0});
    CHECK(g_midi_note == 36 && !hal::midiConnected() && !hal::midiSend(hal::MidiMsg{0x90, 1, 1, 0}));
    Settings s, r;
    CHECK(!hal::settingsLoad(r));   // no file yet -> defaults
    CHECK(r.brightness == Settings{}.brightness);
    s.brightness = 33;
    s.sample_rate = 44100;
    CHECK(hal::settingsSave(s));
    CHECK(hal::settingsLoad(r) && r.brightness == 33 && r.sample_rate == 44100);
    remove(halHostSettingsPath());
    CHECK(hal::boardInfo()[0] && hal::batteryPercent() == -1 && !hal::charging());
    CHECK(hal::lvLock(10));
    hal::lvUnlock();
    CHECK(hal::displayStart() == nullptr);   // HAL_HOST_NO_LVGL
    CHECK(!hal::audioRunning() && hal::audioLatencyMs() == 0.0f);
    int16_t buf[64];
    halHostRenderAudio(32, buf);
    bool silent = true;
    for (int16_t v : buf) silent = silent && v == 0;
    CHECK(silent);
    CHECK(hal::sdMounted() && strstr(hal::sdRoot(), "sdcard"));

    // MIDI out capture, fake clock, service timer, launcher simulation
    halHostMidiSetConnected(true, "HOST KEYS");
    CHECK(hal::midiConnected() && strcmp(hal::midiDeviceName(), "HOST KEYS") == 0);
    CHECK(hal::midiSend(hal::MidiMsg{0xF8, 0, 0, 0}) && hal::midiSend(hal::MidiMsg{0x90, 60, 99, 0}));
    hal::MidiMsg sent[4];
    CHECK(halHostMidiTakeSent(sent, 4) == 2 && sent[0].status == 0xF8 && sent[1].d2 == 99);
    CHECK(halHostMidiTakeSent(sent, 4) == 0);
    halHostMidiSetConnected(false);
    CHECK(!hal::midiConnected() && !hal::midiSend(hal::MidiMsg{0xF8, 0, 0, 0}) && hal::midiDeviceName()[0] == 0);
    halHostSetMicros(123456789ull);
    CHECK(hal::micros() == 123456789ull);
    halHostSetMicros(0);
    CHECK(hal::micros() != 123456789ull);
    static int svc_calls = 0;
    CHECK(!halHostServiceTimer() && halHostServiceTimerPeriod() == 0);
    CHECK(hal::serviceTimerStart([](void* u) { svc_calls += *static_cast<int*>(u); }, &svc_calls, 100));
    svc_calls = 1;
    CHECK(halHostServiceTimerPeriod() == 500);   // clamped like the device
    CHECK(halHostServiceTimer() && svc_calls == 2);
    CHECK(hal::serviceTimerStart(nullptr, nullptr, 0) && !halHostServiceTimer() && halHostServiceTimerPeriod() == 0);
    CHECK(!hal::launcherAvailable() && !hal::restartToLauncher() && !halHostLauncherRequested());
    halHostSetLauncher(true);
    CHECK(hal::launcherAvailable() && hal::restartToLauncher() && halHostLauncherRequested());
    halHostSetLauncher(false);
    CHECK(!hal::launcherAvailable() && !halHostLauncherRequested());
}

// ================================================================== wave 2: App extras
static std::string midimapPath() { return g_sd + "/ORBIT/midimap.bin"; }
static std::vector<std::string> g_user_files;   // SAVE WAV outputs, removed at the end

static void midi(uint8_t status, uint8_t d1 = 0, uint8_t d2 = 0) { halHostMidi(hal::MidiMsg{status, d1, d2, 0}); }

// Pads hit by one note on (engine PadHit -> padFlashBits).
static uint32_t noteHits(uint8_t status, uint8_t note, uint8_t vel)
{
    App& a = app();
    pump(2);
    a.padFlashBits();
    midi(status, note, vel);
    pump(2);
    return a.padFlashBits();
}

static void testRenameQuantUndo(App& a)
{
    // ---- renameProject
    const uint32_t rp = a.revs().project;
    a.renameProject("My Song");
    CHECK(strcmp(a.project().name, "My Song") == 0);
    CHECK(a.revs().project != rp);
    CHECK(a.dirty());
    a.renameProject("a/b:c*?.");
    CHECK(strcmp(a.project().name, "a_b_c__") == 0);   // file-system safe, no trailing dot
    a.renameProject("   ");
    CHECK(strcmp(a.project().name, "a_b_c__") == 0);
    CHECK(strstr(testhooks::lastToast(), "pusta") != nullptr);
    a.renameProject(nullptr);
    CHECK(strcmp(a.project().name, "a_b_c__") == 0);
    a.renameProject("0123456789012345678901234567890123456789");
    CHECK(strlen(a.project().name) == 31);
    if (isFile(projPath())) {   // saved by testSaveOpen
        a.renameProject(kProj);
        CHECK(strcmp(a.project().name, kProj) == 0);
        CHECK(strstr(testhooks::lastToast(), "istnieje") != nullptr);
    }
    a.renameProject("ORBIT_RENAMED");
    CHECK(strstr(testhooks::lastToast(), "ORBIT_RENAMED") != nullptr);

    // ---- setLaunchQuant: stored in the project and used by the sequencer
    const uint32_t rs = a.revs().settings;
    a.setLaunchQuant(LaunchQuant::Beat);
    CHECK(a.project().quant == LaunchQuant::Beat && a.revs().settings != rs);
    a.setLaunchQuant(LaunchQuant::Count);
    CHECK(a.project().quant == LaunchQuant::Beat);
    a.setBpm(120.0f);
    a.selectScene(0);
    pump(2);
    a.setLaunchQuant(LaunchQuant::Off);
    a.play();
    pump(10);
    a.selectScene(2);           // Off: switches at the next step (1/16 = 47 blocks at 120 BPM)
    pump(60);
    CHECK(a.playScene() == 2);
    a.setLaunchQuant(LaunchQuant::Bar);
    a.selectScene(3);           // Bar: waits for beat 4 (750 blocks after Play)
    pump(60);
    CHECK(a.playScene() == 2 && a.queuedScene() == 3);
    pump(720);
    CHECK(a.playScene() == 3);
    a.stop();
    a.stop();
    pump(4);
    a.setLaunchQuant(LaunchQuant::Bar);

    // ---- undoPattern: one level, CLEAR / RANDOM / CLEAR TRACK / COPY (target)
    a.selectScene(1);
    a.clearScene();
    a.toggleStep(0, 0);
    a.toggleStep(2, 6);
    a.setStepLane(2, 6, StepLane::Velocity, 55);
    a.toggleStep(9, 40);
    Scene ref = a.project().scene_data[1];
    CHECK(a.canUndoPattern());   // the clear above
    const uint32_t gen0 = a.patternUndoGen();
    a.clearScene();
    CHECK(a.patternUndoGen() != gen0 && a.undoPatternScene() == 1);   // a new snapshot, of scene B
    CHECK(trackHits(a.project().scene_data[1], 0, kMaxSteps) == 0);
    uint32_t rpat = a.revs().pattern;
    a.undoPattern();
    CHECK(a.revs().pattern != rpat);
    CHECK(memcmp(&a.project().scene_data[1], &ref, sizeof ref) == 0);
    CHECK(!a.canUndoPattern() && a.undoPatternScene() == -1);
    a.undoPattern();   // nothing left
    CHECK(strstr(testhooks::lastToast(), "Brak") != nullptr);
    CHECK(memcmp(&a.project().scene_data[1], &ref, sizeof ref) == 0);

    a.randomScene();
    CHECK(memcmp(&a.project().scene_data[1], &ref, sizeof ref) != 0);
    CHECK(a.canUndoPattern());
    a.undoPattern();
    CHECK(memcmp(&a.project().scene_data[1], &ref, sizeof ref) == 0);

    a.clearTrack(2);
    CHECK(trackHits(a.project().scene_data[1], 2, kMaxSteps) == 0);
    a.undoPattern();
    CHECK(a.project().scene_data[1].track[2].step[6].on == 1 && a.project().scene_data[1].track[2].step[6].vel == 55);

    // COPY: the target scene comes back, the edited (current) scene is not touched
    const Scene scene2 = a.project().scene_data[2];
    a.copyScene(1, 2);
    CHECK(memcmp(&a.project().scene_data[2], &ref, sizeof ref) == 0);
    a.toggleStep(5, 5);   // a later edit of the current scene
    a.undoPattern();
    CHECK(memcmp(&a.project().scene_data[2], &scene2, sizeof scene2) == 0);
    CHECK(a.project().scene_data[1].track[5].step[5].on == 1);
    a.toggleStep(5, 5);
    CHECK(strstr(testhooks::lastToast(), "C") != nullptr);

    // a scene of 64 steps on page 4, undo to 16 steps: the SEQUENCE page follows
    a.setSceneLength(16);
    a.randomScene();
    a.clearScene();
    a.setSceneLength(64);
    a.setSeqPage(3);
    a.randomScene();
    a.undoPattern();   // the scene had length 64 at the snapshot: page stays
    CHECK(a.ui().seq_page == 3);
    a.setSceneLength(16);
    CHECK(a.ui().seq_page == 0);

    // copyScene with invalid arguments does not take a snapshot
    a.clearScene();
    a.undoPattern();
    CHECK(!a.canUndoPattern());
    a.copyScene(1, 1);
    a.copyScene(-1, 2);
    CHECK(!a.canUndoPattern());
    a.clearTrack(99);
    CHECK(!a.canUndoPattern());
}

static void testSaveSampleWav(App& a, bool have_synth)
{
    // built-in kick on A1 -> USER/<name>.wav, a second save -> <name>_2.wav
    a.selectPad(0, false);
    a.selectLayer(0);
    const std::string name = a.padSampleName(0);
    std::string base;
    for (char c : name) base += c == ' ' ? '_' : c;
    const std::string f1 = g_sd + "/ORBIT/USER/" + base + ".wav";
    const std::string f2 = g_sd + "/ORBIT/USER/" + base + "_2.wav";
    const bool pre1 = isFile(f1), pre2 = isFile(f2);
    CHECK(!name.empty());
    CHECK(a.saveSampleWav());
    waitLoader();
    CHECK(isFile(f1));
    if (!pre1) g_user_files.push_back(f1);
    CHECK(strstr(testhooks::lastToast(), "Zapisano") != nullptr);
    CHECK(a.saveSampleWav());
    waitLoader();
    if (!pre1) {
        CHECK(isFile(f2));
        if (!pre2) g_user_files.push_back(f2);
    }
    waitLoader();   // quiet rescan
    a.setCategory(Category::User);
    a.setSearch(base.c_str());
    CHECK(a.resultCount() >= 1);
    a.setSearch("");
    a.setCategory(Category::All);
    WavInfo wi;
    CHECK(wavProbe(f1.c_str(), wi) == WavError::Ok && wi.frames == a.padSample(0, 0)->frames);

    // empty layer
    a.selectLayer(3);
    CHECK(!a.saveSampleWav());
    CHECK(strstr(testhooks::lastToast(), "Brak sampla") != nullptr);
    a.selectLayer(0);

    // An edited sample points at its USER file afterwards: a project save writes no copy.
    if (have_synth) {
        a.setCategory(Category::User);
        a.setSearch(kSynth);
        const int idx = a.resultCount() > 0 ? a.result(0) : -1;
        a.setSearch("");
        CHECK(idx >= 0);
        if (idx >= 0) {
            a.loadFileToPad(idx, 12, 0);
            waitLoader();
            a.selectPad(12, false);
            a.selectLayer(0);
            CHECK(a.applyOp(SampleOp::Normalize));
            waitLoader();
            CHECK(a.saveSampleWav());
            waitLoader();
            const std::string fs = g_sd + "/ORBIT/USER/" + kSynth + ".wav";   // exists: -> _2
            const std::string fs2 = g_sd + "/ORBIT/USER/" + kSynth + "_2.wav";
            CHECK(isFile(fs2));
            g_user_files.push_back(fs2);
            (void)fs;
            const std::string dir = g_sd + "/ORBIT/PROJECTS/ZZ_TEST_APP_WAV";
            CHECK(a.saveAs("ZZ_TEST_APP_WAV"));
            waitLoader();
            CHECK(isFile(dir + ".orb"));
            CHECK(countWavs(dir) == 0);
            remove((dir + ".orb").c_str());
            removeTree(dir);
        }
    }
    a.loadBuiltinKit(1);
    a.selectPad(0, false);
}

static void testMidiNotes(App& a)
{
    CHECK(a.settings().midi_usb == 1 && a.settings().midi_channel == 10);
    // GM / 909 map on channel 10 (status 0x99)
    struct NP {
        uint8_t note;
        int pad;
    };
    const NP map[] = {{36, 0}, {37, 5}, {38, 1}, {39, 4}, {41, 6}, {43, 6}, {42, 2}, {45, 7}, {47, 7}, {46, 3},
                      {48, 8}, {50, 8}, {49, 9}, {51, 10}, {52, 11}, {53, 12}, {54, 13}, {55, 14}, {56, 15},
                      {35, 0}, {40, 1}, {44, 2}};
    for (const NP& m : map) {
        CHECK(a.padSample(m.pad) != nullptr);
        const uint32_t bits = noteHits(0x99, m.note, 100);
        if (bits != (1u << m.pad)) std::printf("note %d -> bits 0x%04x (want pad %d)\n", m.note, (unsigned)bits, m.pad);
        CHECK(bits == (1u << m.pad));
        midi(0x89, m.note, 0);
    }
    CHECK(noteHits(0x99, 60, 100) == 0);   // not in the map
    CHECK(noteHits(0x99, 34, 100) == 0);
    CHECK(noteHits(0x99, 57, 100) == 0);
    CHECK(noteHits(0x99, 36, 0) == 0);     // velocity 0 = note off
    CHECK(noteHits(0x89, 36, 64) == 0);    // note off
    CHECK(noteHits(0x90, 36, 100) == 0);   // channel 1: filtered (Settings channel 10)
    CHECK(noteHits(0xB9, 36, 100) == 0);   // CC 36 is not a note
    // velocity reaches the engine: a soft hit is quieter than a hard one
    float pk_soft = 0.0f, pk_hard = 0.0f, rms = 0.0f;
    for (int i = 0; i < 400 && a.activeVoices() > 0; ++i) pump();
    midi(0x99, 36, 20);
    renderMeasure(12000, pk_soft, rms);
    for (int i = 0; i < 400 && a.activeVoices() > 0; ++i) pump();
    midi(0x99, 36, 127);
    renderMeasure(12000, pk_hard, rms);
    std::printf("MIDI kick: vel 20 peak %.3f, vel 127 peak %.3f\n", (double)pk_soft, (double)pk_hard);
    CHECK(pk_soft > 0.001f && pk_hard > 1.5f * pk_soft);
    // a pad in MUTE pad mode still plays from MIDI (pad mode only filters touch)
    a.setPadMode(PadMode::Mute);
    CHECK(noteHits(0x99, 38, 100) == 2u);
    a.setPadMode(PadMode::Play);

    // omni
    a.settingsEdit().midi_channel = 0;
    a.applySettings();
    CHECK(noteHits(0x90, 38, 100) == 2u);
    CHECK(noteHits(0x9F, 42, 100) == 4u);
    a.settingsEdit().midi_channel = 1;
    a.applySettings();
    CHECK(noteHits(0x90, 38, 100) == 2u);
    CHECK(noteHits(0x99, 38, 100) == 0);
    a.settingsEdit().midi_channel = 10;
    a.applySettings();

    // USB MIDI off: everything is ignored
    a.settingsEdit().midi_usb = 0;
    a.applySettings();
    CHECK(noteHits(0x99, 36, 100) == 0);
    midi(0xFA);
    pump(4);
    CHECK(!a.playing());
    a.settingsEdit().midi_usb = 1;
    a.applySettings();
    CHECK(noteHits(0x99, 36, 100) == 1u);

    // device connected / gone (polled in tick)
    const uint32_t rm = a.revs().midi;
    CHECK(!a.midiConnected() && a.midiDeviceName()[0] == 0);
    halHostMidiSetConnected(true, "TEST KEYS");
    pump(1);
    CHECK(a.midiConnected() && strcmp(a.midiDeviceName(), "TEST KEYS") == 0);
    CHECK(a.revs().midi != rm);
    CHECK(strstr(testhooks::lastToast(), "TEST KEYS") != nullptr);
    halHostMidiSetConnected(false);
    pump(1);
    CHECK(!a.midiConnected());
    CHECK(strstr(testhooks::lastToast(), "odłączone") != nullptr);
}

static void testMidiTransport(App& a)
{
    CHECK(!a.playing());
    uint32_t rt = a.revs().transport;
    midi(0xFA);   // Start
    pump(1);
    CHECK(a.playing() && a.revs().transport != rt);
    pump(4);
    CHECK(a.engine().telemetry().playing.load() == 1);
    CHECK(a.playStep() >= 0);
    pump(100);
    const float beat = a.engine().telemetry().beat.load();
    CHECK(beat > 0.4f);
    rt = a.revs().transport;
    midi(0xFC);   // Stop
    pump(2);
    CHECK(!a.playing() && a.revs().transport != rt);
    CHECK(a.engine().telemetry().playing.load() == 0);
    midi(0xFB);   // Continue: from the same position
    pump(2);
    CHECK(a.playing());
    CHECK(a.engine().telemetry().beat.load() >= beat);
    midi(0xFC);
    pump(2);
    CHECK(!a.playing());
    midi(0xFC);   // second stop = rewind
    pump(2);
    midi(0xFA);
    pump(2);
    CHECK(a.playing() && a.engine().telemetry().beat.load() < 0.1f);
    midi(0xFC);
    pump(2);
    CHECK(!a.playing());
    midi(0xFE);   // active sensing / reset / song select are ignored
    midi(0xFF);
    midi(0xF3, 1);
    pump(2);
    CHECK(!a.playing());
    a.stop();   // rewind
    for (int i = 0; i < 3000 && a.activeVoices() > 0; ++i) pump();
}

// 24 PPQN at `bpm` from fake time t (us), jitter +-jit_us, every 7th tick bundled with the previous one.
static uint64_t sendClock(App& a, uint64_t t, float bpm, int ticks, int jit_us, bool bundle)
{
    const double iv = 60000000.0 / (bpm * 24.0);
    uint32_t r = 12345;
    double ideal = (double)t;
    for (int i = 0; i < ticks; ++i) {
        ideal += iv;
        r = r * 1103515245u + 12345u;
        const int j = jit_us ? (int)((r >> 8) % (uint32_t)(2 * jit_us + 1)) - jit_us : 0;
        uint64_t at = (uint64_t)(ideal + j);
        if (bundle && i % 7 == 6) at = (uint64_t)(ideal + iv - 900);   // late: arrives with the next tick
        if (at < t) at = t;
        t = at;
        halHostSetMicros(t);
        midi(0xF8);
        if (i % 3 == 0) a.tick();
    }
    a.tick();
    return t;
}

static void testMidiClockIn(App& a)
{
    a.setBpm(100.0f);
    uint64_t t = 1000000000ull;
    halHostSetMicros(t);
    // clock in off: ticks do nothing
    t = sendClock(a, t, 120.0f, 60, 0, false);
    CHECK(a.midiClockBpm() == 0.0f);
    NEAR(a.project().bpm, 100.0f, 1e-3f);

    a.settingsEdit().midi_clock_in = 1;
    a.applySettings();
    const uint32_t rm = a.revs().midi;
    t = sendClock(a, t + 5000, 120.0f, 12, 800, true);   // < 24 intervals: no tempo yet
    CHECK(a.midiClockBpm() == 0.0f);
    t = sendClock(a, t, 120.0f, 60, 800, true);           // jitter +-0.8 ms, bundled packets
    std::printf("clock in 120: %.3f BPM (project %.1f)\n", (double)a.midiClockBpm(), (double)a.project().bpm);
    NEAR(a.midiClockBpm(), 120.0f, 0.15f);
    NEAR(a.project().bpm, 120.0f, 0.051f);
    CHECK(a.revs().midi != rm);
    t = sendClock(a, t, 128.0f, 72, 800, true);           // tempo change
    std::printf("clock in 128: %.3f BPM (project %.1f)\n", (double)a.midiClockBpm(), (double)a.project().bpm);
    NEAR(a.midiClockBpm(), 128.0f, 0.15f);
    NEAR(a.project().bpm, 128.0f, 0.051f);
    t = sendClock(a, t, 128.2f, 72, 300, false);          // < 0.3 BPM change: the project keeps 128.0
    NEAR(a.project().bpm, 128.0f, 0.051f);
    // the clock stops: after 1 s the external tempo is gone, the project keeps its BPM
    halHostSetMicros(t + 1500000);
    a.tick();
    CHECK(a.midiClockBpm() == 0.0f);
    NEAR(a.project().bpm, 128.0f, 0.051f);
    // resumes at 90 BPM (the pause restarts the window)
    t = sendClock(a, t + 1500000, 90.0f, 60, 800, false);
    std::printf("clock in 90: %.3f BPM\n", (double)a.midiClockBpm());
    NEAR(a.midiClockBpm(), 90.0f, 0.15f);
    NEAR(a.project().bpm, 90.0f, 0.051f);
    // a single lost tick (gap of 2 intervals) does not disturb the estimate much
    t = sendClock(a, t + 666667, 90.0f, 30, 300, false);
    NEAR(a.midiClockBpm(), 90.0f, 0.3f);
    // 300 BPM extreme
    t = sendClock(a, t + 2000000, 300.0f, 96, 500, false);
    NEAR(a.midiClockBpm(), 300.0f, 0.5f);
    NEAR(a.project().bpm, 300.0f, 0.051f);

    a.settingsEdit().midi_clock_in = 0;
    a.applySettings();
    a.tick();
    CHECK(a.midiClockBpm() == 0.0f);
    a.setBpm(100.0f);
    t = sendClock(a, t, 120.0f, 60, 0, false);
    NEAR(a.project().bpm, 100.0f, 1e-3f);
    halHostSetMicros(0);
    a.setBpm(120.0f);
}

static void testMidiClockOut(App& a)
{
    a.setBpm(120.0f);
    a.stop();
    pump(4);
    CHECK(halHostServiceTimerPeriod() == 0);   // clock out off by default
    halHostMidiSetConnected(true, "CLOCK SINK");
    a.settingsEdit().midi_clock_out = 1;
    a.applySettings();
    CHECK(halHostServiceTimerPeriod() == 1000);
    halHostSetMicros(5000000000ull);   // constant: no extrapolation between blocks
    pump(2);
    halHostServiceTimer();
    CHECK(halHostMidiTakeSent(nullptr, 0) == 0);   // stopped: nothing is sent

    hal::MidiMsg buf[64];
    std::vector<int> tick_block;
    int fa = 0, fb = 0, fc = 0, f2 = 0, other = 0, first = -1;
    a.play();
    const int blocks = 375;   // 2 beats at 120 BPM, 128-frame blocks
    for (int b = 0; b < blocks; ++b) {
        pump(1);
        halHostServiceTimer();
        const int n = halHostMidiTakeSent(buf, 64);
        for (int i = 0; i < n; ++i) {
            if (first < 0) first = buf[i].status;
            switch (buf[i].status) {
            case 0xF8: tick_block.push_back(b); break;
            case 0xFA: ++fa; break;
            default: ++other; break;
            }
        }
    }
    const double beat = a.engine().telemetry().beat.load();
    const double lat_beats = a.latencyMs() / 1000.0 * 2.0;
    const int expect = (int)std::floor((beat - lat_beats) * 24.0) + 1;
    std::printf("clock out: beat %.3f, %d ticks (expected %d), first 0x%02X\n", beat, (int)tick_block.size(), expect,
                first);
    CHECK(first == 0xFA && fa == 1 && other == 0);
    CHECK(std::abs((int)tick_block.size() - expect) <= 1);
    CHECK(tick_block.size() >= 47 && tick_block.size() <= 50);
    // even spacing: 1000 frames per tick = 7.8 blocks
    bool even = true;
    for (size_t i = 1; i < tick_block.size(); ++i) {
        const int d = tick_block[i] - tick_block[i - 1];
        even = even && d >= 7 && d <= 9;
    }
    CHECK(even);
    CHECK(!tick_block.empty() && tick_block[0] <= 3);   // first tick after the output latency

    // Stop -> FC; Continue -> song position + FB, the ticks go on from there
    a.stop();
    pump(2);
    halHostServiceTimer();
    int n = halHostMidiTakeSent(buf, 64);
    CHECK(n == 1 && buf[0].status == 0xFC);
    halHostServiceTimer();
    CHECK(halHostMidiTakeSent(buf, 64) == 0);
    a.togglePlay();   // continue
    pump(1);
    halHostServiceTimer();
    n = halHostMidiTakeSent(buf, 64);
    for (int i = 0; i < n; ++i) {
        if (buf[i].status == 0xF2) {
            ++f2;
            const int spp = buf[i].d1 | (buf[i].d2 << 7);
            CHECK(spp == (int)(beat * 4.0));   // 16ths
        }
        if (buf[i].status == 0xFB) ++fb;
    }
    CHECK(n >= 2 && buf[0].status == 0xF2 && buf[1].status == 0xFB && f2 == 1 && fb == 1);
    int ticks = 0;
    for (int b = 0; b < 100; ++b) {
        pump(1);
        halHostServiceTimer();
        const int k = halHostMidiTakeSent(buf, 64);
        for (int i = 0; i < k; ++i) {
            ticks += buf[i].status == 0xF8;
            fc += buf[i].status == 0xFC;
        }
    }
    CHECK(ticks >= 12 && ticks <= 20 && fc == 0);

    // Play again while playing: Start from the top
    a.play();
    pump(1);
    halHostServiceTimer();
    n = halHostMidiTakeSent(buf, 64);
    bool restarted = false;
    for (int i = 0; i < n; ++i) restarted = restarted || buf[i].status == 0xFA;
    CHECK(restarted);

    // clock out off while playing: the timer stops and receivers get a Stop
    a.settingsEdit().midi_clock_out = 0;
    a.applySettings();
    CHECK(halHostServiceTimerPeriod() == 0);
    n = halHostMidiTakeSent(buf, 64);
    CHECK(n >= 1 && buf[n - 1].status == 0xFC);
    a.stop();
    a.stop();
    pump(2);
    halHostSetMicros(0);
    halHostMidiSetConnected(false);
    for (int i = 0; i < 3000 && a.activeVoices() > 0; ++i) pump();
}

static void testMidiLearn(App& a)
{
    remove(midimapPath().c_str());
    a.midiClearMap();
    CHECK(a.midiMapCount() == 0 && !a.midiLearning());
    a.selectPad(0, false);
    const ParamRef cut = ParamRef::pad(PP_CUTOFF);   // the selected pad
    const ParamRef rev = ParamRef::global(GP_REV_SIZE);
    const ParamRef fader = ParamRef::busLevel(2);

    // a CC without a binding does nothing
    const float c0 = a.paramNorm(cut);
    midi(0xB0, 21, 127);
    pump(1);
    NEAR(a.paramNorm(cut), c0, 1e-6f);

    uint32_t rm = a.revs().midi;
    a.midiLearn(true);
    CHECK(a.midiLearning() && a.revs().midi != rm);
    CHECK(strstr(testhooks::lastToast(), "MIDI LEARN") != nullptr);
    midi(0xB0, 21, 100);   // learn on, no knob moved yet: nothing binds
    pump(1);
    CHECK(a.midiMapCount() == 0);
    a.paramSetNorm(cut, 0.3f);   // knob moved -> armed
    CHECK(strstr(testhooks::lastToast(), padSpec(PP_CUTOFF).label) != nullptr);
    rm = a.revs().midi;
    midi(0xB0, 21, 64);    // binds CC 21 (channel 1) to CUTOFF; the binding message does not move the knob
    pump(1);
    CHECK(a.midiMapCount() == 1 && a.revs().midi != rm);
    CHECK(strstr(testhooks::lastToast(), "CC 21") != nullptr);
    NEAR(a.paramNorm(cut), 0.3f, 0.01f);
    midi(0xB0, 21, 127);
    pump(1);
    NEAR(a.paramNorm(cut), 1.0f, 1e-4f);
    midi(0xB0, 21, 0);
    pump(1);
    NEAR(a.paramNorm(cut), 0.0f, 1e-4f);
    // learn stays on: a second knob, a second controller
    a.paramSetNorm(rev, 0.5f);
    midi(0xB0, 22, 10);
    pump(1);
    CHECK(a.midiMapCount() == 2);
    // a bound CC moving while learn is on (not armed) does not bind anything new
    midi(0xB0, 22, 127);
    pump(1);
    NEAR(a.paramNorm(rev), 1.0f, 1e-4f);
    midi(0xB0, 23, 50);
    pump(1);
    CHECK(a.midiMapCount() == 2);
    // re-learn CC 21 to the fader: CC 21 moves from CUTOFF to the fader (one knob per controller)
    a.paramSetNorm(fader, 0.2f);
    midi(0xB0, 21, 5);
    pump(1);
    CHECK(a.midiMapCount() == 2);
    a.midiLearn(false);
    CHECK(!a.midiLearning());
    const float cut_before = a.paramNorm(cut);
    midi(0xB0, 21, 127);
    pump(1);
    NEAR(a.paramGet(fader), 1.0f, 1e-4f);
    NEAR(a.paramNorm(cut), cut_before, 1e-6f);
    // channel-specific: CC 22 on channel 2 is another controller
    midi(0xB0, 22, 0);
    pump(1);
    NEAR(a.paramNorm(rev), 0.0f, 1e-4f);
    midi(0xB1, 22, 127);
    pump(1);
    NEAR(a.paramNorm(rev), 0.0f, 1e-4f);
    // many values in one frame: the newest wins
    for (int v = 0; v <= 100; v += 10) midi(0xB0, 22, (uint8_t)v);
    pump(1);
    NEAR(a.paramNorm(rev), 100.0f / 127.0f, 1e-4f);
    // a pad knob bound with pad -1 follows the selected pad
    a.midiLearn(true);
    a.paramSetNorm(cut, 0.5f);
    midi(0xB3, 30, 0);   // channel 4
    pump(1);
    a.midiLearn(false);
    CHECK(a.midiMapCount() == 3);
    a.selectPad(7, false);
    const float other = a.project().pad[0].p[PP_CUTOFF];
    midi(0xB3, 30, 127);
    pump(1);
    NEAR(a.paramNorm(ParamRef::pad(PP_CUTOFF, 7)), 1.0f, 1e-4f);
    NEAR(a.project().pad[0].p[PP_CUTOFF], other, 1e-6f);
    a.selectPad(0, false);

    // midimap.bin: magic, count, entries
    {
        FILE* f = fopen(midimapPath().c_str(), "rb");
        CHECK(f != nullptr);
        uint8_t b[8 + 32 * 8] = {};
        const size_t got = f ? fread(b, 1, sizeof b, f) : 0;
        if (f) fclose(f);
        CHECK(got == 8 + 3 * 8);
        CHECK(memcmp(b, "OMM1", 4) == 0 && b[4] == 3);
        bool found30 = false;
        for (int i = 0; i < 3; ++i) {
            const uint8_t* e = b + 8 + 8 * i;
            if (e[0] == 30) found30 = e[1] == 4 && e[2] == (uint8_t)ParamRef::Kind::Pad && e[3] == 0xFF && e[4] == PP_CUTOFF;
        }
        CHECK(found30);
    }
    // table limit: 32 bindings (3 above + 16 cutoffs + resonances)
    a.midiLearn(true);
    for (int i = 0; i < 30; ++i) {
        a.paramSetNorm(ParamRef::pad(PP_CUTOFF, i % 16), 0.5f);
        a.paramSetNorm(ParamRef::pad(i < 16 ? PP_CUTOFF : PP_RESO, i % 16), 0.5f);
        midi(0xB5, (uint8_t)(40 + i), 1);
        pump(1);
    }
    CHECK(a.midiMapCount() == 32);
    CHECK(strstr(testhooks::lastToast(), "pełna") != nullptr);
    a.midiLearn(false);
    a.midiClearMap();
    CHECK(a.midiMapCount() == 0);
    midi(0xB0, 22, 127);
    pump(1);
    NEAR(a.paramNorm(rev), 100.0f / 127.0f, 1e-4f);
    remove(midimapPath().c_str());
}

static void testExitToLauncher(App& a)
{
    CHECK(!a.hasLauncher());
    CHECK(!a.exitToLauncher());
    CHECK(strstr(testhooks::lastToast(), "launchera") != nullptr);
    CHECK(hal::audioRunning());

    // launcher present: stop, save (dirty + autosave), restart into it
    halHostSetLauncher(true);
    a.renameProject(kProj);
    remove(projPath().c_str());
    a.toggleStep(0, 1);
    a.play();
    pump(10);
    CHECK(a.dirty() && a.settings().autosave);
    CHECK(a.exitToLauncher());
    CHECK(halHostLauncherRequested());
    CHECK(a.hasLauncher());
    CHECK(!a.playing());
    CHECK(isFile(projPath()));
    CHECK(!a.dirty());
    CHECK(!hal::audioRunning());
    halHostSetLauncher(false);
    CHECK(hal::audioStart(48000, 128, &App::render, &a.engine()));
    pump(4);

    // ux-4: Auto-save off and unsaved changes: the first BACK TO LAUNCHER only warns, the second leaves.
    a.settingsEdit().autosave = 0;
    a.applySettings();
    halHostSetLauncher(true);
    a.toggleStep(0, 2);
    CHECK(a.dirty());
    CHECK(!a.exitToLauncher());
    CHECK(!halHostLauncherRequested());
    CHECK(strstr(testhooks::lastToast(), "ponownie") != nullptr);
    CHECK(hal::audioRunning());
    CHECK(a.exitToLauncher());
    CHECK(halHostLauncherRequested());
    CHECK(a.dirty());   // left without saving, as confirmed
    halHostSetLauncher(false);
    CHECK(hal::audioStart(48000, 128, &App::render, &a.engine()));
    a.settingsEdit().autosave = 1;
    a.applySettings();
    pump(4);
}

// ================================================================== review fixes
namespace orbit::ui::apphooks {
bool appSampleState(const Sample* p, int* engine, int* preview, int* holds);   // ui/app.cpp
}
static bool owned(const Sample* p, int* eng = nullptr, int* pv = nullptr)
{
    return orbit::ui::apphooks::appSampleState(p, eng, pv, nullptr);
}

static int findUser(App& a, const char* name)
{
    a.setCategory(Category::User);
    a.setSearch(name);
    const int idx = a.resultCount() > 0 ? a.result(0) : -1;
    a.setSearch("");
    a.setCategory(Category::All);
    return idx;
}

template <typename PadT>
static auto hasSliceEnd(const PadT& p, int) -> decltype(p.slice_end, true)
{
    return true;
}
template <typename PadT>
static bool hasSliceEnd(const PadT&, long)
{
    return false;
}

// rt-4 / logic-3: every install of a heap sample is answered exactly once, so it is freed.
static void testSampleOwnership(App& a)
{
    int idx = findUser(a, kSynth);
    int lidx = findUser(a, kLong);
    CHECK(idx >= 0 && lidx >= 0);
    if (idx < 0 || lidx < 0) return;
    a.newProject();
    pump(4);

    // The decoded browser preview onto two pads, and twice onto one: one shared slot, one install.
    a.selectEntry(idx);
    waitLoader();
    const Sample* p = a.previewSample();
    CHECK(p != nullptr);
    CHECK(a.loadFileToPad(idx, 4, 0));
    CHECK(a.loadFileToPad(idx, 5, 0));
    CHECK(a.loadFileToPad(idx, 5, 0));
    waitLoader();
    CHECK(a.padSample(4, 0) == p && a.padSample(5, 0) == p);
    CHECK(a.project().pad[4].layer[0].slot >= 0 && a.project().pad[4].layer[0].slot == a.project().pad[5].layer[0].slot);
    int e = -1, pv = -1;
    CHECK(owned(p, &e) && e == 1);
    // ux-2: CLEAR LAYER can be undone
    a.clearPadLayer(5, 0);
    CHECK(a.padSample(5, 0) == nullptr && a.canUndoPads() && strcmp(a.undoPadsLabel(), "CLEAR LAYER") == 0);
    CHECK(a.undoPads());
    CHECK(a.padSample(5, 0) == p && !a.canUndoPads());
    CHECK(!a.undoPads());
    // Both pads, the preview and the undo let go of it: freed.
    a.clearPadLayer(4, 0);
    a.clearPadLayer(5, 0);
    a.rescan();   // drops the browser preview
    waitLoader();
    pump(10);
    CHECK(owned(p));   // the CLEAR LAYER undo still holds it
    a.newProject();    // drops the pad undo
    pump(20);
    CHECK(!owned(p));

    // The same Sample back on its slot while its retirement is pending (a browser preview still plays it):
    // the install waits for the engine's answer instead of cancelling the retirement unanswered.
    lidx = findUser(a, kLong);
    a.selectEntry(lidx);
    waitLoader();
    const Sample* q = a.previewSample();
    CHECK(q && q->frames == (uint32_t)kLongFrames);
    if (!q) return;
    CHECK(a.loadFileToPad(lidx, 6, 0));
    waitLoader();
    CHECK(a.padSample(6, 0) == q);
    const int slot6 = a.project().pad[6].layer[0].slot;
    a.previewEntry(lidx);   // 8 s
    pump(4);
    a.selectPad(6, false);
    a.selectLayer(0);
    CHECK(a.applyOp(SampleOp::Normalize));
    waitLoader();
    const Sample* out = a.padSample(6, 0);
    CHECK(out && out != q && a.engine().slot(slot6) == out);
    CHECK(owned(q, &e, &pv) && e == 1 && pv == 1);   // retirement pending: the preview plays it
    CHECK(a.applyOp(SampleOp::Undo));
    CHECK(a.padSample(6, 0) == q);
    for (int i = 0; i < 400 && a.engine().slot(slot6) != q; ++i) pump();
    CHECK(a.engine().slot(slot6) == q);
    pump(10);
    CHECK(owned(q, &e, &pv) && e == 1 && pv == 0);
    CHECK(!owned(out));   // the NORMALIZE result was retired and freed
    a.padFlashBits();
    a.padDown(6, 120, Producer::Touch);
    pump(4);
    a.padUp(6, Producer::Touch);
    CHECK(a.padFlashBits() & (1u << 6));
    a.newProject();
    a.rescan();
    waitLoader();
    pump(20);
    CHECK(!owned(q));
    idx = findUser(a, kSynth);
    CHECK(idx >= 0);
}

// logic-1 / logic-2 / logic-13 / logic-6: project files.
// 0.3: NEW PROJECT is completely empty (no steps, no song, nothing shown selected); DELETE removes a saved
// project with its JSON export and its sample folder, and the open project stays open (unsaved).
static void testNewAndDelete(App& a)
{
    a.newProject();
    pump(2);
    const Project& p = a.project();
    int on = 0;
    for (int sc = 0; sc < kScenes; ++sc)
        for (int t = 0; t < kPads; ++t)
            for (int i = 0; i < kMaxSteps; ++i) on += p.scene_data[sc].track[t].step[i].on;
    CHECK(on == 0);
    bool song_empty = !p.song.follow;
    for (int i = 0; i < kSongSlots; ++i) song_empty = song_empty && p.song.slot[i] == -1 && p.song.fx[i] == 0;
    CHECK(song_empty);
    CHECK(p.scene_data[0].swing == 50 && p.scene_data[0].length == 16 && p.scene_data[0].div == Division::D1_16);
    CHECK(a.ui().pad_shown == 0);
    if (builtinKitCount() >= 1) CHECK(a.padSample(0) != nullptr);   // the pads still sound
    a.selectPad(2, false);
    CHECK(a.ui().pad_shown == 1 && a.ui().pad == 2);

    static const char* const kDel = "ZZ_TEST_DELETE";
    const std::string base = g_sd + "/ORBIT/PROJECTS/" + kDel;
    a.renameProject(kDel);
    a.toggleStep(0, 1);
    CHECK(a.saveAs(kDel));
    waitLoader();
    CHECK(isFile(projFile(kDel)) && a.projectHasFile());
    CHECK(writeText(base + ".json", "{}"));
    mkdirOne(base);
    CHECK(writeText(base + "/take.wav", "x"));
    CHECK(a.deleteProject(kDel));
    CHECK(!isFile(projFile(kDel)) && !isFile(base + ".json") && !isFile(base + "/take.wav") && !isDir(base));
    CHECK(!a.projectHasFile() && a.dirty());   // still open, now unsaved
    char names[64][32];
    const int n = a.listProjects(names, 64);
    bool listed = false;
    for (int i = 0; i < n; ++i) listed = listed || strcmp(names[i], kDel) == 0;
    CHECK(!listed);
    CHECK(!a.deleteProject("ZZ_no_such_project"));
    a.newProject();
    pump(2);
}

// 0.3: the ACID BASS line and knobs are saved with the project and come back on open; RANDOM / CLEAR undo.
static void testBassPersist(App& a)
{
    a.newProject();
    pump(2);
    static const char* const kB = "ZZ_TEST_BASS";
    a.renameProject(kB);
    BassStep b;
    b.gate = 1;
    b.note = 7;
    b.accent = 1;
    b.slide = 1;
    a.setBassStep(3, b);
    BassStep rest;
    rest.gate = 0;
    rest.note = -5;   // a resting step keeps its note
    a.setBassStep(9, rest);
    a.paramSet(ParamRef::global(GP_BASS_CUTOFF), 0.8f);
    a.paramSet(ParamRef::global(GP_BASS_WAVE), 1.0f);
    CHECK(a.saveAs(kB));
    waitLoader();
    a.newProject();
    pump(2);
    CHECK(a.bassStep(3).gate == 0);
    CHECK(a.open(kB));
    waitLoader();
    pump(2);
    const BassStep& r3 = a.bassStep(3);
    CHECK(r3.gate == 1 && r3.note == 7 && r3.accent == 1 && r3.slide == 1);
    CHECK(a.bassStep(9).gate == 0 && a.bassStep(9).note == -5);
    CHECK(a.bassStep(4).gate == 0);
    CHECK(std::fabs(a.paramGet(ParamRef::global(GP_BASS_CUTOFF)) - 0.8f) < 1e-3f);
    CHECK(a.paramGet(ParamRef::global(GP_BASS_WAVE)) >= 0.5f);
    // RANDOM and CLEAR are undoable (the scene snapshot holds the bass line)
    a.bassRandom();
    int gates = 0;
    for (int i = 0; i < 16; ++i) gates += a.bassStep(i).gate;
    CHECK(gates >= 4);
    a.undoPattern();
    CHECK(a.bassStep(3).note == 7 && a.bassStep(3).gate == 1);
    a.bassClear();
    gates = 0;
    for (int i = 0; i < 16; ++i) gates += a.bassStep(i).gate;
    CHECK(gates == 0);
    a.undoPattern();
    CHECK(a.bassStep(3).gate == 1);
    CHECK(a.deleteProject(kB));
    a.newProject();
    pump(2);
}

static void testProjectFiles(App& a, bool have_synth)
{
    // SAVE over a .orb this session did not open or save asks first; its own file afterwards does not.
    remove(projFile(kOvr).c_str());
    CHECK(writeText(projFile(kOvr), "{\"name\": \"SOMEONE ELSE\"}"));
    a.renameProject(kOvr);
    CHECK(strcmp(a.project().name, kOvr) == 0 && !a.projectHasFile());
    CHECK(!a.save());
    CHECK(strstr(testhooks::lastToast(), "istnieje") != nullptr);
    CHECK(a.save());   // the second SAVE within 3 s overwrites
    waitLoader();
    CHECK(a.projectHasFile() && !a.dirty());
    a.toggleStep(0, 3);
    CHECK(a.save());
    waitLoader();
    CHECK(!a.dirty());
    remove(projFile(kOvr).c_str());

    // NEW starts under the first ORBIT_NN without a file on the card.
    const std::string o1 = projFile("ORBIT_01");
    const bool had1 = isFile(o1);
    if (!had1) CHECK(writeText(o1, "{}"));
    a.newProject();
    CHECK(strncmp(a.project().name, "ORBIT_", 6) == 0 && strcmp(a.project().name, "ORBIT_01") != 0);
    CHECK(!isFile(projFile(a.project().name)) && !a.projectHasFile());
    if (!had1) remove(o1.c_str());

    // A project copied on a PC opens (and saves) under its own file name, not the "name" inside it.
    CHECK(isFile(projPath()));
    if (isFile(projPath())) {
        CHECK(copyFile(projPath(), projFile(kCopy)));
        CHECK(a.open(kCopy));
        waitLoader();
        CHECK(strcmp(a.project().name, kCopy) == 0 && a.projectHasFile());
        // Opening while playing rewinds: PLAY starts the new song at its top, not at the old position.
        a.play();
        pump(400);
        CHECK(a.playStep() >= 3);
        CHECK(a.open(kCopy));
        waitLoader();
        pump(2);
        CHECK(!a.playing());
        a.togglePlay();   // Continue
        pump(2);
        CHECK(a.playStep() >= 0 && a.playStep() <= 1);
        a.stop();
        a.stop();
        pump(2);
        remove(projFile(kCopy).c_str());
    }

    // An edited sample that cannot be written (a file where its folder goes): partial save, still dirty,
    // and reopening reports the missing sample.
    const int idx = have_synth ? findUser(a, kSynth) : -1;
    if (idx >= 0) {
        const std::string blocker = g_sd + "/ORBIT/PROJECTS/" + kPart;
        removeTree(blocker);
        remove(blocker.c_str());
        remove(projFile(kPart).c_str());
        CHECK(writeText(blocker, "not a folder"));
        a.newProject();
        CHECK(a.loadFileToPad(idx, 7, 0));
        waitLoader();
        a.selectPad(7, false);
        a.selectLayer(0);
        CHECK(a.applyOp(SampleOp::Normalize));
        waitLoader();
        CHECK(a.saveAs(kPart));
        waitLoader();
        CHECK(strstr(testhooks::lastToast(), "częściowy") != nullptr);
        CHECK(a.dirty());
        CHECK(isFile(projFile(kPart)));
        CHECK(a.open(kPart));
        waitLoader();
        CHECK(strstr(testhooks::lastToast(), "brak sampli: 1") != nullptr);
        remove(projFile(kPart).c_str());
        remove(blocker.c_str());
        a.newProject();
        pump(4);
    }
}

// logic-9 / 10 / 11 / 12 / 14 / 17, ux-2 (TO PADS, kits), ux-12.
static void testEditFixes(App& a, bool have_synth)
{
    a.newProject();
    pump(4);

    // A shorter scene does not keep a bar it does not have.
    a.selectScene(1);
    a.setSceneLength(16);
    a.selectScene(0);
    a.setSceneLength(64);
    a.setSeqPage(3);
    CHECK(a.ui().seq_page == 3);
    a.selectScene(1);
    CHECK(a.ui().seq_page == 0);
    a.selectScene(0);
    a.setSceneLength(16);

    // TRIM through a slice keeps slices[0] = 0 (the cut slice's tail is the new first slice).
    a.selectPad(0, false);
    a.selectLayer(0);
    Pad& p0 = a.project().pad[0];
    a.sliceGrid(4);
    CHECK(p0.slice_count == 4);
    a.paramSet(ParamRef::pad(PP_START), 0.3f);
    a.paramSet(ParamRef::pad(PP_END), 0.8f);
    CHECK(a.applyOp(SampleOp::Trim));
    waitLoader();
    CHECK(p0.slice_count == 3);
    NEAR(p0.slices[0], 0.0f, 1e-6f);
    NEAR(p0.slices[1], 0.4f, 1e-3f);
    NEAR(p0.slices[2], 0.9f, 1e-3f);
    CHECK(a.applyOp(SampleOp::Undo));
    pump(3);
    CHECK(p0.slice_count == 4);

    // GRID 16 on a region: the 16th slice ends at the region END (needs Pad::slice_end in project.h).
    a.paramSet(ParamRef::pad(PP_START), 0.2f);
    a.paramSet(ParamRef::pad(PP_END), 0.6f);
    a.sliceGrid(16);
    CHECK(p0.slice_count == 16);
    a.sliceSelect(15);
    if (hasSliceEnd(p0, 0)) NEAR(p0.p[PP_END], 0.6f, 1e-4f);
    else std::printf("note: Pad has no slice_end yet, the 16th slice still ends at 1.0\n");
    a.paramReset(ParamRef::pad(PP_START));
    a.paramReset(ParamRef::pad(PP_END));
    p0.slice_count = 0;

    // TO PADS and a built-in kit can be undone (ux-2).
    char names[kPads][kNameLen];
    for (int i = 0; i < kPads; ++i) snprintf(names[i], kNameLen, "%s", a.project().pad[i].name);
    const Sample* s1 = a.padSample(1, 0);
    a.sliceGrid(4);
    a.slicesToPads();
    CHECK(a.padSample(1, 0) == a.padSample(0, 0) && a.padSample(3, 0) == a.padSample(0, 0));
    CHECK(a.canUndoPads() && strcmp(a.undoPadsLabel(), "TO PADS") == 0);
    CHECK(a.undoPads());
    CHECK(a.padSample(1, 0) == s1);
    for (int i = 0; i < 4; ++i) CHECK(strcmp(a.project().pad[i].name, names[i]) == 0);
    CHECK(p0.slice_count == 4);   // the source pad's slices are back too
    if (builtinKitCount() >= 2) {
        a.loadBuiltinKit(0);
        CHECK(strcmp(a.padSampleName(0), builtinKit(0).sounds[0].name) == 0);
        CHECK(a.canUndoPads());
        CHECK(a.undoPads());
        CHECK(strcmp(a.padSampleName(0), builtinKit(1).sounds[0].name) == 0);
    }
    p0.slice_count = 0;

    // FADE with a region (and REVERSE) prints the fade where the voice applied it.
    const int idx = have_synth ? findUser(a, kSynth) : -1;
    if (idx >= 0) {
        CHECK(a.loadFileToPad(idx, 12, 0));
        waitLoader();
        a.selectPad(12, false);
        a.selectLayer(0);
        const Sample* s0 = a.padSample(12, 0);
        CHECK(s0 && s0->frames == (uint32_t)kSynthFrames);
        if (s0 && s0->frames == (uint32_t)kSynthFrames) {
            const std::vector<int16_t> orig(s0->data, s0->data + s0->frames);
            const int n = kSynthFrames, ra = (int)(0.25f * (float)n), rb = (int)(0.55f * (float)n), fo = 960;   // 20 ms
            auto energy = [](const int16_t* d, int from, int to) {
                double e = 0.0;
                for (int i = from; i < to; ++i) e += std::fabs((double)d[i]);
                return e;
            };
            // f follows orig * gain(i) over [from, to) (+-1 rounding) and something changed
            auto ramped = [](const int16_t* f, const int16_t* o, int from, int to, auto gain) {
                bool ok = true;
                int changed = 0;
                for (int i = from; i < to; ++i) {
                    ok = ok && std::fabs((float)f[i] - (float)o[i] * gain(i)) <= 1.01f;
                    changed += f[i] != o[i] ? 1 : 0;
                }
                return ok && changed > 0;
            };
            a.paramSet(ParamRef::pad(PP_START), 0.25f);
            a.paramSet(ParamRef::pad(PP_END), 0.55f);
            a.paramSet(ParamRef::pad(PP_FADE_IN), 0.0f);
            a.paramSet(ParamRef::pad(PP_FADE_OUT), 20.0f);
            CHECK(a.applyOp(SampleOp::FadeInOut));
            waitLoader();
            const Sample* f = a.padSample(12, 0);
            CHECK(f && f != s0 && f->frames == (uint32_t)n);
            if (f && f->frames == (uint32_t)n) {
                // fade-out: linear to 0 at the region END, as the voice applied it
                CHECK(ramped(f->data, orig.data(), rb - fo, rb, [&](int i) { return (float)(rb - i) / (float)fo; }));
                CHECK(energy(f->data, ra, ra + fo) == energy(orig.data(), ra, ra + fo));   // no fade-in set
                CHECK(memcmp(f->data + rb, orig.data() + rb, sizeof(int16_t) * (size_t)(n - rb)) == 0);   // outside
                NEAR(a.project().pad[12].p[PP_FADE_OUT], 0.0f, 1e-6f);
            }
            CHECK(a.applyOp(SampleOp::Undo));
            pump(3);
            a.setReverse(true);   // play from END backwards: the fade-in is at the region end
            a.paramSet(ParamRef::pad(PP_FADE_IN), 20.0f);
            a.paramSet(ParamRef::pad(PP_FADE_OUT), 0.0f);
            CHECK(a.applyOp(SampleOp::FadeInOut));
            waitLoader();
            f = a.padSample(12, 0);
            if (f && f->frames == (uint32_t)n) {
                // fade-in in play order: from 0 at the region END (where a reversed voice starts)
                CHECK(ramped(f->data, orig.data(), rb - fo, rb, [&](int i) { return (float)(rb - 1 - i) / (float)fo; }));
                CHECK(energy(f->data, ra, ra + fo) == energy(orig.data(), ra, ra + fo));
                CHECK(f->data[rb - 1] == 0);   // the first frame played starts at gain 0
                NEAR(a.project().pad[12].p[PP_FADE_IN], 0.0f, 1e-6f);
            }
            a.setReverse(false);
        }
    }

    // REC stays off in the engine after a sample-rate change (the UI shows it off).
    a.toggleRecord();
    CHECK(a.recording());
    a.settingsEdit().sample_rate = 44100;
    a.applySettings();
    pump(4);
    CHECK(!a.recording() && a.engine().telemetry().recording.load() == 0);
    a.play();
    pump(10);
    CHECK(!a.recording());
    a.stop();
    a.stop();
    a.settingsEdit().sample_rate = 48000;
    a.applySettings();
    pump(4);

    // Auditions (waveform taps) while recording are not written into the pattern; pad hits are.
    a.selectScene(0);
    a.clearTrack(3);
    a.selectPad(3, false);
    a.play();
    pump(20);
    a.toggleRecord();
    pump(2);
    for (int k = 0; k < 4; ++k) {
        a.previewPad();
        pump(30);
    }
    const Scene& sc = a.project().scene_data[0];
    CHECK(trackHits(sc, 3, kMaxSteps) == 0);
    sleepMs(250);
    a.padDown(3, 100, Producer::Touch);
    pump(10);
    a.padUp(3, Producer::Touch);
    CHECK(trackHits(sc, 3, kMaxSteps) >= 1);
    a.toggleRecord();
    a.stop();
    a.stop();
    pump(4);

    // MIDI learn switches itself off after a minute without a knob move or a binding (ux-12).
    const uint64_t t = 5000000000ull;
    halHostSetMicros(t);
    a.midiLearn(true);
    halHostSetMicros(t + 30000000ull);
    pump(1);
    CHECK(a.midiLearning());
    a.paramSetNorm(ParamRef::pad(PP_CUTOFF), 0.4f);   // activity: the minute starts again
    halHostSetMicros(t + 80000000ull);
    pump(1);
    CHECK(a.midiLearning());
    halHostSetMicros(t + 150000000ull);
    pump(1);
    CHECK(!a.midiLearning());
    CHECK(strstr(testhooks::lastToast(), "aktywno") != nullptr);
    halHostSetMicros(0);
    a.newProject();
    pump(4);
}

// Child processes (the App is a process-wide singleton, init() runs once):
//   --btboot: Settings.bt_enabled at boot -> the bridge starts on a tick once the audio stream runs, never in init()
//             and never while app_main's audio start may still run; BACK TO LAUNCHER then puts the Pico to sleep
//   --btfail: Settings.bt_enabled at boot and the boot audio start failed -> no start by a timeout, only once
//             app_main reports the start as over (audioBootDone); the engine moves to 44.1 kHz without a stream
//   --btzero: a saved bt_enabled 0, bt_local 0, bt_volume 0 is a real choice and stays (no "old file" guess)
static int runBtChild(const char* mode)
{
    const bool boot = !strcmp(mode, "--btboot"), fail = !strcmp(mode, "--btfail");
    const char* path = "host/out/test_app_btchild.bin";
    halHostSetSettingsPath(path);
    Settings s;
    settingsDefaults(s);
    s.bt_enabled = boot || fail ? 1 : 0;
    s.bt_local = 0;
    s.bt_volume = boot || fail ? 33 : 0;
    CHECK(hal::settingsSave(s));
    CHECK(hal::boardInit());
    App& a = app();
    CHECK(a.init(nullptr, 48000));
    hal::BtStatus st;
    hal::btStatus(st);
    CHECK(!st.enabled && a.engine().sampleRate() == 48000);   // init() never starts the bridge
    if (boot) {
        CHECK(a.btEnabled() && a.settings().bt_volume == 33);
        a.tick();   // the UI is up, the audio stream not yet (app_main starts it after the UI): still off
        hal::btStatus(st);
        CHECK(!st.enabled && a.engine().sampleRate() == 48000);
        CHECK(hal::audioStart(48000, 128, &App::render, &a.engine()));
        a.tick();
        hal::btStatus(st);
        CHECK(st.enabled && a.engine().sampleRate() == 44100 && halHostAudioRate() == 44100);
        CHECK(!containsCi(testhooks::lastToast(), "Bluetooth"));   // quiet at boot
        a.audioBootDone();   // app_main after its audioStart(): nothing more happens
        a.tick();
        a.btSetEnabled(false);
        CHECK(a.engine().sampleRate() == 48000 && halHostAudioRate() == 48000);
        // BACK TO LAUNCHER with Bluetooth on and a stream to the Pico: STOP + SLEEP go out before the restart.
        a.btSetEnabled(true);
        halHostBtAdvance(800);
        halHostBtFeed("READY version=0.9.5 cap_airpods=1");
        halHostBtFeed("STATE connection=2 audio=0");
        halHostBtAdvance(100);
        std::string sent = btSent();
        CHECK(sentHas(sent, "STREAM 1"));
        halHostSetLauncher(true);
        CHECK(a.exitToLauncher() && halHostLauncherRequested());
        sent = btSent();
        hal::btStatus(st);
        CHECK(sentHas(sent, "STOP") && sentHas(sent, "SLEEP") && !st.enabled && !hal::audioRunning());
        halHostSetLauncher(false);
    } else if (fail) {
        a.tick();
        halHostSetMicros(hal::micros() + 10000000ull);   // 10 s later the stream still did not start
        a.tick();
        hal::btStatus(st);
        CHECK(!st.enabled && a.engine().sampleRate() == 48000);   // no start while app_main may still be at it
        halHostSetMicros(0);
        a.audioBootDone();   // app_main: audioStart() returned false
        a.tick();
        hal::btStatus(st);
        CHECK(st.enabled && a.engine().sampleRate() == 44100 && !hal::audioRunning() && halHostAudioRate() == 0);
        a.btSetEnabled(false);
        CHECK(a.engine().sampleRate() == 48000 && !hal::audioRunning());
    } else {
        a.tick();
        CHECK(!a.btEnabled() && a.settings().bt_local == 0 && a.settings().bt_volume == 0);
    }
    remove(path);
    std::printf("test_app %s: %d checks, %d failed - %s\n", mode, checks, fails, fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}

int main(int argc, char** argv)
{
    mkdirOne("host");
    mkdirOne("host/out");
    if (argc > 1 && (!strcmp(argv[1], "--btboot") || !strcmp(argv[1], "--btfail") || !strcmp(argv[1], "--btzero")))
        return runBtChild(argv[1]);
    for (const char* mode : {"--btboot", "--btfail", "--btzero"}) {
        std::string cmd = "\"" + std::string(argv[0]) + "\" " + mode;
        for (char& ch : cmd)
            if (ch == '/') ch = '\\';
        std::fflush(stdout);
        const int rc = std::system(cmd.c_str());
        CHECK(rc == 0);
    }
    halHostSetSettingsPath("host/out/test_app_settings.bin");
    remove(halHostSettingsPath());
    CHECK(hal::boardInit());
    CHECK(hal::sdMount());
    g_sd = hal::sdRoot();
    std::printf("sdcard: %s\n", g_sd.c_str());
    testHalHost();
    cleanupFiles();
    const bool have_synth = writeSynth();
    CHECK(have_synth);
    CHECK(writeLongSynth());
    // logic-1: a project saved as ORBIT_01 in an earlier session; this boot must start under another name.
    const std::string orbit01 = projFile("ORBIT_01");
    const bool had_orbit01 = isFile(orbit01);
    if (!had_orbit01) CHECK(writeText(orbit01, "{}"));

    App& a = app();
    CHECK(&a == &app());
    CHECK(a.init(g_sd.c_str(), 48000));
    CHECK(strncmp(a.project().name, "ORBIT_", 6) == 0 && strcmp(a.project().name, "ORBIT_01") != 0);
    CHECK(!a.projectHasFile());
    if (!had_orbit01) remove(orbit01.c_str());
    CHECK(a.engine().sampleRate() == 48000);
    CHECK(hal::audioStart(48000, 128, &App::render, &a.engine()));
    pump(4);
    CHECK(testhooks::toastCount() >= 0);
    waitLoader();   // initial library scan

    testInitAndKits(a);
    testPads(a);
    testParams(a);
    testTransportAndTap(a);
    testPattern(a);
    testSamples(a, have_synth);
    testSaveOpen(a, have_synth);
    testSampleOwnership(a);
    testProjectFiles(a, have_synth);
    testNewAndDelete(a);
    testBassPersist(a);
    testEditFixes(a, have_synth);
    testRenameQuantUndo(a);
    testSaveSampleWav(a, have_synth);
    testSettings(a);
    testBluetooth(a);
    testRecordAndAudio(a);
    testMidiNotes(a);
    testMidiTransport(a);
    testMidiClockIn(a);
    testMidiClockOut(a);
    testMidiLearn(a);
    testExitToLauncher(a);

    hal::audioStop();
    cleanupFiles();
    for (const std::string& f : g_user_files) remove(f.c_str());
    remove(midimapPath().c_str());
    remove(halHostSettingsPath());
    std::printf("test_app: %d checks, %d failed - %s\n", checks, fails, fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
