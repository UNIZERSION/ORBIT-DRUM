#include "fx.h"

#include <math.h>
#include "dsp/env.h"
#include "platform.h"
#include "prof.h"

namespace orbit {

using namespace dsp;

// ---------------------------------------------------------------- StereoDelay

void StereoDelay::init(float* mem_l, float* mem_r, int32_t frames, int rate)
{
    bl_ = mem_l;
    br_ = mem_r;
    len_ = frames;
    fs_ = (float)rate;
    dc_a_ = onePoleCoef(30.0f, fs_);
    cur_ = next_ = (int32_t)(0.375f * fs_);
    if (cur_ > len_ - 4) cur_ = next_ = len_ - 4;
    tone_in_ = -1.0f;
    clear();
}

void StereoDelay::clear()
{
    if (bl_)
        for (int32_t i = 0; i < len_; ++i) bl_[i] = 0.0f;
    if (br_)
        for (int32_t i = 0; i < len_; ++i) br_[i] = 0.0f;
    w_ = 0;
    xfading_ = false;
    xf_ = 0.0f;
    lpl_ = lpr_ = dcl_ = dcr_ = 0.0f;
}

ORBIT_HOT void StereoDelay::setParams(float delay_samples, float feedback, float tone, bool pingpong)
{
    float d = delay_samples;
    const float maxd = (float)(len_ - 4);
    while (d > maxd) d *= 0.5f;
    if (d < 1.0f) d = 1.0f;
    const int32_t di = (int32_t)(d + 0.5f);
    if (!xfading_ && di != cur_) {
        next_ = di;
        xfading_ = true;
        xf_ = 0.0f;
        xf_step_ = 1.0f / (0.03f * fs_);
    }
    fb_ = clampf(feedback, 0.0f, 0.95f);
    if (tone != tone_in_) {   // exp2f / expf live in flash: only when TONE moves
        tone_in_ = tone;
        tone_a_ = onePoleCoef(500.0f * exp2f(5.2f * clampf(tone, 0.0f, 1.0f)), fs_);
    }
    pp_ = pingpong;
}

ORBIT_HOT void StereoDelay::process(const float* in_l, const float* in_r, float* out_l, float* out_r, int n)
{
    block_used_ = bl_ && br_ && !ref_ && cur_ >= n && next_ >= n;
    if (!block_used_) {
        processRef(in_l, in_r, out_l, out_r, n);
        return;
    }
    // 1) the read heads (and the crossfade) of the whole block: every position read is >= n samples old, so none
    //    of them is written in this block
    {
        int32_t w = w_;
        for (int i = 0; i < n; ++i) {
            int32_t ia = w - cur_;
            if (ia < 0) ia += len_;
            float yl = bl_[ia], yr = br_[ia];
            if (xfading_) {
                int32_t ib = w - next_;
                if (ib < 0) ib += len_;
                xf_ += xf_step_;
                if (xf_ >= 1.0f) {
                    xf_ = 1.0f;
                    xfading_ = false;
                    cur_ = next_;
                }
                yl += xf_ * (bl_[ib] - yl);
                yr += xf_ * (br_[ib] - yr);
            }
            out_l[i] = yl;
            out_r[i] = yr;
            if (++w >= len_) w = 0;
        }
    }
    // 2) feedback: tone lowpass, DC blocker, soft clip; the writes
    const float fb = fb_, ta = tone_a_, da = dc_a_;
    float lpl = lpl_, lpr = lpr_, dcl = dcl_, dcr = dcr_;
    float* const bl = bl_;
    float* const br = br_;
    const int32_t len = len_;
    int32_t w = w_;
    for (int i = 0; i < n; ++i) {
        const float yl = out_l[i], yr = out_r[i];
        lpl += ta * (yl - lpl);
        lpr += ta * (yr - lpr);
        dcl += da * (lpl - dcl);
        dcr += da * (lpr - dcr);
        const float fl = (lpl - dcl) * fb, fr = (lpr - dcr) * fb;
        float wl, wr;
        if (pp_) {
            wl = 0.5f * (in_l[i] + in_r[i]) + fr;
            wr = fl;
        } else {
            wl = in_l[i] + fl;
            wr = in_r[i] + fr;
        }
        bl[w] = softClip(wl);
        br[w] = softClip(wr);
        if (++w >= len) w = 0;
    }
    w_ = w;
    lpl_ = flushDenormal(lpl);
    lpr_ = flushDenormal(lpr);
    dcl_ = flushDenormal(dcl);
    dcr_ = flushDenormal(dcr);
}

ORBIT_HOT void StereoDelay::processRef(const float* in_l, const float* in_r, float* out_l, float* out_r, int n)
{
    if (!bl_ || !br_) {
        for (int i = 0; i < n; ++i) out_l[i] = out_r[i] = 0.0f;
        return;
    }
    const float fb = fb_, ta = tone_a_, da = dc_a_;
    float lpl = lpl_, lpr = lpr_, dcl = dcl_, dcr = dcr_;
    int32_t w = w_;
    for (int i = 0; i < n; ++i) {
        int32_t ia = w - cur_;
        if (ia < 0) ia += len_;
        float yl = bl_[ia], yr = br_[ia];
        if (xfading_) {
            int32_t ib = w - next_;
            if (ib < 0) ib += len_;
            xf_ += xf_step_;
            if (xf_ >= 1.0f) {
                xf_ = 1.0f;
                xfading_ = false;
                cur_ = next_;
            }
            yl += xf_ * (bl_[ib] - yl);
            yr += xf_ * (br_[ib] - yr);
        }
        // feedback: tone lowpass, DC blocker, soft clip
        lpl += ta * (yl - lpl);
        lpr += ta * (yr - lpr);
        dcl += da * (lpl - dcl);
        dcr += da * (lpr - dcr);
        const float fl = (lpl - dcl) * fb, fr = (lpr - dcr) * fb;
        float wl, wr;
        if (pp_) {
            wl = 0.5f * (in_l[i] + in_r[i]) + fr;
            wr = fl;
        } else {
            wl = in_l[i] + fl;
            wr = in_r[i] + fr;
        }
        bl_[w] = softClip(wl);
        br_[w] = softClip(wr);
        if (++w >= len_) w = 0;
        out_l[i] = yl;
        out_r[i] = yr;
    }
    w_ = w;
    lpl_ = flushDenormal(lpl);
    lpr_ = flushDenormal(lpr);
    dcl_ = flushDenormal(dcl);
    dcr_ = flushDenormal(dcr);
}

// ---------------------------------------------------------------- FxBus

namespace {
float* s_resv_lines = nullptr;   // FxBus::reserveLines: internal RAM, never freed
int32_t s_resv_floats = 0;
const void* s_resv_owner = nullptr;   // the FxBus using it (one at a time; any other one takes PSRAM)
}  // namespace

bool FxBus::reserveLines(int max_rate)
{
    if (s_resv_lines) return true;
    const int32_t n = PlateReverb::lineFloats(max_rate);
    float* p = static_cast<float*>(fastAlloc((size_t)n * sizeof(float)));
    if (p && !memIsInternal(p)) {   // fastAlloc fell back to PSRAM: no gain, init() takes PSRAM itself
        fastFree(p);
        p = nullptr;
    }
    s_resv_lines = p;
    s_resv_floats = p ? n : 0;
    return p != nullptr;
}

bool FxBus::linesReserved() { return s_resv_lines != nullptr; }

bool FxBus::init(int rate)
{
    release();
    rate_ = rate;
    const int32_t lf = PlateReverb::lineFloats(rate), pf = PlateReverb::predelayFloats(rate);
    lines_resv_ = s_resv_lines && s_resv_floats >= lf && !s_resv_owner;
    if (lines_resv_) s_resv_owner = this;
    rev_lines_ = lines_resv_ ? s_resv_lines : static_cast<float*>(bigAlloc((size_t)lf * sizeof(float)));
    rev_pre_ = static_cast<float*>(bigAlloc((size_t)pf * sizeof(float)));
    const int32_t df = StereoDelay::framesFor(rate);
    dly_l_ = static_cast<float*>(bigAlloc((size_t)df * sizeof(float)));
    dly_r_ = static_cast<float*>(bigAlloc((size_t)df * sizeof(float)));
    if (!rev_lines_ || !rev_pre_ || !dly_l_ || !dly_r_) {
        release();
        return false;
    }
    mem_bytes_ = (size_t)(lf + pf) * sizeof(float) + 2 * (size_t)df * sizeof(float);
    reverb_.init(rev_lines_, rev_pre_, rate);
    delay_.init(dly_l_, dly_r_, df, rate);
    dly_level_.reset(0.0f);
    rev_level_.reset(0.0f);
    silent_ = 0x7fffffff;
    idle_ = true;
    echo_dly_ = -1;
    return true;
}

void FxBus::release()
{
    if (rev_lines_ && !lines_resv_) bigFree(rev_lines_);   // the reserved block stays for the next init
    if (lines_resv_ && s_resv_owner == this) s_resv_owner = nullptr;
    if (rev_pre_) bigFree(rev_pre_);
    if (dly_l_) bigFree(dly_l_);
    if (dly_r_) bigFree(dly_r_);
    rev_lines_ = rev_pre_ = dly_l_ = dly_r_ = nullptr;
    lines_resv_ = false;
    mem_bytes_ = 0;
}

void FxBus::clear()
{
    if (!ready()) return;
    reverb_.clear();
    delay_.clear();
}

ORBIT_HOT void FxBus::process(const float* send_l, const float* send_r, const float* space, int n,
                              const FxParams& p, float* out_l, float* out_r)
{
    float pk = 0.0f;
    for (int i = 0; i < n; ++i) {
        pk = maxf(pk, absf(send_l[i]));
        pk = maxf(pk, absf(send_r[i]));
        pk = maxf(pk, absf(space[i]));
    }
    if (pk > 1.0e-9f) {
        silent_ = 0;
        idle_ = false;
    } else if (silent_ < 0x7fff0000u) {
        silent_ += (uint32_t)n;
    }
    if (!idle_) {
        // tail = reverb RT60 + pre-delay + delay echoes down to -80 dB (capped)
        const float fb = clampf(p.dly_feedback, 0.01f, 0.95f);
        if (fb != echo_fb_ || delay_.delay() != echo_dly_) {   // logf only when these change
            echo_fb_ = fb;
            echo_dly_ = delay_.delay();
            const float echo_s = ((float)echo_dly_ / (float)rate_) * (::logf(1.0e-4f) / ::logf(fb)) + 0.1f;
            echo_s_ = minf(echo_s, 60.0f);
        }
        const float tail_s = maxf(p.rev_decay * 1.4f + p.rev_predelay_ms * 0.001f + 0.5f, echo_s_);
        if ((float)silent_ > tail_s * (float)rate_) idle_ = true;
    }
    if (idle_ || !ready() || p.bypass) {
        for (int i = 0; i < n; ++i) out_l[i] = out_r[i] = 0.0f;
        dly_level_.reset(p.dly_level);
        rev_level_.reset(p.rev_level);
        cyc_dly_ = cyc_rev_ = 0;
        return;
    }
    delay_.setReference(p.reference);
    const uint32_t c0 = profCycles();
    delay_.setParams(p.delay_samples, p.dly_feedback, p.dly_tone, p.dly_pingpong);
    delay_.process(send_l, send_r, dl_, dr_, n);
    const uint32_t c1 = profCycles();
    dly_level_.begin(p.dly_level, n);
    for (int i = 0; i < n; ++i) {
        const float g = dly_level_.next();
        dl_[i] *= g;
        dr_[i] *= g;
        rin_[i] = 0.5f * (send_l[i] + send_r[i] + dl_[i] + dr_[i]) + space[i];
    }
    dly_level_.end();
    const uint32_t c2 = profCycles();
    reverb_.setParams(p.rev_size, p.rev_decay, p.rev_damp, p.rev_predelay_ms);
    reverb_.process(rin_, rl_, rr_, n);
    const uint32_t c3 = profCycles();
    cyc_dly_ = c1 - c0;
    cyc_rev_ = c3 - c2;
    rev_level_.begin(p.rev_level, n);
    for (int i = 0; i < n; ++i) {
        const float g = rev_level_.next();
        out_l[i] = dl_[i] + rl_[i] * g;
        out_r[i] = dr_[i] + rr_[i] * g;
    }
    rev_level_.end();
}

// ---------------------------------------------------------------- master processors

ORBIT_HOT void Pump::process(float* gain, int n, const uint32_t* hits, int nhits, float depth, float release_ms,
                             float fs)
{
    if (release_ms != rel_ms || fs != rel_fs) {   // expf only when PUMP RELEASE moves
        rel_ms = release_ms;
        rel_fs = fs;
        rel_c = segmentCoef(release_ms, fs);
    }
    int h = 0;
    float e = env;
    for (int i = 0; i < n; ++i) {
        while (h < nhits && hits[h] <= (uint32_t)i) {
            attacking = 1;
            ++h;
        }
        if (attacking) {
            e += att_step;
            if (e >= 1.0f) {
                e = 1.0f;
                attacking = 0;
            }
        } else if (e > 0.0f) {
            e += rel_c * (-0.01f - e);
            if (e < 0.0f) e = 0.0f;
        }
        gain[i] = 1.0f - depth * e;
    }
    env = e;
}

ORBIT_HOT void LoFi::process(float* l, float* r, int n, float bits, float rate_hz, float mix_from, float mix_to,
                             float fs)
{
    if (mix_from <= 0.0f && mix_to <= 0.0f) return;
    if (bits != crush_bits) {
        crush_bits = bits;
        crush_q = crushQ(bits);
        crush_iq = 1.0f / crush_q;
    }
    const float q = crush_q, iq = crush_iq;
    const bool do_crush = bits < 15.5f;
    const float inc = clampf(rate_hz / fs, 0.0f, 1.0f);
    float m = mix_from;
    const float dm = (mix_to - mix_from) / (float)n;
    for (int i = 0; i < n; ++i) {
        float wl = l[i], wr = r[i];
        rr.tick(inc, wl, wr);
        if (do_crush) {
            wl = crush(wl, q, iq);
            wr = crush(wr, q, iq);
        }
        m += dm;
        l[i] += m * (wl - l[i]);
        r[i] += m * (wr - r[i]);
    }
}

ORBIT_HOT void Wide::process(float* l, float* r, int n, float amt_from, float amt_to, float width)
{
    if (amt_from <= 0.0f && amt_to <= 0.0f) return;
    float a = amt_from;
    const float da = (amt_to - amt_from) / (float)n;
    const float wk = 1.0f + clampf(width, 0.0f, 1.0f);
    for (int i = 0; i < n; ++i) {
        const float m = 0.5f * (l[i] + r[i]);
        const float s = 0.5f * (l[i] - r[i]);
        const float hs = hp.highpass(s, c);
        a += da;
        const float s2 = s + a * (wk * hs - s);
        l[i] = m + s2;
        r[i] = m - s2;
    }
    hp.flush();
}

ORBIT_HOT void Click::trigger(int32_t delay, bool accent, float level, float fs)
{
    wait = delay < 0 ? 0 : delay;
    pend_inc = (accent ? 1760.0f : 1100.0f) / fs;
    pend_amp = clampf(level, 0.0f, 1.0f) * (accent ? 0.6f : 0.42f);
    if (fs != dec_fs) {
        dec_fs = fs;
        decay = expf(-1.0f / (0.008f * fs));
    }
    pend_len = (int32_t)(0.06f * fs);   // 60 ms, ends ~-65 dB down
}

ORBIT_HOT void Click::process(float* l, float* r, int n)
{
    for (int i = 0; i < n; ++i) {
        if (wait > 0) {
            --wait;
            continue;
        }
        if (wait == 0) {
            left = pend_len;
            inc = pend_inc;
            amp = pend_amp;
            phase = 0.0f;
            env = 1.0f;
            wait = -1;
        }
        if (left <= 0) {
            // idle: skip the rest of the block if nothing is pending
            if (wait < 0) return;
            continue;
        }
        const float y = sin2pi(phase) * amp * env;
        phase += inc;
        if (phase >= 1.0f) phase -= 1.0f;
        env *= decay;
        --left;
        l[i] += y;
        r[i] += y;
    }
}

}  // namespace orbit
