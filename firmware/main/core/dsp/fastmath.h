#pragma once
// Fast float32 helpers for the audio path. No double, no libm in per-sample code.
#include <math.h>
#include <stdint.h>
#include <string.h>

namespace orbit {
namespace dsp {

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;
constexpr float kHalfPi = 1.57079632679490f;
constexpr float kInv32768 = 1.0f / 32768.0f;
// ln(0.01 / 1.01): segment coefficient so an "overshoot target" segment ends exactly at T (env.h).
constexpr float kLnSegEnd = -4.61512052f;
// ln(1 - 1/1.3): attack aims at 1.3 and stops at 1.0 after exactly T.
constexpr float kLnAttack = -1.46633707f;

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float minf(float a, float b) { return a < b ? a : b; }
inline float maxf(float a, float b) { return a > b ? a : b; }
inline float absf(float x) { return x < 0.0f ? -x : x; }

// Zero tiny values in feedback paths (once per block on filter / delay states).
inline float flushDenormal(float x) { return (x > -1e-15f && x < 1e-15f) ? 0.0f : x; }

inline int floorToInt(float x)
{
    int i = (int)x;
    return (x < (float)i) ? i - 1 : i;
}

// 2^x. Relative error < 2e-5 (0.03 cent), exact at integers (octaves stay exact).
inline float fastExp2(float x)
{
    x = clampf(x, -126.0f, 126.0f);
    const int fi = floorToInt(x);
    const float f = x - (float)fi;
    const float p =
        1.0f + f * (0.693147181f +
                    f * (0.240226507f +
                         f * (0.0555041087f + f * (0.00961812911f + f * (0.00133335581f + f * 0.000154035304f)))));
    const uint32_t bits = (uint32_t)(fi + 127) << 23;
    float s;
    memcpy(&s, &bits, sizeof(s));
    return p * s;
}

// dB -> linear gain via fastExp2 (log2(10)/20 = 0.1660964).
inline float dbToLin(float db) { return db <= -96.0f ? 0.0f : fastExp2(db * 0.166096404f); }

// log2(x) for normal x > 0 without libm (meters / telemetry on the audio task). Mantissa m in
// [1, 2): log2(m) = 2/ln2 * atanh((m-1)/(m+1)), series to t^7; |error| < 2e-5 (1e-4 dB).
inline float fastLog2(float x)
{
    uint32_t b;
    memcpy(&b, &x, sizeof(b));
    const int e = (int)((b >> 23) & 255u) - 127;
    b = (b & 0x007FFFFFu) | 0x3F800000u;
    float m;
    memcpy(&m, &b, sizeof(m));
    const float t = (m - 1.0f) / (m + 1.0f);
    const float t2 = t * t;
    const float s = t * (1.0f + t2 * (1.0f / 3.0f + t2 * (0.2f + t2 * (1.0f / 7.0f))));
    return (float)e + 2.88539008f * s;
}

// tan(pi * f) for f = fc / fs, stmlib FREQUENCY_FAST polynomial (good for 16 Hz..16 kHz @48 kHz,
// slightly low near Nyquist, which only makes the top of the cutoff range a little darker).
inline float fastTanPi(float f)
{
    const float a = 3.260e-01f * 31.0062767f;   // * pi^3
    const float b = 1.823e-01f * 306.019685f;   // * pi^5
    const float f2 = f * f;
    return f * (kPi + f2 * (a + b * f2));
}

// sin(x) for x in [-pi/2, pi/2] (Taylor to x^7, error < 2e-4). Used for pan gains.
inline float sinHalfPi(float x)
{
    const float x2 = x * x;
    return x * (1.0f - x2 * (1.0f / 6.0f) * (1.0f - x2 * (1.0f / 20.0f) * (1.0f - x2 * (1.0f / 42.0f))));
}

// sin(2*pi*p) for any p (parabola + one refinement, error < 1e-3). LFOs, metronome, reverb mod.
inline float sin2pi(float p)
{
    p -= (float)floorToInt(p);          // [0, 1)
    const float t = p - 0.5f;            // [-0.5, 0.5): sin(2 pi p) = -sin(2 pi t)
    const float at = absf(t);
    float y = 8.0f * t - 16.0f * t * at; // parabola through the sine's zeros and peaks
    y = 0.225f * (y * absf(y) - y) + y;
    return -y;
}

// Constant-power pan for a mono source: pan -1..1 -> L = cos(theta), R = sin(theta), -3 dB centre.
inline void panConstantPower(float pan, float& l, float& r)
{
    pan = clampf(pan, -1.0f, 1.0f);
    const float th = (pan + 1.0f) * (kPi * 0.25f);
    r = sinHalfPi(th);
    l = sinHalfPi(kHalfPi - th);
}

// Balance for stereo sources / buses: unity at centre, constant power towards the sides.
inline void panBalance(float pan, float& l, float& r)
{
    panConstantPower(pan, l, r);
    l = minf(1.0f, l * 1.41421356f);
    r = minf(1.0f, r * 1.41421356f);
}

// Rational soft clip x(27+x^2)/(27+9x^2), |x| clamped to 3 (output saturates at +-1).
inline float softClip(float x)
{
    x = clampf(x, -3.0f, 3.0f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// One-pole lowpass coefficient for a cutoff in Hz (block-rate use: calls expf).
inline float onePoleCoef(float hz, float fs)
{
    const float w = kTwoPi * clampf(hz, 1.0f, 0.49f * fs) / fs;
    return 1.0f - expf(-w);
}

// Small xorshift32 generator (audio thread safe, deterministic).
struct Rng {
    uint32_t s = 0x9E3779B9u;
    inline uint32_t next()
    {
        uint32_t x = s;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s = x;
        return x;
    }
    // Uniform [0, 1)
    inline float uni() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }
    // Uniform [-1, 1)
    inline float bi() { return uni() * 2.0f - 1.0f; }
};

}  // namespace dsp
}  // namespace orbit
