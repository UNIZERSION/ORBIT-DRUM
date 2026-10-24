#pragma once
// 4-point Hermite (Catmull-Rom) interpolation on int16 sample data.
#include <stdint.h>

namespace orbit {
namespace dsp {

// y0..y3 = frames i-1, i, i+1, i+2; x = fraction in [0, 1).
inline float hermite4(float y0, float y1, float y2, float y3, float x)
{
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 1.5f * (y1 - y2) + 0.5f * (y3 - y0);
    return ((c3 * x + c2) * x + c1) * x + y1;
}

// p points at frame i of one channel; s = signed distance in int16 elements to the next frame
// in playback order (+channels forward, -channels reverse). Result in int16 units.
inline float hermiteI16(const int16_t* p, int s, float x)
{
    return hermite4((float)p[-s], (float)p[0], (float)p[s], (float)p[2 * s], x);
}

inline float linearI16(const int16_t* p, int s, float x)
{
    const float a = (float)p[0];
    return a + ((float)p[s] - a) * x;
}

}  // namespace dsp
}  // namespace orbit
