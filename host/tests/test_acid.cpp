// Build + run (Git Bash, project root; or: bash tools/run_host_tests.sh acid):
// export ZIG_GLOBAL_CACHE_DIR=$PWD/host/.zig-cache/global ZIG_LOCAL_CACHE_DIR=$PWD/host/.zig-cache/local; zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -Wall -Wextra -Ifirmware/main -Ihost host/tests/test_acid.cpp firmware/main/core/acid.cpp -o host/bin/test_acid.exe && host/bin/test_acid.exe
// ACID voice (core/acid.cpp): a 2-bar acid line at 48 / 44.1 kHz (host/out/acid_48k.wav,
// acid_44k.wav, acid_sq_drive_48k.wav), slide, accent (+ accent build-up), cutoff, envelopes,
// event timing / queue / gate / NoteOff rules, click checks, block-size independence, a 10 s
// stability torture, panic, and an informational CPU figure.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <vector>
#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#define HAVE_RDTSC 1
#endif

#include "core/acid.h"

using namespace orbit;

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...)                                               \
    do {                                                               \
        ++g_checks;                                                    \
        if (!(cond)) {                                                 \
            ++g_fail;                                                  \
            printf("  FAIL %s:%d: %s | ", __FILE__, __LINE__, #cond); \
            printf(__VA_ARGS__);                                       \
            printf("\n");                                              \
        } else {                                                       \
            printf("  PASS ");                                         \
            printf(__VA_ARGS__);                                       \
            printf("\n");                                              \
        }                                                              \
    } while (0)

// ------------------------------------------------------------------ helpers

static double db(double x) { return 20.0 * log10(x > 1e-12 ? x : 1e-12); }

static double peakAbs(const std::vector<float>& y, size_t a, size_t b)
{
    double p = 0;
    for (size_t i = a; i < b && i < y.size(); ++i) p = std::max(p, (double)fabsf(y[i]));
    return p;
}
static double rms(const std::vector<float>& y, size_t a, size_t b)
{
    double s = 0;
    size_t n = 0;
    for (size_t i = a; i < b && i < y.size(); ++i, ++n) s += (double)y[i] * y[i];
    return n ? sqrt(s / n) : 0.0;
}
// rms(first difference) / rms: rises with the spectral centroid (same rate only).
static double brightness(const std::vector<float>& y, size_t a, size_t b)
{
    double s = 0, d = 0;
    for (size_t i = a + 1; i < b && i < y.size(); ++i) {
        s += (double)y[i] * y[i];
        const double e = (double)y[i] - y[i - 1];
        d += e * e;
    }
    return s > 0 ? sqrt(d / s) : 0.0;
}
static bool allFinite(const std::vector<float>& y)
{
    for (float v : y)
        if (!std::isfinite(v)) return false;
    return true;
}
// Largest sample-to-sample step in [a, b).
static double maxStep(const std::vector<float>& y, size_t a, size_t b)
{
    double m = 0;
    for (size_t i = std::max<size_t>(a, 1); i < b && i < y.size(); ++i) m = std::max(m, (double)fabsf(y[i] - y[i - 1]));
    return m;
}
// Frequency from positive-going zero crossings (linear interpolation) in [a, b).
static double zcFreq(const std::vector<float>& y, size_t a, size_t b, int rate)
{
    double first = -1, last = -1;
    int periods = 0;
    for (size_t i = std::max<size_t>(a, 1); i < b && i < y.size(); ++i) {
        if (y[i - 1] < 0.0f && y[i] >= 0.0f) {
            const double t = (double)(i - 1) + (double)y[i - 1] / ((double)y[i - 1] - (double)y[i]);
            if (first < 0) first = t;
            else {
                last = t;
                ++periods;
            }
        }
    }
    return periods > 0 ? periods * (double)rate / (last - first) : 0.0;
}
static double midiHz(double n) { return 440.0 * pow(2.0, (n - 69.0) / 12.0); }

static void writeWav(const char* path, const std::vector<float>& y, int rate)
{
    FILE* f = fopen(path, "wb");
    if (!f) {
        printf("  cannot write %s\n", path);
        return;
    }
    const uint32_t data = (uint32_t)(y.size() * 2);
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f);
    u32(36 + data);
    fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);
    u16(1);
    u32((uint32_t)rate);
    u32((uint32_t)rate * 2);
    u16(2);
    u16(16);
    fwrite("data", 1, 4, f);
    u32(data);
    for (float v : y) {
        float c = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
        int16_t s = (int16_t)lrintf(c * 32767.0f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

static uint32_t g_rng = 0x1234567u;
static uint32_t rnd()
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static double frand() { return (rnd() >> 8) * (1.0 / 16777216.0); }

// ------------------------------------------------------------------ tiny scheduler

// Event at an absolute frame.
struct TEv {
    uint64_t at;
    AcidEvent e;
};
static TEv noteOn(uint64_t at, int note, bool acc, bool legato, uint32_t gate)
{
    TEv t;
    t.at = at;
    t.e.type = AcidEvtType::NoteOn;
    t.e.note = (uint8_t)note;
    t.e.accent = acc ? 1 : 0;
    t.e.legato = legato ? 1 : 0;
    t.e.gate_frames = gate;
    return t;
}
static TEv simpleEv(uint64_t at, AcidEvtType type, int note = 36)
{
    TEv t;
    t.at = at;
    t.e.type = type;
    t.e.note = (uint8_t)note;
    return t;
}

// Per-block render (block 0 = random sizes 1..256): every event lands in its block with
// offset = at - block start. Optional per-block parameter callback (knob automation).
struct Renderer {
    AcidVoice v;
    int rate;
    std::vector<float> y;
    int sched_fail = 0;
    explicit Renderer(int r) : rate(r) { v.init(r); }
    template <typename F>
    void run(std::vector<TEv> ev, size_t frames, AcidParams p, int block, F perBlock)
    {
        std::stable_sort(ev.begin(), ev.end(), [](const TEv& a, const TEv& b) { return a.at < b.at; });
        size_t k = 0;
        const size_t start = y.size();
        y.resize(start + frames, 0.0f);
        size_t pos = 0;
        while (pos < frames) {
            int n = block > 0 ? block : (int)(1 + rnd() % 256);
            if (pos + (size_t)n > frames) n = (int)(frames - pos);
            const uint64_t b0 = start + pos;
            perBlock(p, b0);
            while (k < ev.size() && ev[k].at < b0 + (uint64_t)n) {
                AcidEvent e = ev[k].e;
                e.offset = ev[k].at >= b0 ? (uint32_t)(ev[k].at - b0) : 0u;
                if (!v.schedule(e)) ++sched_fail;
                ++k;
            }
            v.render(y.data() + start + pos, n, p);
            pos += (size_t)n;
        }
    }
    void run(const std::vector<TEv>& ev, size_t frames, const AcidParams& p, int block = 64)
    {
        run(ev, frames, p, block, [](AcidParams&, uint64_t) {});
    }
    size_t ms(double t) const { return (size_t)(t * 0.001 * rate + 0.5); }
};

// 2-bar acid line, 130 BPM 16ths. slide = this note slides into the next one (303 style: its
// gate is held one step + overlap and the next note is legato). note < 0 = rest.
struct Step {
    int note;
    bool acc, slide;
};
static const Step kLine[32] = {
    {36, true, false}, {36, false, false}, {48, false, true}, {36, false, false},   // bar 1
    {-1, false, false}, {39, true, false}, {36, false, false}, {43, false, true},
    {44, true, false}, {-1, false, false}, {48, false, false}, {36, false, true},
    {46, true, false}, {36, false, false}, {-1, false, false}, {51, false, true},
    {36, true, false}, {36, false, true}, {39, false, false}, {36, false, false},   // bar 2
    {48, true, false}, {-1, false, false}, {36, false, false}, {41, true, true},
    {43, false, true}, {44, false, false}, {-1, false, false}, {36, true, false},
    {48, false, true}, {46, false, true}, {43, true, false}, {39, false, false},
};
static double stepFrames(int rate) { return 60.0 / 130.0 / 4.0 * rate; }

static std::vector<TEv> acidLineEvents(int rate, int bars_repeat = 1)
{
    std::vector<TEv> ev;
    const double sf = stepFrames(rate);
    const uint32_t half = (uint32_t)(sf * 0.5);
    const uint32_t held = (uint32_t)(sf + 0.01 * rate);   // one step + 10 ms overlap
    bool prev_slide = false;
    for (int r = 0; r < bars_repeat; ++r)
        for (int s = 0; s < 32; ++s) {
            const Step& st = kLine[s];
            const uint64_t at = (uint64_t)llround((r * 32 + s) * sf);
            if (st.note < 0) {
                prev_slide = false;
                continue;
            }
            ev.push_back(noteOn(at, st.note, st.acc, prev_slide, st.slide ? held : half));
            prev_slide = st.slide;
        }
    return ev;
}

// ------------------------------------------------------------------ tests

static double g_rms48 = 0, g_rms44 = 0;

static void testAcidLine(int rate)
{
    printf("[acid line %d Hz] 2 bars, 130 BPM, rests, accents, slides\n", rate);
    Renderer r(rate);
    AcidParams p;
    const size_t frames = (size_t)(32 * stepFrames(rate)) + r.ms(400);
    r.run(acidLineEvents(rate), frames, p);
    CHECK(r.sched_fail == 0, "all events queued (%d failed)", r.sched_fail);
    CHECK(allFinite(r.y), "%d Hz: no NaN / Inf", rate);
    const double pk = peakAbs(r.y, 0, r.y.size());
    const double rm = rms(r.y, 0, (size_t)(32 * stepFrames(rate)));
    CHECK(pk > 0.05 && pk < 1.5, "%d Hz: peak %.3f (%.1f dBFS), rms %.3f (%.1f dBFS)", rate, pk, db(pk), rm, db(rm));
    CHECK(pk > 0.25 && pk < 0.7, "%d Hz: default-patch peak %.3f sits next to drums (0.25..0.7)", rate, pk);
    (rate == 48000 ? g_rms48 : g_rms44) = rm;
    char path[64];
    snprintf(path, sizeof path, "host/out/acid_%dk.wav", rate / 1000);
    writeWav(path, r.y, rate);
    printf("  wrote %s\n", path);
    CHECK(!r.v.active(), "voice idle after the line + 400 ms");
    // per-step snapshot: level and brightness of a few notes (informational)
    const double sf = stepFrames(rate);
    printf("  step  note acc  rms(dB)  bright\n");
    for (int s : {0, 1, 2, 3, 5, 8, 12, 16, 20, 23, 27, 30}) {
        const size_t a = (size_t)(s * sf), b = (size_t)((s + 0.5) * sf);
        printf("  %4d  %4d  %d   %6.1f   %.3f\n", s, kLine[s].note, kLine[s].acc, db(rms(r.y, a, b)), brightness(r.y, a, b));
    }
}

static void testRates()
{
    printf("[rates] 48 kHz vs 44.1 kHz\n");
    const double d = db(g_rms48) - db(g_rms44);
    CHECK(fabs(d) < 1.0, "RMS 48k %.4f vs 44.1k %.4f: %.2f dB", g_rms48, g_rms44, d);
    for (int rate : {48000, 44100}) {
        Renderer r(rate);
        AcidParams p;
        p.cutoff = 0.15f;
        p.reso = 0.0f;
        p.envmod = 0.0f;
        r.run({noteOn(0, 45, false, false, 0)}, (size_t)(0.5 * rate), p);
        const double f = zcFreq(r.y, r.ms(100), r.ms(500), rate);
        CHECK(fabs(f / 110.0 - 1.0) < 0.003, "%d Hz: A2 measures %.2f Hz", rate, f);
    }
    for (double tune : {-12.0, 7.0}) {
        Renderer r(48000);
        AcidParams p;
        p.cutoff = 0.15f;
        p.reso = 0.0f;
        p.envmod = 0.0f;
        p.tune = (float)tune;
        r.run({noteOn(0, 45, false, false, 0)}, 24000, p);
        const double f = zcFreq(r.y, r.ms(100), r.ms(500), 48000);
        CHECK(fabs(f / midiHz(45 + tune) - 1.0) < 0.003, "tune %+.0f: %.2f Hz (expected %.2f)", tune, f, midiHz(45 + tune));
    }
}

static void testSlide()
{
    printf("[slide] legato octave up vs non-legato jump\n");
    for (int rate : {48000, 44100}) {
        AcidParams p;
        p.cutoff = 0.15f;
        p.reso = 0.0f;
        p.envmod = 0.0f;
        p.accent = 0.0f;
        Renderer r(rate);
        const uint64_t t1 = (uint64_t)(0.4 * rate);
        r.run({noteOn(0, 36, false, false, (uint32_t)t1 + r.ms(10)), noteOn(t1, 48, false, true, (uint32_t)r.ms(600))},
              (size_t)(1.2 * rate), p);
        const double f0 = zcFreq(r.y, r.ms(150), (size_t)t1, rate);
        const double fg = zcFreq(r.y, t1 + r.ms(20), t1 + r.ms(50), rate);
        const double fe = zcFreq(r.y, t1 + r.ms(300), t1 + r.ms(450), rate);
        CHECK(fabs(f0 / midiHz(36) - 1.0) < 0.01, "%d Hz: before the slide %.2f Hz (C2 %.2f)", rate, f0, midiHz(36));
        CHECK(fg > midiHz(36) * 1.08 && fg < midiHz(48) * 0.92, "%d Hz: 20..50 ms into the slide %.2f Hz (gliding)", rate, fg);
        CHECK(fabs(fe / midiHz(48) - 1.0) < 0.015, "%d Hz: 300..450 ms after %.2f Hz (C3 %.2f)", rate, fe, midiHz(48));
        // time to cover half of the octave (tau 65 ms -> ~45 ms)
        Renderer q(rate);
        q.v.init(rate);
        std::vector<float> trace;
        std::vector<TEv> ev = {noteOn(0, 36, false, false, (uint32_t)t1 + q.ms(10)), noteOn(t1, 48, false, true, 0)};
        double half_ms = -1;
        q.run(ev, (size_t)(0.7 * rate), p, 16, [&](AcidParams&, uint64_t b0) {
            if (half_ms < 0 && b0 > t1 && q.v.pitchSemis() >= 42.0f) half_ms = (double)(b0 - t1) * 1000.0 / rate;
        });
        CHECK(half_ms > 35 && half_ms < 55, "%d Hz: half of the slide after %.1f ms (tau 65 ms: 45 ms)", rate, half_ms);
    }
    {   // non-legato: jumps at once (also when the previous gate is still open)
        AcidParams p;
        p.cutoff = 0.15f;
        p.reso = 0.0f;
        p.envmod = 0.0f;
        Renderer r(48000);
        r.run({noteOn(0, 36, false, false, 0), noteOn(19200, 48, false, false, 0)}, 48000, p);
        const double fj = zcFreq(r.y, 19200 + r.ms(8), 19200 + r.ms(40), 48000);
        CHECK(fabs(fj / midiHz(48) - 1.0) < 0.02, "non-legato jump: %.2f Hz 8..40 ms after (C3 %.2f)", fj, midiHz(48));
    }
    {   // legato after the gate closed: plain note (no slide)
        AcidParams p;
        Renderer r(48000);
        r.run({noteOn(0, 36, false, false, 2000), noteOn(4000, 48, false, true, 2000)}, 4016, p);
        CHECK(r.v.pitchSemis() == 48.0f, "legato NoteOn with the gate closed jumps (pitch %.3f)", r.v.pitchSemis());
    }
}

// Renders one note (or a run of notes) and returns the buffer.
static std::vector<float> oneNote(int rate, const AcidParams& p, int note, bool acc, double gate_s, double len_s)
{
    Renderer r(rate);
    r.run({noteOn(0, note, acc, false, (uint32_t)(gate_s * rate))}, (size_t)(len_s * rate), p);
    return r.y;
}

static void testAccent()
{
    printf("[accent] louder + brighter, build-up over consecutive accents\n");
    const int rate = 48000;
    AcidParams p;
    const double gate = stepFrames(rate) * 0.5 / rate;
    std::vector<float> a = oneNote(rate, p, 36, false, gate, 0.2);
    std::vector<float> b = oneNote(rate, p, 36, true, gate, 0.2);
    const size_t n = (size_t)(0.15 * rate);
    const double ra = rms(a, 0, n), rb = rms(b, 0, n);
    const double ba = brightness(a, 0, n), bb = brightness(b, 0, n);
    CHECK(db(rb) - db(ra) >= 2.0, "accented note %.2f dB louder (rms %.3f vs %.3f)", db(rb) - db(ra), rb, ra);
    CHECK(bb > ba * 1.1, "accented note brighter: %.3f vs %.3f (x%.2f)", bb, ba, bb / ba);
    p.accent = 1.0f;
    std::vector<float> c = oneNote(rate, p, 36, true, gate, 0.2);
    const double pa = peakAbs(a, 0, n), pc = peakAbs(c, 0, n);
    CHECK(db(pc) - db(pa) > 4.0 && db(pc) - db(pa) < 9.0, "ACCENT = 1: peak +%.1f dB (about +6 dB)", db(pc) - db(pa));
    p.accent = 0.0f;
    std::vector<float> d = oneNote(rate, p, 36, true, gate, 0.2);
    CHECK(fabs(db(rms(d, 0, n)) - db(ra)) < 0.3, "ACCENT = 0: accented note = normal (%.2f dB)", db(rms(d, 0, n)) - db(ra));

    // build-up: 6 accented 16ths in a row; per-note peak of the accent sweep and brightness
    p = AcidParams();
    p.reso = 0.8f;
    Renderer r(rate);
    std::vector<TEv> ev;
    const double sf = stepFrames(rate);
    for (int s = 0; s < 6; ++s) ev.push_back(noteOn((uint64_t)llround(s * sf), 36, true, false, (uint32_t)(sf * 0.5)));
    double pk[6] = {0, 0, 0, 0, 0, 0};
    r.run(ev, (size_t)(6 * sf), p, 16, [&](AcidParams&, uint64_t b0) {
        const int s = std::min(5, (int)(b0 / sf));
        pk[s] = std::max(pk[s], (double)r.v.accentSweep());
    });
    printf("  accent sweep peaks: %.3f %.3f %.3f %.3f %.3f %.3f\n", pk[0], pk[1], pk[2], pk[3], pk[4], pk[5]);
    CHECK(pk[1] > pk[0] * 1.05 && pk[3] >= pk[1], "consecutive accents build up (%.3f -> %.3f -> %.3f)", pk[0], pk[1], pk[3]);
    const double b1 = brightness(r.y, 0, (size_t)(0.5 * sf)), b4 = brightness(r.y, (size_t)(3 * sf), (size_t)(3.5 * sf));
    CHECK(b4 > b1 * 1.03, "4th accent brighter than the 1st: %.3f vs %.3f", b4, b1);
}

static void testCutoff()
{
    printf("[cutoff] brightness rises with CUTOFF\n");
    const int rate = 48000;
    double br[3];
    const float cuts[3] = {0.1f, 0.5f, 0.9f};
    for (int i = 0; i < 3; ++i) {
        AcidParams p;
        p.cutoff = cuts[i];
        p.envmod = 0.2f;
        std::vector<float> y = oneNote(rate, p, 36, false, 0.3, 0.3);
        br[i] = brightness(y, rate / 100, (size_t)(0.25 * rate));
    }
    CHECK(br[0] < br[1] && br[1] < br[2], "brightness %.4f < %.4f < %.4f", br[0], br[1], br[2]);
    {   // RESO raises a peak at the cutoff: energy near fc vs the fundamental (informational check)
        AcidParams p;
        p.envmod = 0.0f;
        p.cutoff = 0.6f;
        p.reso = 0.0f;
        std::vector<float> y0 = oneNote(rate, p, 36, false, 0.5, 0.5);
        p.reso = 1.0f;
        std::vector<float> y1 = oneNote(rate, p, 36, false, 0.5, 0.5);
        const double b0 = brightness(y0, rate / 10, rate * 4 / 10), b1 = brightness(y1, rate / 10, rate * 4 / 10);
        CHECK(b1 > b0 * 1.3, "RESO 1 vs 0 at CUTOFF 0.6: brightness x%.2f", b1 / b0);
        printf("  rms reso 0: %.1f dB, reso 1: %.1f dB\n", db(rms(y0, rate / 10, rate * 4 / 10)), db(rms(y1, rate / 10, rate * 4 / 10)));
    }
    {   // RESO 1 is at the edge of self-oscillation: the ring after a saw drop (note 0 = 8 Hz, so
        // the drops are 122 ms apart) lasts, RESO 0.5 dies fast. Second difference removes the ramp.
        auto ring = [&](float reso, float cut, double t_ms) {
            AcidParams p;
            p.envmod = 0.0f;
            p.accent = 0.0f;
            p.cutoff = cut;
            p.reso = reso;
            std::vector<float> y = oneNote(rate, p, 0, false, 0.0, 0.6);
            size_t drop = 0;
            double best = 0;
            for (size_t i = (size_t)(0.3 * rate); i < (size_t)(0.45 * rate); ++i)
                if (y[i - 1] - y[i] > best) {
                    best = y[i - 1] - y[i];
                    drop = i;
                }
            auto env = [&](size_t a) {
                double m = 0;
                for (size_t i = a; i < a + (size_t)rate / 500; ++i) m = std::max(m, fabs((double)y[i + 1] - 2.0 * y[i] + y[i - 1]));
                return m;
            };
            return db(env(drop + (size_t)(t_ms * 0.001 * rate)) / env(drop + 24));
        };
        const double r1 = ring(1.0f, 0.75f, 30.0), r05 = ring(0.5f, 0.75f, 10.0), r1lo = ring(1.0f, 0.15f, 30.0);
        CHECK(r1 > -45.0 && r05 < -45.0, "ring at ~1.6 kHz: RESO 1 %.1f dB after 30 ms, RESO 0.5 %.1f dB after 10 ms", r1, r05);
        printf("  RESO 1 at CUTOFF 0.15 (~160 Hz): %.1f dB after 30 ms (loop highpass: no low boom)\n", r1lo);
    }
}

static void testEnvelopes()
{
    printf("[envelopes] MEG decay knob, VEG release, gate timing\n");
    const int rate = 48000;
    {   // DECAY: brightness 300 ms in is higher with a long decay
        AcidParams p;
        p.envmod = 1.0f;
        p.cutoff = 0.2f;
        p.decay = 0.0f;
        std::vector<float> s = oneNote(rate, p, 36, false, 0.6, 0.6);
        p.decay = 1.0f;
        std::vector<float> l = oneNote(rate, p, 36, false, 0.6, 0.6);
        const double bs = brightness(s, rate * 3 / 10, rate * 4 / 10), bl = brightness(l, rate * 3 / 10, rate * 4 / 10);
        CHECK(bl > bs * 1.5, "DECAY 1 vs 0 at 300 ms: brightness %.3f vs %.3f", bl, bs);
    }
    {   // gate_frames: the gate closes exactly after gate_frames
        AcidParams p;
        Renderer r(rate);
        r.v.schedule(noteOn(0, 36, false, false, 1000).e);
        std::vector<float> buf(1000, 0.0f);
        r.v.render(buf.data(), 999, p);
        const bool open = r.v.gate();
        r.v.render(buf.data(), 1, p);
        CHECK(open && !r.v.gate(), "gate open for exactly gate_frames (999: %d, 1000: %d)", (int)open, (int)r.v.gate());
        // release (tau 8 ms): below -60 dB of the note level within 60 ms, exactly 0 once done
        std::vector<float> y = oneNote(rate, p, 36, false, 0.1, 0.3);
        const double lv = rms(y, (size_t)(0.05 * rate), (size_t)(0.1 * rate));
        const double tail = peakAbs(y, (size_t)(0.16 * rate), (size_t)(0.3 * rate));
        const double rest = peakAbs(y, (size_t)(0.22 * rate), (size_t)(0.3 * rate));
        CHECK(db(tail) - db(lv) < -60.0 && rest == 0.0, "release: %.1f dB 60 ms after the gate, %.2g from 120 ms",
              db(tail) - db(lv), rest);
    }
    {   // attack: 2.5 ms from silence, no click
        AcidParams p;
        std::vector<float> y = oneNote(rate, p, 36, true, 0.1, 0.1);
        const double first = peakAbs(y, 0, 24), later = peakAbs(y, 0, rate / 20);
        CHECK(first < 0.25 * later, "attack: first 0.5 ms peak %.4f vs note peak %.3f", first, later);
    }
    {   // NoteOff rules
        AcidParams p;
        Renderer r(rate);
        r.run({noteOn(0, 36, false, false, 0), simpleEv(1000, AcidEvtType::NoteOff, 40)}, 2000, p);
        const bool still = r.v.gate();
        r.run({simpleEv(r.y.size() + 10, AcidEvtType::NoteOff, 36)}, 100, p);
        const bool closed = !r.v.gate();
        r.run({noteOn(r.y.size(), 50, false, false, 0), simpleEv(r.y.size() + 5, AcidEvtType::NoteOff, kAcidAnyNote)}, 100, p);
        CHECK(still && closed && !r.v.gate(), "NoteOff: other note ignored (%d), own note closes (%d), any-note closes (%d)",
              (int)still, (int)closed, (int)!r.v.gate());
        r.run({}, (size_t)(0.5 * rate), p);
        CHECK(!r.v.active(), "voice idle 0.5 s after the last NoteOff");
    }
}

static void testEventsAndQueue()
{
    printf("[events] offsets, queue, block-size independence\n");
    const int rate = 48000;
    AcidParams p;
    {
        AcidVoice v;
        v.init(rate);
        AcidEvent e;
        e.offset = 37;
        CHECK(v.schedule(e), "schedule");
        std::vector<float> buf(64, 0.0f);
        v.render(buf.data(), 64, p);
        bool silent_before = true;
        for (int i = 0; i < 37; ++i) silent_before = silent_before && buf[i] == 0.0f;
        bool sound_after = false;
        for (int i = 38; i < 64; ++i) sound_after = sound_after || buf[i] != 0.0f;
        CHECK(silent_before && sound_after, "NoteOn at offset 37 starts at frame 37 (before: %d, after: %d)", (int)silent_before,
              (int)sound_after);
        int ok = 0;
        for (int i = 0; i < AcidVoice::kQueue + 4; ++i) ok += v.schedule(e) ? 1 : 0;
        CHECK(ok == AcidVoice::kQueue, "queue holds %d events (got %d)", AcidVoice::kQueue, ok);
        e.offset = 500;   // >= n: clamped to n - 1
        v.render(buf.data(), 64, p);
        CHECK(v.schedule(e), "queue empty after render()");
        v.render(buf.data(), 64, p);
        CHECK(v.active() && allFinite(buf), "late event clamped to the block end");
    }
    {   // the same events give the same output with 64-frame and random 1..256-frame blocks
        Renderer a(rate), b(rate);
        const size_t frames = (size_t)(32 * stepFrames(rate)) + a.ms(300);
        AcidParams q;
        q.wave = 1;
        q.drive = 0.4f;
        a.run(acidLineEvents(rate), frames, q, 64);
        b.run(acidLineEvents(rate), frames, q, 0);
        double md = 0;
        for (size_t i = 0; i < frames; ++i) md = std::max(md, (double)fabsf(a.y[i] - b.y[i]));
        CHECK(md < 1e-5, "block-size independent: max diff %.2g (64 vs random 1..256)", md);
    }
}

static void testClicks()
{
    printf("[clicks] retrigger while sounding, wave switch, knob steps\n");
    const int rate = 48000;
    AcidParams p;
    p.envmod = 0.0f;
    p.cutoff = 0.5f;
    p.reso = 0.3f;
    {   // non-legato retrigger in the middle of a sounding note
        Renderer r(rate);
        r.run({noteOn(0, 36, false, false, 0), noteOn(12000, 36, true, false, 0)}, 24000, p);
        const double base = maxStep(r.y, 2400, 11990);
        const double at = maxStep(r.y, 11990, 12200);
        CHECK(at < 1.6 * base, "retrigger step %.4f vs steady %.4f", at, base);
    }
    {   // saw -> square while a note plays: crossfaded
        Renderer r(rate);
        r.run({noteOn(0, 36, false, false, 0)}, 12000, p);
        const double base = maxStep(r.y, 2400, 12000);
        AcidParams q = p;
        q.wave = 1;
        r.run({}, 12000, q);
        const double sw = maxStep(r.y, 12000, 12600);
        const double sq = maxStep(r.y, 13000, 24000);
        CHECK(sw < 1.25 * std::max(base, sq), "wave switch step %.4f vs saw %.4f / square %.4f", sw, base, sq);
    }
    {   // a cutoff jump 0.1 -> 0.9 between blocks is smoothed
        Renderer r(rate);
        AcidParams q = p;
        q.cutoff = 0.1f;
        r.run({noteOn(0, 36, false, false, 0)}, 12032, q);
        q.cutoff = 0.9f;
        r.run({}, 12000, q);
        const double fc_at = r.v.cutoffHz();
        AcidParams z = q;
        Renderer s(rate);
        z.cutoff = 0.1f;
        s.run({noteOn(0, 36, false, false, 0)}, 12032, z);
        z.cutoff = 0.9f;
        s.run({}, 64, z);
        const double fc_1 = s.v.cutoffHz();
        CHECK(fc_1 < 0.5 * fc_at, "cutoff knob glides: %.0f Hz one block after the jump, %.0f Hz later", fc_1, fc_at);
    }
}

static void testTorture(int rate)
{
    printf("[torture %d Hz] RESO 1, DRIVE 1, cutoff sweeps, rapid notes, random blocks, 10 s\n", rate);
    Renderer r(rate);
    std::vector<TEv> ev;
    const size_t len = (size_t)10 * rate;
    uint64_t t = 0;
    uint64_t last_gate_end = 0;
    while (t < len - (size_t)rate / 2) {
        const int note = 24 + (int)(rnd() % 61);
        const bool acc = (rnd() & 1) != 0, leg = (rnd() % 3) == 0;
        const uint32_t gate = (uint32_t)(rate * (0.005 + 0.15 * frand()));
        ev.push_back(noteOn(t, note, acc, leg, gate));
        last_gate_end = std::max(last_gate_end, t + gate);
        if ((rnd() % 7) == 0) ev.push_back(simpleEv(t + gate / 2, AcidEvtType::NoteOff, note));
        t += (uint64_t)(rate * (0.01 + 0.08 * frand()));
    }
    AcidParams p;
    p.reso = 1.0f;
    p.drive = 1.0f;
    p.envmod = 1.0f;
    p.accent = 1.0f;
    p.decay = 1.0f;
    double ph = 0;
    r.run(ev, len, p, 0, [&](AcidParams& q, uint64_t b0) {
        ph += 0.004;
        q.cutoff = (float)(0.5 + 0.5 * sin(ph * 13.0));
        q.reso = (float)(0.9 + 0.1 * sin(ph * 3.1));
        q.wave = (b0 / (size_t)(rate * 2)) & 1;
        q.tune = (float)(12.0 * sin(ph * 0.7));
    });
    const double pk = peakAbs(r.y, 0, len);
    CHECK(allFinite(r.y), "no NaN / Inf");
    CHECK(pk < 4.0, "bounded: peak %.3f", pk);
    printf("  rms %.1f dBFS, events %zu (queue failures %d)\n", db(rms(r.y, 0, len)), ev.size(), r.sched_fail);
    const double tail = peakAbs(r.y, (size_t)last_gate_end + (size_t)rate, len);
    CHECK(db(tail) < -60.0, "release tail 1 s after the last gate: %.1f dBFS", db(tail));
    CHECK(!r.v.active(), "voice idle at the end");

    // garbage parameters (out of range, NaN) stay finite and bounded
    AcidParams g;
    g.cutoff = NAN;
    g.reso = 7.0f;
    g.envmod = -3.0f;
    g.decay = INFINITY;
    g.accent = 9.0f;
    g.drive = NAN;
    g.level = 3.0f;
    g.tune = 1e9f;
    Renderer z(rate);
    z.run({noteOn(0, 0, true, false, 0), noteOn(5000, 127, true, true, 0), noteOn(9000, 200, false, false, 300)}, 30000, g, 0);
    CHECK(allFinite(z.y) && peakAbs(z.y, 0, z.y.size()) < 12.0, "garbage parameters: finite, peak %.3f",
          peakAbs(z.y, 0, z.y.size()));
}

static void testPanic()
{
    printf("[panic] silence within 10 ms\n");
    for (int rate : {48000, 44100}) {
        Renderer r(rate);
        AcidParams p;
        p.reso = 0.9f;
        p.drive = 0.5f;
        const uint64_t at = (uint64_t)(0.2 * rate) + 13;
        r.run({noteOn(0, 36, true, false, 0), simpleEv(at, AcidEvtType::Panic)}, (size_t)(0.4 * rate), p);
        const double before = peakAbs(r.y, (size_t)at - r.ms(20), (size_t)at);
        const double after = peakAbs(r.y, (size_t)at + r.ms(10), r.y.size());
        CHECK(db(after) < -80.0, "%d Hz: %.1f dBFS from 10 ms after the panic (before %.1f dBFS)", rate, db(after), db(before));
        CHECK(!r.v.active() && !r.v.gate(), "%d Hz: idle after the panic", rate);
        const double st = maxStep(r.y, (size_t)at - 10, (size_t)at + r.ms(10));
        const double st0 = maxStep(r.y, (size_t)at - r.ms(30), (size_t)at - 10);
        CHECK(st < 1.2 * st0, "%d Hz: panic fade without a click (step %.4f vs %.4f)", rate, st, st0);
    }
    {   // a NoteOn right after a panic starts once the fade is done
        Renderer r(48000);
        AcidParams p;
        r.run({noteOn(0, 36, false, false, 0), simpleEv(9600, AcidEvtType::Panic), noteOn(9700, 48, false, false, 0)}, 19200, p);
        const double before = peakAbs(r.y, 9000, 9600);
        const double gap = peakAbs(r.y, 9600 + 170, 9600 + 200);   // fade end (4 ms = 192 frames)
        const double later = peakAbs(r.y, 12000, 19200);
        CHECK(r.v.gate() && later > 0.05 && gap < 0.1 * before, "NoteOn during the fade plays after it (fade end %.4f, later %.3f)",
              gap, later);
        CHECK(fabs(r.v.pitchSemis() - 48.0f) < 1e-3f, "pending note is the new one (%.2f)", r.v.pitchSemis());
    }
}

static void testDriveAndWave()
{
    printf("[drive / wave] level compensation, square vs saw\n");
    const int rate = 48000;
    const double sf = stepFrames(rate);
    const size_t frames = (size_t)(32 * sf);
    double r0 = 0;
    for (float d : {0.0f, 0.25f, 0.5f, 1.0f}) {
        Renderer r(rate);
        AcidParams p;
        p.drive = d;
        r.run(acidLineEvents(rate), frames, p);
        const double rm = rms(r.y, 0, frames), pk = peakAbs(r.y, 0, frames);
        if (d == 0.0f) r0 = rm;
        printf("  drive %.2f: rms %.1f dBFS, peak %.1f dBFS, brightness %.3f\n", d, db(rm), db(pk), brightness(r.y, 0, frames));
        if (d > 0.0f) CHECK(fabs(db(rm) - db(r0)) < 3.0, "drive %.2f changes the RMS by %.1f dB", d, db(rm) - db(r0));
    }
    Renderer s(rate), q(rate);
    AcidParams p;
    s.run(acidLineEvents(rate), frames, p);
    p.wave = 1;
    q.run(acidLineEvents(rate), frames, p);
    const double rs = rms(s.y, 0, frames), rq = rms(q.y, 0, frames);
    CHECK(fabs(db(rq) - db(rs)) < 3.0, "square vs saw RMS %.1f dB", db(rq) - db(rs));
    // a heavier take for listening: square, drive, high resonance
    Renderer w(rate);
    AcidParams h;
    h.wave = 1;
    h.drive = 0.6f;
    h.reso = 0.85f;
    h.envmod = 0.7f;
    h.cutoff = 0.3f;
    h.accent = 0.9f;
    w.run(acidLineEvents(rate, 2), (size_t)(64 * sf) + w.ms(300), h);
    CHECK(allFinite(w.y) && peakAbs(w.y, 0, w.y.size()) < 1.0, "heavy take peak %.3f", peakAbs(w.y, 0, w.y.size()));
    writeWav("host/out/acid_sq_drive_48k.wav", w.y, rate);
    printf("  wrote host/out/acid_sq_drive_48k.wav\n");
}

// CPU: 10 s of the acid line in 64-frame blocks, best of 5 runs (the PC is shared).
static void benchmark()
{
    printf("[benchmark] 10 s acid line, 64-frame blocks, best of 5 (informational)\n");
    for (int variant = 0; variant < 2; ++variant) {
        const int rate = 48000;
        AcidParams p;
        if (variant == 1) {
            p.wave = 1;
            p.drive = 0.7f;
            p.reso = 1.0f;
        }
        const std::vector<TEv> ev = acidLineEvents(rate, 6);   // 12 bars = 22 s
        const size_t frames = (size_t)10 * rate;
        std::vector<float> buf(64);
        double best_ns = 1e30, best_tsc = 0, sink = 0;
        for (int rep = 0; rep < 5; ++rep) {
            AcidVoice v;
            v.init(rate);
            size_t k = 0;
            const auto t0 = std::chrono::steady_clock::now();
#ifdef HAVE_RDTSC
            const unsigned long long c0 = __rdtsc();
#endif
            for (size_t pos = 0; pos < frames; pos += 64) {
                while (k < ev.size() && ev[k].at < pos + 64) {
                    AcidEvent e = ev[k].e;
                    e.offset = (uint32_t)(ev[k].at - pos);
                    v.schedule(e);
                    ++k;
                }
                for (float& x : buf) x = 0.0f;
                v.render(buf.data(), 64, p);
                sink += buf[7];
            }
#ifdef HAVE_RDTSC
            const double tsc = (double)(__rdtsc() - c0) / frames;
#else
            const double tsc = 0;
#endif
            const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / frames;
            if (ns < best_ns) {
                best_ns = ns;
                best_tsc = tsc;
            }
        }
        printf("  %s: %.1f ns/sample on the PC (%.2f %% of one core at 48 kHz), %.0f TSC cycles/sample%s\n",
               variant ? "square + drive + reso 1" : "default saw            ", best_ns, best_ns * 48000.0 / 1e7, best_tsc,
               sink == 12345.0 ? " " : "");
    }
}

int main()
{
    testAcidLine(48000);
    testAcidLine(44100);
    testRates();
    testSlide();
    testAccent();
    testCutoff();
    testEnvelopes();
    testEventsAndQueue();
    testClicks();
    testTorture(48000);
    testTorture(44100);
    testPanic();
    testDriveAndWave();
    benchmark();
    if (g_fail) printf("test_acid: %d of %d checks FAILED\n", g_fail, g_checks);
    else printf("test_acid: all %d checks passed\n", g_checks);
    return g_fail ? 1 : 0;
}
