#pragma once
// Float delay line on caller-provided memory (no allocation). Read-before-write convention:
// read(d) with d = 1 returns the most recently written sample; a delay of D samples is
// y = read(D); write(x).
#include <stdint.h>
#include "fastmath.h"

namespace orbit {
namespace dsp {

struct DelayLine {
    float* buf = nullptr;
    int32_t len = 0;
    int32_t w = 0;

    void init(float* mem, int32_t n)
    {
        buf = mem;
        len = n;
        w = 0;
        clear();
    }
    void clear()
    {
        for (int32_t i = 0; i < len; ++i) buf[i] = 0.0f;
        w = 0;
    }
    inline void write(float x)
    {
        buf[w] = x;
        if (++w >= len) w = 0;
    }
    // d in 1..len
    inline float read(int32_t d) const
    {
        int32_t i = w - d;
        if (i < 0) i += len;
        return buf[i];
    }
    // Fractional delay d in [1, len-1], linear interpolation.
    inline float readFrac(float d) const
    {
        const int32_t di = (int32_t)d;
        const float f = d - (float)di;
        const float a = read(di);
        const float b = read(di + 1);
        return a + (b - a) * f;
    }
    // Schroeder allpass built on this line: v = x - g z, y = z + g v (z = v delayed by d).
    inline float allpass(float x, int32_t d, float g)
    {
        const float z = read(d);
        const float v = x - g * z;
        write(v);
        return z + g * v;
    }
    inline float allpassFrac(float x, float d, float g)
    {
        const float z = readFrac(d);
        const float v = x - g * z;
        write(v);
        return z + g * v;
    }
};

}  // namespace dsp
}  // namespace orbit
