#pragma once
// LO-FI: bit crush y = round(x q) / q, q = 2^(bits-1) (no dither, for character) and sample-rate
// reduction by a phase accumulator (ph += f_target / fs; sample and hold when ph wraps).
#include <math.h>
#include "fastmath.h"

namespace orbit {
namespace dsp {

inline float crushQ(float bits)
{
    bits = clampf(bits, 1.0f, 24.0f);
    return exp2f(bits - 1.0f);
}

inline float crush(float x, float q, float inv_q) { return (float)floorToInt(x * q + 0.5f) * inv_q; }

struct RateReducer {
    float ph = 1.0f;   // starts "due" so the first sample is taken at once
    float hl = 0.0f, hr = 0.0f;
    inline void tick(float inc, float& l, float& r)
    {
        ph += inc;
        if (ph >= 1.0f) {
            ph -= (float)floorToInt(ph);
            hl = l;
            hr = r;
        }
        l = hl;
        r = hr;
    }
    void reset()
    {
        ph = 1.0f;
        hl = hr = 0.0f;
    }
};

}  // namespace dsp
}  // namespace orbit
