// Build + run (Git Bash, project root; or: bash tools/run_host_tests.sh core_basics):
// export ZIG_GLOBAL_CACHE_DIR=$PWD/host/.zig-cache/global ZIG_LOCAL_CACHE_DIR=$PWD/host/.zig-cache/local; zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -Wall -Wextra -Ifirmware/main -Ihost host/tests/test_core_basics.cpp firmware/main/core/params.cpp firmware/main/core/project.cpp firmware/main/core/sample.cpp firmware/main/core/settings.cpp host/platform_host.cpp -o host/bin/test_core_basics.exe && host/bin/test_core_basics.exe
// Smoke test of the shared core: parameter mapping/format and project defaults.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include "core/dsp/fastmath.h"
#include "core/params.h"
#include "core/project.h"
#include "core/sample.h"

using namespace orbit;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

// projectDefaults() on the LIVE project (App::newProject) while the audio task reads it: a layer
// slot must go straight from its old value to -1, never through 0 (slot 0 = the old kit's kick).
static void testDefaultsOnLiveProject()
{
    static Project garbage, zero;
    std::memset(static_cast<void*>(&garbage), 0xA5, sizeof garbage);
    std::memset(static_cast<void*>(&zero), 0, sizeof zero);
    projectDefaults(garbage);
    projectDefaults(zero);
    CHECK(std::memcmp(&garbage, &zero, sizeof(Project)) == 0);   // every byte rewritten (padding too)
    bool all_empty = true;
    for (const Pad& pd : garbage.pad)
        for (const Layer& L : pd.layer) all_empty = all_empty && L.slot == -1 && L.vel_hi == 127 && L.vel_lo == 0;
    CHECK(all_empty);

    static Project live;
    projectDefaults(live);
    std::atomic<bool> stop{false};
    std::atomic<long> zeros{0}, reads{0};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            for (const Pad& pd : live.pad)
                for (const Layer& L : pd.layer)
                    if (__atomic_load_n(&L.slot, __ATOMIC_RELAXED) == 0) zeros.fetch_add(1, std::memory_order_relaxed);
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });
    for (int k = 0; k < 200000 && (k < 3000 || reads.load() < 200); ++k) {
        for (Pad& pd : live.pad)
            for (Layer& L : pd.layer) __atomic_store_n(&L.slot, (int16_t)(7 + (k & 3)), __ATOMIC_RELAXED);
        projectDefaults(live);
    }
    stop.store(true);
    reader.join();
    CHECK(zeros.load() == 0);
    std::printf("live projectDefaults: %ld concurrent scans, %ld zero slots seen\n", reads.load(), zeros.load());
}

// sampleAnalyze(): the single integer pass must give the same overview, peak and RMS as the
// straightforward double-precision reference.
static void testAnalyze()
{
    for (int ch = 1; ch <= 2; ++ch)
        for (uint32_t frames : {1u, 7u, 1000u, 1023u, 1024u, 1025u, 4096u, 48000u}) {
            Sample* s = sampleCreate(frames, ch, 48000);
            CHECK(s);
            if (!s) continue;
            const uint32_t total = frames * (uint32_t)ch;
            uint32_t r = 0x1234567u + total;
            for (uint32_t i = 0; i < total; ++i) {
                r ^= r << 13;
                r ^= r >> 17;
                r ^= r << 5;
                int v = (int)(r % 65536u) - 32768;
                if (i == total / 2) v = -32768;   // extreme value
                s->data[i] = (int16_t)v;
            }
            sampleAnalyze(s);
            int peak = 0;
            double sum = 0.0;
            for (uint32_t i = 0; i < total; ++i) {
                const int v = s->data[i];
                peak = std::max(peak, v < 0 ? -v : v);
                sum += (double)v * v;
            }
            const double rms = std::sqrt(sum / (double)total) / 32768.0;
            CHECK(s->peak == (float)peak / 32768.0f);
            CHECK(std::fabs(s->rms - rms) < 1e-5 * (rms + 1e-3));
            bool bins = true;
            for (int b = 0; b < kPeakBins; ++b) {
                const uint32_t a = (uint32_t)((uint64_t)frames * b / kPeakBins);
                uint32_t e = (uint32_t)((uint64_t)frames * (b + 1) / kPeakBins);
                if (e <= a) e = a + 1;
                if (e > frames) e = frames;
                int lo = 32767, hi = -32768;
                for (uint32_t i = a; i < e; ++i) {
                    int v = s->data[i * ch];
                    if (ch == 2) v = (v + s->data[i * ch + 1]) / 2;
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
                if (lo > hi) lo = hi = 0;
                bins = bins && s->pk_min[b] == (int8_t)(lo / 258) && s->pk_max[b] == (int8_t)(hi / 258);
            }
            CHECK(bins);
            sampleFree(s);
        }
}

int main()
{
    char b[32];
    for (int i = 0; i < kPadParamCount; ++i) {
        const ParamSpec& s = padSpec((PadParam)i);
        CHECK(s.label && s.label[0]);
        CHECK(s.def >= s.min && s.def <= s.max);
        for (int k = 0; k <= 20; ++k) {
            float n = k / 20.f, v = fromNorm(s, n);
            CHECK(v >= s.min - 1e-4f && v <= s.max + 1e-4f);
            if (s.curve != Curve::Int) CHECK(std::fabs(toNorm(s, v) - n) < 2e-3f);
        }
        CHECK(formatParam(s, s.def, b, sizeof b) > 0);
    }
    for (int i = 0; i < kGlobalParamCount; ++i) {
        const ParamSpec& s = globalSpec((GlobalParam)i);
        CHECK(s.def >= s.min && s.def <= s.max);
        CHECK(formatParam(s, s.def, b, sizeof b) > 0);
    }
    formatParam(padSpec(PP_CUTOFF), 4200.f, b, sizeof b); CHECK(!std::strcmp(b, "4.2 kHz"));
    formatParam(padSpec(PP_PAN), 0.f, b, sizeof b); CHECK(!std::strcmp(b, "C"));
    formatParam(padSpec(PP_PAN), -0.2f, b, sizeof b); CHECK(!std::strcmp(b, "L 20"));
    formatParam(padSpec(PP_ATTACK), 50.f, b, sizeof b); CHECK(!std::strcmp(b, "50 ms"));
    formatParam(padSpec(PP_VOLUME), 0.f, b, sizeof b); CHECK(!std::strcmp(b, "0.0 dB"));
    formatParam(padSpec(PP_SUSTAIN), 0.72f, b, sizeof b); CHECK(!std::strcmp(b, "72 %"));
    formatParam(padSpec(PP_RESO), 0.29f, b, sizeof b); CHECK(!std::strcmp(b, "0.29"));

    static Project p;
    projectDefaults(p);
    CHECK(p.bpm == 120.f);
    CHECK(p.scene_data[0].track[0].step[0].on == 1);
    CHECK(trackLength(p.scene_data[0], 0) == 16);
    CHECK(std::fabs(busGain(0.7071f) - 1.f) < 1e-3f);
    std::printf("sizeof(Project)=%u sizeof(Scene)=%u sizeof(Pad)=%u\n", (unsigned)sizeof(Project), (unsigned)sizeof(Scene), (unsigned)sizeof(Pad));

    Sample* s = sampleCreate(1000, 1, 44100);
    CHECK(s);
    for (int i = 0; i < 1000; ++i) s->data[i] = (int16_t)(16000 * std::sin(i * 0.05));
    sampleAnalyze(s);
    CHECK(s->peak > 0.48f && s->peak < 0.5f);
    CHECK(s->data[-1] == 0 && s->data[1000] == 0);
    sampleFree(s);
    testDefaultsOnLiveProject();
    testAnalyze();
    // fastLog2 (limiter gain-reduction telemetry on the audio task, no libm)
    double worst = 0.0;
    for (int k = 0; k <= 4000; ++k) {
        const float x = std::pow(10.0f, -6.0f + 6.0f * (float)k / 4000.0f);
        worst = std::max(worst, std::fabs((double)orbit::dsp::fastLog2(x) - std::log2((double)x)));
    }
    CHECK(worst < 2e-5);
    std::printf(fails ? "core basics: %d FAILED\n" : "core basics: all passed\n", fails);
    return fails ? 1 : 0;
}
