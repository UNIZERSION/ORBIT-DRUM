// Build (from the project root, Git Bash; stub sequencer):
//   zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -Wall -Wextra -Ifirmware/main -Ihost host/tests/test_engine.cpp firmware/main/core/engine.cpp firmware/main/core/voice.cpp firmware/main/core/fx.cpp firmware/main/core/params.cpp firmware/main/core/project.cpp firmware/main/core/sample.cpp host/platform_host.cpp host/tests/stub_seq.cpp -o host/bin/test_engine.exe
// With the real sequencer: replace host/tests/stub_seq.cpp by firmware/main/core/seq.cpp (-o host/bin/test_engine_seq.exe).
// Run: host/bin/test_engine.exe  [--bench-only]   (writes host/out/engine_demo.wav)
//
// Audio engine tests: pitch, region, reverse, loop, layers, envelopes, filter, pan, buses, FX,
// PUMP / LO-FI / WIDE, metronome, choke, stealing, retirement, preview, events, telemetry,
// limiter ceiling, NaN safety, a CPU benchmark and an 8 s demo render.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#define HAVE_RDTSC 1
#endif

#include "core/dsp/reverb.h"
#include "core/engine.h"
#include "core/fx.h"
#include "core/fxcheck.h"
#include "core/params.h"
#include "core/project.h"
#include "core/sample.h"

using namespace orbit;

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_fail;                                                      \
            printf("  FAIL %s:%d: %s | ", __FILE__, __LINE__, #cond);     \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

static const double kPiD = 3.14159265358979323846;

// ------------------------------------------------------------------ samples

template <typename F>
static Sample* mkSample(int frames, int ch, int rate, F f)
{
    Sample* s = sampleCreate((uint32_t)frames, ch, (uint32_t)rate);
    if (!s) {
        printf("sampleCreate failed\n");
        exit(2);
    }
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < ch; ++c) {
            double v = f(i, c);
            if (v > 1.0) v = 1.0;
            if (v < -1.0) v = -1.0;
            s->data[i * ch + c] = (int16_t)lround(v * 32767.0);
        }
    sampleAnalyze(s);
    return s;
}

static Sample* sine(double hz, double sec, int rate, double amp = 0.5, double phase = 0.0)
{
    return mkSample((int)(sec * rate), 1, rate, [=](int i, int) { return amp * sin(2 * kPiD * hz * i / rate + phase); });
}
static Sample* dc(double v, int frames, int rate, int ramp_in = 0)
{
    return mkSample(frames, 1, rate, [=](int i, int) { return ramp_in > 0 && i < ramp_in ? v * i / ramp_in : v; });
}
static uint32_t g_rng = 12345;
static double frand()
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (g_rng >> 8) / 16777216.0 * 2.0 - 1.0;
}
static Sample* noise(double sec, int rate, double amp, double decay_s = 0.0, int ch = 1)
{
    return mkSample((int)(sec * rate), ch, rate, [=](int i, int) {
        double e = decay_s > 0 ? exp(-(double)i / rate / decay_s) : 1.0;
        return amp * e * frand();
    });
}

// ------------------------------------------------------------------ rig

struct Rig {
    Project prj;
    Engine eng;
    int rate;
    std::vector<float> L, R;
    struct Ev {
        Event e;
        size_t at;
    };
    std::vector<Ev> ev;

    explicit Rig(int r = 48000, bool clear_scenes = false) : rate(r)
    {
        // cleared scenes: the defaults without the demo (0.4.6: its 111 BPM, 2-bar scenes and 303 sound too)
        if (clear_scenes) projectEmpty(prj);
        else projectDefaults(prj);
        prj.g[GP_MASTER] = 0.0f;  // level checks below assume a 0 dB master (default is -1 dB headroom)
        if (clear_scenes)
            for (auto& sc : prj.scene_data) {
                for (auto& t : sc.track)
                    for (auto& st : t.step) st.on = 0;
                sc.bass = BassLine{};   // the demo's ACID BASS line too (0.3)
            }
        if (!eng.init(rate, &prj)) {
            printf("engine init failed\n");
            exit(2);
        }
    }
    void cmd(CmdType t, int pad = 0, int vel = 127, int value = 0, const Sample* s = nullptr, Producer p = Producer::Ui)
    {
        Command c;
        c.type = t;
        c.pad = (uint8_t)pad;
        c.vel = (uint8_t)vel;
        c.value = value;
        c.sample = s;
        if (!eng.post(p, c)) printf("  queue full\n");
    }
    void trig(int pad, int vel = 127, int pitch = 0, Producer p = Producer::Touch) { cmd(CmdType::Trigger, pad, vel, pitch, nullptr, p); }
    void load(int slot, const Sample* s, int pad = -1)
    {
        cmd(CmdType::SetSample, 0, 0, slot, s, Producer::Loader);
        if (pad >= 0) prj.pad[pad].layer[0].slot = (int16_t)slot;
        render(64);
    }
    void render(int frames, int block = 128)
    {
        std::vector<float> l(block), r(block);
        int done = 0;
        while (done < frames) {
            const int m = frames - done < block ? frames - done : block;
            eng.process(l.data(), r.data(), m);
            L.insert(L.end(), l.begin(), l.begin() + m);
            R.insert(R.end(), r.begin(), r.begin() + m);
            done += m;
            Event e;
            while (eng.poll(e)) ev.push_back({e, L.size()});
        }
    }
    size_t pos() const { return L.size(); }
    int count(EvtType t) const
    {
        int n = 0;
        for (auto& e : ev) n += e.e.type == t;
        return n;
    }
    size_t ms(double m) const { return (size_t)(m * 0.001 * rate); }
};

static Rig* newRig(int rate = 48000, bool clear = false) { return new Rig(rate, clear); }

// ------------------------------------------------------------------ measurements

static double peakAbs(const std::vector<float>& x, size_t a, size_t b)
{
    double p = 0;
    for (size_t i = a; i < b && i < x.size(); ++i) p = fmax(p, fabs(x[i]));
    return p;
}
static double rms(const std::vector<float>& x, size_t a, size_t b)
{
    double s = 0;
    size_t n = 0;
    for (size_t i = a; i < b && i < x.size(); ++i, ++n) s += (double)x[i] * x[i];
    return n ? sqrt(s / n) : 0.0;
}
static double freqZC(const std::vector<float>& x, size_t a, size_t b, int rate)
{
    double first = -1, last = -1;
    int n = 0;
    for (size_t i = a + 1; i < b && i < x.size(); ++i) {
        if (x[i - 1] < 0 && x[i] >= 0) {
            const double t = (i - 1) + (-x[i - 1]) / (x[i] - x[i - 1]);
            if (first < 0) first = t;
            last = t;
            ++n;
        }
    }
    if (n < 2) return 0;
    return (n - 1) / ((last - first) / rate);
}
static size_t firstAbove(const std::vector<float>& x, size_t a, double thr)
{
    for (size_t i = a; i < x.size(); ++i)
        if (fabs(x[i]) > thr) return i;
    return x.size();
}
static bool allFinite(const std::vector<float>& x)
{
    for (float v : x)
        if (!std::isfinite(v)) return false;
    return true;
}
static double maxStep(const std::vector<float>& x, size_t a, size_t b)
{
    double m = 0;
    for (size_t i = a + 1; i < b && i < x.size(); ++i) m = fmax(m, fabs((double)x[i] - x[i - 1]));
    return m;
}

static void filterOff(Project& p, int pad) { p.pad[pad].p[PP_FTYPE] = (float)FilterType::Off; }

// ------------------------------------------------------------------ tests

static void testPitch()
{
    printf("[pitch]\n");
    for (int rate : {48000, 44100}) {
        Rig& r = *newRig(rate);
        Sample* s48 = sine(440, 1.0, 48000);
        Sample* s44 = sine(440, 1.0, 44100);
        r.load(0, s48, 0);
        r.load(1, s44, 1);
        size_t a = r.pos();
        r.trig(0);
        r.render(rate / 2);
        const double f0 = freqZC(r.L, a + r.ms(50), a + r.ms(450), rate);
        CHECK(fabs(f0 - 440) < 0.5, "48k sample on %d engine: %.3f Hz", rate, f0);
        a = r.pos();
        r.trig(1);
        r.render(rate / 2);
        const double f1 = freqZC(r.L, a + r.ms(50), a + r.ms(450), rate);
        CHECK(fabs(f1 - 440) < 0.5, "44.1k sample on %d engine: %.3f Hz", rate, f1);
        r.prj.pad[0].p[PP_TUNE] = 12.0f;
        a = r.pos();
        r.trig(0);
        r.render(rate / 2);
        const double f2 = freqZC(r.L, a + r.ms(50), a + r.ms(450), rate);
        CHECK(fabs(f2 - 880) < 1.0, "+12 st: %.3f Hz", f2);
        r.prj.pad[0].p[PP_TUNE] = -12.0f;
        r.prj.pad[0].p[PP_FINE] = 0.0f;
        a = r.pos();
        r.trig(0, 127, 7);   // command pitch +7 on top: -5 st
        r.render(rate / 2);
        const double f3 = freqZC(r.L, a + r.ms(50), a + r.ms(450), rate);
        const double e3 = 440 * pow(2.0, -5.0 / 12.0);
        CHECK(fabs(f3 - e3) < 0.5, "-12 st + 7 cmd: %.3f Hz (exp %.3f)", f3, e3);
        printf("  rate %d: 440 -> %.2f / %.2f, +12 -> %.2f, -5 -> %.2f\n", rate, f0, f1, f2, f3);
    }
    // pitch envelope: +12 st decaying -> starts near 880, ends at 440
    Rig& r = *newRig();
    r.load(0, sine(440, 1.0, 48000), 0);
    r.prj.pad[0].p[PP_PENV] = 12.0f;
    r.prj.pad[0].p[PP_PDECAY] = 200.0f;
    size_t a = r.pos();
    r.trig(0);
    r.render(24000);
    const double fa = freqZC(r.L, a + r.ms(2), a + r.ms(12), 48000);
    const double fb = freqZC(r.L, a + r.ms(300), a + r.ms(450), 48000);
    CHECK(fa > 780 && fa < 900, "pitch env start %.1f Hz", fa);
    CHECK(fabs(fb - 440) < 1.0, "pitch env end %.1f Hz", fb);
    printf("  pitch env: %.1f Hz -> %.1f Hz\n", fa, fb);
}

static void testRegion()
{
    printf("[region / reverse]\n");
    Rig& r = *newRig();
    Sample* s = dc(0.5, 4800, 48000);
    r.load(0, s, 0);
    filterOff(r.prj, 0);
    r.prj.pad[0].p[PP_START] = 0.25f;
    r.prj.pad[0].p[PP_END] = 0.75f;
    size_t a = r.pos();
    r.trig(0);
    r.render(8000);
    size_t nz = 0;
    for (size_t i = a; i < r.L.size(); ++i) nz += fabs(r.L[i]) > 1e-6;
    CHECK(nz == 2400, "region frames %zu (expected 2400)", nz);
    const double lvl = peakAbs(r.L, a, r.L.size());
    CHECK(fabs(lvl - 0.5 * 0.70710678) < 0.002, "region level %.4f", lvl);
    // end micro-fade: the last frames ramp down instead of stepping
    size_t last = a;
    for (size_t i = a; i < r.L.size(); ++i)
        if (fabs(r.L[i]) > 1e-6) last = i;
    CHECK(fabs(r.L[last]) < 0.02, "micro-fade end value %.4f", r.L[last]);
    printf("  region: %zu frames, level %.4f, last %.5f\n", nz, lvl, r.L[last]);

    // reverse: a rising ramp plays falling
    Rig& q = *newRig();
    Sample* ramp = mkSample(4800, 1, 48000, [](int i, int) { return 0.9 * i / 4799.0; });
    q.load(0, ramp, 0);
    filterOff(q.prj, 0);
    q.prj.pad[0].reverse = 1;
    a = q.pos();
    q.trig(0);
    q.render(6000);
    const size_t on = firstAbove(q.L, a, 1e-6);
    bool mono = true;
    for (size_t i = on + 1; i < on + 4790 && i < q.L.size(); ++i)
        if (q.L[i] > q.L[i - 1] + 1e-6) mono = false;
    CHECK(mono, "reverse output not falling");
    CHECK(fabs(q.L[on] - 0.9 * 0.70710678) < 0.003, "reverse starts at the end value: %.4f", q.L[on]);
    size_t nzr = 0;
    for (size_t i = a; i < q.L.size(); ++i) nzr += fabs(q.L[i]) > 1e-6;
    CHECK(nzr >= 4795 && nzr <= 4800, "reverse length %zu", nzr);
    printf("  reverse: first %.4f, length %zu, monotonic %d\n", q.L[on], nzr, (int)mono);
}

static void testLoop()
{
    printf("[loop / gate]\n");
    Rig& r = *newRig();
    // 0.1 s of a 330 Hz sine: loop start at 0.37 is mid-cycle -> seam needs the crossfade
    r.load(0, sine(330, 0.1, 48000, 0.5, 0.3), 0);
    filterOff(r.prj, 0);
    r.prj.pad[0].mode = PlayMode::Loop;
    r.prj.pad[0].p[PP_LOOP] = 0.37f;
    r.prj.pad[0].p[PP_RELEASE] = 100.0f;
    size_t a = r.pos();
    r.trig(0);
    r.render(48000);
    const double lvl = peakAbs(r.L, a + r.ms(200), a + r.ms(1000));
    const double st = maxStep(r.L, a + r.ms(5), a + r.ms(1000));
    const double sine_step = lvl * 2 * kPiD * 330 / 48000;
    CHECK(lvl > 0.3, "loop still playing after 1 s: %.3f", lvl);
    CHECK(st < 2.0 * sine_step, "loop seam step %.4f vs sine slope %.4f", st, sine_step);
    size_t rel = r.pos();
    r.cmd(CmdType::Release, 0);
    r.render(48000 / 4);
    const double after = peakAbs(r.L, rel + r.ms(110), r.pos());
    CHECK(after < 1e-6, "loop release ends: %.6f", after);
    printf("  loop level %.3f, max step %.4f (sine slope %.4f), after release %.2g\n", lvl, st, sine_step, after);

    // loop over the whole sample (no pre-loop data): gain dip at the seam instead of a click
    Rig& q = *newRig();
    q.load(0, sine(250, 0.05, 48000, 0.5, 1.0), 0);
    filterOff(q.prj, 0);
    q.prj.pad[0].mode = PlayMode::Loop;
    a = q.pos();
    q.trig(0);
    q.render(24000);
    const double st2 = maxStep(q.L, a + r.ms(5), q.pos());
    CHECK(st2 < 0.05, "full-sample loop seam step %.4f", st2);
    printf("  full loop seam max step %.4f\n", st2);

    // gate: release mid-sample ends the voice after the release time
    Rig& g = *newRig();
    g.load(0, sine(200, 2.0, 48000), 0);
    g.prj.pad[0].mode = PlayMode::Gate;
    g.prj.pad[0].p[PP_RELEASE] = 50.0f;
    g.trig(0);
    g.render(4800);
    rel = g.pos();
    g.cmd(CmdType::Release, 0);
    g.render(9600);
    CHECK(peakAbs(g.L, rel + g.ms(60), g.pos()) < 1e-6, "gate release");
    CHECK(g.eng.telemetry().voices.load() == 0, "gate voice freed");
}

static void testEnvelope()
{
    printf("[envelope]\n");
    Rig& r = *newRig();
    r.load(0, dc(0.5, 96000, 48000), 0);
    Pad& p = r.prj.pad[0];
    filterOff(r.prj, 0);
    p.mode = PlayMode::Gate;
    p.p[PP_ATTACK] = 50.0f;
    p.p[PP_HOLD] = 0.0f;
    p.p[PP_DECAY] = 100.0f;
    p.p[PP_SUSTAIN] = 0.5f;
    p.p[PP_RELEASE] = 200.0f;
    const size_t a = r.pos();
    r.trig(0);
    r.render(48000 / 2);
    const size_t rel_at = r.pos();
    r.cmd(CmdType::Release, 0);
    r.render(48000 / 2);
    const size_t on = firstAbove(r.L, a, 1e-7);
    const size_t lat = on - a;   // limiter look-ahead
    const double pk = peakAbs(r.L, a, rel_at);
    size_t ipk = on;
    while (ipk < r.L.size() && fabs(r.L[ipk]) < 0.999 * pk) ++ipk;
    const double att = (double)(ipk - on) / 48000 * 1000;
    const double sus_lvl = 0.5 * pk;
    size_t isus = ipk;
    while (isus < r.L.size() && fabs(r.L[isus]) > sus_lvl + 0.002 * (pk - sus_lvl)) ++isus;
    const double dec = (double)(isus - ipk) / 48000 * 1000;
    size_t iend = rel_at + lat;
    while (iend < r.L.size() && fabs(r.L[iend]) >= 1e-4 * pk) ++iend;
    const double rls = (double)(iend - rel_at - lat) / 48000 * 1000;
    CHECK(fabs(att - 50) < 5, "attack %.1f ms", att);
    CHECK(fabs(dec - 100) < 10, "decay %.1f ms", dec);
    CHECK(fabs(rls - 200) < 20, "release %.1f ms", rls);
    CHECK(fabs(fabs(r.L[rel_at - 10]) - sus_lvl) < 0.002, "sustain %.4f vs %.4f", r.L[rel_at - 10], sus_lvl);
    printf("  attack %.1f ms, decay %.1f ms, release %.1f ms (latency %zu samples)\n", att, dec, rls, lat);

    // velocity depth: vel 64 with PP_VEL_AMP 1 -> (64/127)^2
    Rig& v = *newRig();
    v.load(0, dc(0.5, 48000, 48000), 0);
    filterOff(v.prj, 0);
    v.prj.pad[0].p[PP_VEL_AMP] = 1.0f;
    size_t b = v.pos();
    v.trig(0, 64);
    v.render(4800);
    const double lv = peakAbs(v.L, b, v.pos());
    const double ev = 0.5 * 0.70710678 * (64.0 / 127) * (64.0 / 127);
    CHECK(fabs(lv - ev) < 0.002, "velocity gain %.4f vs %.4f", lv, ev);
}

static void testFilter()
{
    printf("[filter]\n");
    auto level = [](double hz, FilterType ft, float cutoff, float reso) {
        Rig& r = *newRig();
        r.load(0, sine(hz, 1.0, 48000, 0.5), 0);
        r.prj.pad[0].p[PP_FTYPE] = (float)ft;
        r.prj.pad[0].p[PP_CUTOFF] = cutoff;
        r.prj.pad[0].p[PP_RESO] = reso;
        size_t a = r.pos();
        r.trig(0);
        r.render(24000);
        const double v = rms(r.L, a + r.ms(100), a + r.ms(450));
        delete &r;
        return v;
    };
    const double ref8 = level(8000, FilterType::Off, 1000, 0);
    const double lp8 = level(8000, FilterType::LowPass, 1000, 0);
    const double ref2 = level(200, FilterType::Off, 1000, 0);
    const double lp2 = level(200, FilterType::LowPass, 1000, 0);
    const double hp2 = level(200, FilterType::HighPass, 1000, 0);
    const double bp1 = level(1000, FilterType::BandPass, 1000, 0.5f);
    const double ref1 = level(1000, FilterType::Off, 1000, 0);
    const double att8 = 20 * log10(lp8 / ref8), att2 = 20 * log10(lp2 / ref2), hpa = 20 * log10(hp2 / ref2);
    const double bpa = 20 * log10(bp1 / ref1);
    CHECK(att8 < -20, "LP 1 kHz at 8 kHz: %.1f dB", att8);
    CHECK(att2 > -1.0, "LP 1 kHz at 200 Hz: %.2f dB", att2);
    CHECK(hpa < -20, "HP 1 kHz at 200 Hz: %.1f dB", hpa);
    CHECK(fabs(bpa) < 1.0, "BP at centre: %.2f dB", bpa);
    printf("  LP1k: 8 kHz %.1f dB, 200 Hz %.2f dB; HP1k 200 Hz %.1f dB; BP centre %.2f dB\n", att8, att2, hpa, bpa);

    // drive + full resonance + filter envelope + LFO stays finite and bounded
    Rig& r = *newRig();
    r.load(0, noise(1.0, 48000, 0.9), 0);
    Pad& p = r.prj.pad[0];
    p.p[PP_RESO] = 1.0f;
    p.p[PP_DRIVE] = 1.0f;
    p.p[PP_CUTOFF] = 300.0f;
    p.p[PP_FENV] = 1.0f;
    p.p[PP_FDECAY] = 200.0f;
    p.p[PP_LFO_DEPTH] = 1.0f;
    p.p[PP_LFO_RATE] = 8.0f;
    p.p[PP_LFO_TARGET] = (float)LfoTarget::Cutoff;
    r.trig(0);
    r.render(48000);
    CHECK(allFinite(r.L) && allFinite(r.R), "resonant drive NaN");
}

static void testPan()
{
    printf("[pan]\n");
    float res[3][2];
    const float pans[3] = {-1.0f, 0.0f, 1.0f};
    for (int k = 0; k < 3; ++k) {
        Rig& r = *newRig();
        r.load(0, dc(0.5, 9600, 48000), 0);
        filterOff(r.prj, 0);
        r.prj.pad[0].p[PP_PAN] = pans[k];
        size_t a = r.pos();
        r.trig(0);
        r.render(4800);
        res[k][0] = r.L[a + 2000];
        res[k][1] = r.R[a + 2000];
        delete &r;
    }
    CHECK(fabs(res[0][0] - 0.5) < 0.002 && fabs(res[0][1]) < 1e-4, "hard left %.4f %.4f", res[0][0], res[0][1]);
    CHECK(fabs(res[2][1] - 0.5) < 0.002 && fabs(res[2][0]) < 1e-4, "hard right %.4f %.4f", res[2][0], res[2][1]);
    CHECK(fabs(res[1][0] - 0.5 * 0.70710678) < 0.002 && fabs(res[1][0] - res[1][1]) < 1e-5, "centre %.4f %.4f",
          res[1][0], res[1][1]);
    const double power = res[1][0] * res[1][0] + res[1][1] * res[1][1];
    CHECK(fabs(power - 0.25) < 0.002, "constant power %.4f", power);
    printf("  L: %.4f/%.4f  C: %.4f/%.4f  R: %.4f/%.4f\n", res[0][0], res[0][1], res[1][0], res[1][1], res[2][0],
           res[2][1]);
}

static void testBuses()
{
    printf("[buses]\n");
    Rig& r = *newRig();
    r.load(0, sine(300, 2.0, 48000), 0);
    r.load(1, sine(500, 2.0, 48000), 1);
    r.prj.pad[0].bus = 0;
    r.prj.pad[1].bus = 1;
    r.prj.bus[0].mute = 1;
    size_t a = r.pos();
    r.trig(0);
    r.render(4800);
    CHECK(peakAbs(r.L, a + 300, r.pos()) < 1e-6, "muted bus is silent");
    r.prj.bus[0].mute = 0;
    r.prj.bus[1].solo = 1;
    r.cmd(CmdType::Panic);
    r.render(2400);
    a = r.pos();
    r.trig(0);
    r.render(4800);
    CHECK(peakAbs(r.L, a + 300, r.pos()) < 1e-6, "non-soloed bus is silent");
    a = r.pos();
    r.trig(1);
    r.render(4800);
    const double lv = peakAbs(r.L, a + 300, r.pos());
    CHECK(lv > 0.3, "soloed bus plays %.3f", lv);
    CHECK(r.eng.telemetry().bus_peak[1].load() > 0.3f, "bus peak telemetry %.3f", r.eng.telemetry().bus_peak[1].load());
    CHECK(r.eng.telemetry().bus_peak[0].load() == 0.0f, "muted bus peak");
    // fader: level 1.0 = +6 dB
    r.prj.bus[1].level = 1.0f;
    r.render(4800);
    const double lv6 = peakAbs(r.L, r.pos() - 2400, r.pos());
    CHECK(fabs(20 * log10(lv6 / lv) - 6.02) < 0.3, "fader +6 dB: %.2f dB", 20 * log10(lv6 / lv));
    printf("  solo level %.3f, +6 dB fader -> %.3f\n", lv, lv6);
}

static void testFx()
{
    printf("[fx send]\n");
    auto tail = [](float send, float dly_level, float rev_level, std::vector<float>* keep) {
        Rig& r = *newRig();
        r.load(0, noise(0.05, 48000, 0.7), 0);
        r.prj.pad[0].p[PP_SEND] = send;
        r.prj.g[GP_DLY_LEVEL] = dly_level;
        r.prj.g[GP_REV_LEVEL] = rev_level;
        size_t a = r.pos();
        r.trig(0);
        r.render(48000);
        if (keep) keep->assign(r.L.begin() + a, r.L.end());
        const double v = rms(r.L, a + r.ms(300), a + r.ms(800));
        delete &r;
        return v;
    };
    const double dry = tail(0.0f, 0.5f, 0.7f, nullptr);
    const double rev = tail(1.0f, 0.0f, 0.7f, nullptr);
    CHECK(dry < 1e-7, "no send, no tail: %.3g", dry);
    CHECK(rev > 1e-3, "reverb tail rms %.4f", rev);
    std::vector<float> d;
    tail(1.0f, 1.0f, 0.0f, &d);   // delay only: echo at 1/8. = 375 ms @120 BPM
    double best = 0;
    size_t at = 0;
    for (size_t i = (size_t)(0.2 * 48000); i < (size_t)(0.6 * 48000) && i < d.size(); ++i)
        if (fabs(d[i]) > best) {
            best = fabs(d[i]);
            at = i;
        }
    const double echo_ms = at / 48.0;
    CHECK(best > 0.05 && echo_ms > 370 && echo_ms < 430, "delay echo at %.1f ms (peak %.3f)", echo_ms, best);
    printf("  reverb tail rms %.4f (dry %.2g), delay echo %.1f ms peak %.3f\n", rev, dry, echo_ms, best);

    // SPACE macro: reverb from all pads without per-pad sends
    Rig& s = *newRig();
    s.load(0, noise(0.05, 48000, 0.7), 0);
    s.prj.space_on = 1;
    size_t a = s.pos();
    s.trig(0);
    s.render(48000);
    const double sp = rms(s.L, a + s.ms(300), a + s.ms(800));
    CHECK(sp > 1e-3, "SPACE tail %.4f", sp);
}

static void testChoke()
{
    printf("[choke]\n");
    Rig& r = *newRig();
    r.load(0, noise(1.0, 48000, 0.6), 3);                  // open hat, choke 1
    r.load(1, mkSample(4800, 1, 48000, [](int, int) { return 0.0; }), 2);   // closed hat (silent), choke 1
    r.trig(3);
    r.render(4800);
    const double before = rms(r.L, r.pos() - 2400, r.pos());
    const size_t t = r.pos();
    r.trig(2);
    r.render(4800);
    const double after = peakAbs(r.L, t + r.ms(10), r.pos());
    CHECK(before > 0.1, "open hat plays %.3f", before);
    CHECK(after < 1e-4, "choked within 10 ms: %.2g", after);
    printf("  open hat rms %.3f -> peak %.2g 10 ms after the closed hat\n", before, after);
}

static void testStealing()
{
    printf("[stealing]\n");
    Rig& r = *newRig();
    // DC with a 10 ms ramp-in: starts are smooth, so any step comes from a cut voice
    const double a_in = 0.035;
    for (int p = 0; p < 10; ++p) {
        r.load(p, dc(a_in, 96000, 48000, 480), p);
        filterOff(r.prj, p);
    }
    const double a_out = a_in * 0.70710678;
    const size_t start = r.pos();
    int max_voices = 0;
    for (int k = 0; k < 40; ++k) {
        r.trig(k % 10);
        r.render(128);
        max_voices = std::max(max_voices, (int)r.eng.telemetry().voices.load());
    }
    r.render(9600);
    const double st = maxStep(r.L, start, r.pos());
    const int live = r.eng.telemetry().voices.load();
    CHECK(st < 0.05 * a_out, "steal step %.5f vs voice %.4f (%.1f %%)", st, a_out, 100 * st / a_out);
    CHECK(live == kVoices, "voices after 40 hits: %d", live);
    CHECK(max_voices <= kVoices + 8, "pool bound %d", max_voices);
    const double sum = peakAbs(r.L, r.pos() - 2000, r.pos());
    CHECK(fabs(sum - 32 * a_out) < 0.01, "32 voices sum %.4f vs %.4f", sum, 32 * a_out);
    printf("  40 hits: max step %.5f (%.2f %% of one voice), %d voices left (peak pool %d)\n", st, 100 * st / a_out,
           live, max_voices);

    // poly 1 = mono retrigger, also click-free
    Rig& m = *newRig();
    m.load(0, sine(220, 2.0, 48000), 0);
    m.prj.pad[0].poly = 1;
    for (int k = 0; k < 6; ++k) {
        m.trig(0);
        m.render(3000 + 37 * k);
        CHECK(m.eng.telemetry().voices.load() <= 2, "mono voices %d", (int)m.eng.telemetry().voices.load());
    }
    m.render(1000);
    CHECK(m.eng.telemetry().voices.load() == 1, "mono: one voice left");
}

static void testRetire()
{
    printf("[sample retirement]\n");
    Rig& r = *newRig();
    Sample* old = sine(300, 2.0, 48000, 0.5);
    Sample* fresh = sine(600, 2.0, 48000, 0.5);
    r.load(0, old, 0);
    r.trig(0);
    r.render(4800);
    const size_t set_at = r.pos();
    r.cmd(CmdType::SetSample, 0, 0, 0, fresh, Producer::Loader);
    size_t got = 0;
    for (int b = 0; b < 20 && !got; ++b) {
        r.render(128);
        for (auto& e : r.ev)
            if (e.e.type == EvtType::SampleRetired && e.e.sample == old) got = e.at;
    }
    CHECK(got > 0, "SampleRetired received");
    CHECK(got >= set_at + 256, "retired only after the fade (%zu samples after SetSample)", got - set_at);
    CHECK(got <= set_at + 128 * 4, "retired promptly (%zu)", got - set_at);
    CHECK(r.eng.slot(0) == fresh, "slot shows the new sample");
    // simulate reuse of the freed memory: the engine must not read it any more
    for (uint32_t i = 0; i < old->frames; ++i) old->data[i] = 32767;
    const size_t a = r.pos();
    r.render(4800);
    // only the end of the 4 ms fade still in the 71-sample limiter look-ahead may come out
    // (reading the overwritten data would give ~0.7)
    CHECK(peakAbs(r.L, a, r.pos()) < 0.05, "no access after retirement (%.3f)", peakAbs(r.L, a, r.pos()));
    CHECK(peakAbs(r.L, a + 128, r.pos()) < 1e-7, "silent after retirement");
    // idle slot: retired at once
    Sample* other = sine(100, 0.1, 48000);
    r.cmd(CmdType::SetSample, 0, 0, 5, other, Producer::Loader);
    r.render(128);
    r.cmd(CmdType::SetSample, 0, 0, 5, nullptr, Producer::Loader);
    r.render(128);
    bool ok = false;
    for (auto& e : r.ev) ok |= e.e.type == EvtType::SampleRetired && e.e.sample == other;
    CHECK(ok, "cleared idle slot retired");
    printf("  SampleRetired %zu samples after SetSample\n", got - set_at);

    // Every install is answered exactly once (rt-4 / logic-3, engine side): a pointer installed into a
    // second slot answers that extra install at once, and the last slot it leaves answers the other.
    auto answers = [&](const Sample* smp) {
        int n = 0;
        for (auto& e : r.ev) n += e.e.type == EvtType::SampleRetired && e.e.sample == smp;
        return n;
    };
    Sample* twin = sine(150, 0.1, 48000);
    r.ev.clear();
    r.cmd(CmdType::SetSample, 0, 0, 6, twin, Producer::Loader);
    r.render(128);
    CHECK(answers(twin) == 0, "first install: no answer yet");
    r.cmd(CmdType::SetSample, 0, 0, 7, twin, Producer::Loader);
    r.render(128);
    CHECK(answers(twin) == 1, "second slot: that install answered at once (%d)", answers(twin));
    r.cmd(CmdType::SetSample, 0, 0, 6, nullptr, Producer::Loader);
    r.render(128);
    CHECK(answers(twin) == 1, "still in slot 7: nothing more (%d)", answers(twin));
    r.cmd(CmdType::SetSample, 0, 0, 7, nullptr, Producer::Loader);
    r.render(128);
    CHECK(answers(twin) == 2, "left its last slot: answered (%d)", answers(twin));
    // re-installed while its retirement is pending (a voice still fades it): the pending answer comes at once
    Sample* busy = sine(330, 2.0, 48000, 0.5);
    r.load(8, busy, 1);
    r.trig(1);
    r.render(2400);
    r.ev.clear();
    r.cmd(CmdType::SetSample, 0, 0, 8, nullptr, Producer::Loader);
    r.cmd(CmdType::SetSample, 0, 0, 9, busy, Producer::Loader);   // same block: the fade has not ended
    r.render(128);
    CHECK(answers(busy) == 1, "pending retirement answered on re-install (%d)", answers(busy));
    CHECK(r.eng.slot(9) == busy, "re-installed sample in slot 9");
    r.cmd(CmdType::SetSample, 0, 0, 9, nullptr, Producer::Loader);
    for (int b = 0; b < 10; ++b) r.render(128);
    CHECK(answers(busy) == 2, "second install answered when it left slot 9 (%d)", answers(busy));
    r.prj.pad[1].layer[0].slot = -1;
}

// A re-init (sample-rate change) starts with REC disarmed and no metronome override (logic-12).
static void testReinitState()
{
    printf("[re-init state]\n");
    Rig& r = *newRig();
    r.cmd(CmdType::SetRecord, 0, 0, 1);
    r.cmd(CmdType::Metronome, 0, 0, 1);
    r.render(128);
    CHECK(r.eng.telemetry().recording.load() == 1, "REC armed");
    CHECK(r.eng.init(44100, &r.prj), "re-init at 44.1 kHz");
    r.render(128);
    CHECK(r.eng.telemetry().recording.load() == 0, "REC disarmed after init");
}

static void testPreview()
{
    printf("[preview]\n");
    Rig& r = *newRig();
    Sample* s = sine(500, 0.1, 44100, 0.5);
    const size_t a = r.pos();
    r.cmd(CmdType::Preview, 0, 127, 0, s);
    r.render(9600);
    size_t done = 0;
    for (auto& e : r.ev)
        if (e.e.type == EvtType::PreviewDone && e.e.sample == s) done = e.at;
    CHECK(done > 0, "PreviewDone received");
    const double d_ms = (done - a) / 48.0;
    CHECK(d_ms > 95 && d_ms < 110, "PreviewDone after %.1f ms", d_ms);
    CHECK(peakAbs(r.L, a + r.ms(10), a + r.ms(90)) > 0.3, "preview audible");
    const double fp = freqZC(r.L, a + r.ms(10), a + r.ms(90), 48000);
    CHECK(fabs(fp - 500) < 1.0, "preview pitch %.2f", fp);
    // replacement: the first one is reported soon, the second when it ends
    Sample* l1 = sine(300, 1.0, 48000);
    Sample* l2 = sine(400, 0.2, 48000);
    const size_t b = r.pos();
    r.cmd(CmdType::Preview, 0, 100, 0, l1);
    r.render(4800);
    r.cmd(CmdType::Preview, 0, 100, 0, l2);
    r.render(4800 * 3);
    size_t d1 = 0, d2 = 0;
    for (auto& e : r.ev) {
        if (e.e.type == EvtType::PreviewDone && e.e.sample == l1) d1 = e.at;
        if (e.e.type == EvtType::PreviewDone && e.e.sample == l2) d2 = e.at;
    }
    CHECK(d1 > 0 && (d1 - b) / 48.0 < 112, "replaced preview done at %.1f ms", d1 ? (d1 - b) / 48.0 : -1.0);
    CHECK(d2 > 0 && (d2 - b) / 48.0 > 295, "second preview done at %.1f ms", d2 ? (d2 - b) / 48.0 : -1.0);
    r.cmd(CmdType::Preview, 0, 100, 0, l1);
    r.render(1200);
    r.cmd(CmdType::StopPreview);
    r.render(1200);
    CHECK(peakAbs(r.L, r.pos() - 600, r.pos()) < 1e-6, "StopPreview silences");
    printf("  done after %.1f ms; replaced at %.1f ms, second at %.1f ms\n", d_ms, (d1 - b) / 48.0, (d2 - b) / 48.0);
}

static void testMacros()
{
    printf("[PUMP / LO-FI / WIDE]\n");
    Rig& r = *newRig();
    r.load(1, dc(0.3, 96000, 48000), 1);   // pad 1 on bus 1
    filterOff(r.prj, 1);
    r.prj.pump_on = 1;
    r.prj.pump_src = 0;   // pad 0 (bus 0, no sample needed): its hits duck everything else
    r.prj.g[GP_PUMP_AMT] = 0.6f;
    r.prj.g[GP_PUMP_RELEASE] = 180.0f;
    r.trig(1);
    r.render(9600);
    const double before = fabs(r.L[r.pos() - 10]);
    const size_t t = r.pos();
    r.trig(0);
    r.render(4800 * 6);
    double duck = 1.0;
    for (size_t i = t; i < t + r.ms(20); ++i) duck = fmin(duck, fabs(r.L[i]));
    const double back = fabs(r.L[t + r.ms(300)]);
    CHECK(fabs(duck / before - 0.4) < 0.02, "pump ducks to %.3f of the level", duck / before);
    CHECK(fabs(back / before - 1.0) < 0.01, "pump recovers %.3f", back / before);
    printf("  pump: %.4f -> %.4f -> %.4f\n", before, duck, back);

    auto render = [](bool lofi, bool wide, std::vector<float>* L, std::vector<float>* R) {
        Rig& q = *newRig();
        Sample* st = mkSample(48000, 2, 48000, [](int i, int c) {
            return 0.2 * sin(2 * kPiD * (c ? 1210.0 : 997.0) * i / 48000) + 0.1 * sin(2 * kPiD * 60.0 * i / 48000);
        });
        q.load(0, st, 0);
        q.prj.lofi_on = lofi;
        q.prj.g[GP_LOFI_BITS] = 6;
        q.prj.g[GP_LOFI_RATE] = 8000;
        q.prj.wide_on = wide;
        q.prj.g[GP_WIDE] = 1.0f;
        q.render(4800);   // let macro smoothing settle
        size_t a = q.pos();
        q.trig(0);
        q.render(24000);
        L->assign(q.L.begin() + a, q.L.end());
        R->assign(q.R.begin() + a, q.R.end());
        delete &q;
    };
    std::vector<float> l0, r0, l1, r1, l2, r2;
    render(false, false, &l0, &r0);
    render(true, false, &l1, &r1);
    render(false, true, &l2, &r2);
    double diff = 0, ref = 0;
    for (size_t i = 4800; i < l0.size(); ++i) {
        diff += (l0[i] - l1[i]) * (l0[i] - l1[i]);
        ref += l0[i] * l0[i];
    }
    CHECK(sqrt(diff / ref) > 0.1, "LO-FI changes the output (%.3f)", sqrt(diff / ref));
    CHECK(allFinite(l1) && allFinite(r1), "LO-FI finite");
    // WIDE: side energy grows, mid unchanged
    double s0 = 0, s2 = 0, m0 = 0, m2 = 0;
    for (size_t i = 4800; i < l0.size(); ++i) {
        s0 += (l0[i] - r0[i]) * (l0[i] - r0[i]);
        s2 += (l2[i] - r2[i]) * (l2[i] - r2[i]);
        m0 += (l0[i] + r0[i]) * (l0[i] + r0[i]);
        m2 += (l2[i] + r2[i]) * (l2[i] + r2[i]);
    }
    CHECK(sqrt(s2 / s0) > 1.6, "WIDE side gain %.2f", sqrt(s2 / s0));
    CHECK(fabs(sqrt(m2 / m0) - 1.0) < 0.02, "WIDE keeps mid %.3f", sqrt(m2 / m0));
    printf("  LO-FI rel. difference %.3f; WIDE side x%.2f, mid x%.3f\n", sqrt(diff / ref), sqrt(s2 / s0),
           sqrt(m2 / m0));
}

static void testMetronome()
{
    printf("[metronome]\n");
    Rig& r = *newRig(48000, true);
    r.prj.metronome = 1;
    r.prj.g[GP_METRO_LEVEL] = 1.0f;
    const size_t a = r.pos();
    r.cmd(CmdType::Play);
    r.render(48000 * 2 + 4800);
    std::vector<size_t> on;
    size_t i = a;
    while (i < r.L.size()) {
        if (fabs(r.L[i]) > 0.02) {
            on.push_back(i);
            i += r.ms(100);
        } else {
            ++i;
        }
    }
    CHECK(on.size() == 5, "clicks in 2.1 s at 120 BPM: %zu", on.size());
    if (on.size() >= 3) {
        const double gap = (on[2] - on[1]) / 48.0;
        CHECK(fabs(gap - 500) < 2, "click interval %.1f ms", gap);
        const double f_acc = freqZC(r.L, on[0], on[0] + r.ms(30), 48000);
        const double f_n = freqZC(r.L, on[1], on[1] + r.ms(30), 48000);
        CHECK(f_acc > f_n * 1.3, "accent pitch %.0f vs %.0f Hz", f_acc, f_n);
        CHECK(peakAbs(r.L, on[0], on[0] + r.ms(60)) > peakAbs(r.L, on[1], on[1] + r.ms(60)), "accent louder");
        printf("  %zu clicks, %.1f ms apart, accent %.0f Hz / beat %.0f Hz, first at %zu\n", on.size(), gap, f_acc, f_n,
               on[0] - a);
    }
    // runtime override off
    r.cmd(CmdType::Metronome, 0, 0, 0);
    r.render(4800);
    const size_t b = r.pos();
    r.render(48000);
    CHECK(peakAbs(r.L, b, r.pos()) < 1e-6, "metronome override off");
}

static void testLayersEvents()
{
    printf("[layers / events / telemetry]\n");
    Rig& r = *newRig(48000, true);
    r.load(0, sine(300, 0.5, 48000), -1);
    r.load(1, sine(600, 0.5, 48000), -1);
    r.load(2, sine(900, 0.5, 48000), -1);
    r.load(3, sine(1200, 0.5, 48000), -1);
    Pad& p = r.prj.pad[0];
    for (int l = 0; l < 4; ++l) p.layer[l].slot = (int16_t)l;
    // velocity split
    p.layer_mode = LayerMode::Velocity;
    p.layer[0].vel_lo = 0;
    p.layer[0].vel_hi = 63;
    p.layer[1].vel_lo = 64;
    p.layer[1].vel_hi = 127;
    p.layer[2].vel_lo = p.layer[2].vel_hi = 0;
    p.layer[3].vel_lo = p.layer[3].vel_hi = 0;
    size_t a = r.pos();
    r.trig(0, 30);
    r.render(9600);
    const double f_lo = freqZC(r.L, a + r.ms(20), a + r.ms(190), 48000);
    a = r.pos();
    r.trig(0, 100);
    r.render(9600);
    const double f_hi = freqZC(r.L, a + r.ms(20), a + r.ms(190), 48000);
    CHECK(fabs(f_lo - 300) < 1 && fabs(f_hi - 600) < 1, "velocity layers %.1f / %.1f", f_lo, f_hi);
    // round robin cycles
    p.layer_mode = LayerMode::RoundRobin;
    double rr[4];
    for (int k = 0; k < 4; ++k) {
        r.cmd(CmdType::Panic);
        r.render(1200);
        a = r.pos();
        r.trig(0);
        r.render(9600);
        rr[k] = freqZC(r.L, a + r.ms(20), a + r.ms(190), 48000);
    }
    CHECK(fabs(rr[1] - rr[0]) > 100 && fabs(rr[2] - rr[1]) > 100 && fabs(rr[3] - rr[2]) > 100, "round robin %.0f %.0f %.0f %.0f",
          rr[0], rr[1], rr[2], rr[3]);
    // stack: 4 voices, 1 PadHit
    r.cmd(CmdType::Panic);
    r.render(1200);
    p.layer_mode = LayerMode::Stack;
    r.ev.clear();
    r.trig(0);
    r.render(256);
    CHECK(r.eng.telemetry().voices.load() == 4, "stack voices %d", (int)r.eng.telemetry().voices.load());
    CHECK(r.count(EvtType::PadHit) == 1, "one PadHit per trigger (%d)", r.count(EvtType::PadHit));
    CHECK((r.eng.telemetry().pad_hits.exchange(0) & 1u) != 0, "pad_hits bit");
    CHECK(r.eng.telemetry().pad_env[0].load() > 0.9f, "pad_env %.3f", r.eng.telemetry().pad_env[0].load());
    r.eng.telemetry().watch_pad.store(0);
    r.render(4800);
    const float pp1 = r.eng.telemetry().play_pos.load();
    r.render(4800);
    const float pp2 = r.eng.telemetry().play_pos.load();
    CHECK(pp1 > 0.15f && pp1 < 0.25f && pp2 > pp1, "play_pos %.3f -> %.3f", pp1, pp2);
    r.render(24000);
    CHECK(r.eng.telemetry().play_pos.load() < 0.0f, "play_pos none after the end");

    // transport / scene / record events
    r.ev.clear();
    r.cmd(CmdType::QueueScene, 2);
    r.render(256);
    CHECK(r.count(EvtType::SceneChanged) == 1, "SceneChanged when stopped (%d)", r.count(EvtType::SceneChanged));
    CHECK(r.eng.telemetry().scene.load() == 2, "telemetry scene %d", (int)r.eng.telemetry().scene.load());
    r.cmd(CmdType::Play);
    r.render(256);
    CHECK(r.count(EvtType::TransportChanged) >= 1, "TransportChanged on play");
    CHECK(r.eng.telemetry().playing.load() == 1, "telemetry playing");
    r.cmd(CmdType::SetRecord, 0, 0, 1);
    r.render(6000 * 4 - 512);   // ~step 4
    r.trig(0, 99, 0, Producer::Midi);
    r.render(256);
    bool rec = false;
    for (auto& e : r.ev)
        if (e.e.type == EvtType::RecordHit) {
            rec = true;
            CHECK(e.e.pad == 0 && e.e.vel == 99 && e.e.scene == 2, "RecordHit fields pad %d vel %d scene %d", e.e.pad,
                  e.e.vel, e.e.scene);
            CHECK(e.e.step == 4, "RecordHit step %d nudge %d", e.e.step, e.e.nudge);
        }
    CHECK(rec, "RecordHit emitted");
    CHECK(r.eng.telemetry().recording.load() == 1, "telemetry recording");
    r.cmd(CmdType::Stop);
    r.render(256);
    CHECK(r.eng.telemetry().playing.load() == 0 && r.eng.telemetry().step.load() == -1, "stopped");
    printf("  vel layers %.0f/%.0f Hz, RR %.0f %.0f %.0f %.0f, play_pos %.3f->%.3f, events ok=%d\n", f_lo, f_hi, rr[0],
           rr[1], rr[2], rr[3], pp1, pp2, (int)rec);
}

// LayerMode::Velocity with the default 0..127 ranges (nothing in the UI sets vel_lo/vel_hi):
// the usable layers split the velocity range in layer order, one voice per hit (not STACK).
static void testVelocityZones()
{
    printf("[velocity zones]\n");
    Rig& r = *newRig(48000, true);
    r.load(0, sine(300, 0.5, 48000), -1);
    r.load(1, sine(600, 0.5, 48000), -1);
    r.load(2, sine(900, 0.5, 48000), -1);
    Pad& p = r.prj.pad[0];
    filterOff(r.prj, 0);
    for (int l = 0; l < 3; ++l) p.layer[l].slot = (int16_t)l;
    p.layer[3].slot = -1;
    p.layer_mode = LayerMode::Velocity;
    struct Z {
        int vel;
        double hz;
    };
    auto hitAt = [&](int vel, int* voices) {
        r.cmd(CmdType::Panic);
        r.render(1200);
        const size_t a = r.pos();
        r.trig(0, vel);
        r.render(256);
        *voices = r.eng.telemetry().voices.load();
        r.render(9600);
        return freqZC(r.L, a + r.ms(20), a + r.ms(190), 48000);
    };
    for (const Z& z : {Z{1, 300}, Z{42, 300}, Z{43, 600}, Z{85, 600}, Z{86, 900}, Z{127, 900}}) {
        int nv = 0;
        const double f = hitAt(z.vel, &nv);
        CHECK(fabs(f - z.hz) < 1 && nv == 1, "vel %d: %.1f Hz (want %.0f), %d voices", z.vel, f, z.hz, nv);
    }
    // an empty layer in the middle: the zones are shared by the usable ones (LAYER 1 and 3)
    p.layer[1].slot = -1;
    int nv = 0;
    const double f_soft = hitAt(40, &nv), f_hard = hitAt(100, &nv);
    CHECK(fabs(f_soft - 300) < 1 && fabs(f_hard - 900) < 1, "two zones %.1f / %.1f Hz", f_soft, f_hard);
    // explicit ranges still win
    p.layer[1].slot = 1;
    p.layer[0].vel_lo = 0;
    p.layer[0].vel_hi = 127;
    p.layer[2].vel_lo = 100;
    p.layer[2].vel_hi = 127;
    hitAt(110, &nv);
    CHECK(nv == 3, "explicit ranges overlap: %d voices", nv);
    printf("  vel 1..127 -> 300/600/900 Hz zones, gaps shared, explicit ranges honoured\n");
    delete &r;
}

// A layer's slot is UI-written at any time: out-of-range values never index the slot table, and
// a slot changing between selection and voice start never plays an unvalidated sample.
static void testTornLayerSlots()
{
    printf("[layer slot races]\n");
    Rig& r = *newRig(48000, true);
    r.load(3, sine(440, 0.3, 48000), 0);
    Pad& p = r.prj.pad[0];
    p.layer[0].slot = 3;
    for (int16_t bad : {(int16_t)-1, (int16_t)-2, (int16_t)128, (int16_t)200, (int16_t)32767, (int16_t)-32768}) {
        p.layer[0].slot = bad;
        r.ev.clear();
        r.trig(0);
        r.render(256);
        CHECK(r.count(EvtType::PadHit) == 0 && r.eng.telemetry().voices.load() == 0, "slot %d played", (int)bad);
    }
    // a UI thread flips the slot between -1 / valid / out of range while hits keep firing
    std::atomic<bool> stop{false};
    std::thread ui([&] {
        uint32_t k = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const int16_t v = (k % 3 == 0) ? (int16_t)3 : (k % 3 == 1 ? (int16_t)-1 : (int16_t)(kMaxSamples + (int)(k & 7)));
            __atomic_store_n(&p.layer[0].slot, v, __ATOMIC_RELAXED);
            ++k;
        }
    });
    int hits = 0;
    for (int k = 0; k < 3000; ++k) {
        r.trig(0, 100);
        r.render(64, 64);
        hits += r.count(EvtType::PadHit);
        r.ev.clear();
    }
    stop.store(true);
    ui.join();
    CHECK(allFinite(r.L), "finite output under slot races");
    CHECK(hits > 0, "some hits played (%d)", hits);
    printf("  bad slots ignored, %d of 3000 racing hits played\n", hits);
    delete &r;
}

// CPU governor: sustained Telemetry::cpu > 0.8 sheds voices (click-free fades), < 0.6 gives them
// back; Engine::setVoiceLimit caps polyphony on top.
static void testGovernor()
{
    printf("[cpu governor / voice limit]\n");
    Rig& r = *newRig(48000, true);
    const double a_in = 0.01;
    for (int p = 0; p < kPads; ++p) {
        r.load(p, dc(a_in, 480000, 48000, 480), p);
        filterOff(r.prj, p);
        r.prj.pad[p].poly = 4;
        r.prj.pad[p].choke = 0;
    }
    const double a_out = a_in * 0.70710678;
    for (int k = 0; k < 2; ++k)
        for (int p = 0; p < kPads; ++p) r.trig(p);
    r.render(4800);
    Telemetry& t = r.eng.telemetry();
    CHECK(t.voices.load() == kVoices && t.voice_cap.load() == kVoices, "start: %d voices, cap %d", (int)t.voices.load(),
          (int)t.voice_cap.load());
    // the driver reports the measured render time after every block (128 frames = 2667 us)
    auto run = [&](int blocks, uint32_t render_us) {
        for (int b = 0; b < blocks; ++b) {
            r.render(128, 128);
            r.eng.reportTiming(render_us, 2667);
        }
    };
    const size_t a = r.pos();
    run(60, 2400);   // 90 % for 160 ms
    const int cap1 = t.voice_cap.load();
    run(12, 1800);   // 67 %: hold
    CHECK(cap1 < kVoices && cap1 >= Engine::kMinVoiceLimit, "overload cap %d", cap1);
    CHECK((int)t.voice_cap.load() == cap1 && (int)t.voices.load() <= cap1, "hold: cap %d, %d voices",
          (int)t.voice_cap.load(), (int)t.voices.load());
    const double st = maxStep(r.L, a, r.pos());
    CHECK(st < 0.05 * a_out, "shedding is click-free: step %.6f vs voice %.5f", st, a_out);
    run(30, 2660);   // ~100 %: sheds faster but never below the floor
    run(200, 2660);
    CHECK((int)t.voice_cap.load() == Engine::kMinVoiceLimit, "floor %d", (int)t.voice_cap.load());
    run(1200, 800);  // 30 % for 3.2 s: one voice back per 100 ms
    CHECK((int)t.voice_cap.load() == kVoices, "recovered cap %d", (int)t.voice_cap.load());
    for (int k = 0; k < 2; ++k)
        for (int p = 0; p < kPads; ++p) r.trig(p);
    r.render(2400);
    CHECK((int)t.voices.load() == kVoices, "full polyphony again: %d", (int)t.voices.load());
    // user limit
    r.eng.setVoiceLimit(12);
    CHECK(r.eng.voiceLimit() == 12, "voiceLimit %d", r.eng.voiceLimit());
    r.render(1200);
    CHECK((int)t.voice_cap.load() == 12 && (int)t.voices.load() <= 12, "user limit: cap %d, %d voices",
          (int)t.voice_cap.load(), (int)t.voices.load());
    for (int p = 0; p < kPads; ++p) r.trig(p);
    r.render(1200);
    CHECK((int)t.voices.load() <= 12, "new hits respect the limit: %d", (int)t.voices.load());
    r.eng.setVoiceLimit(1);
    CHECK(r.eng.voiceLimit() == Engine::kMinVoiceLimit, "limit clamped to %d", r.eng.voiceLimit());
    r.eng.setVoiceLimit(kVoices);
    printf("  overload cap %d, floor %d, recovered %d; user limit 12 ok; max step %.2g\n", cap1,
           Engine::kMinVoiceLimit, kVoices, st);
    delete &r;
}

// PlateReverb::setParams caches its libm work: a parameter changed later must sound exactly like
// one set from the start (and a change must still take effect).
static void testReverbParamCache()
{
    printf("[reverb parameter cache]\n");
    const int n = 128, fs = 48000;
    auto run = [&](float decay_before, float decay_after, float damp, std::vector<float>& out) {
        std::vector<float> mem((size_t)dsp::PlateReverb::memoryFloats(fs));
        dsp::PlateReverb rv;
        rv.init(mem.data(), fs);
        std::vector<float> in(n, 0.0f), l(n), rr(n);
        for (int b = 0; b < 100; ++b) {   // silence: the size glide settles
            rv.setParams(0.5f, decay_before, damp, 10.0f);
            rv.process(in.data(), l.data(), rr.data(), n);
        }
        out.clear();
        for (int b = 0; b < 2 * fs / n; ++b) {
            in.assign(n, 0.0f);
            if (b == 0) in[0] = 1.0f;
            rv.setParams(0.5f, decay_after, damp, 10.0f);
            rv.process(in.data(), l.data(), rr.data(), n);
            out.insert(out.end(), l.begin(), l.end());
        }
    };
    std::vector<float> fixed, changed, shortd, dark;
    run(6.0f, 6.0f, 0.3f, fixed);
    run(0.4f, 6.0f, 0.3f, changed);
    run(0.4f, 0.4f, 0.3f, shortd);
    run(6.0f, 6.0f, 1.0f, dark);
    double diff = 0;
    for (size_t i = 0; i < fixed.size(); ++i) diff = fmax(diff, fabs((double)fixed[i] - changed[i]));
    const double e_long = rms(fixed, 48000, 96000), e_short = rms(shortd, 48000, 96000);
    double ddiff = 0;
    for (size_t i = 0; i < fixed.size(); ++i) ddiff = fmax(ddiff, fabs((double)fixed[i] - dark[i]));
    CHECK(diff < 1e-6, "decay set later differs from decay set first by %.3g", diff);
    CHECK(e_long > 50 * e_short, "decay change takes effect: tail %.3g vs %.3g", e_long, e_short);
    CHECK(ddiff > 1e-4, "damp change takes effect (%.3g)", ddiff);
    printf("  late change identical (%.2g), tail 6 s %.3g vs 0.4 s %.3g\n", diff, e_long, e_short);
}

static void testSongThrow()
{
    printf("[song FX throw / SongSlotChanged / cpu telemetry]\n");
    auto run = [](bool fx_lane, int* slot_events, int* slot_seen) {
        Rig& r = *newRig();
        r.load(0, noise(0.08, 48000, 0.6), 0);   // kick pad: scene A plays it on every beat
        r.prj.pad[0].p[PP_SEND] = 0.2f;
        r.prj.song.follow = 1;
        r.prj.song.fx[0] = fx_lane ? 1 : 0;
        r.cmd(CmdType::Play);
        r.render(48000);
        *slot_events = r.count(EvtType::SongSlotChanged);
        *slot_seen = r.eng.telemetry().song_slot.load();
        const double v = rms(r.L, 0, r.pos());
        delete &r;
        return v;
    };
    int ev0 = 0, ev1 = 0, sl0 = -1, sl1 = -1;
    const double dry = run(false, &ev0, &sl0);
    const double thr = run(true, &ev1, &sl1);
    if (sl0 < 0) {
        printf("  skipped: the linked sequencer has no song mode (stub)\n");
    } else {
        CHECK(sl0 == 0 && sl1 == 0, "song slot %d / %d", sl0, sl1);
        CHECK(ev1 >= 1, "SongSlotChanged events %d", ev1);
        CHECK(thr > dry * 1.05, "FX throw raises the wet level: %.4f vs %.4f", thr, dry);
        printf("  slot %d, %d SongSlotChanged, rms %.4f -> %.4f with the FX lane\n", sl1, ev1, dry, thr);
    }
    Rig& c = *newRig();
    for (int k = 0; k < 40; ++k) c.eng.reportTiming(1000, 2667);
    CHECK(fabs(c.eng.telemetry().cpu.load() - 0.375f) < 0.01f, "cpu telemetry %.3f", c.eng.telemetry().cpu.load());
    c.eng.reportXrun();
    CHECK(c.eng.telemetry().xruns.load() == 1, "xruns");
    delete &c;
}

static void testCrush()
{
    printf("[crush / downsample]\n");
    Rig& r = *newRig();
    r.load(0, sine(100, 0.5, 48000, 0.9), 0);
    filterOff(r.prj, 0);
    r.prj.pad[0].p[PP_BITS] = 4.0f;
    size_t a = r.pos();
    r.trig(0);
    r.render(9600);
    std::vector<float> lv;
    for (size_t i = a + 500; i < a + 9000; ++i) {
        bool f = false;
        for (float v : lv) f |= fabs(v - r.L[i]) < 1e-5;
        if (!f) lv.push_back(r.L[i]);
    }
    CHECK(lv.size() <= 16, "4-bit levels %zu", lv.size());
    r.prj.pad[0].p[PP_BITS] = 16.0f;
    r.prj.pad[0].p[PP_DOWNSAMPLE] = 8.0f;
    r.cmd(CmdType::Panic);
    r.render(1200);
    a = r.pos();
    r.trig(0);
    r.render(4800);
    int changes = 0;
    for (size_t i = a + 1000; i < a + 4000; ++i) changes += r.L[i] != r.L[i - 1];
    CHECK(changes <= 3000 / 8 + 20, "downsample x8: %d changes in 3000 samples", changes);
    printf("  4-bit: %zu levels; x8 downsample: %d changes / 3000\n", lv.size(), changes);
}

// ACID BASS (0.3): the scene's bass line plays on the step grid into bus GP_BASS_BUS; slide, mute, live notes.
// SYNTH (0.4.9): the scene's synth line plays into bus GP_SYN_BUS (default 7), a stop releases the line, live notes
// sound until SynthOff, GP_SYN_MUTE silences the line, a repeated note is not cut short by the older note's off.
// 0.4.9: SYNTH knobs keep working although the engine re-reads them only on a change, and the render on the helper
// core (dual, the default) gives the same output as the inline one (on the PC the helper runs the job at once, so
// this checks the engine's dual path: begin / job / wait / mix).
// 0.4.9: the block-wise stereo delay gives the same bits as its per-sample reference code (core/fxcheck.cpp: noise
// bursts, delay-time crossfades, feedback / tone / ping-pong moves, block lengths 1..128, delays shorter than a block
// that take the fallback path).
static void testFxBlockExact()
{
    for (int rate : {48000, 44100}) {
        const FxCheck c = fxSelfCheck(rate, 3000);
        CHECK(c.ran, "fxcheck %d Hz: memory", rate);
        CHECK(c.delay_diff == 0, "fxcheck %d Hz: delay block == reference (%lld of %lld samples differ)", rate,
              (long long)c.delay_diff, (long long)c.samples);
        CHECK(c.delay_block > c.blocks / 2 && c.delay_block < c.blocks, "fxcheck %d Hz: delay block path %lld of %lld",
              rate, (long long)c.delay_block, (long long)c.blocks);
        CHECK(c.delay_rms > 1e-3, "fxcheck %d Hz: signal (delay rms %.4f)", rate, c.delay_rms);
    }
}

// 0.4.9: FxBus::reserveLines (the Tab5 puts the reverb's lines into internal RAM at boot): one FxBus at a time uses
// the block (a second one takes PSRAM), a rate change re-uses it, and the output is the same as with lines allocated
// per init. Runs last: the reservation is global.
static void testFxReservedLines()
{
    FxBus* plain = new FxBus();
    CHECK(plain->init(48000) && !plain->linesInternal(), "no reservation: lines allocated per init");
    CHECK(FxBus::reserveLines(48000) && FxBus::linesReserved(), "reserveLines(48000)");
    FxBus* resv = new FxBus();
    FxBus* other = new FxBus();
    CHECK(resv->init(48000) && resv->linesInternal(), "the first FxBus takes the reserved block");
    CHECK(other->init(48000) && !other->linesInternal(), "a second FxBus at the same time takes PSRAM");
    delete other;
    bool same = true;
    FxParams p;
    std::vector<float> sl(128), sr(128), sp(128), al(128), ar(128), bl(128), br(128);
    uint32_t rng = 1;
    for (int rate : {48000, 44100, 48000}) {
        CHECK(plain->init(rate) && resv->init(rate) && resv->linesInternal(), "rate %d: re-init keeps the block", rate);
        p.delay_samples = 0.25f * (float)rate;
        for (int b = 0; b < 600; ++b) {
            for (int i = 0; i < 128; ++i) {
                rng = rng * 1664525u + 1013904223u;
                const float x = (b % 40) < 4 ? ((float)(rng >> 9) / 4194304.0f - 1.0f) * 0.5f : 0.0f;
                sl[i] = x;
                sr[i] = -x;
                sp[i] = 0.3f * x;
            }
            plain->process(sl.data(), sr.data(), sp.data(), 128, p, al.data(), ar.data());
            resv->process(sl.data(), sr.data(), sp.data(), 128, p, bl.data(), br.data());
            for (int i = 0; i < 128 && same; ++i) same = al[i] == bl[i] && ar[i] == br[i];
        }
    }
    CHECK(same, "reverb output with reserved lines == lines allocated per init (48 / 44.1 / 48 kHz)");
    delete resv;
    FxBus* again = new FxBus();
    CHECK(again->init(48000) && again->linesInternal(), "a released block can be taken again");
    delete again;
    delete plain;
}

static void testSynthKnobsCores()
{
    Rig& a = *newRig(48000, true);
    Rig& b = *newRig(48000, true);
    b.eng.setDualCore(false);
    CHECK(a.eng.dualCore() && !b.eng.dualCore(), "setDualCore / dualCore");
    for (Rig* r : {&a, &b}) {
        r->prj.g[GP_SYN_SEND] = 0.0f;
        r->cmd(CmdType::SynthNote, 0, 100, 57);
        r->cmd(CmdType::SynthNote, 0, 100, 64);
        r->render(r->ms(300));
    }
    CHECK(a.eng.telemetry().dual.load() == 1 && b.eng.telemetry().dual.load() == 0, "Telemetry::dual %d / %d",
          (int)a.eng.telemetry().dual.load(), (int)b.eng.telemetry().dual.load());
    const double before = rms(a.L, a.pos() - a.ms(150), a.pos());
    for (Rig* r : {&a, &b}) {
        r->prj.g[GP_SYN_CUTOFF] = 150.0f;   // Hz (default 1280): darker and quieter
        r->render(r->ms(300));
    }
    const double dark = rms(a.L, a.pos() - a.ms(150), a.pos());
    CHECK(before > 0.01 && dark < 0.5 * before, "SYNTH cutoff change heard while a note holds (%.4f -> %.4f)", before, dark);
    for (Rig* r : {&a, &b}) {
        r->prj.g[GP_SYN_CUTOFF] = 20480.0f;
        r->prj.g[GP_SYN_OSC1_WAVE] = 1.0f;   // SQUARE
        r->render(r->ms(200));
    }
    const double open = rms(a.L, a.pos() - a.ms(100), a.pos());
    CHECK(open > 1.5 * dark, "SYNTH cutoff back open + wave change heard (%.4f)", open);
    for (Rig* r : {&a, &b}) {
        r->prj.g[GP_SYN_LEVEL] = -60.0f;   // dB
        r->render(r->ms(300));
    }
    CHECK(rms(a.L, a.pos() - a.ms(100), a.pos()) < 0.02 * open, "SYNTH level change heard");
    bool same = a.L.size() == b.L.size();
    for (size_t i = 0; same && i < a.L.size(); ++i) same = a.L[i] == b.L[i] && a.R[i] == b.R[i];
    CHECK(same, "dual and inline SYNTH render give the same samples");
    CHECK(a.eng.telemetry().helper_stalls.load() == 0, "no helper stalls");
}

static void testSynth()
{
    Rig& r = *newRig(48000, true);
    r.prj.g[GP_SYN_SEND] = 0.0f;          // dry: the silence checks must not hear reverb / delay tails
    r.prj.g[GP_SYN_A_RELEASE] = 30.0f;    // ms
    r.prj.g[GP_SYN_A_SUSTAIN] = 0.8f;
    Scene& sc = r.prj.scene_data[0];
    auto put = [&](int step, int slot, int note, int vel, int len) {
        SynthNote& n = sc.synth.step[step].n[slot];
        n.note = (uint8_t)note;
        n.vel = (uint8_t)vel;
        n.len = (uint8_t)len;
    };
    put(0, 0, 60, 110, 2);   // a chord on step 0, two steps long
    put(0, 1, 64, 100, 2);
    put(8, 0, 67, 100, 1);
    const uint32_t n0 = r.eng.telemetry().synth_notes.load();
    r.cmd(CmdType::Play);
    r.render(r.ms(100));
    CHECK(r.eng.telemetry().synth_voices.load() == 2, "chord: 2 synth voices (%d)", (int)r.eng.telemetry().synth_voices.load());
    CHECK(r.eng.telemetry().bus_peak[6].load() > 0.01f, "synth reaches bus 7 (%.4f)", (double)r.eng.telemetry().bus_peak[6].load());
    r.render(r.ms(960));   // 120 BPM 1/16: step 8 starts at 1.000 s
    const uint32_t notes = r.eng.telemetry().synth_notes.load() - n0;
    CHECK(notes == 3, "synth line notes in 1 s: %u (expect 3)", (unsigned)notes);
    CHECK(allFinite(r.L) && allFinite(r.R), "synth output finite");
    const double pk = peakAbs(r.L, 0, r.L.size());
    CHECK(pk > 0.02 && pk < 1.5, "synth peak %.3f", pk);
    // between the notes (step 2..7: the chord ended after 2 steps minus 1/16 and its 30 ms release) it is silent
    {
        Rig& q = *newRig(48000, true);
        q.prj.g[GP_SYN_SEND] = 0.0f;
        q.prj.g[GP_SYN_A_RELEASE] = 30.0f;
        q.prj.scene_data[0].synth = sc.synth;
        q.cmd(CmdType::Play);
        q.render(q.ms(220));   // step 2 starts at 250 ms; the gate ended at ~242 ms - 1/16 step
        q.render(q.ms(110));   // gate end ~242 ms + 30 ms release: silent from ~273 ms
        CHECK(peakAbs(q.L, q.pos() - q.ms(40), q.pos()) < 1e-3, "line note ended after its length (%.5f)",
              peakAbs(q.L, q.pos() - q.ms(40), q.pos()));
    }
    // the mixer: bus 7 muted -> silent
    r.prj.bus[6].mute = 1;
    r.cmd(CmdType::Play);
    r.render(r.ms(100));
    CHECK(peakAbs(r.L, r.pos() - r.ms(50), r.pos()) < 1e-4, "bus 7 muted: synth silent");
    r.prj.bus[6].mute = 0;
    // stop releases the line's held notes
    sc.synth.step[0].n[0].len = 16;
    sc.synth.step[0].n[1].len = 16;
    r.cmd(CmdType::Play);
    r.render(r.ms(200));
    CHECK(peakAbs(r.L, r.pos() - r.ms(50), r.pos()) > 0.01, "a long line note sounds");
    r.cmd(CmdType::Stop);
    r.render(r.ms(200));
    CHECK(peakAbs(r.L, r.pos() - r.ms(80), r.pos()) < 1e-3, "stop: synth line released (%.5f)",
          peakAbs(r.L, r.pos() - r.ms(80), r.pos()));
    // live notes (keyboard / MIDI): until SynthOff, also while stopped
    r.cmd(CmdType::SynthNote, 0, 100, 69);
    r.render(r.ms(200));
    CHECK(peakAbs(r.L, r.pos() - r.ms(100), r.pos()) > 0.01, "live synth note sounds");
    r.cmd(CmdType::SynthOff, 0, 0, 69);
    r.render(r.ms(200));
    CHECK(peakAbs(r.L, r.pos() - r.ms(80), r.pos()) < 1e-3, "SynthOff: silent");
    // GP_SYN_MUTE: the line plays no note
    r.prj.g[GP_SYN_MUTE] = 1.0f;
    const uint32_t m0 = r.eng.telemetry().synth_notes.load();
    r.cmd(CmdType::Play);
    r.render(r.ms(1000));
    CHECK(r.eng.telemetry().synth_notes.load() == m0, "GP_SYN_MUTE: no line notes");
    r.cmd(CmdType::Stop);
    r.prj.g[GP_SYN_MUTE] = 0.0f;
    // the same note on every step (len 1): each one sounds its full step (the older off does not cut the newer)
    {
        Rig& q = *newRig(48000, true);
        q.prj.g[GP_SYN_SEND] = 0.0f;
        q.prj.g[GP_SYN_A_RELEASE] = 5.0f;
        q.prj.scene_data[0].swing = 66;   // swung odd steps start late: gates overlap the next even step
        for (int s = 0; s < 16; ++s) {
            SynthNote& n = q.prj.scene_data[0].synth.step[s].n[0];
            n.note = 57;
            n.vel = 100;
            n.len = 1;
        }
        const size_t start = q.pos();
        q.cmd(CmdType::Play);
        q.render(q.ms(1600));
        // An odd step starts 40 ms late (swing 66 %) and its gate (117 ms) ends 32 ms into the next even step, which
        // retriggered the same note: that older off must not end the even step's note (it sounds to ~117 ms).
        bool cut = false;
        for (int k = 2; k < 12; k += 2) {
            const size_t a = start + q.ms(125.0 * k + 45.0), b = start + q.ms(125.0 * k + 105.0);
            if (peakAbs(q.L, a, b) < 0.005) cut = true;
        }
        CHECK(!cut, "repeated note: the older note's off does not cut the retriggered one");
    }
}

static void testBass()
{
    Rig& r = *newRig(48000, true);
    r.prj.g[GP_BASS_SEND] = 0.0f;   // dry: the silence checks below must not hear reverb / delay tails
    Scene& sc = r.prj.scene_data[0];
    sc.bass = BassLine{};
    for (int i = 0; i < 16; i += 2) {
        sc.bass.step[i].gate = 1;
        sc.bass.step[i].note = (int8_t)((i / 2) % 2 ? 12 : 0);
    }
    sc.bass.step[4].slide = 1;   // into step 6
    sc.bass.step[5].gate = 0;
    sc.bass.step[6].accent = 1;
    const uint32_t n0 = r.eng.telemetry().bass_notes.load();
    r.cmd(CmdType::Play);
    r.render(r.ms(1000));   // 120 BPM 1/16: 8 steps, 4 notes
    const uint32_t notes = r.eng.telemetry().bass_notes.load() - n0;
    CHECK(notes >= 3 && notes <= 5, "bass notes in 1 s: %u (expect 4)", (unsigned)notes);
    CHECK(allFinite(r.L) && allFinite(r.R), "bass output finite");
    const double pk = peakAbs(r.L, 0, r.L.size());
    CHECK(pk > 0.05 && pk < 1.5, "bass peak %.3f", pk);
    CHECK(r.eng.telemetry().bus_peak[7].load() > 0.0f || peakAbs(r.L, r.pos() - r.ms(100), r.pos()) >= 0.0,
          "bass reaches bus 8");
    // the bass bus fader controls it
    r.prj.bus[7].mute = 1;
    r.render(r.ms(300));
    CHECK(peakAbs(r.L, r.pos() - r.ms(150), r.pos()) < 1e-4, "bus 8 muted: silent (%.6f)",
          peakAbs(r.L, r.pos() - r.ms(150), r.pos()));
    r.prj.bus[7].mute = 0;
    // GP_BASS_MUTE: no new notes
    r.prj.g[GP_BASS_MUTE] = 1.0f;
    const uint32_t m0 = r.eng.telemetry().bass_notes.load();
    r.render(r.ms(1000));
    CHECK(r.eng.telemetry().bass_notes.load() == m0, "muted bass starts no note");
    r.prj.g[GP_BASS_MUTE] = 0.0f;
    // Stop closes a held note: silence soon after
    r.cmd(CmdType::Stop);
    r.render(r.ms(400));
    CHECK(peakAbs(r.L, r.pos() - r.ms(100), r.pos()) < 1e-3, "stop: bass silent (%.5f)",
          peakAbs(r.L, r.pos() - r.ms(100), r.pos()));
    // live note (BASS keyboard): sounds until BassOff
    r.cmd(CmdType::BassNote, 0, 1, kBassRootNote + 7);
    r.render(r.ms(200));
    CHECK(peakAbs(r.L, r.pos() - r.ms(100), r.pos()) > 0.02, "live bass note sounds");
    r.cmd(CmdType::BassOff);
    r.render(r.ms(300));
    CHECK(peakAbs(r.L, r.pos() - r.ms(100), r.pos()) < 1e-3, "BassOff: silent");
    // KEYS (0.4.2): a held live note owns the voice while the line plays; legato flag accepted; the line goes on
    // after the release
    r.cmd(CmdType::Play);
    r.render(r.ms(300));
    r.cmd(CmdType::BassNote, 0, 1, kBassRootNote + 5);
    r.render(r.ms(50));
    r.cmd(CmdType::BassNote, 0, 1 | 2, kBassRootNote + 7);   // slide to the next key (finger moved)
    r.render(r.ms(20));                                      // (the slid note itself counts as one)
    const uint32_t h0 = r.eng.telemetry().bass_notes.load();
    r.render(r.ms(1000));   // the line would start ~4 notes in 1 s
    CHECK(r.eng.telemetry().bass_notes.load() == h0, "held live note: the line waits (%u notes)",
          (unsigned)(r.eng.telemetry().bass_notes.load() - h0));
    CHECK(peakAbs(r.L, r.pos() - r.ms(200), r.pos()) > 0.02, "the held live note keeps sounding");
    r.cmd(CmdType::BassOff);
    const uint32_t h1 = r.eng.telemetry().bass_notes.load();
    r.render(r.ms(1000));
    CHECK(r.eng.telemetry().bass_notes.load() - h1 >= 3, "released: the line plays again (%u notes)",
          (unsigned)(r.eng.telemetry().bass_notes.load() - h1));
    r.cmd(CmdType::Stop);
    r.render(r.ms(400));
    CHECK(peakAbs(r.L, r.pos() - r.ms(100), r.pos()) < 1e-3, "stop after KEYS: silent");
    delete &r;
}

static void testStress()
{
    printf("[stress: NaN / limiter ceiling]\n");
    Rig& r = *newRig();
    for (int p = 0; p < kPads; ++p) {
        r.load(p, p % 2 ? noise(1.5, 48000, 1.0, 0.0, 1 + p % 3 / 2) : sine(55.0 * (p + 1), 1.5, 48000, 1.0), p);
        Pad& pd = r.prj.pad[p];
        pd.p[PP_GAIN] = 24.0f;
        pd.p[PP_VOLUME] = 6.0f;
        pd.p[PP_RESO] = 1.0f;
        pd.p[PP_DRIVE] = (p % 3) * 0.5f;
        pd.p[PP_CUTOFF] = 200.0f + 900.0f * p;
        pd.p[PP_FTYPE] = (float)(p % 4);
        pd.p[PP_FENV] = 1.0f;
        pd.p[PP_SEND] = 1.0f;
        pd.p[PP_LFO_DEPTH] = 1.0f;
        pd.p[PP_LFO_SHAPE] = (float)(p % 5);
        pd.p[PP_LFO_TARGET] = (float)(p % 4);
        pd.p[PP_LFO_SYNC] = (float)(p % 2);
        pd.p[PP_PENV] = 24.0f;
        pd.p[PP_BITS] = (p % 4 == 0) ? 6.0f : 16.0f;
        pd.p[PP_DOWNSAMPLE] = (p % 5 == 0) ? 4.0f : 1.0f;
        pd.reverse = p % 3 == 0;
        pd.mode = (PlayMode)(p % 3);
        pd.poly = 8;
    }
    for (int b = 0; b < kBuses; ++b) r.prj.bus[b].level = 1.0f;
    r.prj.g[GP_MASTER] = 6.0f;
    r.prj.g[GP_DLY_FEEDBACK] = 0.95f;
    r.prj.g[GP_REV_DECAY] = 12.0f;
    r.prj.lofi_on = r.prj.wide_on = r.prj.pump_on = r.prj.space_on = 1;
    r.cmd(CmdType::Play);
    for (int k = 0; k < 400; ++k) {
        r.trig(k % kPads, 1 + (k * 37) % 127, (k % 7) - 3, (Producer)(k % 4));
        if (k % 13 == 0) r.cmd(CmdType::Release, (k / 13) % kPads);
        if (k % 50 == 0) r.prj.bpm = 60.0f + (float)(k % 200);
        r.render(600);
    }
    const double pk = std::max(peakAbs(r.L, 0, r.pos()), peakAbs(r.R, 0, r.pos()));
    CHECK(allFinite(r.L) && allFinite(r.R), "stress NaN/Inf");
    CHECK(pk <= 0.9661 + 1e-4, "limiter ceiling %.5f (-0.3 dBFS = 0.96605)", pk);
    CHECK(r.eng.telemetry().gain_reduction.load() > 0.0f, "gain reduction reported %.1f dB",
          r.eng.telemetry().gain_reduction.load());
    // int16 path stays clipped and finite
    std::vector<int16_t> buf(2 * 4800);
    r.eng.processI16(buf.data(), 4800);
    printf("  peak %.5f, GR %.1f dB, voices %d\n", pk, r.eng.telemetry().gain_reduction.load(),
           (int)r.eng.telemetry().voices.load());
    // 16 loud voices at once, plain
    Rig& q = *newRig();
    for (int p = 0; p < kPads; ++p) {
        q.load(p, sine(80.0 + 20 * p, 1.0, 48000, 1.0), p);
        q.prj.pad[p].p[PP_GAIN] = 12.0f;
    }
    for (int p = 0; p < kPads; ++p) q.trig(p);
    q.render(24000);
    const double pq = std::max(peakAbs(q.L, 0, q.pos()), peakAbs(q.R, 0, q.pos()));
    CHECK(pq <= 0.9661 + 1e-4, "16 loud voices peak %.5f", pq);
    printf("  16 loud voices peak %.5f (GR %.1f dB)\n", pq, q.eng.telemetry().gain_reduction.load());
}

// ------------------------------------------------------------------ benchmark

// heavy = 1/4 stereo, 44.1 kHz material (interpolated), LP/BP/HP with resonance, envelope and
// LFO, drive on half, FX sends. heavy = false: the common case (48 kHz mono kit samples at
// native pitch, default open filter, no drive, no sends).
static double benchmark(bool heavy, bool fx, double* tsc_per_frame)
{
    Rig& r = *newRig(48000, true);
    for (int p = 0; p < kPads; ++p) {
        const int ch = (heavy && p % 4 == 3) ? 2 : 1;
        const int sr = heavy ? 44100 : 48000;
        r.load(p, ch == 2 ? noise(14.0, sr, 0.3, 0.0, 2) : sine(70.0 + 37.0 * p, 14.0, sr, 0.3), p);
        Pad& pd = r.prj.pad[p];
        pd.poly = 2;
        pd.choke = 0;
        pd.p[PP_VOLUME] = -12.0f;
        if (!heavy) continue;
        pd.p[PP_FTYPE] = (float)(p % 3);
        pd.p[PP_CUTOFF] = 800.0f + 300.0f * p;
        pd.p[PP_RESO] = 0.6f;
        pd.p[PP_FENV] = 0.4f;
        pd.p[PP_FDECAY] = 500.0f;
        pd.p[PP_DRIVE] = (p % 2) ? 0.3f : 0.0f;
        pd.p[PP_LFO_DEPTH] = 0.3f;
        pd.p[PP_LFO_RATE] = 3.0f;
        pd.p[PP_TUNE] = (float)((p % 7) - 3) + 0.37f;
        pd.p[PP_SEND] = fx ? 0.3f : 0.0f;
    }
    for (int p = 0; p < kPads; ++p) r.trig(p);
    r.render(256);
    for (int p = 0; p < kPads; ++p) r.trig(p);
    r.render(256);
    const int voices = r.eng.telemetry().voices.load();
    const int frames = 48000 * 10;
    std::vector<float> l(128), rr(128);
    const auto t0 = std::chrono::steady_clock::now();
#ifdef HAVE_RDTSC
    const unsigned long long c0 = __rdtsc();
#endif
    for (int done = 0; done < frames; done += 128) r.eng.process(l.data(), rr.data(), 128);
#ifdef HAVE_RDTSC
    const unsigned long long c1 = __rdtsc();
    *tsc_per_frame = (double)(c1 - c0) / frames;
#else
    *tsc_per_frame = 0;
#endif
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / frames;
    CHECK(voices == 32, "benchmark voices %d", voices);
    CHECK(r.eng.telemetry().voices.load() == 32, "benchmark voices at the end %d", (int)r.eng.telemetry().voices.load());
    delete &r;
    return ns;
}

static void runBenchmark()
{
    printf("[benchmark] 32 voices for 10 s of audio in 128-frame blocks, limiter on\n");
    double t_heavy = 0, t_dry = 0, t_typ = 0;
    benchmark(true, true, &t_heavy);   // warm-up
    const double ns_heavy = benchmark(true, true, &t_heavy);
    const double ns_dry = benchmark(true, false, &t_dry);
    const double ns_typ = benchmark(false, false, &t_typ);
    const double budget_ns = 1e9 / 48000;
    printf("  PC heavy  (1/4 stereo, 44.1k->48k, filters+reso+env+LFO, drive, reverb+delay): %.0f ns/frame (%.1f %% RT)\n",
           ns_heavy, 100 * ns_heavy / budget_ns);
    printf("  PC heavy without FX sends (FX bus idle):                                     %.0f ns/frame\n", ns_dry);
    printf("  PC typical (48k mono kit samples, native pitch, open filter):                %.0f ns/frame\n", ns_typ);
    printf("  FX memory @48 kHz: reverb %.0f KB + delay %.0f KB (PSRAM)\n",
           dsp::PlateReverb::memoryFloats(48000) * 4.0 / 1024.0, 2.0 * StereoDelay::framesFor(48000) * 4.0 / 1024.0);
    if (t_heavy > 0) {
        // Scaling assumption (ESP32-P4 HP core @360 MHz, in-order RV32IMAFC, FPU without SIMD):
        // P4 cycles = PC TSC cycles x 3 (range x2.5 .. x4). Basis: ESP-IDF GCC 14.2 -O2 emits per
        // voice-sample ~33 instructions for the mono Hermite read, 15 for the SVF, 21 for env+mix
        // and ~15 for control-rate work; at an assumed P4 IPC of 0.7..1.0 that is 2.5..4x the TSC
        // cycles this x86 core (i7-9750H, TSC 2.6 GHz, turbo ~3.3 GHz) needs. The host is shared
        // with other jobs, so the PC numbers are pessimistic. Budget: 360e6 / 48000 = 7500 cycles.
        const double budget = 360e6 / 48000;
        auto est = [&](const char* name, double tsc) {
            printf("  ESP32-P4 estimate %-8s: %5.0f TSC cycles/frame -> %5.0f P4 cycles = %3.0f %% of one core (%.0f..%.0f %%)\n",
                   name, tsc, tsc * 3.0, 100 * tsc * 3.0 / budget, 100 * tsc * 2.5 / budget, 100 * tsc * 4.0 / budget);
        };
        est("heavy", t_heavy);
        est("no FX", t_dry);
        est("typical", t_typ);
    }
}

// ------------------------------------------------------------------ demo

static void writeWav(const char* path, const std::vector<int16_t>& lr, int rate)
{
    FILE* f = fopen(path, "wb");
    if (!f) {
        printf("  cannot write %s\n", path);
        return;
    }
    const uint32_t data = (uint32_t)(lr.size() * 2);
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
    fwrite(lr.data(), 2, lr.size(), f);
    fclose(f);
}

static void renderDemo()
{
    printf("[demo] host/out/engine_demo.wav\n");
    const int sr = 48000;
    Rig& r = *newRig(sr);
    Sample* kick = mkSample(sr * 6 / 10, 1, sr, [&](int i, int) {
        const double t = (double)i / sr;
        const double ph = 2 * kPiD * (45 * t + (160 - 45) * 0.04 * (1 - exp(-t / 0.04)));
        return 0.95 * sin(ph) * exp(-t / 0.35) * (t < 0.002 ? t / 0.002 : 1.0) + 0.25 * exp(-t / 0.003) * frand();
    });
    double hp = 0, prev = 0;
    Sample* snare = mkSample(sr * 4 / 10, 1, sr, [&](int i, int) {
        const double t = (double)i / sr;
        const double n = frand();
        hp = n - prev;
        prev = n;
        return 0.5 * (sin(2 * kPiD * 185 * t) + 0.6 * sin(2 * kPiD * 330 * t)) * exp(-t / 0.06) +
               0.45 * hp * exp(-t / 0.13);
    });
    auto hat = [&](double decay) {
        double p1 = 0;
        return mkSample((int)(sr * decay * 6), 1, sr, [&, decay](int i, int) {
            const double t = (double)i / sr;
            const double n = frand();
            const double h = n - p1;
            p1 = n;
            return 0.55 * h * exp(-t / decay);
        });
    };
    Sample* chh = hat(0.035);
    Sample* ohh = hat(0.30);
    Sample* clap = mkSample(sr * 4 / 10, 1, sr, [&](int i, int) {
        const double t = (double)i / sr;
        double e = 0;
        for (int k = 0; k < 3; ++k) {
            const double tk = t - 0.011 * k;
            if (tk >= 0) e += exp(-tk / 0.006);
        }
        e += 0.6 * exp(-t / 0.12) * (t > 0.022);
        return 0.6 * e * frand();
    });
    Sample* rim = mkSample(sr / 10, 1, sr, [&](int i, int) {
        const double t = (double)i / sr;
        return 0.7 * (sin(2 * kPiD * 1720 * t) + 0.5 * sin(2 * kPiD * 520 * t)) * exp(-t / 0.012);
    });
    Sample* set[6] = {kick, snare, chh, ohh, clap, rim};
    for (int p = 0; p < 6; ++p) r.load(p, set[p], p);
    r.prj.g[GP_MASTER] = -3.0f;   // headroom: the limiter only catches kick peaks
    r.prj.pad[4].p[PP_SEND] = 0.35f;   // clap
    r.prj.pad[3].p[PP_SEND] = 0.2f;    // open hat
    r.prj.pad[5].p[PP_SEND] = 0.4f;    // rim
    r.prj.pad[0].p[PP_PENV] = 7.0f;    // kick snap
    r.prj.pad[0].p[PP_PDECAY] = 40.0f;
    r.prj.pad[2].p[PP_CUTOFF] = 9000.0f;
    r.prj.pad[2].p[PP_FTYPE] = (float)FilterType::HighPass;
    r.prj.pad[2].p[PP_CUTOFF] = 6000.0f;
    r.prj.pad[3].p[PP_PAN] = 0.25f;
    r.prj.pad[2].p[PP_PAN] = -0.2f;
    r.cmd(CmdType::Play);
    std::vector<int16_t> out;
    std::vector<int16_t> buf(256);
    const int total = sr * 8;
    float gr_max = 0.0f;
    for (int done = 0; done < total; done += 128) {
        if (done == sr * 4) r.prj.pump_on = 1;   // second half: PUMP on
        r.eng.processI16(buf.data(), 128);
        out.insert(out.end(), buf.begin(), buf.end());
        gr_max = std::max(gr_max, r.eng.telemetry().gain_reduction.load());
        Event e;
        while (r.eng.poll(e)) {}
    }
    writeWav("host/out/engine_demo.wav", out, sr);
    int pk = 0;
    for (int16_t v : out) pk = std::max(pk, abs((int)v));
    CHECK(pk > 8000, "demo audible (peak %d)", pk);
    printf("  8 s, peak %d (%.1f dBFS), max limiter reduction %.1f dB\n", pk, 20 * log10(pk / 32768.0), gr_max);
}

int main(int argc, char** argv)
{
    const bool bench_only = argc > 1 && strcmp(argv[1], "--bench-only") == 0;
    if (!bench_only) {
        testPitch();
        testRegion();
        testLoop();
        testEnvelope();
        testFilter();
        testPan();
        testBuses();
        testFx();
        testChoke();
        testStealing();
        testRetire();
        testReinitState();
        testPreview();
        testMacros();
        testMetronome();
        testLayersEvents();
        testVelocityZones();
        testTornLayerSlots();
        testGovernor();
        testReverbParamCache();
        testSongThrow();
        testCrush();
        testBass();
        testSynth();
        testSynthKnobsCores();
        testFxBlockExact();
        testStress();
        renderDemo();
    }
    runBenchmark();
    testFxReservedLines();   // last: the reservation is global
    if (g_fail) printf("test_engine: %d of %d checks FAILED\n", g_fail, g_checks);
    else printf("test_engine: all %d checks passed\n", g_checks);
    return g_fail ? 1 : 0;
}
