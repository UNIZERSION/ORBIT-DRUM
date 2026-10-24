#include "voice.h"

#include "dsp/fastmath.h"
#include "dsp/interp.h"

namespace orbit {

using namespace dsp;

namespace {

constexpr float kLoopXfadeS = 0.005f;   // loop seam crossfade (source time)
constexpr int kMicroFade = 64;          // output samples of automatic fade at a non-zero end
constexpr float kFrac32 = 1.0f / 4294967296.0f;

// Gain from PP_FADE_IN / PP_FADE_OUT and the micro-fade at region position t (source frames).
inline float regionGain(const Voice& v, const PadCache& pc, float t)
{
    float g = 1.0f;
    if (v.first_pass && pc.fade_in_s > 0.0f) {
        const float fi = pc.fade_in_s * (float)v.smp->rate;
        if (t < fi) g = t / fi;
    }
    if (!v.loop) {
        float fo = pc.fade_out_s * (float)v.smp->rate;
        if (v.micro > fo) fo = v.micro;
        if (fo > 0.0f) {
            const float left = (float)v.len - t;
            if (left < fo) g = minf(g, left / fo);
        }
    }
    return clampf(g, 0.0f, 1.0f);
}

inline void setIncrement(Voice& v, float inc)
{
    v.inc = inc;
    const int32_t ii = (int32_t)inc;
    float f = (inc - (float)ii) * 4294967296.0f;
    if (f > 4294967040.0f) f = 4294967040.0f;
    v.inc_i = ii;
    v.inc_f = (uint32_t)f;
}

// SVF coefficients in "delta state-space" form. The HP-first TPT SVF
//   hp = (x - gk s1 - s2) a1, bp = g hp + s1, lp = g bp + s2, s1 += 2g hp, s2 += 2g bp
// expanded so each state update is one short multiply-add chain:
//   s1 += b1 x + e11 s1 + e12 s2,  s2 += b2 x + e21 s1 + e22 s2,  y = d x + c1 s1 + c2 s2
// (same poles; the increment form keeps the float precision of HP-first at low cutoffs).
void filterCoefs(Voice& v, const PadCache& pc, float fcn)
{
    const SvfCoefs c = svfCoefs(fcn, pc.k);
    const float g = c.g, gk = c.gk, a1 = c.a1;
    const float ga1 = g * a1;
    const float h = 1.0f - ga1 * gk;
    v.fb1 = 2.0f * ga1;
    v.fe11 = -v.fb1 * gk;
    v.fe12 = -v.fb1;
    v.fb2 = 2.0f * g * ga1;
    v.fe21 = 2.0f * g * h;
    v.fe22 = -v.fb2;
    v.fd = pc.mix_lp * g * ga1 + pc.mix_bp * ga1 + pc.mix_hp * a1;
    v.fc1 = pc.mix_lp * g * h + pc.mix_bp * h - pc.mix_hp * a1 * gk;
    v.fc2 = pc.mix_lp * (1.0f - g * ga1) - pc.mix_bp * ga1 - pc.mix_hp * a1;
}

// Control-rate update: pitch, envelopes, filter coefficients, gain ramp targets.
// Returns false when the voice has finished.
bool control(Voice& v, const PadCache& pc)
{
    const bool first = !v.started;
    v.started = 1;
    if (!first) {
        if (v.state == VS_FADE && v.fade <= 0.0f) return false;
        if (v.amp.done()) return false;
    }
    const int m = kCtlBlock;
    const float fm = (float)m;

    // LFO (with fade-in after the trigger)
    float l_pitch = 0.0f, l_cut = 0.0f, l_amp = 1.0f, l_pan = 0.0f;
    if (pc.lfo_depth > 0.0f) {
        float amt = pc.lfo_depth;
        if (pc.lfo_fade_inv > 0.0f) amt *= minf(1.0f, (float)v.age_samples * pc.lfo_fade_inv);
        const float raw = v.lfo.step(pc.lfo_shape, pc.lfo_inc * fm);
        const float lv = raw * amt;
        switch (pc.lfo_target) {
        case 0: l_pitch = lv * 12.0f; break;
        case 1: l_cut = lv * 4.0f; break;
        case 2: l_amp = 1.0f - 0.5f * amt * (1.0f - raw); break;
        default: l_pan = lv; break;
        }
    }
    v.age_samples += (uint32_t)m;

    // pitch: tune + layer/step/command + pitch envelope + LFO; the rate ratio absorbs sample rates
    const float semis = pc.tune + v.pitch + pc.penv * v.penv.x + l_pitch;
    if (semis != v.last_semis) {
        v.last_semis = semis;
        setIncrement(v, clampf(fastExp2(semis * (1.0f / 12.0f)) * v.rate_ratio, 1.0f / 256.0f, 32.0f));
    }
    v.penv.advance(pc.pdec_a);

    // filter: log-frequency smoothed at control rate, coefficients constant per sub-block.
    // An open lowpass (>= ~0.41 fs, no resonance) is bypassed and re-engages from the steady
    // state of the last input, so there is no step.
    v.filt = pc.filter_on;
    if (v.filt) {
        const float target = pc.cutoff_l2 + pc.fenv_oct * v.fenv.x + l_cut;
        v.f_l2 = first ? target : v.f_l2 + 0.5f * (target - v.f_l2);
        if (absf(v.f_l2 - target) < 1.0e-4f) v.f_l2 = target;
        const float fcn = fastExp2(v.f_l2);
        const bool lp = pc.mix_lp > 0.5f;
        const bool open = lp && pc.k >= 1.6f && fcn >= (v.filt_active ? 0.41f : 0.40f);
        if (open) {
            v.filt_active = 0;
        } else {
            const float key = pc.k + pc.mix_lp * 4.0f + pc.mix_bp * 16.0f + pc.mix_hp * 64.0f;
            if (!v.filt_active || key != v.f_key || absf(v.f_l2 - v.f_l2_coef) > 1.0e-4f) {
                filterCoefs(v, pc, fcn);
                v.f_key = key;
                v.f_l2_coef = v.f_l2;
            }
            if (!v.filt_active) {
                v.filt_active = 1;
                for (int ch = 0; ch < 2; ++ch) {
                    v.svf[ch].s1 = 0.0f;
                    v.svf[ch].s2 = lp ? v.last_in[ch] : 0.0f;
                }
            }
        }
    } else {
        v.filt_active = 0;
    }
    v.fenv.advance(pc.fdec_a);
    v.amp.update(pc.env, m);

    // gains: pad gain, layer gain, velocity, LFO amp, pan, region fades, ghost fade.
    // Sample i of the sub-block uses the ramp value after i+1 steps, i.e. the gain at the
    // position of the last sample read: t_now + inc * (m - 1).
    const float pan = pc.pan + v.layer_pan + l_pan;
    if (pan != v.last_pan) {
        v.last_pan = pan;
        if (v.channels == 2) panBalance(pan, v.pl, v.pr);
        else panConstantPower(pan, v.pl, v.pr);
    }
    const float pl = v.pl, pr = v.pr;
    const float base = pc.gain * v.layer_gain * v.vel_gain * l_amp * kInv32768;
    const float t_now = (float)v.t_int + (float)v.t_fr * kFrac32;
    const float t_end = t_now + v.inc * (fm - 1.0f);
    float fade_end = v.fade;
    if (v.state == VS_FADE) fade_end = maxf(0.0f, v.fade - v.fade_step * fm);
    const float ge = base * regionGain(v, pc, t_end) * fade_end;
    const float tl = ge * pl, tr = ge * pr;
    const float tsl = tl * pc.send, tsr = tr * pc.send;
    if (first) {
        const float gs = base * regionGain(v, pc, t_now) * v.fade;
        v.gl = gs * pl;
        v.gr = gs * pr;
        v.gsl = v.gl * pc.send;
        v.gsr = v.gr * pc.send;
    }
    const float inv = 1.0f / fm;
    v.dgl = (tl - v.gl) * inv;
    v.dgr = (tr - v.gr) * inv;
    v.dgsl = (tsl - v.gsl) * inv;
    v.dgsr = (tsr - v.gsr) * inv;
    v.has_send = (v.gsl != 0.0f || v.gsr != 0.0f || tsl != 0.0f || tsr != 0.0f) ? 1 : 0;
    v.fade = fade_end;

    // fast path: the whole sub-block stays clear of the region end / loop seam
    int32_t limit = v.len;
    if (v.loop && v.xf > 0) limit = v.len - v.xf;
    bool fast = v.t_int + (int32_t)(v.inc * fm) + 2 < limit;
    if (fast && v.loop && v.xf_dip && !v.first_pass && v.t_int < v.loop_off + v.xf) fast = false;
    v.fast = fast ? 1 : 0;
    return true;
}

// Stage 1: read + interpolate up to m frames (int16 units). SLOW handles region end and loops.
// DIR = +1 forward / -1 reverse: a compile-time stride gives immediate-offset loads.
template <int CH, int DIR, bool SLOW>
ORBIT_HOT int readStage(Voice& v, float* __restrict bl, float* __restrict br, int m)
{
    const int16_t* const base = v.base;
    constexpr int s = DIR * CH;
    int32_t ti = v.t_int;
    uint32_t fr = v.t_fr;
    const int32_t ii = v.inc_i;
    const uint32_t fi = v.inc_f;
    const int32_t len = v.len, xf_start = v.len - v.xf;
    int i = 0;
    if (!SLOW && fr == 0 && fi == 0) {
        // integer step (native rate, whole-semitone-free pitch): Hermite at x = 0 is the sample
        const int16_t* p = base + ti * s;
        for (; i < m; ++i) {
            bl[i] = (float)p[0];
            if (CH == 2) br[i] = (float)p[1];
            p += ii * s;
        }
        v.t_int = ti + ii * m;
        return m;
    }
    for (; i < m; ++i) {
        if (SLOW) {
            if (ti >= len) {
                if (!v.loop) {
                    v.ended = 1;
                    break;
                }
                while (ti >= len) ti -= v.loop_len;
                v.first_pass = 0;
            }
        }
        const float x = (float)(fr >> 8) * (1.0f / 16777216.0f);
        const int16_t* p = base + ti * s;
        float yl = hermiteI16(p, s, x);
        float yr = (CH == 2) ? hermiteI16(p + 1, s, x) : 0.0f;
        if (SLOW && v.loop && v.xf > 0) {
            if (ti >= xf_start) {
                const float a = ((float)(ti - xf_start) + x) * v.inv_xf;
                if (v.xf_dip) {
                    yl *= 1.0f - a;
                    if (CH == 2) yr *= 1.0f - a;
                } else {
                    const int16_t* q = p - v.loop_len * s;
                    yl += a * (hermiteI16(q, s, x) - yl);
                    if (CH == 2) yr += a * (hermiteI16(q + 1, s, x) - yr);
                }
            } else if (v.xf_dip && !v.first_pass && ti < v.loop_off + v.xf) {
                const float a = ((float)(ti - v.loop_off) + x) * v.inv_xf;
                yl *= a;
                if (CH == 2) yr *= a;
            }
        }
        bl[i] = yl;
        if (CH == 2) br[i] = yr;
        const uint32_t nf = fr + fi;
        ti += ii + (nf < fr ? 1 : 0);
        fr = nf;
    }
    v.t_int = ti;
    v.t_fr = fr;
    return i;
}

// Stage 2: per-pad crush / downsample / drive (only when enabled).
template <int CH>
void colorStage(Voice& v, const PadCache& pc, float* bl, float* br, int k)
{
    if (pc.crush_q > 0.0f) {
        const float q = pc.crush_q, st = pc.crush_step;
        for (int i = 0; i < k; ++i) {
            bl[i] = (float)floorToInt(bl[i] * q + 0.5f) * st;
            if (CH == 2) br[i] = (float)floorToInt(br[i] * q + 0.5f) * st;
        }
    }
    if (pc.downsample > 1) {
        const int32_t ds = pc.downsample;
        for (int i = 0; i < k; ++i) {
            if (--v.ds_cnt <= 0) {
                v.ds_cnt = ds;
                v.hold[0] = bl[i];
                if (CH == 2) v.hold[1] = br[i];
            }
            bl[i] = v.hold[0];
            if (CH == 2) br[i] = v.hold[1];
        }
    }
    if (pc.drive_pre > 0.0f) {
        const float pre = pc.drive_pre, post = pc.drive_post;
        for (int i = 0; i < k; ++i) {
            bl[i] = softClip(bl[i] * pre) * post;
            if (CH == 2) br[i] = softClip(br[i] * pre) * post;
        }
    }
}

// Stage 3: SVF, delta state-space form (see filterCoefs).
template <int CH>
ORBIT_HOT void filterStage(Voice& v, float* __restrict bl, float* __restrict br, int k)
{
    const float b1 = v.fb1, e11 = v.fe11, e12 = v.fe12, b2 = v.fb2, e21 = v.fe21, e22 = v.fe22;
    const float d = v.fd, c1 = v.fc1, c2 = v.fc2;
    float s1l = v.svf[0].s1, s2l = v.svf[0].s2, s1r = v.svf[1].s1, s2r = v.svf[1].s2;
    for (int i = 0; i < k; ++i) {
        const float x = bl[i];
        bl[i] = d * x + c1 * s1l + c2 * s2l;
        const float u1 = b1 * x + e12 * s2l + e11 * s1l;
        const float u2 = b2 * x + e22 * s2l + e21 * s1l;
        s1l += u1;
        s2l += u2;
        if (CH == 2) {
            const float xr = br[i];
            br[i] = d * xr + c1 * s1r + c2 * s2r;
            const float w1 = b1 * xr + e12 * s2r + e11 * s1r;
            const float w2 = b2 * xr + e22 * s2r + e21 * s1r;
            s1r += w1;
            s2r += w2;
        }
    }
    v.svf[0].s1 = s1l;
    v.svf[0].s2 = s2l;
    v.svf[1].s1 = s1r;
    v.svf[1].s2 = s2r;
}

// Stage 4: amp envelope, gain ramps, mix into the bus (and the FX send when used).
template <int CH, bool SEND>
ORBIT_HOT void mixStage(Voice& v, const PadCache& pc, const float* bl, const float* br, const VoiceOut& o, int i0,
                        int k)
{
    float ex = v.amp.x, ec = v.amp.c, et = v.amp.tgt, elo = v.amp.lo, ehi = v.amp.hi;
    float gl = v.gl, gr = v.gr, gsl = v.gsl, gsr = v.gsr;
    const float dgl = v.dgl, dgr = v.dgr, dgsl = v.dgsl, dgsr = v.dgsr;
    float* __restrict ol = o.l + i0;
    float* __restrict orr = o.r + i0;
    float* __restrict osl = o.sl + i0;
    float* __restrict osr = o.sr + i0;
    for (int i = 0; i < k; ++i) {
        ex += ec * (et - ex);
        if (ex > ehi || ex < elo) {
            v.amp.x = ex;
            v.amp.segmentEnd(pc.env);
            ex = v.amp.x;
            ec = v.amp.c;
            et = v.amp.tgt;
            elo = v.amp.lo;
            ehi = v.amp.hi;
        }
        gl += dgl;
        gr += dgr;
        const float yl = bl[i] * ex;
        const float yr = (CH == 2) ? br[i] * ex : yl;
        ol[i] += yl * gl;
        orr[i] += yr * gr;
        if (SEND) {
            gsl += dgsl;
            gsr += dgsr;
            osl[i] += yl * gsl;
            osr[i] += yr * gsr;
        }
    }
    if (!SEND) {
        gsl += dgsl * (float)k;
        gsr += dgsr * (float)k;
    }
    v.amp.x = ex;
    v.amp.c = ec;
    v.amp.tgt = et;
    v.amp.lo = elo;
    v.amp.hi = ehi;
    v.gl = gl;
    v.gr = gr;
    v.gsl = gsl;
    v.gsr = gsr;
}

template <int CH>
ORBIT_HOT void runChunk(Voice& v, const PadCache& pc, const VoiceOut& o, int i0, int m)
{
    float bl[kCtlBlock], br[kCtlBlock];
    int k;
    if (v.rev) k = v.fast ? readStage<CH, -1, false>(v, bl, br, m) : readStage<CH, -1, true>(v, bl, br, m);
    else k = v.fast ? readStage<CH, 1, false>(v, bl, br, m) : readStage<CH, 1, true>(v, bl, br, m);
    if (k <= 0) return;
    colorStage<CH>(v, pc, bl, br, k);
    v.last_in[0] = bl[k - 1];
    v.last_in[1] = (CH == 2) ? br[k - 1] : bl[k - 1];
    if (v.filt && v.filt_active) filterStage<CH>(v, bl, br, k);
    if (v.has_send) mixStage<CH, true>(v, pc, bl, br, o, i0, k);
    else mixStage<CH, false>(v, pc, bl, br, o, i0, k);
}

}  // namespace

ORBIT_HOT bool voiceStart(Voice& v, const VoiceStart& s, const PadCache& pc, const VoiceCtx& ctx)
{
    const Sample* smp = s.smp;
    if (!smp || !smp->data || smp->frames < 1 || smp->rate == 0) return false;
    const int ch = smp->channels == 2 ? 2 : 1;
    const int32_t frames = (int32_t)smp->frames;
    int32_t rs = (int32_t)(clampf(pc.start, 0.0f, 1.0f) * (float)frames);
    int32_t re = (int32_t)(clampf(pc.end, 0.0f, 1.0f) * (float)frames);
    if (rs >= frames) rs = frames - 1;
    if (rs < 0) rs = 0;
    if (re > frames) re = frames;
    if (re <= rs) re = rs + 1;

    v = Voice();
    v.smp = smp;
    v.pad = s.pad;
    v.layer = s.layer;
    v.channels = (uint8_t)ch;
    v.hit = s.hit;
    v.age = s.age;
    v.reg_start = rs;
    v.reg_end = re;
    v.len = re - rs;
    v.rev = pc.reverse ? 1 : 0;
    v.loop = (pc.mode == (uint8_t)PlayMode::Loop) ? 1 : 0;
    v.gate = (pc.mode == (uint8_t)PlayMode::Gate) ? 1 : 0;
    v.base = v.rev ? smp->data + (size_t)(re - 1) * ch : smp->data + (size_t)rs * ch;
    v.rate_ratio = (float)smp->rate * ctx.inv_fs;
    v.pitch = s.pitch;
    v.layer_gain = s.layer_gain;
    v.layer_pan = s.layer_pan;
    const float v01 = (float)(s.vel < 1 ? 1 : (s.vel > 127 ? 127 : s.vel)) * (1.0f / 127.0f);
    v.vel_gain = 1.0f - pc.vel_amp + pc.vel_amp * v01 * v01;

    if (v.loop) {
        int32_t lo = (int32_t)(clampf(pc.loop, 0.0f, 1.0f) * (float)v.len);
        if (lo > v.len - 1) lo = v.len - 1;
        v.loop_off = lo;
        v.loop_len = v.len - lo;
        int32_t xf = (int32_t)(kLoopXfadeS * (float)smp->rate);
        if (xf > v.loop_len / 2) xf = v.loop_len / 2;
        if (xf < 4) xf = 0;
        v.xf = xf;
        v.inv_xf = xf > 0 ? 1.0f / (float)xf : 0.0f;
        const int32_t avail = v.rev ? (frames - re) + lo : rs + lo;   // data before the loop start
        v.xf_dip = avail < xf + 2 ? 1 : 0;
    } else {
        v.loop_off = 0;
        v.loop_len = v.len;
        // micro-fade when the region stops on a non-zero value
        const int32_t last = v.rev ? rs : re - 1;
        int a = smp->data[(size_t)last * ch];
        if (a < 0) a = -a;
        if (ch == 2) {
            int b = smp->data[(size_t)last * ch + 1];
            if (b < 0) b = -b;
            if (b > a) a = b;
        }
        if (a > 32) v.micro = (float)kMicroFade * v.rate_ratio;
    }

    v.amp.start(pc.env);
    v.fenv.start();
    v.penv.start();
    v.lfo.start(s.seed);
    v.ds_cnt = 0;
    v.state = VS_ON;
    v.ctl_left = 0;
    v.started = 0;
    v.first_pass = 1;
    return true;
}

ORBIT_HOT bool voiceRender(Voice& v, const PadCache& pc, const VoiceOut& out, int n, const VoiceCtx& ctx)
{
    (void)ctx;
    int i = 0;
    while (i < n) {
        if (v.ctl_left <= 0) {
            if (!control(v, pc)) {
                v.state = VS_FREE;
                return false;
            }
            v.ctl_left = kCtlBlock;
        }
        int m = n - i;
        if (m > v.ctl_left) m = v.ctl_left;
        if (v.channels == 2) runChunk<2>(v, pc, out, i, m);
        else runChunk<1>(v, pc, out, i, m);
        if (v.ended) {
            v.state = VS_FREE;
            return false;
        }
        v.ctl_left -= m;
        i += m;
    }
    return true;
}

ORBIT_HOT void voiceRelease(Voice& v, const PadCache& pc)
{
    if (v.state != VS_ON || v.released) return;
    v.released = 1;
    if (v.gate || v.loop) v.amp.release(pc.env);
}

ORBIT_HOT void voiceFadeOut(Voice& v, float samples)
{
    if (v.state == VS_FREE) return;
    if (samples < 1.0f) samples = 1.0f;
    const float step = 1.0f / samples;
    if (v.state == VS_FADE && v.fade_step >= step) return;   // already fading faster
    v.state = VS_FADE;
    v.fade_step = step;
    v.ctl_left = 0;   // re-target the gain ramps now
}

ORBIT_HOT float voicePosition(const Voice& v)
{
    if (!v.smp || v.smp->frames == 0) return -1.0f;
    const float t = (float)v.t_int + (float)v.t_fr * kFrac32;
    const float pos = v.rev ? (float)(v.reg_end - 1) - t : (float)v.reg_start + t;
    return clampf(pos / (float)v.smp->frames, 0.0f, 1.0f);
}

ORBIT_HOT void voiceFlush(Voice& v)
{
    v.svf[0].flush();
    v.svf[1].flush();
}

}  // namespace orbit
