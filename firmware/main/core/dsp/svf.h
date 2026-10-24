#pragma once
// Trapezoidal (TPT) state variable filter after Andrew Simper (Cytomic), "HP-first" form
// (best float32 precision at low cutoffs): hp = (x - (g+k) s1 - s2) / (1 + g (g+k)).
#include "fastmath.h"

namespace orbit {
namespace dsp {

struct SvfCoefs {
    float g = 0.0f;    // tan(pi fc / fs)
    float gk = 0.0f;   // g + k
    float a1 = 1.0f;   // 1 / (1 + g (g + k))
};

// fc_norm = fc / fs (clamped to 0.45), k = 1/Q (2 - 1.98 * reso for the pad filter).
inline SvfCoefs svfCoefs(float fc_norm, float k)
{
    SvfCoefs c;
    fc_norm = clampf(fc_norm, 1.0e-5f, 0.45f);
    c.g = fastTanPi(fc_norm);
    c.gk = c.g + k;
    c.a1 = 1.0f / (1.0f + c.g * c.gk);
    return c;
}

// Resonance 0..1 -> damping k.
inline float svfK(float reso) { return 2.0f - 1.98f * clampf(reso, 0.0f, 1.0f); }

struct Svf {
    float s1 = 0.0f, s2 = 0.0f;

    inline void tick(float x, float g, float gk, float a1, float& lp, float& bp, float& hp)
    {
        hp = (x - gk * s1 - s2) * a1;
        bp = g * hp + s1;
        lp = g * bp + s2;
        s1 = g * hp + bp;
        s2 = g * bp + lp;
    }
    inline float lowpass(float x, const SvfCoefs& c)
    {
        float lp, bp, hp;
        tick(x, c.g, c.gk, c.a1, lp, bp, hp);
        return lp;
    }
    inline float highpass(float x, const SvfCoefs& c)
    {
        float lp, bp, hp;
        tick(x, c.g, c.gk, c.a1, lp, bp, hp);
        return hp;
    }
    void reset() { s1 = s2 = 0.0f; }
    void flush()
    {
        s1 = flushDenormal(s1);
        s2 = flushDenormal(s2);
    }
};

}  // namespace dsp
}  // namespace orbit
