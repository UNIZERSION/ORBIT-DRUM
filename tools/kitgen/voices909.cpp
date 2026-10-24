// "909 STYLE" kit: analogue-style kick/snare/toms/rim/clap and 6-bit, ~30 kHz sample-and-hold
// metallic voices (the classic machine stored its cymbals as 6-bit ROM samples; we only
// emulate that character on our own synthetic metal noise). Own synthesis, no samples.
#include "voices.h"

namespace kitgen {

namespace {

// Kick: sine with an exponential pitch sweep (ratio x fb -> fb, tau_p), soft-shaped like the
// waveshaped triangle of the original, plus an attack click (1 ms pulse + low-passed noise).
// Amplitude: exponential decay with a gaussian roll-off (decay knob style).
struct Kick909 {
    float fb, ratio, tau_p, tau_a, gauss_s, click, drive, seconds;
    uint32_t seed;
};

Buf kick909(const Kick909& k)
{
    const int n = osFrames(k.seconds);
    Buf b((size_t)n);
    Phasor ph;
    Rng r(k.seed);
    Svf nlp;
    nlp.set(5000.0f, 0.7f);
    OnePole chp, plp;
    chp.set(150.0f);
    plp.set(6000.0f);
    for (int i = 0; i < n; ++i) {
        const float t = osTime(i);
        const float f = k.fb * (1.0f + (k.ratio - 1.0f) * expf(-t / k.tau_p));
        const float g = (t / k.gauss_s) * (t / k.gauss_s);
        const float amp = rampUp(t, 0.0004f) * expf(-t / k.tau_a - g);
        const float body = amp * sat(sinOf(ph.step(f)), k.drive);
        const float pulse = t < 0.001f ? 1.0f : 0.0f;
        const float nz = nlp.lp(r.uni()) * expf(-t / 0.0015f);
        const float cl = chp.hp(0.55f * plp.lp(pulse) + 1.6f * nz);
        b[(size_t)i] = body + k.click * cl;
    }
    return b;
}

Buf kick() { return kick909({50.0f, 3.5f, 0.016f, 0.17f, 0.60f, 0.30f, 1.4f, 1.3f, 0x90901u}); }
Buf kickLong() { return kick909({46.0f, 3.2f, 0.020f, 0.36f, 1.45f, 0.18f, 1.2f, 2.8f, 0x90902u}); }
Buf kickHard()
{
    Buf b = kick909({54.0f, 4.0f, 0.012f, 0.15f, 0.45f, 0.65f, 1.4f, 1.0f, 0x90903u});
    for (float& x : b) x = sat(x * 1.2f, 2.6f);  // extra saturation stage
    return b;
}

// Snare: two triangle VCOs (~180 / 330 Hz) with a ~20 ms pitch bend + filtered noise 'snappy'.
struct Snare909 {
    float f1, f2, bend, t1, t2, nhp, nlp, tn, noise_db;
    uint32_t seed;
};

Buf snare909(const Snare909& s)
{
    const int n = osFrames(0.7f);
    Buf tone((size_t)n), nz((size_t)n);
    Phasor p1, p2;
    Rng r(s.seed);
    Svf hp;
    hp.set(s.nhp, 0.7f);
    OnePole lp;
    lp.set(s.nlp);
    for (int i = 0; i < n; ++i) {
        const float t = osTime(i);
        const float b = 1.0f + s.bend * expf(-t / 0.007f);
        tone[(size_t)i] = sat(expf(-t / s.t1) * triOf(p1.step(s.f1 * b)) +
                                  0.65f * expf(-t / s.t2) * triOf(p2.step(s.f2 * b)),
                              1.2f);
        nz[(size_t)i] = lp.lp(hp.hp(r.uni())) * rampUp(t, 0.0004f) * expf(-t / s.tn);
    }
    return mixNorm(tone, 0.0f, nz, s.noise_db, 0.03f);
}

Buf snare() { return snare909({180.0f, 330.0f, 0.55f, 0.055f, 0.032f, 700.0f, 8000.0f, 0.070f, -1.0f, 0x90921u}); }
Buf snareSnap()
{
    return snare909({200.0f, 360.0f, 0.50f, 0.040f, 0.024f, 1500.0f, 11000.0f, 0.105f, 2.5f, 0x90922u});
}

// Toms: shaped sine with a pronounced downward pitch sweep + a little band-passed noise.
Buf tom909(float f0, float tau, float gauss_s, uint32_t seed)
{
    const int n = osFrames(gauss_s * 2.2f);
    Buf b((size_t)n);
    Phasor ph;
    Rng r(seed);
    Svf bp;
    bp.set(1500.0f, 0.8f);
    for (int i = 0; i < n; ++i) {
        const float t = osTime(i);
        const float f = f0 * (1.0f + 0.55f * expf(-t / 0.045f));
        const float g = (t / gauss_s) * (t / gauss_s);
        const float body = rampUp(t, 0.0003f) * expf(-t / tau - g) * sat(sinOf(ph.step(f)), 1.3f);
        const float nz = bp.bp(r.uni()) * expf(-t / 0.03f);
        b[(size_t)i] = body + 0.10f * nz;
    }
    return b;
}
Buf loTom() { return tom909(95.0f, 0.20f, 0.65f, 0x90961u); }
Buf midTom() { return tom909(135.0f, 0.18f, 0.55f, 0x90962u); }
Buf hiTom() { return tom909(185.0f, 0.16f, 0.50f, 0x90963u); }

// Rim: three damped resonators (pulse excited) + a click, overdriven, high-passed.
Buf rim()
{
    const int n = osFrames(0.12f);
    Buf b((size_t)n);
    Rng r(0x909A1u);
    OnePole hp, chp;
    hp.set(250.0f);
    chp.set(2000.0f);
    for (int i = 0; i < n; ++i) {
        const float t = osTime(i);
        const float x = 0.6f * expf(-t / 0.007f) * sinf(kTwoPi * 480.0f * t) +
                        1.0f * expf(-t / 0.0045f) * sinf(kTwoPi * 1230.0f * t) +
                        0.5f * expf(-t / 0.003f) * sinf(kTwoPi * 1850.0f * t);
        const float c = 0.4f * chp.hp(r.uni()) * expf(-t / 0.0007f);
        b[(size_t)i] = hp.hp(sat(1.6f * (x + c), 2.2f));
    }
    return b;
}

// Clap: noise -> BPF ~1.15 kHz, 4 sawtooth bursts (~8.5 ms apart) + 'reverb' VCA.
Buf clapDry(float tail_t20, float seconds)
{
    const int n = osFrames(seconds);
    Buf b((size_t)n);
    Rng r(0x909C1u);
    Svf bp, bp2;
    bp.set(1150.0f, 1.6f);
    bp2.set(1300.0f, 1.0f);
    OnePole hp;
    hp.set(450.0f);
    const float teeth[4] = {0.0f, 0.0085f, 0.0170f, 0.0255f};
    for (int i = 0; i < n; ++i) {
        const float t = osTime(i);
        float eb = 0.0f;
        for (int k = 3; k >= 0; --k) {
            if (t >= teeth[k]) {
                const float d = t - teeth[k];
                eb = rampUp(d, 0.00015f) * expf(-d / (k == 3 ? 0.006f : 0.003f));
                break;
            }
        }
        const float er = rampUp(t, 0.006f) * expf(-t / tau20(tail_t20));
        const float x = r.uni();
        b[(size_t)i] = hp.hp(bp.bp(x) * eb + 0.5f * bp2.bp(x) * er);
    }
    return b;
}
Buf clap() { return clapDry(0.16f, 0.8f); }
Buf clapRoom()
{
    Buf dry = clapDry(0.12f, 1.8f);
    Room rv;
    rv.init(0.95f);
    OnePole wl, wh;
    wl.set(6000.0f);
    wh.set(250.0f);
    Buf wet(dry.size());
    for (size_t i = 0; i < dry.size(); ++i) wet[i] = wh.hp(wl.lp(rv.tick(dry[i])));
    return mixNorm(dry, 0.0f, wet, -4.0f, 0.15f);
}

// Metallic source for the 909-style cymbals: three ring-modulated square pairs (XOR) at our own
// inharmonic frequencies + white noise.
struct Metal909 {
    Pulse o[6];
    Rng r;
    float noise;
    Metal909(uint32_t seed, float fscale, float noise_amt) : r(seed), noise(noise_amt)
    {
        static const float f[6] = {263.0f, 371.0f, 419.0f, 557.0f, 619.0f, 877.0f};
        for (int i = 0; i < 6; ++i) o[i].set(f[i] * fscale, 0.5f, r.u01());
    }
    float tick()
    {
        float s[6];
        for (int i = 0; i < 6; ++i) s[i] = o[i].tick();
        return (s[0] * s[1] + s[2] * s[3] + s[4] * s[5]) * (1.0f / 3.0f) + noise * r.uni();
    }
};

// 'Recording' -> 6-bit / 30 kHz S&H 'ROM' -> reconstruction LPF -> analogue VCA decay.
struct Cym909 {
    float fscale, noise, hp_hz, peak_hz, peak_q, peak_mix, pre_tau, vca_tau, vca_gauss, seconds;
    uint32_t seed;
    int bits;
};

Buf cym909(const Cym909& c)
{
    const int n = osFrames(c.seconds);
    Buf b((size_t)n);
    Metal909 m(c.seed, c.fscale, c.noise);
    Svf hp, pk, aa1, aa2;
    hp.set(c.hp_hz, 0.8f);
    pk.set(c.peak_hz, c.peak_q);
    aa1.set(13500.0f, 0.54f);  // anti-alias of the 'recording' before the 30 kHz S&H
    aa2.set(13500.0f, 1.31f);
    for (int i = 0; i < n; ++i) {
        const float t = osTime(i);
        const float x = hp.hp(m.tick());
        const float y = x + c.peak_mix * pk.bp(x);
        b[(size_t)i] = aa2.lp(aa1.lp(y)) * rampUp(t, 0.0003f) * expf(-t / c.pre_tau);
    }
    scale(b, 0.98f / peakOf(b));
    degrade(b, 30000.0f, c.bits);
    OnePole rc;
    rc.set(17000.0f);
    for (int i = 0; i < n; ++i) {
        const float t = osTime(i);
        const float g = c.vca_gauss > 0.0f ? (t / c.vca_gauss) * (t / c.vca_gauss) : 0.0f;
        b[(size_t)i] = rc.lp(b[(size_t)i]) * expf(-t / c.vca_tau - g);
    }
    return b;
}

Buf clHat() { return cym909({1.0f, 0.35f, 6500.0f, 10000.0f, 1.2f, 1.2f, 0.07f, 0.035f, 0.0f, 0.40f, 0x909C2u, 6}); }
Buf opHat() { return cym909({1.0f, 0.35f, 6500.0f, 10000.0f, 1.2f, 1.2f, 0.45f, 0.17f, 0.0f, 1.00f, 0x909C2u, 6}); }
Buf opHatLong()
{
    return cym909({1.0f, 0.35f, 6500.0f, 10000.0f, 1.2f, 1.2f, 0.80f, 0.32f, 1.30f, 1.90f, 0x909C3u, 6});
}
Buf crash() { return cym909({0.82f, 0.55f, 3000.0f, 5500.0f, 0.9f, 1.0f, 0.70f, 0.55f, 1.6f, 2.6f, 0x909C4u, 6}); }

// Ride: inharmonic bell partials + metallic wash, then the same 6-bit / 30 kHz 'ROM' path.
Buf ride()
{
    const float sec = 2.4f;
    const int n = osFrames(sec);
    Buf b((size_t)n);
    static const float pf[8] = {2966.0f, 3541.0f, 4377.0f, 5252.0f, 6118.0f, 7234.0f, 8467.0f, 9610.0f};
    static const float pa[8] = {1.0f, 0.8f, 0.9f, 0.6f, 0.55f, 0.45f, 0.35f, 0.3f};
    static const float pt[8] = {0.9f, 0.75f, 0.7f, 0.55f, 0.5f, 0.4f, 0.35f, 0.3f};
    Metal909 m(0x909D1u, 1.12f, 0.5f);
    Rng r(0x909D2u);
    Svf hp, aa1, aa2, sk;
    hp.set(5500.0f, 0.8f);
    sk.set(4000.0f, 0.9f);
    aa1.set(13500.0f, 0.54f);
    aa2.set(13500.0f, 1.31f);
    for (int i = 0; i < n; ++i) {
        const float t = osTime(i);
        float bell = 0.0f;
        for (int k = 0; k < 8; ++k) bell += pa[k] * expf(-t / pt[k]) * sinf(kTwoPi * pf[k] * t + (float)k);
        const float wash = hp.hp(m.tick()) * expf(-t / 0.35f);
        const float stick = sk.bp(r.uni()) * expf(-t / 0.004f);
        const float y = 0.22f * bell + 1.3f * wash + 1.5f * stick;
        b[(size_t)i] = aa2.lp(aa1.lp(y)) * rampUp(t, 0.0002f);
    }
    scale(b, 0.98f / peakOf(b));
    degrade(b, 30000.0f, 6);
    OnePole rc;
    rc.set(17000.0f);
    for (int i = 0; i < n; ++i) {
        const float t = osTime(i);
        b[(size_t)i] = rc.lp(b[(size_t)i]) * expf(-t / 0.75f - (t / 1.7f) * (t / 1.7f));
    }
    return b;
}

const SoundDef k909[kSoundsPerKit] = {
    {"Kick", "Bass Drum", "Kick", kick, 0.0f, true, 0, 0, 0.0f, 0.0f, 0.0f,
     "sine 175->50 Hz (tau 16 ms) + click, amp tau 170 ms"},
    {"Snare", "Snare Drum", "Snare", snare, -2.0f, false, 0, 1, 0.0f, 0.0f, 0.0f,
     "tri 180 + 330 Hz, bend 20 ms + noise 0.7-8 kHz (snappy)"},
    {"Cl Hat", "Closed Hi-Hat", "Closed_Hat", clHat, -11.0f, false, 1, 2, 0.27f, 12.0f, 0.0f,
     "metal noise, 6 bit @ 30 kHz, VCA tau 35 ms, ROM 0.27 s"},
    {"Op Hat", "Open Hi-Hat", "Open_Hat", opHat, -12.0f, false, 1, 2, 0.82f, 40.0f, 0.0f,
     "metal noise, 6 bit @ 30 kHz, VCA tau 170 ms, ROM 0.82 s"},
    {"Clap", "Hand Clap", "Clap", clap, -3.0f, false, 0, 1, 0.0f, 0.0f, 0.0f,
     "noise BPF 1.15 kHz, 4 bursts in 26 ms + tail 160 ms"},
    {"Rim", "Rim Shot", "Rim", rim, -7.0f, false, 0, 1, 0.0f, 0.0f, 0.0f, "480/1230/1850 Hz resonators + click, driven"},
    {"Lo Tom", "Low Tom", "Low_Tom", loTom, -7.0f, false, 0, 4, 0.0f, 0.0f, 0.0f, "95 Hz, sweep x1.55, tau 200 ms"},
    {"Mid Tom", "Mid Tom", "Mid_Tom", midTom, -7.0f, false, 0, 4, 0.0f, 0.0f, 0.0f, "135 Hz, sweep x1.55, tau 180 ms"},
    {"Hi Tom", "High Tom", "High_Tom", hiTom, -7.0f, false, 0, 4, 0.0f, 0.0f, 0.0f, "185 Hz, sweep x1.55, tau 160 ms"},
    {"Crash", "Crash Cymbal", "Crash", crash, -12.0f, false, 0, 5, 0.0f, 0.0f, 2.2f,
     "metal noise 3-13 kHz, 6 bit @ 30 kHz, VCA tau 550 ms"},
    {"Ride", "Ride Cymbal", "Ride", ride, -14.0f, false, 0, 5, 0.0f, 0.0f, 2.0f,
     "8 bell partials 3-9.6 kHz + wash, 6 bit @ 30 kHz, tau 750 ms"},
    {"Kick Lng", "Kick Long", "Kick_Long", kickLong, 0.0f, true, 0, 0, 0.0f, 0.0f, 0.0f,
     "sine 147->46 Hz (tau 20 ms), amp tau 360 ms"},
    {"Snr Snap", "Snare Snap", "Snare_Snap", snareSnap, -2.0f, false, 0, 1, 0.0f, 0.0f, 0.0f,
     "tri 200 + 360 Hz, louder bright noise 1.5-11 kHz (105 ms)"},
    {"Op Hat L", "Open Hat Long", "Open_Hat_Long", opHatLong, -12.0f, false, 1, 2, 0.0f, 0.0f, 0.0f,
     "metal noise, 6 bit @ 30 kHz, VCA tau 320 ms"},
    {"Clap Rm", "Clap Room", "Clap_Room", clapRoom, -4.0f, false, 0, 1, 0.0f, 0.0f, 0.0f,
     "clap + Schroeder room (RT60 0.95 s)"},
    {"Kick Hrd", "Kick Hard", "Kick_Hard", kickHard, 0.0f, true, 0, 0, 0.0f, 0.0f, 0.0f,
     "sine 216->54 Hz (tau 12 ms), big click, saturated"},
};

}  // namespace

const SoundDef* kit909() { return k909; }

}  // namespace kitgen
