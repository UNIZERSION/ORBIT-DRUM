// Build + run (Git Bash, project root; kits_*.o come from tools/run_host_tests.sh, or use
// "-x c++ firmware/main/core/kits_808.c firmware/main/core/kits_909.c" instead of the two .o):
// export ZIG_GLOBAL_CACHE_DIR=$PWD/host/.zig-cache/global ZIG_LOCAL_CACHE_DIR=$PWD/host/.zig-cache/local; C=firmware/main/core; zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -Wall -Wextra -Ifirmware/main -Ihost host/tests/render_scenes.cpp $C/engine.cpp $C/voice.cpp $C/fx.cpp $C/seq.cpp $C/kits.cpp $C/params.cpp $C/project.cpp $C/sample.cpp host/platform_host.cpp host/bin/kits_808.o host/bin/kits_909.o -o host/bin/render_scenes.exe && host/bin/render_scenes.exe
//
// Integration render: real Engine + real Sequencer + built-in "909 STYLE" kit, default project
// (projectDefaults), loaded the way App::loadBuiltinKit does it. Each scene A..D plays 4 bars,
// then Stop and a 1.5 s tail, into host/out/scene_A..D.wav (48 kHz, 16-bit stereo).
// Checks: no NaN/Inf, no sample at or above full scale (float output, before int16 conversion),
// peak <= -0.3 dBFS limiter ceiling (+tolerance), audible level, and that every pad hit of the
// pattern was played (PadHit events vs the steps, ratchets and flams of the scene). Each scene
// is rendered a second time with Project::limiter_on = 0 to report the mix's own headroom.
#include <cmath>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "core/engine.h"
#include "core/kits.h"
#include "core/params.h"
#include "core/project.h"
#include "core/sample.h"

using namespace orbit;

static int g_checks = 0, g_fails = 0;
#define CHECK(c, ...)                                                   \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(c)) {                                                     \
            ++g_fails;                                                  \
            printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c);          \
            printf(__VA_ARGS__);                                        \
            printf("\n");                                               \
        }                                                               \
    } while (0)

static void put16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }
static void put32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }

static bool writeWav(const char* path, const std::vector<float>& l, const std::vector<float>& r, int rate)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const uint32_t frames = (uint32_t)l.size(), bytes = frames * 4;
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 2);
    put32(f, (uint32_t)rate);
    put32(f, (uint32_t)rate * 4);
    put16(f, 4);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, bytes);
    for (uint32_t i = 0; i < frames; ++i) {
        for (float x : {l[i], r[i]}) {
            float y = x * 32767.0f;
            y = y > 32767.0f ? 32767.0f : (y < -32768.0f ? -32768.0f : y);
            put16(f, (uint16_t)(int16_t)lrintf(y));
        }
    }
    return fclose(f) == 0;
}

static double dbfs(double x) { return x > 1e-12 ? 20.0 * log10(x) : -240.0; }

// Expected PadHit count per pass of scene s, pad t (only exact for prob 100 / Always steps).
static int expectedHits(const Scene& sc, int t, bool* exact)
{
    int n = 0;
    *exact = true;
    const int len = trackLength(sc, t);
    for (int i = 0; i < len; ++i) {
        const Step& st = sc.track[t].step[i];
        if (!st.on) continue;
        if (st.prob < 100 || st.cond != TrigCond::Always) *exact = false;
        n += st.ratchet < 1 ? 1 : st.ratchet;
        if (st.flam) ++n;
    }
    return n;
}

namespace {

constexpr int kRate = 48000, kBlock = 128, kBars = 4;

struct Render {
    std::vector<float> l, r;
    int play_frames = 0;
    int hits[kPads] = {};
    int nonfinite = 0, overs = 0, scene_at_end = -1;
    float max_gr = 0.0f;
    double peak[2] = {0, 0}, rms = 0.0;
};

// Plays scene s of proj for kBars bars from a fresh engine, then Stop and a 1.5 s tail.
void renderScene(const Project& proj, Sample* const* smp, int s, Render& out)
{
    const Scene& sc = proj.scene_data[s];
    Engine eng;
    CHECK(eng.init(kRate, &proj), "init");
    for (int i = 0; i < kPads; ++i) {
        Command c;
        c.type = CmdType::SetSample;
        c.value = i;
        c.sample = smp[i];
        CHECK(eng.post(Producer::Loader, c), "post SetSample %d", i);
    }
    float L[kBlock], R[kBlock];
    eng.process(L, R, kBlock);   // installs the slots
    Command q;
    q.type = CmdType::QueueScene;   // stopped: switches at once, so PLAY starts this scene
    q.pad = (uint8_t)s;
    CHECK(eng.post(Producer::Ui, q), "QueueScene");
    Command play;
    play.type = CmdType::Play;
    CHECK(eng.post(Producer::Ui, play), "Play");

    const double step_frames = 60.0 * kRate / proj.bpm * divisionBeats(sc.div);
    out = Render{};
    out.play_frames = (int)lround(step_frames * sc.length * kBars);
    const int total = out.play_frames + kRate * 3 / 2;
    out.l.reserve((size_t)(total + kBlock));
    out.r.reserve((size_t)(total + kBlock));
    bool stopped = false;
    for (int produced = 0; produced < total; produced += kBlock) {
        if (!stopped && produced >= out.play_frames) {
            Command st;
            st.type = CmdType::Stop;
            CHECK(eng.post(Producer::Ui, st), "Stop");
            stopped = true;
        }
        eng.process(L, R, kBlock);
        for (int i = 0; i < kBlock; ++i) {
            if (!std::isfinite(L[i]) || !std::isfinite(R[i])) ++out.nonfinite;
            if (fabsf(L[i]) >= 1.0f || fabsf(R[i]) >= 1.0f) ++out.overs;
            out.l.push_back(L[i]);
            out.r.push_back(R[i]);
        }
        const float gr = eng.telemetry().gain_reduction.load(std::memory_order_relaxed);
        if (gr > out.max_gr) out.max_gr = gr;
        Event e;
        while (eng.poll(e))
            if (e.type == EvtType::PadHit && e.pad < kPads) ++out.hits[e.pad];
        if (!stopped) out.scene_at_end = eng.telemetry().scene.load(std::memory_order_relaxed);
    }
    double sum = 0.0;
    for (size_t i = 0; i < out.l.size(); ++i) {
        out.peak[0] = fmax(out.peak[0], fabs((double)out.l[i]));
        out.peak[1] = fmax(out.peak[1], fabs((double)out.r[i]));
        if ((int)i < out.play_frames) sum += (double)out.l[i] * out.l[i] + (double)out.r[i] * out.r[i];
    }
    out.rms = sqrt(sum / (2.0 * out.play_frames));
}

}  // namespace

int main()
{
    static Project proj;
    projectDefaults(proj);

    // The App loads kit 1 ("909 STYLE") at start (App::init -> loadBuiltinKit(1)); same steps here.
    int kit = -1;
    for (int i = 0; i < builtinKitCount(); ++i)
        if (strstr(builtinKit(i).name, "909")) kit = i;
    CHECK(kit >= 0, "no 909 STYLE kit");
    if (kit < 0) {
        printf("render_scenes: FAILED (no kit)\n");
        return 1;
    }
    const BuiltinKit& k = builtinKit(kit);
    Sample* smp[kPads] = {};
    for (int i = 0; i < k.count && i < kPads; ++i) {
        const BuiltinSound& snd = k.sounds[i];
        smp[i] = sampleWrapEmbedded(builtinPcm(kit, i), snd.frames, 1, k.rate, snd.name);   // 0.4.9: packed kits
        Pad& p = proj.pad[i];
        p.layer[0].slot = (int16_t)i;
        for (int l = 1; l < kLayers; ++l) p.layer[l] = Layer{};
        snprintf(p.name, kNameLen, "%s", snd.name);
        p.choke = snd.choke;
        p.bus = snd.bus < kBuses ? snd.bus : 0;
        p.p[PP_START] = 0.0f;
        p.p[PP_END] = 1.0f;
        p.slice_count = 0;
    }

    printf("909 STYLE, default project, %.0f BPM, %d bars per scene + 1.5 s tail, block %d\n", (double)proj.bpm, kBars,
           kBlock);
    printf("scene  peakL dBFS  peakR dBFS  RMS dBFS  maxGR dB  >=FS  hits | limiter off: peak dBFS  >=FS\n");
    Render r, raw;
    for (int s = 0; s < kScenes; ++s) {
        const Scene& sc = proj.scene_data[s];
        proj.limiter_on = 1;
        renderScene(proj, smp, s, r);
        proj.limiter_on = 0;   // same render without the limiter: the mix's own headroom (report only)
        renderScene(proj, smp, s, raw);
        proj.limiter_on = 1;

        int total_hits = 0;
        for (int h : r.hits) total_hits += h;
        char path[64];
        snprintf(path, sizeof path, "host/out/scene_%c.wav", 'A' + s);
        const bool wrote = writeWav(path, r.l, r.r, kRate);
        printf("  %c    %9.2f   %9.2f   %7.2f   %7.2f  %4d  %4d |              %9.2f  %4d\n", 'A' + s, dbfs(r.peak[0]),
               dbfs(r.peak[1]), dbfs(r.rms), (double)r.max_gr, r.overs, total_hits,
               dbfs(fmax(raw.peak[0], raw.peak[1])), raw.overs);

        printf("       hits per pad:");
        for (int t = 0; t < kPads; ++t)
            if (r.hits[t]) printf(" %s=%d", proj.pad[t].name, r.hits[t]);
        printf("\n");
        CHECK(wrote, "write %s", path);
        CHECK(r.scene_at_end == s, "scene %c: telemetry scene %d", 'A' + s, r.scene_at_end);
        CHECK(r.nonfinite == 0 && raw.nonfinite == 0, "scene %c: non-finite samples", 'A' + s);
        CHECK(r.overs == 0, "scene %c: %d samples at/above full scale", 'A' + s, r.overs);
        CHECK(r.peak[0] <= 0.9661 + 1e-3 && r.peak[1] <= 0.9661 + 1e-3,
              "scene %c: peak above the -0.3 dBFS ceiling (%.4f %.4f)", 'A' + s, r.peak[0], r.peak[1]);
        CHECK(dbfs(r.rms) > -40.0 && dbfs(r.rms) < -3.0, "scene %c: RMS %.1f dBFS out of range", 'A' + s, dbfs(r.rms));
        for (int t = 0; t < kPads; ++t) {
            bool exact = false;
            const int len = trackLength(sc, t);
            const int per = expectedHits(sc, t, &exact);
            // Whole passes of this track in the played frames (polymeter tracks loop on their own).
            const int passes = (sc.length * kBars) / len;
            const int got = r.hits[t];
            CHECK(got == raw.hits[t] || !exact, "scene %c pad %d: %d vs %d hits between renders", 'A' + s, t, got,
                  raw.hits[t]);
            if (per == 0) {
                CHECK(got == 0, "scene %c pad %d: %d unexpected hits", 'A' + s, t, got);
            } else if (exact) {
                CHECK(got >= per * passes && got <= per * (passes + 1), "scene %c pad %d: %d hits, expected %d", 'A' + s,
                      t, got, per * passes);
            } else {
                CHECK(got <= per * (passes + 1), "scene %c pad %d: %d hits > %d", 'A' + s, t, got, per * (passes + 1));
            }
        }
    }
    for (Sample*& p : smp) {
        sampleFree(p);
        p = nullptr;
    }
    if (g_fails) {
        printf("render_scenes: %d of %d checks FAILED\n", g_fails, g_checks);
        return 1;
    }
    printf("render_scenes: all %d checks passed (host/out/scene_A..D.wav)\n", g_checks);
    return 0;
}
