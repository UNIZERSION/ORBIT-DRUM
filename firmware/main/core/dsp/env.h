#pragma once
// Exponential envelopes using the "overshoot target" method: x += c * (target - x) with a target
// beyond the segment end, so every segment ends after exactly its time T (not asymptotically):
//  attack : target 1.3, stops at 1.0;     c = 1 - exp(ln(1 - 1/1.3) / N)
//  decay  : target S - 0.01 (1 - S), stops at S;  c = 1 - exp(ln(0.01/1.01) / N)
//  release: target -0.01 * level, stops (voice free) below 1e-4;  same c as decay
// N = T * fs. Times are "full segment" times, which is what the tests measure.
#include <math.h>
#include <stdint.h>
#include "fastmath.h"

namespace orbit {
namespace dsp {

inline float attackCoef(float ms, float fs)
{
    const float n = ms * 0.001f * fs;
    return n < 1.0f ? 1.0f : 1.0f - expf(kLnAttack / n);
}
inline float segmentCoef(float ms, float fs)
{
    const float n = ms * 0.001f * fs;
    return n < 1.0f ? 1.0f : 1.0f - expf(kLnSegEnd / n);
}
// Per-sample log of the decay factor (control-rate envelopes raise it to the power m).
inline float segmentLnPerSample(float ms, float fs)
{
    const float n = ms * 0.001f * fs;
    return n < 1.0f ? -100.0f : kLnSegEnd / n;
}

constexpr float kEnvFloor = 1.0e-4f;

enum EnvStage : uint8_t { ES_ATTACK = 0, ES_HOLD, ES_DECAY, ES_SUSTAIN, ES_RELEASE, ES_DONE };

struct AmpEnvCoefs {
    float att_c = 1.0f;   // 1 = instant
    float dec_c = 1.0f;
    float rel_c = 1.0f;
    float sus = 1.0f;
    int32_t hold = 0;     // samples
};

// AHDSR. tick() is per sample; hold and coefficient refresh happen at control rate (update()).
struct AmpEnv {
    float x = 0.0f;
    float c = 0.0f;
    float tgt = 0.0f;
    float lo = -1.0f, hi = 2.0f;   // leaving [lo, hi] ends the current segment
    int32_t hold_left = 0;
    uint8_t stage = ES_DONE;

    void start(const AmpEnvCoefs& k)
    {
        hold_left = k.hold;
        if (k.att_c >= 1.0f) {
            x = 1.0f;
            enterHoldOrDecay(k);
        } else {
            x = 0.0f;
            stage = ES_ATTACK;
            c = k.att_c;
            tgt = 1.3f;
            lo = -1.0f;
            hi = 1.0f;
        }
    }
    void release(const AmpEnvCoefs& k)
    {
        if (stage >= ES_RELEASE) return;
        stage = ES_RELEASE;
        c = k.rel_c;
        tgt = -0.01f * x;
        lo = kEnvFloor;
        hi = 2.0f;
        if (x < kEnvFloor) finish();
    }
    // Control rate: refresh coefficients (knobs move live) and run the hold counter.
    void update(const AmpEnvCoefs& k, int m)
    {
        switch (stage) {
        case ES_ATTACK: c = k.att_c; break;
        case ES_HOLD:
            hold_left -= m;
            if (hold_left <= 0) enterDecay(k);
            break;
        case ES_DECAY:
            c = k.dec_c;
            decayTarget(k);
            break;
        case ES_SUSTAIN:
            if (x != k.sus) enterDecay(k);   // sustain knob moved: glide there
            if (x < kEnvFloor && k.sus < kEnvFloor) finish();
            break;
        case ES_RELEASE: c = k.rel_c; break;
        default: break;
        }
    }
    inline float tick(const AmpEnvCoefs& k)
    {
        x += c * (tgt - x);
        if (x > hi || x < lo) segmentEnd(k);
        return x;
    }
    bool done() const { return stage == ES_DONE; }

    // The helpers below are public so a voice loop can keep x/c/tgt in registers and call
    // segmentEnd() only when the value leaves [lo, hi].
    void finish()
    {
        stage = ES_DONE;
        x = 0.0f;
        c = 0.0f;
        tgt = 0.0f;
        lo = -1.0f;
        hi = 2.0f;
    }
    void enterHoldOrDecay(const AmpEnvCoefs& k)
    {
        if (hold_left > 0) {
            stage = ES_HOLD;
            c = 0.0f;
            tgt = x;
            lo = -1.0f;
            hi = 2.0f;
        } else {
            enterDecay(k);
        }
    }
    void enterDecay(const AmpEnvCoefs& k)
    {
        if (k.sus >= 1.0f && x >= 1.0f) {
            stage = ES_SUSTAIN;
            c = 0.0f;
            x = 1.0f;
            tgt = 1.0f;
            lo = -1.0f;
            hi = 2.0f;
            return;
        }
        stage = ES_DECAY;
        c = k.dec_c;
        decayTarget(k);
    }
    void decayTarget(const AmpEnvCoefs& k)
    {
        if (x > k.sus) {
            tgt = k.sus - 0.01f * (1.0f - k.sus);
            lo = k.sus;
            hi = 2.0f;
        } else {   // sustain raised above the current level: rise to it
            tgt = k.sus + 0.01f;
            lo = -1.0f;
            hi = k.sus;
        }
    }
    void segmentEnd(const AmpEnvCoefs& k)
    {
        switch (stage) {
        case ES_ATTACK:
            x = 1.0f;
            enterHoldOrDecay(k);
            break;
        case ES_DECAY:
            x = k.sus;
            stage = ES_SUSTAIN;
            c = 0.0f;
            tgt = x;
            lo = -1.0f;
            hi = 2.0f;
            if (x < kEnvFloor) finish();
            break;
        case ES_RELEASE: finish(); break;
        default:
            lo = -1.0f;
            hi = 2.0f;
            break;
        }
    }
};

// Control-rate 1 -> 0 decay (filter and pitch envelopes): exponential, reaches 0 after T.
struct DecayEnv {
    float x = 0.0f;
    void start() { x = 1.0f; }
    // a = exp(segmentLnPerSample() * m): advances m samples.
    inline void advance(float a)
    {
        if (x <= 0.0f) return;
        x = -0.01f + (x + 0.01f) * a;
        if (x < 0.0f) x = 0.0f;
    }
};

}  // namespace dsp
}  // namespace orbit
