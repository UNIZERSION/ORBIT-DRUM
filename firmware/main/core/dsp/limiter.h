#pragma once
// Look-ahead brickwall limiter (stereo linked).
//   t[n]   = min(1, thr / max|x|)            gain needed by this sample
//   h[n]   = min(t[n-B+1 .. n])              moving minimum (monotonic deque)
//   r[n]   = instant down, one-pole release up (r <= h always)
//   g[n]   = mean(r[n-B+1 .. n])             box filter = linear attack ramp
//   out[n] = x[n-(B-1)] * g[n]
// Because the box window lies inside the hold window, |out| <= thr is guaranteed (a final clamp
// only removes float rounding of the running box sum at extreme gain reduction).
// Latency = B - 1 samples, constant also when disabled (gain glides back to 1).
#include <math.h>
#include <stdint.h>
#include "fastmath.h"

namespace orbit {
namespace dsp {

class LookaheadLimiter {
public:
    static constexpr int kMax = 256;   // ring size (power of two), max look-ahead B < kMax

    void init(float fs, float lookahead_ms = 1.5f, float release_ms = 60.0f, float thr_db = -0.3f)
    {
        int b = (int)(lookahead_ms * 0.001f * fs + 0.5f);
        if (b < 1) b = 1;
        if (b > kMax - 1) b = kMax - 1;
        B_ = b;
        thr_ = powf(10.0f, thr_db * 0.05f);
        rel_a_ = 1.0f - expf(-2.19722458f / (release_ms * 0.001f * fs));   // ln 9: 10..90 % time
        reset();
    }
    void reset()
    {
        for (int i = 0; i < kMax; ++i) {
            dl_[i] = dr_[i] = 0.0f;
            box_[i] = 1.0f;
            qv_[i] = 1.0f;
            qi_[i] = 0;
        }
        dw_ = 0;
        bw_ = 0;
        qh_ = qt_ = 0;
        r_ = 1.0f;
        n_ = 0;
        bsum_ = (float)B_;
    }
    int latency() const { return B_ - 1; }
    float threshold() const { return thr_; }

    // In-place. enabled = false: no new reduction (gain releases to 1), latency unchanged.
    // Returns the smallest gain applied in this block.
    float process(float* l, float* r, int n, bool enabled)
    {
        constexpr int M = kMax - 1;
        // exact box sum once per block (no float drift)
        float s = 0.0f;
        for (int i = 0; i < B_; ++i) s += box_[i];
        bsum_ = s;
        const float invB = 1.0f / (float)B_;
        const int D = B_ - 1;
        float gmin = 1.0f;
        for (int i = 0; i < n; ++i) {
            const float xl = l[i], xr = r[i];
            float t = 1.0f;
            if (enabled) {
                const float pk = maxf(absf(xl), absf(xr));
                if (pk > thr_) t = thr_ / pk;
            }
            // moving minimum over the last B gains
            while (qt_ != qh_ && qv_[(qt_ - 1) & M] >= t) --qt_;
            qv_[qt_ & M] = t;
            qi_[qt_ & M] = n_;
            ++qt_;
            while ((uint32_t)(n_ - qi_[qh_ & M]) >= (uint32_t)B_) ++qh_;
            const float h = qv_[qh_ & M];
            // instant attack (hold), smooth release
            if (h < r_) r_ = h;
            else r_ += rel_a_ * (h - r_);
            // box filter
            bsum_ += r_ - box_[bw_];
            box_[bw_] = r_;
            if (++bw_ >= B_) bw_ = 0;
            float g = bsum_ * invB;
            if (g > 1.0f) g = 1.0f;
            if (g < gmin) gmin = g;
            // delay
            dl_[dw_ & M] = xl;
            dr_[dw_ & M] = xr;
            const uint32_t rd = (dw_ - (uint32_t)D) & (uint32_t)M;
            ++dw_;
            float yl = dl_[rd] * g, yr = dr_[rd] * g;
            if (enabled) {   // float residue of the running sum at extreme reduction
                yl = clampf(yl, -thr_, thr_);
                yr = clampf(yr, -thr_, thr_);
            }
            l[i] = yl;
            r[i] = yr;
            ++n_;
        }
        return gmin;
    }

private:
    float dl_[kMax], dr_[kMax];
    float box_[kMax];
    float qv_[kMax];
    uint32_t qi_[kMax];
    uint32_t dw_ = 0;
    int bw_ = 0;
    uint32_t qh_ = 0, qt_ = 0;
    uint32_t n_ = 0;
    float r_ = 1.0f, bsum_ = 1.0f, rel_a_ = 0.001f, thr_ = 0.966f;
    int B_ = 72;
};

}  // namespace dsp
}  // namespace orbit
