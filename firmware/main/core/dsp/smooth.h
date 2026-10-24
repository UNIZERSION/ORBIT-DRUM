#pragma once
// Parameter smoothing: linear ramp across a block (gains). Block-rate one-pole followers are
// written inline where they are used (engine.cpp), with the coefficient from the block length.
#include <math.h>
#include "fastmath.h"

namespace orbit {
namespace dsp {

// Linear ramp across one block: begin(target, n), then next() n times; ends exactly on target.
struct Ramp {
    float cur = 0.0f;
    float step = 0.0f;
    float target = 0.0f;
    void reset(float v)
    {
        cur = target = v;
        step = 0.0f;
    }
    inline void begin(float t, int n)
    {
        target = t;
        step = (n > 0) ? (t - cur) / (float)n : 0.0f;
        if (n <= 0) cur = t;
    }
    inline float next()
    {
        cur += step;
        return cur;
    }
    inline void end()
    {
        cur = target;
        step = 0.0f;
    }
    bool moving() const { return step != 0.0f; }
};

}  // namespace dsp
}  // namespace orbit
