#pragma once
// kitgen DSP primitives. Everything runs in float32 at kFs = 4 x 48 kHz (oversampled), the
// result is decimated to 48 kHz by a linear-phase windowed-sinc FIR (see decimate()).
#include <math.h>
#include <stdint.h>
#include <vector>

namespace kitgen {

constexpr int kOs = 4;
constexpr float kOutRate = 48000.0f;
constexpr float kFs = kOutRate * (float)kOs;  // 192 kHz synthesis rate
constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;
constexpr float kLn10 = 2.30258509299405f;

using Buf = std::vector<float>;

inline int osFrames(float seconds) { return (int)(seconds * kFs); }
inline float osTime(int i) { return (float)i * (1.0f / kFs); }
// Decay time to -20 dB -> exponential time constant (tau = T / ln 10).
inline float tau20(float t20) { return t20 / kLn10; }
inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float dbToLin(float db) { return powf(10.0f, db * 0.05f); }
inline float linToDb(float x) { return x > 1e-12f ? 20.0f * log10f(x) : -240.0f; }

// xorshift32: deterministic white noise, fixed seed per sound.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float uni() { return (float)(next() >> 8) * (1.0f / 8388608.0f) - 1.0f; }  // [-1, 1)
    float u01() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }        // [0, 1)
};

// One-pole TPT filter (LP; HP = x - LP).
struct OnePole {
    float g = 0.0f, s = 0.0f;
    void set(float fc)
    {
        float w = tanf(kPi * clampf(fc, 1.0f, kFs * 0.45f) / kFs);
        g = w / (1.0f + w);
    }
    float lp(float x)
    {
        float v = (x - s) * g;
        float y = v + s;
        s = y + v;
        return y;
    }
    float hp(float x) { return x - lp(x); }
};

// State-variable filter (Simper/Cytomic TPT). bp() is normalised to unity gain at fc.
struct Svf {
    float k = 1.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, ic1 = 0.0f, ic2 = 0.0f;
    float lpo = 0.0f, bpo = 0.0f, hpo = 0.0f;
    void set(float fc, float q)
    {
        float g = tanf(kPi * clampf(fc, 1.0f, kFs * 0.45f) / kFs);
        k = 1.0f / q;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void tick(float x)
    {
        float v3 = x - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        lpo = v2;
        bpo = v1;
        hpo = x - k * v1 - v2;
    }
    float lp(float x) { tick(x); return lpo; }
    float bp(float x) { tick(x); return k * bpo; }
    float hp(float x) { tick(x); return hpo; }
};

// Band-limited pulse (PolyBLEP) on top of the 4x oversampling.
struct Pulse {
    float ph = 0.0f, dt = 0.0f, duty = 0.5f;
    void set(float hz, float d, float phase0)
    {
        dt = hz / kFs;
        duty = d;
        ph = phase0 - floorf(phase0);
    }
    static float blep(float t, float dt)
    {
        if (t < dt) {
            t /= dt;
            return t + t - t * t - 1.0f;
        }
        if (t > 1.0f - dt) {
            t = (t - 1.0f) / dt;
            return t * t + t + t + 1.0f;
        }
        return 0.0f;
    }
    float tick()
    {
        float y = ph < duty ? 1.0f : -1.0f;
        y += blep(ph, dt);
        float t2 = ph - duty;
        if (t2 < 0.0f) t2 += 1.0f;
        y -= blep(t2, dt);
        ph += dt;
        if (ph >= 1.0f) ph -= 1.0f;
        return y;
    }
};

// Phase accumulator for swept sines / triangles (frequency may change every sample).
struct Phasor {
    float ph = 0.0f;
    float step(float hz)
    {
        float p = ph;
        ph += hz / kFs;
        ph -= floorf(ph);
        return p;
    }
};
inline float sinOf(float p) { return sinf(kTwoPi * p); }
inline float triOf(float p)  // 0 at p = 0, rising like a sine
{
    float q = p + 0.25f;
    q -= floorf(q);
    return 4.0f * fabsf(q - 0.5f) - 1.0f;
}

// Soft saturation normalised to unity small-signal gain region (tanh(d x) / tanh(d)).
inline float sat(float x, float drive) { return tanhf(drive * x) / tanhf(drive); }

// Smooth onset ramp: 0 -> 1 in `rise` seconds (raised cosine).
inline float rampUp(float t, float rise)
{
    if (t >= rise) return 1.0f;
    return 0.5f - 0.5f * cosf(kPi * t / rise);
}

// Schroeder reverb (4 combs + 2 allpasses) at kFs, used for the "room" variants.
struct Room {
    std::vector<float> c[4], a[2];
    int ci[4] = {0, 0, 0, 0}, ai[2] = {0, 0};
    float cg[4] = {0, 0, 0, 0};
    OnePole damp[4];
    void init(float rt60)
    {
        const float cms[4] = {29.7f, 37.1f, 41.1f, 43.7f};
        const float ams[2] = {5.0f, 1.7f};
        for (int i = 0; i < 4; ++i) {
            c[i].assign((size_t)(cms[i] * 0.001f * kFs), 0.0f);
            cg[i] = powf(10.0f, -3.0f * cms[i] * 0.001f / rt60);
            damp[i].set(5500.0f);
        }
        for (int i = 0; i < 2; ++i) a[i].assign((size_t)(ams[i] * 0.001f * kFs), 0.0f);
    }
    float tick(float x)
    {
        float s = 0.0f;
        for (int i = 0; i < 4; ++i) {
            float y = c[i][(size_t)ci[i]];
            c[i][(size_t)ci[i]] = x + cg[i] * damp[i].lp(y);
            if (++ci[i] >= (int)c[i].size()) ci[i] = 0;
            s += y;
        }
        s *= 0.25f;
        for (int i = 0; i < 2; ++i) {
            float d = a[i][(size_t)ai[i]];
            float v = s + 0.7f * d;
            a[i][(size_t)ai[i]] = v;
            if (++ai[i] >= (int)a[i].size()) ai[i] = 0;
            s = d - 0.7f * v;
        }
        return s;
    }
};

// 909-style sample degradation: sample-and-hold at `rate` Hz with `bits` quantisation of a
// signal already normalised to +-1 (mid-tread, symmetric, so no DC is added).
inline void degrade(Buf& b, float rate, int bits)
{
    const float levels = (float)((1 << (bits - 1)) - 1);  // 6 bit -> +-31
    float acc = 1.0f, hold = 0.0f;
    const float inc = rate / kFs;
    for (float& x : b) {
        acc += inc;
        if (acc >= 1.0f) {
            acc -= 1.0f;
            hold = roundf(clampf(x, -1.0f, 1.0f) * levels) / levels;
        }
        x = hold;
    }
}

inline float peakOf(const Buf& b)
{
    float p = 0.0f;
    for (float x : b) p = fmaxf(p, fabsf(x));
    return p;
}
inline void scale(Buf& b, float g)
{
    for (float& x : b) x *= g;
}

// 4:1 decimation, linear phase (delay compensated), Blackman-Harris windowed sinc.
Buf decimate(const Buf& in);

}  // namespace kitgen
