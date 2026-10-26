#pragma once
// Plate reverb after Jon Dattorro, "Effect Design Part 1" (J. AES 1997): pre-delay, bandwidth
// lowpass, 4 input diffusers, figure-eight tank of 2 x (modulated allpass, delay, damping,
// allpass, delay) with the 7+7 output taps of the paper. Float lines on caller memory
// (~146 KB tank + pre-delay at 48 kHz). Delay lengths scale with SIZE (rounded once per block),
// decay is derived from the requested RT60 and the current loop length.
// process() and setParams() run every block on the audio task: ORBIT_HOT puts them in IRAM on the
// Tab5, and setParams() calls libm (powf / exp2f / expf, in flash) only when an input changed.
#include <math.h>
#include <stdint.h>
#include "delayline.h"
#include "fastmath.h"

#ifndef ORBIT_HOT
#define ORBIT_HOT
#endif

namespace orbit {
namespace dsp {

class PlateReverb {
public:
    static constexpr float kRefRate = 29761.0f;
    static constexpr float kMaxPredelayS = 0.2f;
    static constexpr float kExcursion = 16.0f;   // modulation depth at kRefRate

    // Floats needed for a given engine rate (tank at maximum size + pre-delay).
    static int32_t memoryFloats(int rate) { return lineFloats(rate) + predelayFloats(rate); }
    // 0.4.9: the 12 lines of the diffusers and the tank (read and written every sample, ~45 streams) and the
    // pre-delay (one stream in, one out) apart: FxBus keeps the lines in internal RAM, the pre-delay in PSRAM.
    static int32_t lineFloats(int rate)
    {
        int32_t total = 0;
        for (int i = 0; i < kLines; ++i) total += lineLen(i, (float)rate);
        return total;
    }
    static int32_t predelayFloats(int rate) { return predelayLen((float)rate); }

    // One block of memoryFloats(rate): the lines, then the pre-delay.
    void init(float* mem, int rate) { init(mem, mem + lineFloats(rate), rate); }

    // lines: lineFloats(rate) floats, pre: predelayFloats(rate) floats.
    void init(float* lines, float* pre, int rate)
    {
        fs_ = (float)rate;
        float* p = lines;
        for (int i = 0; i < kLines; ++i) {
            const int32_t n = lineLen(i, fs_);
            line_[i].init(p, n);
            p += n;
        }
        pre_.init(pre, predelayLen(fs_));
        bw_a_ = onePoleCoef(9000.0f, fs_);
        size_ = -1.0f;
        geo_size_ = last_decay_ = last_damp_ = -1.0f;   // force a full recompute
        exc_ = kExcursion * fs_ / kRefRate;
        setParams(0.55f, 2.2f, 0.45f, 12.0f);
        clear();
    }

    void clear()
    {
        for (int i = 0; i < kLines; ++i) line_[i].clear();
        pre_.clear();
        bw_ = dampL_ = dampR_ = 0.0f;
        tailL_ = tailR_ = 0.0f;
        lfo_ph_ = 0.0f;
    }

    // Once per block. size01 0..1, decay_s = RT60, damp01 0..1 (more = darker), predelay ms.
    ORBIT_HOT void setParams(float size01, float decay_s, float damp01, float predelay_ms)
    {
        size01 = clampf(size01, 0.0f, 1.0f);
        // glide the size so line lengths move by small steps only (snapped at the end, so the
        // geometry below is recomputed only while it moves)
        if (size_ < 0.0f) {
            size_ = size01;
        } else {
            size_ += 0.08f * (size01 - size_);
            if (absf(size01 - size_) < 1.0e-5f) size_ = size01;
        }
        const bool geom = size_ != geo_size_;
        if (geom) {
            geo_size_ = size_;
            const float scale = (0.35f + 0.65f * size_) * (fs_ / kRefRate);
            for (int i = 0; i < kLines; ++i) {
                int32_t n = (int32_t)(kBase[i] * scale + 0.5f);
                const int32_t maxn =
                    line_[i].len - 3 - (isMod(i) ? (int32_t)(2.0f * kExcursion * fs_ / kRefRate) + 2 : 0);
                if (n > maxn) n = maxn;
                if (n < 1) n = 1;
                n_[i] = n;
            }
            for (int i = 0; i < 14; ++i) {
                int32_t t = (int32_t)(kTap[i] * scale + 0.5f);
                const int32_t lim = n_[kTapLine[i]];
                if (t >= lim) t = lim - 1;
                if (t < 1) t = 1;
                tap_[i] = t;
            }
            // decay per pass from RT60: average segment between two decay multiplications
            const float loop = (float)(n_[L_AP1] + n_[L_DL1] + n_[L_AP2] + n_[L_DL2] + n_[R_AP1] + n_[R_DL1] +
                                       n_[R_AP2] + n_[R_DL2]);
            seg_s_ = loop * 0.25f / fs_;
        }
        decay_s = clampf(decay_s, 0.1f, 30.0f);
        if (geom || decay_s != last_decay_) {
            last_decay_ = decay_s;
            decay_ = clampf(powf(10.0f, -3.0f * seg_s_ / decay_s), 0.0f, 0.97f);
            dd2_ = clampf(decay_ + 0.15f, 0.25f, 0.5f);
        }
        damp01 = clampf(damp01, 0.0f, 1.0f);
        if (damp01 != last_damp_) {
            last_damp_ = damp01;
            damp_a_ = onePoleCoef(18000.0f * exp2f(-5.0f * damp01), fs_);
        }
        const float pd = clampf(predelay_ms, 0.0f, kMaxPredelayS * 1000.0f) * 0.001f * fs_ + 1.0f;
        pd_target_ = minf(pd, (float)(pre_.len - 3));
        if (pd_ < 1.0f) pd_ = pd_target_;
    }

    // in: mono send (n samples). Writes the wet stereo signal to outL/outR (overwrites).
    ORBIT_HOT void process(const float* in, float* outL, float* outR, int n)
    {
        const float lfo_inc = 0.9f / fs_;     // modulation ~0.9 Hz (quadrature pair for L/R)
        const float m0 = sin2pi(lfo_ph_), m1 = sin2pi(lfo_ph_ + 0.25f);
        lfo_ph_ += lfo_inc * (float)n;
        lfo_ph_ -= (float)floorToInt(lfo_ph_);
        const float m0e = sin2pi(lfo_ph_), m1e = sin2pi(lfo_ph_ + 0.25f);
        const float inv_n = 1.0f / (float)n;
        float modL = (float)n_[L_AP1] + exc_ * (1.0f + m0);
        float modR = (float)n_[R_AP1] + exc_ * (1.0f + m1);
        const float dmodL = ((float)n_[L_AP1] + exc_ * (1.0f + m0e) - modL) * inv_n;
        const float dmodR = ((float)n_[R_AP1] + exc_ * (1.0f + m1e) - modR) * inv_n;
        float pd = pd_;
        const float dpd = (pd_target_ - pd_) * 0.05f * inv_n * 4.0f;   // gentle pre-delay glide
        const float dec = decay_, dd1 = 0.70f, dd2 = dd2_, da = damp_a_, ba = bw_a_;

        for (int i = 0; i < n; ++i) {
            // input: pre-delay, bandwidth, 4 diffusers (+ tiny DC so tails never go subnormal)
            const float x0 = pre_.readFrac(pd);
            pre_.write(in[i] + 1.0e-20f);
            pd += dpd;
            bw_ += ba * (x0 - bw_);
            float x = bw_;
            x = line_[IN1].allpass(x, n_[IN1], 0.75f);
            x = line_[IN2].allpass(x, n_[IN2], 0.75f);
            x = line_[IN3].allpass(x, n_[IN3], 0.625f);
            x = line_[IN4].allpass(x, n_[IN4], 0.625f);

            const float lt = tailL_, rt = tailR_;
            // left half of the tank
            float a = x + dec * rt;
            a = line_[L_AP1].allpassFrac(a, modL, -dd1);
            float b = line_[L_DL1].read(n_[L_DL1]);
            line_[L_DL1].write(a);
            dampL_ += da * (b - dampL_);
            b = line_[L_AP2].allpass(dampL_ * dec, n_[L_AP2], dd2);
            tailL_ = line_[L_DL2].read(n_[L_DL2]);
            line_[L_DL2].write(b);
            // right half
            float c = x + dec * lt;
            c = line_[R_AP1].allpassFrac(c, modR, -dd1);
            float d = line_[R_DL1].read(n_[R_DL1]);
            line_[R_DL1].write(c);
            dampR_ += da * (d - dampR_);
            d = line_[R_AP2].allpass(dampR_ * dec, n_[R_AP2], dd2);
            tailR_ = line_[R_DL2].read(n_[R_DL2]);
            line_[R_DL2].write(d);
            modL += dmodL;
            modR += dmodR;

            // output taps (Dattorro table 2)
            const float yl = line_[R_DL1].read(tap_[0]) + line_[R_DL1].read(tap_[1]) - line_[R_AP2].read(tap_[2]) +
                             line_[R_DL2].read(tap_[3]) - line_[L_DL1].read(tap_[4]) - line_[L_AP2].read(tap_[5]) -
                             line_[L_DL2].read(tap_[6]);
            const float yr = line_[L_DL1].read(tap_[7]) + line_[L_DL1].read(tap_[8]) - line_[L_AP2].read(tap_[9]) +
                             line_[L_DL2].read(tap_[10]) - line_[R_DL1].read(tap_[11]) - line_[R_AP2].read(tap_[12]) -
                             line_[R_DL2].read(tap_[13]);
            outL[i] = 0.6f * yl;
            outR[i] = 0.6f * yr;
        }
        pd_ = pd;
        bw_ = flushDenormal(bw_);
        dampL_ = flushDenormal(dampL_);
        dampR_ = flushDenormal(dampR_);
        tailL_ = flushDenormal(tailL_);
        tailR_ = flushDenormal(tailR_);
    }

private:
    enum { IN1 = 0, IN2, IN3, IN4, L_AP1, L_DL1, L_AP2, L_DL2, R_AP1, R_DL1, R_AP2, R_DL2, kLines };
    static constexpr float kBase[kLines] = {142.f, 107.f, 379.f, 277.f, 672.f, 4453.f, 1800.f, 3720.f,
                                            908.f, 4217.f, 2656.f, 3163.f};
    // taps: L = +R_DL1 266, +R_DL1 2974, -R_AP2 1913, +R_DL2 1996, -L_DL1 1990, -L_AP2 187, -L_DL2 1066
    //       R = +L_DL1 353, +L_DL1 3627, -L_AP2 1228, +L_DL2 2673, -R_DL1 2111, -R_AP2 335, -R_DL2 121
    static constexpr float kTap[14] = {266.f, 2974.f, 1913.f, 1996.f, 1990.f, 187.f,  1066.f,
                                       353.f, 3627.f, 1228.f, 2673.f, 2111.f, 335.f, 121.f};
    static constexpr uint8_t kTapLine[14] = {R_DL1, R_DL1, R_AP2, R_DL2, L_DL1, L_AP2, L_DL2,
                                             L_DL1, L_DL1, L_AP2, L_DL2, R_DL1, R_AP2, R_DL2};

    static bool isMod(int i) { return i == L_AP1 || i == R_AP1; }
    static int32_t lineLen(int i, float fs)
    {
        const float k = fs / kRefRate;
        float n = kBase[i] * k + 4.0f;
        if (isMod(i)) n += 2.0f * kExcursion * k + 4.0f;
        return (int32_t)n;
    }
    static int32_t predelayLen(float fs) { return (int32_t)(kMaxPredelayS * fs) + 8; }

    DelayLine line_[kLines];
    DelayLine pre_;
    int32_t n_[kLines] = {};
    int32_t tap_[14] = {};
    float fs_ = 48000.0f;
    float size_ = -1.0f;
    float decay_ = 0.5f, dd2_ = 0.5f, damp_a_ = 0.5f, bw_a_ = 0.7f;
    float pd_ = 0.0f, pd_target_ = 1.0f;
    float exc_ = 16.0f;
    float geo_size_ = -1.0f, last_decay_ = -1.0f, last_damp_ = -1.0f;   // setParams() input cache
    float seg_s_ = 0.0f;
    float bw_ = 0.0f, dampL_ = 0.0f, dampR_ = 0.0f, tailL_ = 0.0f, tailR_ = 0.0f;
    float lfo_ph_ = 0.0f;
};

}  // namespace dsp
}  // namespace orbit
