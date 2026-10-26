// Build + run (Git Bash, project root; or: bash tools/run_host_tests.sh synth once the suite is listed):
// export ZIG_GLOBAL_CACHE_DIR=$PWD/host/.zig-cache/global ZIG_LOCAL_CACHE_DIR=$PWD/host/.zig-cache/local; zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -Wall -Wextra -Ifirmware/main -Ihost host/tests/test_synth.cpp firmware/main/core/synth.cpp firmware/main/core/acid.cpp -o host/bin/test_synth.exe && host/bin/test_synth.exe
// SYNTH (core/synth.cpp): note on/off and exact silence, event offsets and the queue, pitch at
// 48 / 44.1 kHz, POLY allocation (free voice, oldest released, oldest held, same-note retrigger),
// MONO legato / glide / last-note priority, UNISON voices and stereo, filter brightness / HPF /
// bounded resonance, envelope times, LFO depths, tempo sync and fade, sync / ring / cross-mod,
// band-limiting (aliases vs naive waveforms), block-size determinism (1, 37, 64, 128 frames:
// bit-identical), random parameter sweeps, panic,
// factory patches, host/out/synth_demo.wav (every patch playing a phrase) and an informational
// CPU figure next to the ACID voice.
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
#include "core/synth.h"

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

static const double kPiD = 3.14159265358979323846;

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
// Amplitude of a sine at f in [a, b) (Hann-windowed Goertzel).
static double tone(const std::vector<float>& y, size_t a, size_t b, double f, int rate)
{
    b = std::min(b, y.size());
    const size_t n = b - a;
    const double w = 2.0 * kPiD * f / rate, c = 2.0 * cos(w);
    double s1 = 0, s2 = 0, wsum = 0;
    for (size_t i = 0; i < n; ++i) {
        const double h = 0.5 - 0.5 * cos(2.0 * kPiD * (double)i / (double)(n - 1));
        wsum += h;
        const double s0 = h * y[a + i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double pw = s1 * s1 + s2 * s2 - c * s1 * s2;
    return 2.0 * sqrt(pw > 0 ? pw : 0) / wsum;
}
static double midiHz(double n) { return 440.0 * pow(2.0, (n - 69.0) / 12.0); }

static void writeWav(const char* path, const std::vector<float>& l, const std::vector<float>& r, int rate)
{
    FILE* f = fopen(path, "wb");
    if (!f) {
        printf("  cannot write %s\n", path);
        return;
    }
    const uint32_t frames = (uint32_t)std::min(l.size(), r.size());
    const uint32_t data = frames * 4;
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f);
    u32(36 + data);
    fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);
    u16(2);
    u32((uint32_t)rate);
    u32((uint32_t)rate * 4);
    u16(4);
    u16(16);
    fwrite("data", 1, 4, f);
    u32(data);
    for (uint32_t i = 0; i < frames; ++i) {
        for (float v : {l[i], r[i]}) {
            const float c = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
            const int16_t s = (int16_t)lrintf(c * 32767.0f);
            fwrite(&s, 2, 1, f);
        }
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

enum { E_ON = 0, E_OFF, E_ALLOFF, E_PANIC };
struct Ev {
    uint64_t at;
    int type;
    int note;
    int vel;
};
static Ev on(uint64_t at, int note, int vel = 100) { return Ev{at, E_ON, note, vel}; }
static Ev off(uint64_t at, int note) { return Ev{at, E_OFF, note, 0}; }
static Ev allOff(uint64_t at) { return Ev{at, E_ALLOFF, 0, 0}; }
static Ev panicEv(uint64_t at) { return Ev{at, E_PANIC, 0, 0}; }   // applies at the start of its block

// Per-block render (block 0 = random sizes 1..256): every event lands in its block with
// offset = at - block start. Optional per-block callback (knob automation, state traces).
struct Renderer {
    Synth s;
    int rate;
    std::vector<float> L, R;
    int qfail = 0;
    float bpm = 120.0f;
    explicit Renderer(int r = 48000) : rate(r) { s.init(r); }
    template <typename F>
    void run(std::vector<Ev> ev, size_t frames, SynthParams p, int block, F perBlock)
    {
        std::stable_sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.at < b.at; });
        size_t k = 0;
        const size_t start = L.size();
        L.resize(start + frames, 0.0f);
        R.resize(start + frames, 0.0f);
        size_t pos = 0;
        while (pos < frames) {
            int n = block > 0 ? block : (int)(1 + rnd() % 256);
            if (pos + (size_t)n > frames) n = (int)(frames - pos);
            const uint64_t b0 = start + pos;
            perBlock(p, b0);
            while (k < ev.size() && ev[k].at < b0 + (uint64_t)n) {
                const Ev& e = ev[k];
                const int off_ = e.at >= b0 ? (int)(e.at - b0) : 0;
                bool ok = true;
                switch (e.type) {
                case E_ON: ok = s.noteOn((uint8_t)e.note, (uint8_t)e.vel, off_); break;
                case E_OFF: ok = s.noteOff((uint8_t)e.note, off_); break;
                case E_ALLOFF: ok = s.allNotesOff(off_); break;
                default: s.panic(); break;
                }
                if (!ok) ++qfail;
                ++k;
            }
            s.render(L.data() + start + pos, R.data() + start + pos, n, p, bpm);
            pos += (size_t)n;
        }
    }
    void run(const std::vector<Ev>& ev, size_t frames, const SynthParams& p, int block = 64)
    {
        run(ev, frames, p, block, [](SynthParams&, uint64_t) {});
    }
    size_t ms(double t) const { return (size_t)(t * 0.001 * rate + 0.5); }
    size_t sec(double t) const { return (size_t)(t * rate + 0.5); }
};

// A clean, open patch for pitch measurements: OSC1 TRIANGLE COLOR 0.25 (= sine), filter open.
static SynthParams sinePatch()
{
    SynthParams p;
    p.osc1_wave = kSynthTriangle;
    p.osc1_color = 0.25f;
    p.cutoff = 1.0f;
    p.reso = 0.0f;
    p.env_amt = 0.0f;
    p.key_follow = 0.0f;
    p.a_attack = 0.002f;
    p.a_sustain = 1.0f;
    p.spread = 0.0f;
    return p;
}

// ------------------------------------------------------------------ tests

static void testBasic()
{
    printf("[basic] note on -> sound, note off -> exact silence after the release\n");
    SynthParams p;
    p.a_release = 0.1f;
    Renderer r;
    r.run({on(0, 60, 100), off(24000, 60)}, 48000, p);
    CHECK(r.qfail == 0 && allFinite(r.L) && allFinite(r.R), "finite, all events queued");
    const double rm = rms(r.L, 2400, 24000), pk = std::max(peakAbs(r.L, 0, 24000), peakAbs(r.R, 0, 24000));
    CHECK(rm > 0.02, "note sounds: rms %.3f (%.1f dBFS)", rm, db(rm));
    CHECK(pk > 0.2 && pk < 0.8, "default patch, one note: peak %.3f (%.1f dBFS) next to the drums", pk, db(pk));
    size_t last = 0;
    for (size_t i = 0; i < r.L.size(); ++i)
        if (r.L[i] != 0.0f || r.R[i] != 0.0f) last = i;
    const double tail_ms = (double)(last - 24000) * 1000.0 / 48000.0;
    CHECK(tail_ms > 90.0 && tail_ms <= 100.4, "last non-zero sample %.2f ms after NoteOff (release 100 ms), then exactly 0", tail_ms);
    CHECK(!r.s.active() && r.s.activeVoices() == 0, "voice free after the release");
    CHECK(peakAbs(r.L, 26400, 26400 + 480) > 1e-4, "still releasing 50 ms after NoteOff (%.4f)", peakAbs(r.L, 26400, 26880));
}

static void testEvents()
{
    printf("[events] offsets, queue, late offsets, velocity 0\n");
    SynthParams p;
    {
        Synth s;
        s.init(48000);
        CHECK(s.noteOn(60, 100, 37), "queued");
        std::vector<float> l(64, 0.0f), rr(64, 0.0f);
        s.render(l.data(), rr.data(), 64, p, 120.0f);
        bool silent = true, sound = false;
        for (int i = 0; i < 37; ++i) silent = silent && l[i] == 0.0f && rr[i] == 0.0f;
        for (int i = 38; i < 64; ++i) sound = sound || l[i] != 0.0f;
        CHECK(silent && sound, "NoteOn at offset 37 starts at frame 37 (silent before: %d, sound after: %d)", (int)silent, (int)sound);
        int ok = 0;
        for (int i = 0; i < Synth::kQueue + 5; ++i) ok += s.noteOn((uint8_t)(40 + (i % 8)), 100, 500) ? 1 : 0;
        CHECK(ok == Synth::kQueue, "queue holds %d events (accepted %d)", Synth::kQueue, ok);
        s.render(l.data(), rr.data(), 64, p, 120.0f);
        CHECK(s.noteOn(60, 100, 0) && allFinite(l), "late offsets clamped to the block end, queue empty after render()");
        s.panic();
        s.render(l.data(), rr.data(), 64, p, 120.0f);
    }
    {
        Renderer r;
        r.run({on(0, 60, 100), on(4800, 60, 0)}, 4800 + 9600, p);   // velocity 0 = NoteOff
        CHECK(!r.s.voiceGate(0), "NoteOn with velocity 0 closes the gate");
    }
}

static void testPitch()
{
    printf("[pitch] 48 / 44.1 kHz, OSC2 coarse / fine, SUB octaves\n");
    for (int rate : {48000, 44100}) {
        Renderer r(rate);
        r.run({on(0, 69)}, (size_t)(0.5 * rate), sinePatch());
        const double f = zcFreq(r.L, r.sec(0.1), r.sec(0.5), rate);
        CHECK(fabs(f / 440.0 - 1.0) < 0.002, "%d Hz: A4 measures %.2f Hz", rate, f);
    }
    struct Case {
        const char* what;
        double hz;
        SynthParams p;
    };
    SynthParams a = sinePatch();
    a.osc1_level = 0.0f;
    a.osc2_wave = kSynthTriangle;
    a.osc2_color = 0.25f;
    a.osc2_level = 1.0f;
    a.osc2_coarse = 7.0f;
    SynthParams b = a;
    b.osc2_fine = 50.0f;
    SynthParams c = sinePatch();
    c.osc1_level = 0.0f;
    c.sub_level = 1.0f;
    c.cutoff = 0.5f;   // 640 Hz: the square's fundamental for a clean zero crossing
    SynthParams d = c;
    d.sub_oct = 2;
    SynthParams e = sinePatch();
    e.tune = -12.0f;
    e.bend = 2.0f;
    const Case cases[] = {
        {"OSC2 +7 semitones", midiHz(69 + 7), a}, {"OSC2 +7 st +50 cents", midiHz(69 + 7.5), b},
        {"SUB -1 octave", midiHz(69 - 12), c},     {"SUB -2 octaves", midiHz(69 - 24), d},
        {"TUNE -12, BEND +2", midiHz(69 - 10), e},
    };
    for (const Case& k : cases) {
        Renderer r;
        r.run({on(0, 69)}, 24000, k.p);
        const double f = zcFreq(r.L, 4800, 24000, 48000);
        CHECK(fabs(f / k.hz - 1.0) < 0.003, "%s: %.2f Hz (expected %.2f)", k.what, f, k.hz);
    }
}

static void testPoly()
{
    printf("[poly] 4 voices, stealing, same-note retrigger, free voice first\n");
    SynthParams p;
    p.a_attack = 0.005f;
    p.a_sustain = 1.0f;
    p.a_release = 1.0f;
    p.spread = 0.0f;
    Renderer r;
    auto notes = [&]() {
        std::vector<int> n;
        for (int v = 0; v < kSynthVoices; ++v) n.push_back(r.s.voiceActive(v) ? r.s.voiceNote(v) : -1);
        return n;
    };
    auto voiceOf = [&](int note) {
        for (int v = 0; v < kSynthVoices; ++v)
            if (r.s.voiceActive(v) && r.s.voiceNote(v) == note) return v;
        return -1;
    };
    r.run({on(0, 48), on(10, 55), on(20, 60), on(30, 64)}, r.sec(0.3), p);
    CHECK(r.s.activeVoices() == 4, "4 notes -> 4 active voices (%d)", r.s.activeVoices());
    {
        const double t48 = tone(r.L, r.sec(0.05), r.sec(0.3), midiHz(48), 48000), t55 = tone(r.L, r.sec(0.05), r.sec(0.3), midiHz(55), 48000);
        const double t60 = tone(r.L, r.sec(0.05), r.sec(0.3), midiHz(60), 48000), t64 = tone(r.L, r.sec(0.05), r.sec(0.3), midiHz(64), 48000);
        const double t50 = tone(r.L, r.sec(0.05), r.sec(0.3), midiHz(50), 48000);
        const double lo = std::min(std::min(t48, t55), std::min(t60, t64));
        CHECK(lo > 10.0 * t50, "all 4 fundamentals present (weakest %.4f) vs an unplayed pitch %.5f", lo, t50);
    }
    // (each run() renders from the current end; its events lie inside that window)
    const int v55 = voiceOf(55), v48 = voiceOf(48), v60 = voiceOf(60);
    r.run({off(r.sec(0.3), 55), on(r.sec(0.35), 67)}, r.sec(0.1), p);
    CHECK(voiceOf(67) == v55 && voiceOf(48) == v48 && voiceOf(60) == v60 && voiceOf(64) >= 0,
          "5th note takes the released voice (55 was on %d, 67 is on %d), the held ones stay", v55, voiceOf(67));
    r.run({on(r.sec(0.42), 71)}, r.sec(0.08), p);
    CHECK(voiceOf(71) == v48 && voiceOf(48) < 0, "6th note (all held) steals the oldest held voice (48 was on %d, 71 is on %d)", v48,
          voiceOf(71));
    std::vector<int> before = notes();
    r.run({on(r.sec(0.5), 60)}, r.sec(0.1), p);
    int count60 = 0;
    for (int n : notes()) count60 += n == 60 ? 1 : 0;
    CHECK(count60 == 1 && voiceOf(60) == v60 && notes() == before && r.s.activeVoices() == 4 && r.s.voiceAmpEnv(v60) > 0.99f,
          "repeated note 60 retriggers its own voice (%d), no other voice changes", voiceOf(60));
    {   // retrigger: attack from the current level, no click
        const size_t at = r.sec(0.5);
        const double st0 = maxStep(r.L, at - r.ms(40), at - 10), st = maxStep(r.L, at - 10, at + r.ms(10));
        CHECK(st < 1.6 * st0, "same-note retrigger without a click (step %.4f vs %.4f)", st, st0);
    }
    r.run({allOff(r.sec(0.6))}, r.sec(1.5), p);
    CHECK(r.s.activeVoices() == 0 && peakAbs(r.L, r.L.size() - 4800, r.L.size()) == 0.0, "all notes off -> idle and exact silence");
    // free voice before a released one: A B C held, B released (still sounding), D -> the 4th voice
    const uint64_t t0 = r.L.size();
    r.run({on(t0, 50), on(t0 + 10, 53), on(t0 + 20, 57), off(t0 + r.sec(0.1), 53), on(t0 + r.sec(0.2), 62)}, r.sec(0.3), p);
    CHECK(r.s.activeVoices() == 4 && voiceOf(53) >= 0 && !r.s.voiceGate(voiceOf(53)) && voiceOf(62) >= 0,
          "new note takes the free voice, the releasing one keeps sounding (53 on %d, 62 on %d)", voiceOf(53), voiceOf(62));
}

static void testMono()
{
    printf("[mono] legato slide, retrigger without legato, last-note priority\n");
    auto trace = [](SynthParams p, std::vector<Ev> ev, double len, std::vector<float>& env, std::vector<float>& pitch,
                    std::vector<float>* out, int* voices_at_05) {
        Renderer r;
        r.run(ev, r.sec(len), p, 16, [&](SynthParams&, uint64_t b0) {
            env.push_back(r.s.voiceAmpEnv(0));
            pitch.push_back(r.s.voicePitch(0));
            if (b0 == 24000 && voices_at_05) *voices_at_05 = r.s.activeVoices();
        });
        if (out) *out = r.L;
    };
    SynthParams p = sinePatch();
    p.voice_mode = kSynthMono;
    p.legato = 1;
    p.glide = 0.05f;
    p.a_attack = 0.005f;
    p.a_decay = 0.05f;
    p.a_sustain = 0.6f;
    const std::vector<Ev> ev = {on(0, 48), on(14400, 60), off(38400, 60), off(38400, 48)};
    std::vector<float> env, pitch, y;
    int nv = 0;
    trace(p, ev, 1.0, env, pitch, &y, &nv);
    auto blk = [](double t) { return (size_t)(t * 48000.0 / 16.0); };
    float emax = 0;
    for (size_t i = blk(0.3); i < blk(0.7); ++i) emax = std::max(emax, env[i]);
    CHECK(nv == 1, "MONO uses one voice (%d)", nv);
    CHECK(fabsf(env[blk(0.29)] - 0.6f) < 1e-3f && emax <= 0.6001f, "legato: no envelope retrigger (level %.3f before, max %.4f after)",
          env[blk(0.29)], emax);
    const float pg = pitch[blk(0.33)], pe = pitch[blk(0.7)];
    CHECK(pg > 49.0f && pg < 59.0f && fabsf(pe - 60.0f) < 0.01f, "legato glides: %.2f 30 ms in, %.3f at 0.7 s", pg, pe);
    const double f = zcFreq(y, 31200, 37000, 48000);
    CHECK(fabs(f / midiHz(60) - 1.0) < 0.003, "after the slide %.2f Hz (C4 %.2f)", f, midiHz(60));

    SynthParams q = p;
    q.legato = 0;
    env.clear();
    pitch.clear();
    trace(q, ev, 1.0, env, pitch, nullptr, nullptr);
    emax = 0;
    for (size_t i = blk(0.3); i < blk(0.4); ++i) emax = std::max(emax, env[i]);
    CHECK(emax > 0.95f && pitch[blk(0.33)] < 59.0f, "LEGATO off: envelope retriggers (max %.3f) and GLIDE still glides (%.2f)", emax,
          pitch[blk(0.33)]);

    q = p;
    q.glide = 0.0f;
    env.clear();
    pitch.clear();
    trace(q, ev, 1.0, env, pitch, nullptr, nullptr);
    emax = 0;
    for (size_t i = blk(0.3); i < blk(0.7); ++i) emax = std::max(emax, env[i]);
    CHECK(pitch[blk(0.3) + 2] == 60.0f && emax <= 0.6001f, "LEGATO + GLIDE 0: pitch jumps (%.2f), no retrigger (max %.4f)",
          pitch[blk(0.3) + 2], emax);

    env.clear();
    pitch.clear();
    trace(p, {on(0, 48), on(9600, 60), off(19200, 60), off(38400, 48)}, 1.0, env, pitch, nullptr, nullptr);
    float emax2 = 0;
    for (size_t i = blk(0.4); i < blk(0.7); ++i) emax2 = std::max(emax2, env[i]);
    CHECK(fabsf(pitch[blk(0.75)] - 48.0f) < 0.02f && emax2 <= 0.6001f,
          "last-note priority: releasing the top note slides back to the held one (%.3f), no retrigger", pitch[blk(0.75)]);

    // UNISON
    SynthParams u;
    u.voice_mode = kSynthUnison;
    u.spread = 1.0f;
    u.uni_detune = 0.5f;
    Renderer a;
    a.run({on(0, 57)}, 24000, u);
    double dlr = 0;
    for (size_t i = 0; i < a.L.size(); ++i) dlr += (double)(a.L[i] - a.R[i]) * (a.L[i] - a.R[i]);
    dlr = sqrt(dlr / a.L.size());
    CHECK(a.s.activeVoices() == 4, "UNISON: one note uses %d voices", a.s.activeVoices());
    CHECK(dlr > 0.1 * rms(a.L, 0, a.L.size()), "UNISON SPREAD 1: L != R (rms(L-R) %.3f, rms(L) %.3f)", dlr, rms(a.L, 0, a.L.size()));
    u.spread = 0.0f;
    Renderer b;
    b.run({on(0, 57)}, 24000, u);
    double md = 0;
    for (size_t i = 0; i < b.L.size(); ++i) md = std::max(md, (double)fabsf(b.L[i] - b.R[i]));
    CHECK(md == 0.0, "UNISON SPREAD 0: L == R exactly (max diff %.3g)", md);
    u.uni_detune = 0.0f;
    Renderer c;
    c.run({on(0, 57)}, 24000, u);
    double dd = 0;
    for (size_t i = 0; i < c.L.size(); ++i) dd = std::max(dd, (double)fabsf(c.L[i] - b.L[i]));
    CHECK(dd > 0.05, "UNI DETUNE changes the sound (max diff %.3f)", dd);
    printf("  unison peak %.3f, single-voice default peak for comparison below\n", peakAbs(a.L, 0, a.L.size()));
}

static void testFilter()
{
    printf("[filter] brightness vs cutoff, HPF, bounded resonance\n");
    for (uint8_t ft : {(uint8_t)kSynthLp24, (uint8_t)kSynthLp12}) {
        double br[3];
        const float cuts[3] = {0.35f, 0.55f, 0.75f};
        for (int i = 0; i < 3; ++i) {
            SynthParams p;
            p.filter_type = ft;
            p.cutoff = cuts[i];
            p.env_amt = 0.0f;
            p.key_follow = 0.0f;
            Renderer r;
            r.run({on(0, 48)}, 19200, p);
            br[i] = brightness(r.L, 2400, 19200);
        }
        CHECK(br[0] < br[1] && br[1] < br[2], "%s: brightness %.4f < %.4f < %.4f", ft == kSynthLp24 ? "LPF -24" : "LPF -12", br[0], br[1],
              br[2]);
    }
    {
        SynthParams p;
        p.cutoff = 1.0f;
        p.env_amt = 0.0f;
        p.key_follow = 0.0f;
        p.reso = 0.0f;
        Renderer a;
        a.run({on(0, 36)}, 24000, p);
        p.filter_type = kSynthHp12;
        p.cutoff = 0.55f;   // ~900 Hz
        Renderer b;
        b.run({on(0, 36)}, 24000, p);
        const double fa = tone(a.L, 4800, 24000, midiHz(36), 48000), fb = tone(b.L, 4800, 24000, midiHz(36), 48000);
        const double ha = tone(a.L, 4800, 24000, midiHz(36) * 40, 48000), hb = tone(b.L, 4800, 24000, midiHz(36) * 40, 48000);
        CHECK(fb < 0.01 * fa, "HPF -12 at ~900 Hz removes the 65 Hz fundamental: %.1f dB", db(fb / fa));
        CHECK(hb > 0.5 * ha, "HPF keeps the 40th harmonic (2.6 kHz): %.1f dB", db(hb / ha));
    }
    {   // LPF -24 self-oscillates at RESO 1 (a little noise starts it), bounded
        SynthParams p;
        p.osc1_level = 0.0f;
        p.noise_level = 0.02f;
        p.cutoff = 0.6f;
        p.env_amt = 0.0f;
        p.key_follow = 0.0f;
        p.reso = 1.0f;
        Renderer a;
        a.run({on(0, 60)}, 48000, p);
        p.reso = 0.5f;
        Renderer b;
        b.run({on(0, 60)}, 48000, p);
        const double ra = rms(a.L, 24000, 48000), rb = rms(b.L, 24000, 48000);
        const double f = zcFreq(a.L, 24000, 48000, 48000);
        CHECK(ra > 20.0 * rb && ra < 1.0, "LPF -24 RESO 1 self-oscillates: rms %.3f vs %.5f at RESO 0.5, %.0f Hz (cutoff %.0f Hz)", ra, rb,
              f, synthCutoffHz(0.6f));
    }
    for (uint8_t ft : {(uint8_t)kSynthLp24, (uint8_t)kSynthLp12, (uint8_t)kSynthHp12}) {
        // 10 s at RESO 1 with everything loud: chords, cutoff sweeps, drive
        Renderer r;
        std::vector<Ev> ev;
        uint64_t t = 0;
        while (t < 470000) {
            const int root = 30 + (int)(rnd() % 50);
            for (int k = 0; k < 4; ++k) ev.push_back(on(t + (uint64_t)k, root + 4 * k, 40 + (int)(rnd() % 88)));
            const uint64_t len = 2000 + rnd() % 20000;
            for (int k = 0; k < 4; ++k) ev.push_back(off(t + len, root + 4 * k));
            t += 1000 + rnd() % 24000;
        }
        SynthParams p;
        p.filter_type = ft;
        p.reso = 1.0f;
        p.osc2_level = 1.0f;
        p.osc2_wave = kSynthSquare;
        p.osc2_coarse = 7.0f;
        p.sub_level = 1.0f;
        p.noise_level = 0.3f;
        p.drive = 1.0f;
        p.env_amt = 1.0f;
        p.key_follow = 1.0f;
        p.level = 1.0f;
        double ph = 0;
        r.run(ev, 480000, p, 0, [&](SynthParams& q, uint64_t) {
            ph += 0.003;
            q.cutoff = (float)(0.5 + 0.5 * sin(ph * 11.0));
        });
        const double pk = std::max(peakAbs(r.L, 0, r.L.size()), peakAbs(r.R, 0, r.R.size()));
        CHECK(allFinite(r.L) && allFinite(r.R) && pk < 6.0, "%s RESO 1, 10 s, 4-note chords, drive 1, level 1: finite, peak %.3f",
              ft == kSynthLp24 ? "LPF -24" : (ft == kSynthLp12 ? "LPF -12" : "HPF -12"), pk);
    }
}

static void testEnvelopes()
{
    printf("[envelopes] ADSR times (amp per sample, filter per control period)\n");
    SynthParams p;
    p.a_attack = 0.1f;
    p.a_decay = 0.2f;
    p.a_sustain = 0.5f;
    p.a_release = 0.3f;
    p.f_attack = 0.05f;
    p.f_decay = 0.1f;
    p.f_sustain = 0.3f;
    p.f_release = 0.2f;
    Renderer r;
    std::vector<float> ae, fe;
    std::vector<int> act;
    r.run({on(0, 60), off(r.sec(0.6), 60)}, r.sec(1.2), p, 16, [&](SynthParams&, uint64_t) {
        ae.push_back(r.s.voiceAmpEnv(0));
        fe.push_back(r.s.voiceFilterEnv(0));
        act.push_back(r.s.voiceActive(0) ? 1 : 0);
    });
    auto firstAt = [&](size_t from, auto pred) {
        for (size_t i = from; i < ae.size(); ++i)
            if (pred(i)) return (double)i * 16.0 / 48000.0;
        return -1.0;
    };
    const double ta = firstAt(0, [&](size_t i) { return ae[i] >= 0.9999f; });
    const double td = firstAt((size_t)(ta * 3000), [&](size_t i) { return ae[i] <= 0.5f + 1e-6f && ae[i] > 0.0f; });
    const double tr = firstAt(40, [&](size_t i) { return act[i] == 0; });
    CHECK(fabs(ta - 0.1) < 0.002, "amp attack 100 ms: at 1.0 after %.1f ms", ta * 1000);
    CHECK(fabs(td - 0.3) < 0.003, "amp decay 200 ms to sustain 0.5: reached at %.1f ms", td * 1000);
    CHECK(fabs(tr - 0.9) < 0.003, "amp release 300 ms: voice free %.1f ms after NoteOff", (tr - 0.6) * 1000);
    CHECK(fabsf(ae[(size_t)(0.5 * 3000)] - 0.5f) < 1e-6f, "sustain level %.4f", ae[(size_t)(0.5 * 3000)]);
    {   // halfway through the release (exponential): well below half, above zero
        const float h = ae[(size_t)(0.75 * 3000)];
        CHECK(h > 0.0f && h < 0.25f * 0.5f, "release is exponential: %.4f of 0.5 halfway", h);
    }
    const double fa = firstAt(0, [&](size_t i) { return fe[i] >= 0.9999f; });
    const double fd = firstAt((size_t)(fa * 3000), [&](size_t i) { return fe[i] <= 0.3f + 1e-6f && fe[i] > 0.0f; });
    const double fr = firstAt(40, [&](size_t i) { return fe[i] <= 0.0f; });
    CHECK(fabs(fa - 0.05) < 0.0015, "filter attack 50 ms: %.1f ms", fa * 1000);
    CHECK(fabs(fd - 0.15) < 0.003, "filter decay 100 ms to 0.3: %.1f ms", fd * 1000);
    CHECK(fabs(fr - 0.8) < 0.004, "filter release 200 ms: at 0 %.1f ms after NoteOff", (fr - 0.6) * 1000);
    {   // a click-free start from silence
        SynthParams q;
        q.a_attack = 0.0005f;
        Renderer a;
        a.run({on(0, 48)}, 4800, q);
        const double first = peakAbs(a.L, 0, 6), later = peakAbs(a.L, 0, 4800);
        CHECK(first < 0.3 * later, "fastest attack (0.5 ms): first 6 samples %.4f vs note peak %.3f", first, later);
    }
}

static void testLfo()
{
    printf("[lfo] pitch / cutoff / amp depth, tempo sync, fade, key trigger\n");
    {
        SynthParams p = sinePatch();
        p.lfo_wave = kSynthLfoTriangle;
        p.lfo_rate = 2.0f;
        p.lfo_pitch = (float)sqrt(2.0 / 12.0);   // +-2 semitones
        Renderer r;
        r.run({on(0, 69)}, r.sec(1.2), p);
        double fmin = 1e9, fmax = 0;
        for (size_t a = r.sec(0.1); a + r.ms(20) < r.sec(1.15); a += r.ms(10)) {
            const double f = zcFreq(r.L, a, a + r.ms(20), 48000);
            fmin = std::min(fmin, f);
            fmax = std::max(fmax, f);
        }
        CHECK(fmax / fmin > 1.2 && fmax / fmin < 1.3, "LFO -> pitch +-2 st: frequency %.1f .. %.1f Hz (x%.3f, expected ~x1.26)", fmin,
              fmax, fmax / fmin);
    }
    auto cutoffTrace = [](SynthParams p, float bpm, double len, std::vector<float>& fc) {
        Renderer r;
        r.bpm = bpm;
        r.run({on(0, 60)}, r.sec(len), p, 16, [&](SynthParams&, uint64_t) { fc.push_back(r.s.voiceCutoffHz(0)); });
    };
    {
        SynthParams p;
        p.env_amt = 0.0f;
        p.key_follow = 0.0f;
        p.cutoff = 0.5f;
        p.lfo_wave = kSynthLfoSawDown;
        p.lfo_rate = 4.0f;
        p.lfo_cutoff = 0.5f;   // +-2 octaves
        std::vector<float> fc;
        cutoffTrace(p, 120.0f, 1.0, fc);
        const float mn = *std::min_element(fc.begin() + 30, fc.end()), mx = *std::max_element(fc.begin() + 30, fc.end());
        CHECK(mx / mn > 12.0f && mx / mn < 17.0f, "LFO -> cutoff +-2 oct: %.0f .. %.0f Hz (x%.1f, expected <16)", mn, mx, mx / mn);
    }
    {
        SynthParams p;
        p.lfo_wave = kSynthLfoSquare;
        p.lfo_rate = 5.0f;
        p.lfo_amp = 1.0f;
        p.a_sustain = 1.0f;
        Renderer r;
        r.run({on(0, 57)}, r.sec(1.0), p);
        double mn = 1e9, mx = 0;
        for (size_t a = r.sec(0.1); a + r.ms(10) < r.sec(1.0); a += r.ms(10)) {
            const double v = rms(r.L, a, a + r.ms(10));
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
        CHECK(mn < 0.02 * mx, "LFO -> amp depth 1 (square): 10 ms rms %.4f .. %.4f", mn, mx);
    }
    auto divIndex = [](const char* name) {
        for (int i = 1; i <= kSynthLfoDivs; ++i)
            if (strcmp(synthLfoDivName(i), name) == 0) return i;
        return 0;
    };
    for (int k = 0; k < 2; ++k) {   // tempo sync: period from the saw's wraps in the cutoff trace
        const float bpm = k == 0 ? 120.0f : 100.0f;
        const char* dn = k == 0 ? "1/4" : "1/8T";
        const double expect = 60.0 / bpm * synthLfoDivBeats(divIndex(dn));
        SynthParams p;
        p.env_amt = 0.0f;
        p.key_follow = 0.0f;
        p.cutoff = 0.5f;
        p.lfo_wave = kSynthLfoSawDown;
        p.lfo_sync = (uint8_t)divIndex(dn);
        p.lfo_cutoff = 0.5f;
        std::vector<float> fc;
        cutoffTrace(p, bpm, 3.0, fc);
        std::vector<double> jumps;
        for (size_t i = 1; i < fc.size(); ++i)
            if (fc[i] > fc[i - 1] * 4.0f) jumps.push_back((double)i * 16.0 / 48000.0);
        const double per = jumps.size() > 2 ? (jumps.back() - jumps.front()) / (double)(jumps.size() - 1) : 0.0;
        CHECK(fabs(per / expect - 1.0) < 0.003, "tempo sync %s at %.0f BPM: period %.4f s (expected %.4f, %zu cycles)", dn, bpm, per,
              expect, jumps.size() - 1);
    }
    {   // fade-in
        SynthParams p;
        p.env_amt = 0.0f;
        p.key_follow = 0.0f;
        p.cutoff = 0.5f;
        p.lfo_wave = kSynthLfoTriangle;
        p.lfo_rate = 8.0f;
        p.lfo_cutoff = 0.5f;
        p.lfo_fade = 0.5f;
        std::vector<float> fc;
        cutoffTrace(p, 120.0f, 1.0, fc);
        auto range = [&](double a, double b) {
            const auto i0 = fc.begin() + (long)(a * 3000), i1 = fc.begin() + (long)(b * 3000);
            return *std::max_element(i0, i1) / *std::min_element(i0, i1);
        };
        const float early = range(0.001, 0.1), late = range(0.6, 1.0);   // (index 0: before the note)
        CHECK(early < 1.8f && late > 12.0f, "LFO FADE 0.5 s: cutoff range x%.2f in the first 100 ms, x%.1f after 0.6 s", early, late);
    }
    {   // key trigger: every note starts the LFO at phase 0 (SAW UP = -1); free: the global phase
        SynthParams p;
        p.lfo_wave = kSynthLfoSawUp;
        p.lfo_rate = 3.0f;
        p.lfo_key_trig = 1;
        Renderer r;
        float l1 = 0, l2 = 0;
        r.run({on(0, 60), on(r.sec(0.37), 64)}, r.sec(0.4), p, 16, [&](SynthParams&, uint64_t b0) {
            if (b0 == 16) l1 = r.s.voiceLfo(0);
            if (b0 == r.sec(0.37) + 16) l2 = r.s.voiceLfo(1);
        });
        CHECK(l1 < -0.99f && l2 < -0.99f, "KEY TRIG: each voice's LFO starts at its note (%.3f, %.3f)", l1, l2);
        p.lfo_key_trig = 0;
        Renderer q;
        float a = 0, b = 1;
        q.run({on(0, 60), on(q.sec(0.37), 64)}, q.sec(0.4), p, 16, [&](SynthParams&, uint64_t b0) {
            if (b0 == q.sec(0.37) + 16) {
                a = q.s.voiceLfo(0);
                b = q.s.voiceLfo(1);
            }
        });
        CHECK(a == b && a > -0.9f, "KEY TRIG off: voices share the free-running LFO (%.3f, %.3f)", a, b);
    }
    {   // random waves stay in range
        for (uint8_t w : {(uint8_t)kSynthLfoSampleHold, (uint8_t)kSynthLfoSmoothRandom}) {
            SynthParams p;
            p.lfo_wave = w;
            p.lfo_rate = 20.0f;
            p.lfo_key_trig = 1;
            Renderer r;
            float mn = 2, mx = -2;
            int changes = 0;
            float last = 0;
            r.run({on(0, 60)}, r.sec(1.0), p, 16, [&](SynthParams&, uint64_t b0) {
                const float v = r.s.voiceLfo(0);
                if (b0 > 32) {
                    mn = std::min(mn, v);
                    mx = std::max(mx, v);
                    changes += v != last ? 1 : 0;
                }
                last = v;
            });
            CHECK(mn >= -1.0f && mx <= 1.0f && mx - mn > 0.8f && (w != kSynthLfoSampleHold || (changes >= 17 && changes <= 21)),
                  "%s: %.3f .. %.3f, %d changes in 1 s at 20 Hz", w == kSynthLfoSampleHold ? "S&H" : "smooth random", mn, mx, changes);
        }
    }
}

static void testSyncRingXmod()
{
    printf("[sync / ring / xmod]\n");
    {   // hard sync: the output repeats with OSC1's period (128 samples) although OSC2 is detuned
        SynthParams p = sinePatch();
        p.osc1_wave = kSynthSaw;
        p.osc1_color = 0.0f;   // (SAW COLOR would detune OSC1's own phase)
        p.osc1_level = 0.0f;
        p.osc2_wave = kSynthSaw;
        p.osc2_level = 1.0f;
        p.osc2_coarse = 5.0f;
        p.osc2_fine = 13.0f;
        p.tune = (float)(12.0 * log2(375.0 / midiHz(66)));   // OSC1 = 375 Hz = 48000 / 128
        auto periodicity = [](const std::vector<float>& y) {
            double d = 0, s = 0;
            for (size_t i = 9600; i < 33600; ++i) {
                d += (double)(y[i] - y[i + 128]) * (y[i] - y[i + 128]);
                s += (double)y[i] * y[i];
            }
            return sqrt(d / s);
        };
        p.sync = 1;
        Renderer a;
        a.run({on(0, 66)}, 36000, p);
        p.sync = 0;
        Renderer b;
        b.run({on(0, 66)}, 36000, p);
        const double ea = periodicity(a.L), eb = periodicity(b.L);
        CHECK(ea < 0.05 && eb > 0.3, "SYNC: OSC2 repeats with OSC1's period (error %.4f synced, %.3f free)", ea, eb);
        CHECK(allFinite(a.L) && peakAbs(a.L, 0, a.L.size()) < 1.5, "SYNC output finite, peak %.3f", peakAbs(a.L, 0, a.L.size()));
        const double ba = brightness(a.L, 9600, 33600), bb = brightness(b.L, 9600, 33600);
        printf("  brightness synced %.3f, free %.3f\n", ba, bb);
    }
    {   // ring: OSC1 x OSC2 (two sines) = sum and difference tones, no originals
        SynthParams p = sinePatch();
        p.osc1_level = 0.0f;
        p.osc2_wave = kSynthTriangle;
        p.osc2_color = 0.25f;
        p.osc2_level = 1.0f;
        p.osc2_coarse = 7.0f;
        const double f1 = midiHz(57), f2 = midiHz(64);
        p.ring = 1;
        Renderer a;
        a.run({on(0, 57)}, 36000, p);
        p.ring = 0;
        Renderer b;
        b.run({on(0, 57)}, 36000, p);
        auto t = [](const std::vector<float>& y, double f) { return tone(y, 9600, 36000, f, 48000); };
        const double sum = t(a.L, f1 + f2), dif = t(a.L, f2 - f1), o1 = t(a.L, f1), o2 = t(a.L, f2);
        CHECK(std::min(sum, dif) > 20.0 * std::max(o1, o2), "RING: sum %.3f / difference %.3f tones, originals %.4f / %.4f", sum, dif, o1,
              o2);
        CHECK(t(b.L, f2) > 20.0 * t(b.L, f1 + f2), "RING off: OSC2 itself (%.3f) and no sum tone (%.4f)", t(b.L, f2), t(b.L, f1 + f2));
    }
    {   // cross-mod changes OSC1 (brighter, still finite)
        SynthParams p;
        p.cutoff = 1.0f;
        p.env_amt = 0.0f;
        p.osc2_wave = kSynthTriangle;
        p.osc2_level = 0.0f;
        p.osc2_coarse = 7.0f;
        Renderer a;
        a.run({on(0, 48)}, 24000, p);
        p.xmod = 0.6f;
        Renderer b;
        b.run({on(0, 48)}, 24000, p);
        double d = 0;
        for (size_t i = 0; i < a.L.size(); ++i) d += (double)(a.L[i] - b.L[i]) * (a.L[i] - b.L[i]);
        d = sqrt(d / a.L.size());
        CHECK(d > 0.3 * rms(a.L, 0, a.L.size()) && allFinite(b.L), "XMOD 0.6 changes the sound (rms diff %.3f of %.3f), finite", d,
              rms(a.L, 0, a.L.size()));
        p.xmod = 1.0f;
        p.sync = 1;
        p.osc2_wave = kSynthSquare;
        p.osc2_color = 1.0f;
        p.osc1_wave = kSynthTriangle;
        p.osc1_color = 1.0f;
        p.osc2_level = 1.0f;
        p.ring = 1;
        p.reso = 1.0f;
        Renderer c;
        std::vector<Ev> ev;
        for (int i = 0; i < 40; ++i) ev.push_back(on((uint64_t)i * 4800, 20 + (int)(rnd() % 90)));
        c.run(ev, 200000, p, 0);
        CHECK(allFinite(c.L) && allFinite(c.R) && peakAbs(c.L, 0, c.L.size()) < 4.0,
              "XMOD 1 + SYNC + RING + folds + RESO 1, random notes: finite, peak %.3f", peakAbs(c.L, 0, c.L.size()));
    }
}

// Power that is not at harmonics of f0, relative to the harmonics' power (Hann-windowed DFT of n
// samples from a; bins within 3 bins of a harmonic count as harmonic).
static double aliasRatioDb(const std::vector<float>& y, size_t a, size_t n, double f0, double fs)
{
    std::vector<double> w(n);
    for (size_t i = 0; i < n; ++i) w[i] = (0.5 - 0.5 * cos(2 * kPiD * (double)i / (double)(n - 1))) * y[a + i];
    double harm = 0, rest = 0;
    for (size_t k = 1; k < n / 2; ++k) {
        double re = 0, im = 0;
        const double om = 2 * kPiD * (double)k / (double)n;
        for (size_t i = 0; i < n; ++i) {
            re += w[i] * cos(om * (double)i);
            im -= w[i] * sin(om * (double)i);
        }
        const double h = (double)k * fs / (double)n / f0;
        const double dist = fabs(h - floor(h + 0.5)) * f0;   // Hz to the nearest harmonic
        (dist < 3.0 * fs / (double)n ? harm : rest) += re * re + im * im;
    }
    return 10 * log10(rest / harm);
}

static void testBandLimit()
{
    printf("[band-limiting] alias power at 2.6 kHz vs naive waveforms (PolyBLEP / PolyBLAMP signs)\n");
    const int note = 100;
    const double f0 = midiHz(note), r2 = pow(2.0, 5.3 / 12.0);
    for (int wave = 0; wave < 3; ++wave)
        for (int sync = 0; sync < 2; ++sync) {
            SynthParams p;   // one oscillator at a low level (soft clip linear), HPF at 20 Hz = open
            p.osc1_wave = (uint8_t)wave;
            p.osc1_level = sync ? 0.0f : 0.2f;
            p.osc2_wave = (uint8_t)wave;
            p.osc2_level = sync ? 0.2f : 0.0f;
            p.osc2_coarse = 5.3f;
            p.sync = (uint8_t)sync;
            p.filter_type = kSynthHp12;
            p.cutoff = 0.0f;
            p.reso = 0.0f;
            p.env_amt = 0.0f;
            p.key_follow = 0.0f;
            p.a_sustain = 1.0f;
            p.spread = 0.0f;
            Renderer r;
            r.run({on(0, note, 127)}, 24000, p);
            std::vector<float> nv(24000);   // naive: same pitch, OSC2 restarted by OSC1 for SYNC
            for (size_t i = 0; i < nv.size(); ++i) {
                double ph = (double)i * f0 / 48000.0;
                ph -= floor(ph);
                if (sync) {
                    ph *= r2;
                    ph -= floor(ph);
                }
                nv[i] = (float)(wave == 0 ? 2 * ph - 1 : (wave == 1 ? (ph < 0.5 ? 1 : -1) : (ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph)));
            }
            const double s = aliasRatioDb(r.L, 12000, 4096, f0, 48000), n = aliasRatioDb(nv, 12000, 4096, f0, 48000);
            CHECK(n - s > 10.0, "%s%s: aliases %.1f dB under the harmonics (naive %.1f dB): %.1f dB better",
                  wave == 0 ? "SAW" : (wave == 1 ? "SQUARE" : "TRIANGLE"), sync ? " + SYNC" : "", -s, -n, n - s);
        }
}

static std::vector<Ev> randomEvents(uint32_t seed, size_t frames, bool chords)
{
    const uint32_t keep = g_rng;
    g_rng = seed;
    std::vector<Ev> ev;
    uint64_t t = 100;
    while (t + 24000 < frames) {
        const int n = 36 + (int)(rnd() % 40);
        const int cnt = chords ? 1 + (int)(rnd() % 4) : 1;
        for (int k = 0; k < cnt; ++k) ev.push_back(on(t + (uint64_t)(rnd() % 7), n + 3 * k + (int)(rnd() % 2), 20 + (int)(rnd() % 108)));
        const uint64_t len = 500 + rnd() % 20000;
        for (int k = 0; k < cnt; ++k) ev.push_back(off(t + len + (uint64_t)k * 3, n + 3 * k + (int)(rnd() % 2)));
        if (rnd() % 11 == 0) ev.push_back(allOff(t + len / 2));
        t += 300 + rnd() % 9000;
    }
    g_rng = keep;
    return ev;
}

static void testDeterminism()
{
    printf("[determinism] block sizes 1, 37, 64, 128 -> bit-identical output\n");
    SynthParams a;
    a.osc1_color = 0.6f;
    a.osc2_wave = kSynthSquare;
    a.osc2_color = 0.4f;
    a.osc2_level = 0.7f;
    a.osc2_coarse = 7.0f;
    a.sync = 1;
    a.xmod = 0.3f;
    a.sub_level = 0.5f;
    a.sub_oct = 2;
    a.noise_level = 0.2f;
    a.drive = 0.5f;
    a.reso = 0.9f;
    a.lfo_wave = kSynthLfoSampleHold;
    a.lfo_rate = 7.0f;
    a.lfo_key_trig = 1;
    a.lfo_pitch = 0.2f;
    a.lfo_cutoff = 0.3f;
    a.lfo_amp = 0.3f;
    a.lfo_pw = 0.4f;
    a.lfo_fade = 0.3f;
    a.glide = 0.05f;
    a.vel_cutoff = 0.5f;
    SynthParams b;
    b.voice_mode = kSynthUnison;
    b.filter_type = kSynthHp12;
    b.osc1_wave = kSynthTriangle;
    b.osc1_color = 0.7f;
    b.osc2_wave = kSynthTriangle;
    b.osc2_level = 0.8f;
    b.osc2_coarse = -5.0f;
    b.ring = 1;
    b.cutoff = 0.3f;
    b.lfo_wave = kSynthLfoSmoothRandom;
    b.lfo_sync = 11;
    b.lfo_cutoff = 0.4f;
    b.legato = 1;
    b.glide = 0.08f;
    b.reso = 0.7f;
    SynthParams c;
    c.voice_mode = kSynthMono;
    c.filter_type = kSynthLp12;
    c.osc2_level = 0.6f;
    c.osc2_env = 0.6f;
    c.sync = 1;
    c.glide = 0.03f;
    c.lfo_wave = kSynthLfoSquare;
    c.lfo_key_trig = 1;
    c.lfo_pitch = 0.1f;
    c.a_sustain = 0.0f;
    c.a_decay = 0.3f;
    const SynthParams* cfg[3] = {&a, &b, &c};
    const char* names[3] = {"POLY LPF-24 sync/xmod/noise/drive/S&H", "UNISON HPF ring/fold/synced smooth random",
                            "MONO LPF-12 glide/osc2 env/square LFO"};
    for (int k = 0; k < 3; ++k) {
        const size_t frames = 3 * 48000;
        const std::vector<Ev> ev = randomEvents(0xC0FFEEu + (uint32_t)k * 77u, frames, k == 0);
        std::vector<float> refL, refR;
        bool same = true;
        double md = 0;
        for (int blk : {1, 37, 64, 128}) {
            Renderer r;
            r.bpm = 128.0f;
            r.run(ev, frames, *cfg[k], blk);
            if (blk == 1) {
                refL = r.L;
                refR = r.R;
                CHECK(allFinite(r.L) && rms(r.L, 0, frames) > 0.01 && r.qfail == 0, "%s: rms %.3f, finite", names[k], rms(r.L, 0, frames));
                continue;
            }
            const bool eq = memcmp(refL.data(), r.L.data(), frames * 4) == 0 && memcmp(refR.data(), r.R.data(), frames * 4) == 0;
            for (size_t i = 0; i < frames; ++i) md = std::max(md, (double)fabsf(refL[i] - r.L[i]));
            same = same && eq;
        }
        CHECK(same, "%s: blocks 1 / 37 / 64 / 128 bit-identical (max diff %.3g)", names[k], md);
    }
}

static void testSweeps()
{
    printf("[sweeps] random parameters every block (garbage included), 20 s\n");
    Renderer r;
    const size_t frames = 20 * 48000;
    std::vector<Ev> ev = randomEvents(0xBADC0DEu, frames, true);
    auto rf = [](float lo, float hi) { return (float)(lo + (hi - lo) * frand()); };
    r.run(ev, frames, SynthParams(), 0, [&](SynthParams& p, uint64_t) {
        if (rnd() % 8) return;   // keep a setting for a few blocks
        p.osc1_wave = (uint8_t)(rnd() % 4);
        p.osc2_wave = (uint8_t)(rnd() % 4);
        p.osc1_color = rf(-0.2f, 1.2f);
        p.osc2_color = rf(-0.2f, 1.2f);
        p.osc1_level = rf(0, 1);
        p.osc2_level = rf(0, 1);
        p.osc2_coarse = rf(-30, 30);
        p.osc2_fine = rf(-60, 60);
        p.osc2_env = rf(-1.2f, 1.2f);
        p.sync = (uint8_t)(rnd() % 2);
        p.ring = (uint8_t)(rnd() % 2);
        p.xmod = rf(0, 1.2f);
        p.sub_oct = (uint8_t)(rnd() % 4);
        p.sub_level = rf(0, 1);
        p.noise_level = rf(0, 1);
        p.tune = rf(-30, 30);
        p.bend = rf(-30, 30);
        p.filter_type = (uint8_t)(rnd() % 4);
        p.cutoff = rf(-0.1f, 1.1f);
        p.reso = rf(0, 1.3f);
        p.key_follow = rf(0, 1);
        p.env_amt = rf(-1.5f, 1.5f);
        p.vel_cutoff = rf(0, 1);
        p.f_attack = rf(-0.1f, 0.5f);
        p.f_decay = rf(0, 2);
        p.f_sustain = rf(0, 1);
        p.f_release = rf(0, 1);
        p.a_attack = rf(-0.1f, 0.3f);
        p.a_decay = rf(0, 2);
        p.a_sustain = rf(-0.1f, 1.1f);
        p.a_release = rf(0, 1);
        p.vel_amp = rf(0, 1);
        p.lfo_wave = (uint8_t)(rnd() % 8);
        p.lfo_rate = rf(0, 40);
        p.lfo_sync = (uint8_t)(rnd() % 16);
        p.lfo_fade = rf(0, 1);
        p.lfo_key_trig = (uint8_t)(rnd() % 2);
        p.lfo_pitch = rf(0, 1);
        p.lfo_cutoff = rf(0, 1);
        p.lfo_amp = rf(0, 1);
        p.lfo_pw = rf(0, 1);
        p.voice_mode = (uint8_t)(rnd() % 4);
        p.legato = (uint8_t)(rnd() % 2);
        p.glide = rf(0, 0.5f);
        p.uni_detune = rf(0, 1);
        p.spread = rf(0, 1);
        p.drive = rf(0, 1.2f);
        p.level = rf(0, 1);
        if (rnd() % 10 == 0) {   // garbage
            const float g[4] = {NAN, INFINITY, -INFINITY, 1e30f};
            p.cutoff = g[rnd() % 4];
            p.reso = g[rnd() % 4];
            p.tune = g[rnd() % 4];
            p.a_release = g[rnd() % 4];
            p.lfo_rate = g[rnd() % 4];
            p.osc2_fine = g[rnd() % 4];
            p.level = g[rnd() % 4];
            p.glide = g[rnd() % 4];
        }
    });
    const double pk = std::max(peakAbs(r.L, 0, frames), peakAbs(r.R, 0, frames));
    CHECK(allFinite(r.L) && allFinite(r.R) && pk < 16.0, "no NaN / Inf, peak %.3f (rms %.1f dBFS, %zu events)", pk,
          db(rms(r.L, 0, frames)), ev.size());
}

static void testPanic()
{
    printf("[panic] silence within 10 ms, pending note afterwards\n");
    for (int rate : {48000, 44100}) {
        SynthParams p;
        p.reso = 0.9f;
        p.drive = 0.5f;
        p.a_release = 2.0f;
        Renderer r(rate);
        r.run({on(0, 48), on(0, 55), on(0, 60)}, (size_t)(rate / 5), p, 64);
        const size_t at = r.L.size();
        r.run({panicEv(at)}, (size_t)(rate / 5), p, 64);
        const double before = peakAbs(r.L, at - r.ms(20), at), after = peakAbs(r.L, at + r.ms(10), r.L.size());
        CHECK(after == 0.0 && r.s.activeVoices() == 0, "%d Hz: exact silence from 10 ms after the panic (before %.1f dBFS)", rate,
              db(before));
        const double st = maxStep(r.L, at - 1, at + r.ms(10)), st0 = maxStep(r.L, at - r.ms(30), at - 1);
        CHECK(st < 1.2 * st0, "%d Hz: panic fade without a click (step %.4f vs %.4f)", rate, st, st0);
    }
    {
        Renderer r;
        SynthParams p;
        r.run({on(0, 48)}, 9600, p, 64);
        r.run({panicEv(9600), on(9600 + 64, 60)}, 9600, p, 64);
        int v60 = -1;
        for (int v = 0; v < kSynthVoices; ++v)
            if (r.s.voiceActive(v) && r.s.voiceNote(v) == 60) v60 = v;
        const double gap = peakAbs(r.L, 9600 + 180, 9600 + 200);
        CHECK(r.s.activeVoices() == 1 && v60 >= 0 && peakAbs(r.L, 9600 + 400, 19200) > 0.05,
              "a NoteOn during the fade plays once it is over (voice %d; %.4f at the fade end)", v60, gap);
    }
}

// Phrase per patch (beats at 120 BPM: 0.5 s); mono patches get lines, poly ones chords.
struct PNote {
    double at, len;
    int note, vel;
};
static std::vector<PNote> phrase(int i)
{
    switch (i) {
    case 0:   // BRASS: stabs
        return {{0, 0.75, 60, 110}, {0, 0.75, 63, 110}, {0, 0.75, 67, 110}, {0, 0.75, 70, 110},
                {1.0, 0.5, 56, 100}, {1.0, 0.5, 60, 100}, {1.0, 0.5, 63, 100}, {1.0, 0.5, 68, 100},
                {1.75, 1.5, 58, 120}, {1.75, 1.5, 62, 120}, {1.75, 1.5, 65, 120}, {1.75, 1.5, 70, 120}};
    case 1:   // STRINGS: two chords
        return {{0, 2.0, 55, 90}, {0, 2.0, 62, 90}, {0, 2.0, 67, 90}, {0, 2.0, 71, 90},
                {2.0, 2.0, 57, 90}, {2.0, 2.0, 64, 90}, {2.0, 2.0, 69, 90}, {2.0, 2.0, 72, 90}};
    case 2: {   // PLUCK: 16th arpeggio
        std::vector<PNote> v;
        const int arp[8] = {60, 64, 67, 72, 76, 72, 67, 64};
        for (int s = 0; s < 16; ++s) v.push_back({s * 0.25, 0.2, arp[s % 8] - (s >= 8 ? 3 : 0), 70 + 40 * (s % 2 == 0)});
        return v;
    }
    case 3:   // SYNC LEAD: legato line
        return {{0, 0.8, 60, 110}, {0.75, 0.6, 67, 110}, {1.3, 0.5, 65, 110}, {1.75, 0.3, 63, 110}, {2.0, 0.6, 65, 110},
                {2.5, 1.0, 72, 110}, {3.4, 0.5, 70, 110}};
    case 4: {   // ACID BASS: 16ths with slides (overlapping notes)
        std::vector<PNote> v;
        const int line[16] = {36, 36, 48, 36, 39, 36, 43, 44, 36, 48, 36, 46, 36, 39, 51, 36};
        const bool slide[16] = {0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0};
        for (int s = 0; s < 16; ++s) v.push_back({s * 0.25, slide[s] ? 0.3 : 0.12, line[s], (s % 4 == 0) ? 127 : 90});
        return v;
    }
    case 5:   // UNISON SAW (unison: a line)
        return {{0, 0.9, 64, 110}, {1.0, 0.45, 66, 110}, {1.5, 0.45, 67, 110}, {2.0, 0.9, 71, 110}, {3.0, 0.9, 69, 110}};
    case 6:   // BELL RING
        return {{0, 0.3, 72, 110}, {0.5, 0.3, 79, 90}, {1.0, 0.3, 76, 100}, {1.5, 0.3, 84, 80}, {2.5, 0.5, 67, 110}};
    default:   // PWM PAD
        return {{0, 3.6, 48, 90}, {0, 3.6, 55, 90}, {0, 3.6, 60, 90}, {0, 3.6, 64, 90}};
    }
}

static void testPresetsAndDemo()
{
    printf("[presets] every factory patch: chord + single note, demo WAV\n");
    int n = 0;
    const SynthPreset* ps = synthPresets(&n);
    CHECK(n >= 6 && n <= 8, "%d factory patches", n);
    for (int i = 0; i < n; ++i) {
        bool bad_name = false;
        for (const char* c = ps[i].name; *c; ++c) bad_name = bad_name || (*c >= 'a' && *c <= 'z');
        bad_name = bad_name || strstr(ps[i].name, "ROLAND") || strstr(ps[i].name, "SYSTEM");
        Renderer a, b;
        a.run({on(0, 60, 110), on(5, 64, 110), on(10, 67, 110), on(15, 71, 110), off(28800, 60), off(28800, 64), off(28800, 67),
               off(28800, 71)},
              48000, ps[i].p);
        b.run({on(0, 48, 110), off(19200, 48)}, 33600, ps[i].p);
        const double pa = std::max(peakAbs(a.L, 0, a.L.size()), peakAbs(a.R, 0, a.R.size()));
        const double pb = std::max(peakAbs(b.L, 0, b.L.size()), peakAbs(b.R, 0, b.R.size()));
        CHECK(!bad_name && allFinite(a.L) && allFinite(a.R) && allFinite(b.L) && allFinite(b.R) && pa > 0.05 && pa < 1.5 && pb > 0.05 &&
                  pb < 1.5,
              "%-10s chord peak %.3f (%.1f dBFS), single note peak %.3f (%.1f dBFS)", ps[i].name, pa, db(pa), pb, db(pb));
    }
    // demo: each patch plays its phrase (2.5 s slots, 120 BPM)
    const double slot = 2.5;
    std::vector<float> outL, outR;
    for (int i = 0; i < n; ++i) {
        Renderer s;
        std::vector<Ev> ev;
        for (const PNote& pn : phrase(i)) {
            const uint64_t t0 = (uint64_t)llround(pn.at * 0.5 * 48000), t1 = t0 + (uint64_t)llround(pn.len * 0.5 * 48000);
            ev.push_back(on(t0, pn.note, pn.vel));
            ev.push_back(off(t1, pn.note));
        }
        s.run(ev, (size_t)(slot * 48000), ps[i].p, 64);
        // short fade at the slot end (release tails of slow patches would run into the next one)
        for (size_t k = 0; k < 2400; ++k) {
            const float g = (float)(2400 - k) / 2400.0f;
            s.L[s.L.size() - 2400 + k] *= g;
            s.R[s.R.size() - 2400 + k] *= g;
        }
        for (size_t k = 0; k < s.L.size(); ++k) {   // -1.9 dB headroom for the 16-bit file
            s.L[k] *= 0.8f;
            s.R[k] *= 0.8f;
        }
        outL.insert(outL.end(), s.L.begin(), s.L.end());
        outR.insert(outR.end(), s.R.begin(), s.R.end());
        printf("  %4.1f s  %-10s peak %.3f\n", i * slot, ps[i].name, std::max(peakAbs(s.L, 0, s.L.size()), peakAbs(s.R, 0, s.R.size())));
    }
    writeWav("host/out/synth_demo.wav", outL, outR, 48000);
    CHECK(allFinite(outL) && outL.size() == (size_t)(n * slot * 48000), "wrote host/out/synth_demo.wav (%.1f s, 48 kHz stereo)",
          outL.size() / 48000.0);
}

// CPU: 10 s, 64-frame blocks, best of 5 runs (the PC is shared). Informational.
static void benchmark()
{
    printf("[benchmark] 10 s, 64-frame blocks, best of 5 (informational, host numbers are relative)\n");
    const int rate = 48000;
    const size_t frames = (size_t)10 * rate;
    struct Variant {
        const char* name;
        SynthParams p;
    };
    SynthParams a;   // typical: 2 saws, sub, LPF -24, LFO -> cutoff
    a.osc2_level = 0.8f;
    a.osc2_fine = 7.0f;
    a.sub_level = 0.4f;
    a.lfo_cutoff = 0.2f;
    a.a_sustain = 1.0f;
    SynthParams b = a;   // heavy: folds, sync, xmod, noise, drive, key-trig S&H, PW mod
    b.osc1_wave = kSynthTriangle;
    b.osc1_color = 0.6f;
    b.osc2_wave = kSynthSaw;
    b.osc2_color = 0.6f;
    b.sync = 1;
    b.xmod = 0.4f;
    b.noise_level = 0.2f;
    b.drive = 0.5f;
    b.filter_type = kSynthLp12;
    b.reso = 0.8f;
    b.lfo_wave = kSynthLfoSampleHold;
    b.lfo_key_trig = 1;
    SynthParams c = synthPresets(nullptr)[5].p;   // UNISON SAW (unison)
    c.a_sustain = 1.0f;
    const Variant vars[3] = {{"4 voices, 2 saws + sub, LPF-24", a}, {"4 voices, heavy (fold/sync/xmod/noise/drive)", b}, {"UNISON SAW (4 voices)", c}};
    for (const Variant& v : vars) {
        std::vector<float> l(64), r(64);
        double best_ns = 1e30, best_tsc = 0, sink = 0;
        for (int rep = 0; rep < 5; ++rep) {
            Synth s;
            s.init(rate);
            const int chord[4] = {48, 55, 60, 64};
            for (int k = 0; k < 4; ++k) s.noteOn((uint8_t)chord[k], 100, k);
            const auto t0 = std::chrono::steady_clock::now();
#ifdef HAVE_RDTSC
            const unsigned long long c0 = __rdtsc();
#endif
            for (size_t pos = 0; pos < frames; pos += 64) {
                for (int i = 0; i < 64; ++i) l[i] = r[i] = 0.0f;
                s.render(l.data(), r.data(), 64, v.p, 120.0f);
                sink += l[7];
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
        printf("  SYNTH %-46s %6.1f ns/sample (%5.2f %% of one PC core at 48 kHz), %4.0f TSC cycles/sample%s\n", v.name, best_ns,
               best_ns * 48000.0 / 1e7, best_tsc, sink == 12345.0 ? " " : "");
    }
    {   // ACID (one voice) for comparison: notes retriggered every 16th
        double best_ns = 1e30, best_tsc = 0, sink = 0;
        std::vector<float> buf(64);
        for (int rep = 0; rep < 5; ++rep) {
            AcidVoice v;
            v.init(rate);
            AcidParams p;
            const auto t0 = std::chrono::steady_clock::now();
#ifdef HAVE_RDTSC
            const unsigned long long c0 = __rdtsc();
#endif
            for (size_t pos = 0; pos < frames; pos += 64) {
                if (pos % 5504 == 0) {
                    AcidEvent e;
                    e.note = (uint8_t)(36 + (pos / 5504) % 12);
                    e.gate_frames = 2752;
                    v.schedule(e);
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
        printf("  ACID  %-46s %6.1f ns/sample (%5.2f %% of one PC core at 48 kHz), %4.0f TSC cycles/sample%s\n", "1 voice, default saw",
               best_ns, best_ns * 48000.0 / 1e7, best_tsc, sink == 12345.0 ? " " : "");
    }
}

int main()
{
    testBasic();
    testEvents();
    testPitch();
    testPoly();
    testMono();
    testFilter();
    testEnvelopes();
    testLfo();
    testSyncRingXmod();
    testBandLimit();
    testDeterminism();
    testSweeps();
    testPanic();
    testPresetsAndDemo();
    benchmark();
    if (g_fail) printf("test_synth: %d of %d checks FAILED\n", g_fail, g_checks);
    else printf("test_synth: all %d checks passed\n", g_checks);
    return g_fail ? 1 : 0;
}
