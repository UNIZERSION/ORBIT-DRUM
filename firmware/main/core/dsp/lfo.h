#pragma once
// Control-rate LFO: Sine / Triangle / Saw / Square / Sample&Hold, output -1..1.
#include <stdint.h>
#include "fastmath.h"

namespace orbit {
namespace dsp {

enum LfoWave : uint8_t { LW_SINE = 0, LW_TRI, LW_SAW, LW_SQUARE, LW_SH };

struct Lfo {
    float phase = 0.0f;   // 0..1
    float held = 0.0f;    // S&H value
    Rng rng;

    void start(uint32_t seed)
    {
        phase = 0.0f;
        rng.s = seed ? seed : 0x1234567u;
        held = rng.bi();
    }
    // Value at the current phase, then advance by dphase (cycles).
    inline float step(uint8_t wave, float dphase)
    {
        float v;
        switch (wave) {
        case LW_SINE: v = sin2pi(phase); break;
        case LW_TRI: v = phase < 0.5f ? 4.0f * phase - 1.0f : 3.0f - 4.0f * phase; break;
        case LW_SAW: v = 1.0f - 2.0f * phase; break;   // falling saw (classic "ramp down")
        case LW_SQUARE: v = phase < 0.5f ? 1.0f : -1.0f; break;
        default: v = held; break;
        }
        phase += dphase;
        if (phase >= 1.0f) {
            phase -= (float)floorToInt(phase);
            held = rng.bi();
        }
        return v;
    }
};

}  // namespace dsp
}  // namespace orbit
