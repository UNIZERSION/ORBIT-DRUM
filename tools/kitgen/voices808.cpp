// "808 STYLE" kit: circuit-level recipes from published descriptions of the classic analogue
// machine (service-manual voice table, Sound On Sound analyses). Own synthesis, no samples.
// Decay values quoted as "T" are times to -20 dB (tau = T / ln 10).
#include "voices.h"

namespace kitgen {

namespace {

// Bass drum: bridged-T resonator excited by a 1 ms trigger. While the trigger transistor
// conducts the resonator runs at ~2x for about half a period (4-6 ms), then it settles with a
// slow downward 'sigh' of a few Hz. TONE = output low-pass (mostly shapes the click).
Buf bd808(float f_end, float sigh, float sigh_tau, float t20, float gauss_s, float seconds, float click)
{
    const int n = osFrames(seconds);
    Buf b((size_t)n);
    Phasor ph;
    OnePole clp, chp, tone;
    clp.set(2500.0f);
    chp.set(120.0f);
    tone.set(4000.0f);
    const float tau = tau20(t20);
    for (int i = 0; i < n; ++i) {
        const float t = osTime(i);
        const float f = f_end + sigh * expf(-t / sigh_tau);
        float m = 1.0f;
        if (t < 0.0036f)
            m = 2.0f;
        else if (t < 0.0052f)
            m = 1.5f + 0.5f * cosf(kPi * (t - 0.0036f) / 0.0016f);
        const float p = ph.step(f * m);
        const float g = gauss_s > 0.0f ? (t / gauss_s) * (t / gauss_s) : 0.0f;
        const float body = expf(-t / tau - g) * sinOf(p);
        const float trig = t < 0.001f ? 1.0f : 0.0f;
        b[(size_t)i] = tone.lp(body + click * chp.hp(clp.lp(trig)));
    }
    return b;
}

Buf kick() { return bd808(54.0f, 4.5f, 0.12f, 0.36f, 1.4f, 1.6f, 0.22f); }
Buf kickLong() { return bd808(52.0f, 5.0f, 0.18f, 0.80f, 2.4f, 3.2f, 0.18f); }

// Snare: two bridged-T oscillators 238 + 476 Hz (decay ~60 ms) + white noise through a
// high-pass ('snappy' VCA).
Buf snare()
{
    const int n = osFrames(0.6f);
    Buf tone((size_t)n), nz((size_t)n);
    Phasor p1, p2;
    Rng r(0x80802u);
    Svf hp;
    hp.set(1800.0f, 0.7f);
    OnePole lp;
    lp.set(9000.0f);
    for (int i = 0; i < n; ++i) {
        const float t = osTime(i);
        const float bend = 1.0f + 0.06f * expf(-t / 0.004f);
        tone[(size_t)i] = expf(-t / tau20(0.060f)) * sinOf(p1.step(238.0f * bend)) +
                          0.55f * expf(-t / tau20(0.040f)) * sinOf(p2.step(476.0f * bend));
        nz[(size_t)i] = lp.lp(hp.hp(r.uni())) * rampUp(t, 0.0004f) * expf(-t / tau20(0.075f));
    }
    return mixNorm(tone, 0.0f, nz, -2.0f, 0.03f);
}

// Six Schmitt-trigger square oscillators of the cymbal / hi-hat section (duty ~48 %).
struct Metal808 {
    Pulse o[6];
    explicit Metal808(uint32_t seed)
    {
        static const float f[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
        Rng r(seed);
        for (int i = 0; i < 6; ++i) o[i].set(f[i], 0.4798f, r.u01());
    }
    float tick()
    {
        float s = 0.0f;
        for (Pulse& x : o) s += x.tick();
        return s * (1.0f / 6.0f);
    }
};

// 3rd-order resonant high-pass ~10.5 kHz (2-pole SVF + 1-pole).
struct Hpf3 {
    Svf a;
    OnePole b;
    Hpf3()
    {
        a.set(10500.0f, 1.3f);
        b.set(10500.0f);
    }
    float tick(float x) { return b.hp(a.hp(x)); }
};

// Hi-hats: metal -> BPF 7.1 kHz -> VCA -> HPF3 10.5 kHz.
Buf hat808(float t20, float seconds, uint32_t seed)
{
    const int n = osFrames(seconds);
    Buf b((size_t)n);
    Metal808 m(seed);
    Svf bp;
    bp.set(7100.0f, 2.8f);
    Hpf3 hp;
    const float tau = tau20(t20);
    for (int i = 0; i < n; ++i) {
        const float t = osTime(i);
        const float env = rampUp(t, 0.0002f) * expf(-t / tau);
        b[(size_t)i] = hp.tick(bp.bp(m.tick()) * env);
    }
    return b;
}
Buf clHat() { return hat808(0.050f, 0.4f, 0x808C4u); }
Buf opHat() { return hat808(0.450f, 2.4f, 0x808C4u); }

// Cymbal: low band (BPF 3.44 kHz, long VCA) + high band (BPF 7.1 kHz, short + long VCA, HPF3).
Buf cymbal()
{
    const int n = osFrames(4.2f);
    Buf lo((size_t)n), hi((size_t)n);
    Metal808 m(0x808C9u);
    Svf b1, b2;
    b1.set(3440.0f, 2.2f);
    b2.set(7100.0f, 2.8f);
    OnePole lhp;
    lhp.set(1200.0f);
    Hpf3 hp;
    for (int i = 0; i < n; ++i) {
        const float t = osTime(i);
        const float x = m.tick();
        const float el = rampUp(t, 0.0006f) * expf(-t / tau20(1.0f));
        const float eh = rampUp(t, 0.0002f) * (0.55f * expf(-t / 0.05f) + 0.45f * expf(-t / tau20(0.75f)));
        lo[(size_t)i] = lhp.hp(b1.bp(x) * el);
        hi[(size_t)i] = hp.tick(b2.bp(x) * eh);
    }
    return mixNorm(lo, -3.0f, hi, 0.0f, 0.1f);
}

// Hand clap: noise -> BPF 1 kHz; sawtooth envelope with 3 teeth in ~30 ms + 'reverb' VCA.
Buf clap()
{
    const int n = osFrames(0.7f);
    Buf b((size_t)n);
    Rng r(0x808C1u);
    Svf bp;
    bp.set(1000.0f, 2.0f);
    OnePole hp, rlp;
    hp.set(280.0f);
    rlp.set(2600.0f);
    const float teeth[3] = {0.0f, 0.0105f, 0.0210f};
    for (int i = 0; i < n; ++i) {
        const float t = osTime(i);
        float eb = 0.0f;
        for (int k = 2; k >= 0; --k) {
            if (t >= teeth[k]) {
                const float d = t - teeth[k];
                eb = rampUp(d, 0.0002f) * expf(-d / (k == 2 ? 0.0065f : 0.0038f));
                break;
            }
        }
        const float er = rampUp(t, 0.006f) * expf(-t / tau20(0.10f));
        const float x = bp.bp(r.uni());
        b[(size_t)i] = hp.hp(x * eb + 0.62f * rlp.lp(x) * er);
    }
    return b;
}

// Rim shot: 1667 + 455 Hz resonators, ~10 ms, clipped by the VCA.
Buf rim()
{
    const int n = osFrames(0.12f);
    Buf b((size_t)n);
    OnePole hp;
    hp.set(300.0f);
    for (int i = 0; i < n; ++i) {
        const float t = osTime(i);
        const float env = rampUp(t, 0.00005f) * expf(-t / tau20(0.010f));
        const float x = 0.6f * sinf(kTwoPi * 1667.0f * t) + sinf(kTwoPi * 455.0f * t + 0.3f);
        b[(size_t)i] = hp.hp(sat(x * env, 1.6f));
    }
    return b;
}

// Toms (bridged-T with a short pitch push at the trigger) + a little noise. The low tom also
// gets quiet pink-ish noise decaying slightly slower than the body (the original's fake
// 'reverb' on LT).
Buf tom808(float f0, float t20, uint32_t seed, float pink)
{
    const float tau = tau20(t20);
    const int n = osFrames(t20 * 4.5f + 0.1f);
    Buf b((size_t)n);
    Phasor ph;
    Rng r(seed);
    OnePole nlp, nhp, plp, php;
    nlp.set(2500.0f);
    nhp.set(300.0f);
    plp.set(1800.0f);
    php.set(120.0f);
    float k0 = 0.0f, k1 = 0.0f, k2 = 0.0f;  // Kellet 3-pole pink filter state
    for (int i = 0; i < n; ++i) {
        const float t = osTime(i);
        const float f = f0 * (1.0f + 0.25f * expf(-t / 0.006f) + 0.05f * expf(-t / 0.05f));
        const float body = expf(-t / tau) * sinOf(ph.step(f));
        const float w = r.uni();
        const float nz = nhp.hp(nlp.lp(w)) * expf(-t / (0.35f * tau)) * rampUp(t, 0.0005f);
        k0 = 0.99765f * k0 + 0.0990f * w;
        k1 = 0.96300f * k1 + 0.2965f * w;
        k2 = 0.57000f * k2 + 1.0526f * w;
        const float pk = php.hp(plp.lp(k0 + k1 + k2 + 0.1848f * w)) * expf(-t / (1.25f * tau)) * rampUp(t, 0.002f);
        b[(size_t)i] = body + 0.12f * nz + pink * pk;
    }
    return b;
}
Buf loTom() { return tom808(90.0f, 0.200f, 0x80861u, 0.05f); }
Buf midTom() { return tom808(135.0f, 0.130f, 0x80862u, 0.0f); }
Buf hiTom() { return tom808(185.0f, 0.100f, 0x80863u, 0.0f); }

// Congas: same resonator, no noise, cleaner onset.
Buf conga808(float f0, float t20)
{
    const float tau = tau20(t20);
    const int n = osFrames(t20 * 4.5f + 0.05f);
    Buf b((size_t)n);
    Phasor ph;
    for (int i = 0; i < n; ++i) {
        const float t = osTime(i);
        const float f = f0 * (1.0f + 0.12f * expf(-t / 0.004f));
        b[(size_t)i] = sat(expf(-t / tau) * sinOf(ph.step(f)), 1.3f);
    }
    return b;
}
Buf loConga() { return conga808(220.0f, 0.180f); }
Buf hiConga() { return conga808(370.0f, 0.080f); }

// Cowbell: 540 + 800 Hz squares, two-stage VCA (~50 ms), BPF 2.64 kHz.
Buf cowbell()
{
    const int n = osFrames(0.5f);
    Buf b((size_t)n);
    Pulse a, c;
    a.set(540.0f, 0.5f, 0.0f);
    c.set(800.0f, 0.5f, 0.0f);
    Svf bp;
    bp.set(2640.0f, 1.0f);
    OnePole lp;
    lp.set(3000.0f);
    for (int i = 0; i < n; ++i) {
        const float t = osTime(i);
        const float env = rampUp(t, 0.0003f) * (0.55f * expf(-t / 0.006f) + 0.45f * expf(-t / tau20(0.055f)));
        const float x = 0.5f * (a.tick() + c.tick()) * env;
        b[(size_t)i] = bp.bp(x) + 0.18f * lp.lp(x);
    }
    return b;
}

// Claves: 2.5 kHz resonator, 25 ms.
Buf claves()
{
    const int n = osFrames(0.15f);
    Buf b((size_t)n);
    Phasor ph;
    for (int i = 0; i < n; ++i) {
        const float t = osTime(i);
        const float f = 2500.0f * (1.0f + 0.03f * expf(-t / 0.002f));
        b[(size_t)i] = rampUp(t, 0.00005f) * expf(-t / tau20(0.025f)) * sinOf(ph.step(f));
    }
    return b;
}

// Maracas: high-passed noise with a slow-ish attack, 25-35 ms.
Buf maracas()
{
    const int n = osFrames(0.15f);
    Buf b((size_t)n);
    Rng r(0x808AAu);
    Svf hp;
    hp.set(5000.0f, 0.75f);
    for (int i = 0; i < n; ++i) {
        const float t = osTime(i);
        const float env = (1.0f - expf(-t / 0.0015f)) * expf(-t / tau20(0.028f));
        b[(size_t)i] = hp.hp(r.uni()) * env;
    }
    return b;
}

const SoundDef k808[kSoundsPerKit] = {
    {"Kick", "Bass Drum", "Kick", kick, 0.0f, true, 0, 0, 0.0f, 0.0f, 0.0f,
     "56 Hz bridged-T, 2x pitch for ~4.5 ms, sigh 58.5->54 Hz, -20 dB @ 360 ms"},
    {"Snare", "Snare Drum", "Snare", snare, -3.0f, false, 0, 1, 0.0f, 0.0f, 0.0f,
     "238 + 476 Hz (60/40 ms) + noise HPF 1.8 kHz (75 ms)"},
    {"Cl Hat", "Closed Hi-Hat", "Closed_Hat", clHat, -11.0f, false, 1, 2, 0.0f, 0.0f, 0.0f,
     "6 squares -> BPF 7.1k -> VCA 50 ms -> HPF3 10.5k"},
    {"Op Hat", "Open Hi-Hat", "Open_Hat", opHat, -12.0f, false, 1, 2, 0.0f, 0.0f, 0.0f,
     "6 squares -> BPF 7.1k -> VCA 450 ms -> HPF3 10.5k"},
    {"Clap", "Hand Clap", "Clap", clap, -4.0f, false, 0, 1, 0.0f, 0.0f, 0.0f,
     "noise BPF 1 kHz, 3 saw bursts in 21 ms + reverb VCA 100 ms"},
    {"Rim", "Rim Shot", "Rim", rim, -8.0f, false, 0, 1, 0.0f, 0.0f, 0.0f, "1667 + 455 Hz, 10 ms"},
    {"Lo Tom", "Low Tom", "Low_Tom", loTom, -6.0f, false, 0, 4, 0.0f, 0.0f, 0.0f, "90 Hz, 200 ms + noise + pink tail 1.25x"},
    {"Mid Tom", "Mid Tom", "Mid_Tom", midTom, -6.0f, false, 0, 4, 0.0f, 0.0f, 0.0f, "135 Hz, 130 ms + noise"},
    {"Hi Tom", "High Tom", "High_Tom", hiTom, -6.0f, false, 0, 4, 0.0f, 0.0f, 0.0f, "185 Hz, 100 ms + noise"},
    {"Cymbal", "Cymbal", "Cymbal", cymbal, -13.0f, false, 0, 5, 0.0f, 0.0f, 2.6f,
     "6 squares -> BPF 3.44k (1000 ms) + BPF 7.1k/HPF3 (50 + 750 ms)"},
    {"Cowbell", "Cowbell", "Cowbell", cowbell, -8.0f, false, 0, 3, 0.0f, 0.0f, 0.0f,
     "540 + 800 Hz squares, VCA 6 + 55 ms, BPF 2.64 kHz"},
    {"Claves", "Claves", "Claves", claves, -11.0f, false, 0, 3, 0.0f, 0.0f, 0.0f, "2.5 kHz, 25 ms"},
    {"Maracas", "Maracas", "Maracas", maracas, -14.0f, false, 0, 3, 0.0f, 0.0f, 0.0f, "noise HPF 5 kHz, 28 ms"},
    {"Lo Conga", "Low Conga", "Low_Conga", loConga, -8.0f, false, 0, 3, 0.0f, 0.0f, 0.0f, "220 Hz, 180 ms"},
    {"Hi Conga", "High Conga", "High_Conga", hiConga, -8.0f, false, 0, 3, 0.0f, 0.0f, 0.0f, "370 Hz, 80 ms"},
    {"Kick Lng", "Kick Long", "Kick_Long", kickLong, 0.0f, true, 0, 0, 0.0f, 0.0f, 0.0f,
     "56 Hz boom, -20 dB @ ~800 ms, tail ~2.5 s"},
};

}  // namespace

const SoundDef* kit808() { return k808; }

}  // namespace kitgen
