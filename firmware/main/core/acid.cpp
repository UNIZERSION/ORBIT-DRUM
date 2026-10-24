#include "acid.h"

#include <math.h>
#include "dsp/env.h"
#include "dsp/fastmath.h"

namespace orbit {

using namespace dsp;

namespace {

// VCO
constexpr float kSlideTau = 0.065f;         // s, fixed 303 slide time constant
constexpr float kSquareGain = 0.62f;        // square level vs saw (similar loudness after the VCF)
constexpr float kSquareRoundHz = 4000.0f;   // one-pole that rounds the square's edges
constexpr float kWaveXfadeS = 0.005f;       // saw <-> square crossfade
// VCF
constexpr float kCutLog2 = 6.49185309f;     // log2(90 Hz), CUTOFF = 0
constexpr float kCutRange = 5.64385619f;    // log2(4500 / 90) octaves, CUTOFF = 1
constexpr float kEnvOct = 4.0f;             // MEG depth at ENV MOD = 1
constexpr float kAccOct = 2.5f;             // accent sweep depth at ACCENT = 1
constexpr float kFcMinHz = 20.0f;
constexpr float kFcMaxNorm = 0.45f;
constexpr float kFbHpHz = 150.0f;           // highpass inside the resonance loop
constexpr float kKMax = 4.0f;               // ladder feedback at RESO = 1, high cutoffs
constexpr float kKTrack = 1.0f;             // k = RESO 4 (1 + kKTrack fh / fc): follows the oscillation
                                            // threshold the loop highpass raises (edge above ~800 Hz,
                                            // 0.9 of it at 300 Hz, 0.65 at 90 Hz: no low boom)
constexpr float kKCap = 12.0f;
constexpr float kInGain = 0.35f;            // VCO -> ladder input: the soft clip mostly limits the resonance
constexpr float kResComp = 0.5f;            // makeup above the loop highpass: x (1 + kResComp k)
constexpr float kOutTrim = 0.84f;           // output calibration (default patch peaks ~0.5)
// Envelopes
constexpr float kMegTauMin = 0.15f;         // DECAY = 0
constexpr float kMegTauLog2 = 4.05889369f;  // log2(2.5 / 0.15), DECAY = 1
constexpr float kAccMegTau = 0.2f;          // accented notes
constexpr float kAccChargeTau = 0.04f;      // accent sweep RC charge
constexpr float kAccDischargeLo = 0.15f;    // RC discharge at RESO = 0 (s)
constexpr float kAccDischargeHi = 0.35f;    // RC discharge at RESO = 1 (s)
constexpr float kAccNorm = 1.43f;           // a run of 16th accents settles at ~1.0
constexpr float kAccAmp = 1.0f;             // VCA boost at ACCENT = 1: x2 (+6 dB) at the MEG peak
constexpr float kBoostTau = 0.0015f;        // accent gain smoothing (no click on a retrigger)
constexpr float kVegAttackMs = 2.5f;
constexpr float kVegDecayTau = 3.0f;
constexpr float kVegReleaseTau = 0.008f;
constexpr float kVegFloor = 1.0e-5f;        // -100 dB: VEG done
// Output / control
constexpr float kDcHz = 7.0f;
constexpr float kPanicS = 0.004f;
constexpr float kKnobTau = 0.015f;

enum VegStage : uint8_t { VEG_DONE = 0, VEG_ATTACK, VEG_DECAY, VEG_RELEASE };

// exp(-x) for 0 <= x <= 0.2 (control-rate coefficients without libm), error < 7e-5.
inline float expNegSmall(float x) { return 1.0f - x * (1.0f - x * (0.5f - x * (1.0f / 6.0f))); }

// One-pole knob follower at control rate; lands exactly on the target (no denormal tail).
inline void follow(float& s, float t, float c)
{
    s += c * (t - s);
    if (absf(t - s) < 1.0e-6f) s = t;
}

// clampf that also maps NaN to lo.
inline float sanitize(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }

// PolyBLEP residual of a unit upward step at phase 0 (t in [0, 1), increment dt, idt = 1 / dt).
inline float polyBlep(float t, float dt, float idt)
{
    if (t < dt) {
        const float x = t * idt;
        return x + x - x * x - 1.0f;
    }
    if (t > 1.0f - dt) {
        const float x = (t - 1.0f) * idt;
        return x * x + x + x + 1.0f;
    }
    return 0.0f;
}

}  // namespace

void AcidVoice::init(int sample_rate)
{
    *this = AcidVoice();
    setSampleRate(sample_rate);
}

void AcidVoice::setSampleRate(int sample_rate)
{
    const int sr = sample_rate < 8000 ? 8000 : (sample_rate > 192000 ? 192000 : sample_rate);
    fs_ = (float)sr;
    inv_fs_ = 1.0f / fs_;
    const float ctl = (float)kCtl;
    att_c_ = attackCoef(kVegAttackMs, fs_);
    dec_c_ = 1.0f - expf(-1.0f / (kVegDecayTau * fs_));
    rel_c_ = 1.0f - expf(-1.0f / (kVegReleaseTau * fs_));
    slide_a_ = expf(-ctl / (kSlideTau * fs_));
    knob_c_ = 1.0f - expf(-ctl / (kKnobTau * fs_));
    acc_ca_ = 1.0f - expf(-ctl / (kAccChargeTau * fs_));
    acc_rx_lo_ = ctl / (kAccDischargeLo * fs_);
    acc_rx_hi_ = ctl / (kAccDischargeHi * fs_);
    accmeg_x_ = ctl / (kAccMegTau * fs_);
    decay_in_ = -1.0f;   // meg_x_ follows on the next render()
    const float gh = tanf(kPi * kFbHpHz / fs_);
    gh_ = gh / (1.0f + gh);
    bh_ = 1.0f - gh_;
    rnd_c_ = 1.0f - expf(-kTwoPi * kSquareRoundHz / fs_);
    boost_c_ = 1.0f - expf(-1.0f / (kBoostTau * fs_));
    dc_r_ = 1.0f - kTwoPi * kDcHz / fs_;
    fade_step_ = 1.0f / (kPanicS * fs_);
    wave_step_ = 1.0f / (kWaveXfadeS * fs_);
    if (veg_stage_ == VEG_ATTACK) veg_c_ = att_c_;
    else if (veg_stage_ == VEG_DECAY) veg_c_ = dec_c_;
    else if (veg_stage_ == VEG_RELEASE) veg_c_ = rel_c_;
    jump_ = true;   // new phase increment without a ramp
    ctl_left_ = 0;
}

ORBIT_HOT bool AcidVoice::schedule(const AcidEvent& e)
{
    if (nq_ >= kQueue) return false;
    q_[nq_++] = e;
    return true;
}

ORBIT_HOT void AcidVoice::render(float* out, int n, const AcidParams& p)
{
    if (n <= 0 || !out) {
        nq_ = 0;
        return;
    }
    t_tune_ = sanitize(p.tune, -24.0f, 24.0f);
    t_cut_ = sanitize(p.cutoff, 0.0f, 1.0f);
    t_reso_ = sanitize(p.reso, 0.0f, 1.0f);
    t_env_ = sanitize(p.envmod, 0.0f, 1.0f);
    t_acc_ = sanitize(p.accent, 0.0f, 1.0f);
    t_drive_ = sanitize(p.drive, 0.0f, 1.0f);
    t_level_ = sanitize(p.level, 0.0f, 8.0f);
    wave_t_ = p.wave ? 1.0f : 0.0f;
    const float decay = sanitize(p.decay, 0.0f, 1.0f);
    if (decay != decay_in_) {
        decay_in_ = decay;
        meg_x_ = (float)kCtl / (kMegTauMin * fastExp2(decay * kMegTauLog2) * fs_);
    }
    if (idle_) {
        // nothing audible: knobs jump to their values (no glide at the next note start)
        tune_ = t_tune_;
        cut_ = t_cut_;
        reso_ = t_reso_;
        env_ = t_env_;
        acck_ = t_acc_;
        drive_ = t_drive_;
        level_ = t_level_;
        wave_m_ = wave_end_ = wave_t_;
        gout_ = level_;
        if (nq_ == 0) return;
    }

    // stable insertion sort by offset (equal offsets keep the queue order)
    for (int i = 1; i < nq_; ++i) {
        const AcidEvent e = q_[i];
        int j = i - 1;
        while (j >= 0 && q_[j].offset > e.offset) {
            q_[j + 1] = q_[j];
            --j;
        }
        q_[j + 1] = e;
    }
    int pos = 0;
    for (int i = 0; i < nq_; ++i) {
        const int off = q_[i].offset >= (uint32_t)n ? n - 1 : (int)q_[i].offset;
        if (off > pos) {
            run(out, pos, off);
            pos = off;
        }
        applyEvent(q_[i]);
    }
    nq_ = 0;
    if (pos < n) run(out, pos, n);

    s1_ = flushDenormal(s1_);
    s2_ = flushDenormal(s2_);
    s3_ = flushDenormal(s3_);
    s4_ = flushDenormal(s4_);
    sh_ = flushDenormal(sh_);
    rnd_ = flushDenormal(rnd_);
    dcx_ = flushDenormal(dcx_);
    dcy_ = flushDenormal(dcy_);
}

ORBIT_HOT void AcidVoice::applyEvent(const AcidEvent& e)
{
    switch (e.type) {
    case AcidEvtType::NoteOn:
        if (panic_) {   // starts once the fade is done
            pending_ = true;
            pend_note_ = e.note;
            pend_acc_ = e.accent;
            pend_gate_ = e.gate_frames;
            return;
        }
        noteOn(e.note, e.accent != 0, e.legato != 0, e.gate_frames);
        break;
    case AcidEvtType::NoteOff:
        if (panic_) {
            if (pending_ && (e.note == pend_note_ || e.note == kAcidAnyNote)) pending_ = false;
            return;
        }
        if (gate_ && (e.note == note_ || e.note == kAcidAnyNote)) closeGate();
        break;
    case AcidEvtType::Panic:
        pending_ = false;
        if (idle_) return;
        closeGate();
        panic_ = true;
        ctl_left_ = 0;   // the fade starts at this frame
        break;
    }
}

ORBIT_HOT void AcidVoice::noteOn(uint8_t note, bool accent, bool legato, uint32_t gate_frames)
{
    const bool slide = legato && gate_ && !idle_ && veg_stage_ != VEG_DONE;
    note_ = note;
    semis_t_ = (float)note;
    accented_ = accent;
    gate_ = true;
    gate_timed_ = gate_frames > 0;
    gate_left_ = gate_frames;
    if (!slide) {
        semis_ = semis_t_;
        jump_ = true;
        meg_ = 1.0f;
        veg_stage_ = VEG_ATTACK;   // from the current level: no click on a retrigger
        veg_c_ = att_c_;
        veg_t_ = 1.3f;
        veg_lo_ = -1.0f;
        veg_hi_ = 1.0f;
    }
    idle_ = false;
    ctl_left_ = 0;   // new control period at this frame
}

ORBIT_HOT void AcidVoice::closeGate()
{
    gate_ = false;
    gate_timed_ = false;
    gate_left_ = 0;
    if (veg_stage_ == VEG_DONE) return;
    veg_stage_ = VEG_RELEASE;
    veg_c_ = rel_c_;
    veg_t_ = 0.0f;
    veg_lo_ = kVegFloor;
    veg_hi_ = 2.0f;
    if (veg_x_ < kVegFloor) vegSegmentEnd();
}

ORBIT_HOT void AcidVoice::vegSegmentEnd()
{
    if (veg_stage_ == VEG_ATTACK) {
        veg_x_ = 1.0f;
        veg_stage_ = VEG_DECAY;
        veg_c_ = dec_c_;
        veg_t_ = 0.0f;
        veg_lo_ = kVegFloor;
        veg_hi_ = 2.0f;
        return;
    }
    veg_stage_ = VEG_DONE;
    veg_x_ = 0.0f;
    veg_c_ = 0.0f;
    veg_t_ = 0.0f;
    veg_lo_ = -1.0f;
    veg_hi_ = 2.0f;
}

ORBIT_HOT void AcidVoice::goIdle()
{
    idle_ = true;
    gate_ = gate_timed_ = false;
    gate_left_ = 0;
    panic_ = false;
    fade_ = 1.0f;
    accented_ = false;
    veg_stage_ = VEG_DONE;
    veg_x_ = veg_c_ = veg_t_ = 0.0f;
    veg_lo_ = -1.0f;
    veg_hi_ = 2.0f;
    meg_ = acc_ = 0.0f;
    boost_ = boost_t_ = 1.0f;
    s1_ = s2_ = s3_ = s4_ = sh_ = 0.0f;
    rnd_ = dcx_ = dcy_ = 0.0f;
    ctl_left_ = 0;
    jump_ = true;
    gout_ = level_;
    dgout_ = 0.0f;
}

// Control rate: knob smoothing, envelopes, pitch, filter / drive / gain coefficients for the
// next kCtl samples (values are those at the end of the period).
ORBIT_HOT void AcidVoice::control()
{
    const float kc = knob_c_;
    follow(tune_, t_tune_, kc);
    follow(cut_, t_cut_, kc);
    follow(reso_, t_reso_, kc);
    follow(env_, t_env_, kc);
    follow(acck_, t_acc_, kc);
    follow(drive_, t_drive_, kc);
    follow(level_, t_level_, kc);

    // MEG and accent sweep (the accented MEG charges the RC through a "diode"; the RC always
    // discharges, slower at high resonance like the 303's shared pot)
    meg_ *= expNegSmall(accented_ ? accmeg_x_ : meg_x_);
    if (meg_ < 1.0e-6f) meg_ = 0.0f;
    const float ain = accented_ ? meg_ : 0.0f;
    if (ain > acc_) acc_ += acc_ca_ * (ain - acc_);
    acc_ *= expNegSmall(acc_rx_lo_ + reso_ * (acc_rx_hi_ - acc_rx_lo_));
    if (acc_ < 1.0e-6f) acc_ = 0.0f;

    // pitch: exponential glide in semitones, phase increment ramps across the period
    semis_ = semis_t_ + (semis_ - semis_t_) * slide_a_;
    if (absf(semis_ - semis_t_) < 1.0e-4f) semis_ = semis_t_;
    const float dt = clampf(440.0f * inv_fs_ * fastExp2((semis_ + tune_ - 69.0f) * (1.0f / 12.0f)), 1.0e-6f, 0.25f);
    if (jump_) {
        dt_ = dt;
        ddt_ = 0.0f;
        jump_ = false;
    } else {
        ddt_ = (dt - dt_) * (1.0f / (float)kCtl);
    }
    idt_ = 1.0f / dt;

    // ladder coefficients
    const float oct = kCutLog2 + cut_ * kCutRange + env_ * kEnvOct * meg_ + acck_ * (kAccOct * kAccNorm) * acc_;
    const float fcn = clampf(fastExp2(oct) * inv_fs_, kFcMinHz * inv_fs_, kFcMaxNorm);
    fc_hz_ = fcn * fs_;
    const float g = fastTanPi(fcn);
    g_ = g / (1.0f + g);
    b_ = 1.0f - g_;
    const float k = minf(kKCap, reso_ * kKMax * (1.0f + kKTrack * kFbHpHz / fc_hz_));
    fb_a_ = k * bh_;
    const float g2 = g_ * g_;
    const float inv = 1.0f / (1.0f + fb_a_ * g2 * g2);
    in_x_ = inv * kInGain;
    in_s_ = inv * fb_a_;
    makeup_ = kResComp * minf(k, kKMax);

    // accent gain target (smoothed per sample)
    boost_t_ = 1.0f + (accented_ ? acck_ * kAccAmp * meg_ : 0.0f);

    // drive: y + wet (softClip(y gain) comp - y); wet fades the clipper in over DRIVE 0..0.05
    dgain_ = 1.0f + 8.0f * drive_;
    dcomp_ = 0.075f + 0.853f / dgain_;   // fitted: the default line keeps its RMS within ~1 dB
    dwet_ = minf(1.0f, drive_ * 20.0f);

    // wave crossfade
    if (wave_m_ != wave_t_) {
        const float step = wave_step_ * (float)kCtl;
        wave_end_ = wave_m_ < wave_t_ ? minf(wave_t_, wave_m_ + step) : maxf(wave_t_, wave_m_ - step);
        dwave_ = (wave_end_ - wave_m_) * (1.0f / (float)kCtl);
    } else {
        wave_end_ = wave_m_;
        dwave_ = 0.0f;
    }

    // output gain ramp (LEVEL x panic fade)
    if (panic_) fade_ = maxf(0.0f, fade_ - fade_step_ * (float)kCtl);
    dgout_ = (level_ * fade_ - gout_) * (1.0f / (float)kCtl);
}

// Per sample: VCO -> ladder -> makeup -> drive -> DC blocker -> VCA -> gain, added to out.
// SQ = false: saw only; true: saw / square crossfade (wave_m_ = 1: square).
template <bool SQ>
ORBIT_HOT void AcidVoice::chunk(float* __restrict out, int m)
{
    const bool drive = dwet_ > 0.0f;
    float ph = ph_, dt = dt_;
    const float ddt = ddt_, idt = idt_;
    float s1 = s1_, s2 = s2_, s3 = s3_, s4 = s4_, sh = sh_;
    const float G = g_, B = b_, IX = in_x_, IS = in_s_, Gh = gh_, mk = makeup_;
    float rnd = rnd_;
    const float rc = rnd_c_;
    float wm = wave_m_;
    const float dwm = dwave_;
    float ex = veg_x_, ec = veg_c_, et = veg_t_, elo = veg_lo_, ehi = veg_hi_;
    float bst = boost_;
    const float bt = boost_t_, bc = boost_c_;
    const float dg = dgain_, dcm = dcomp_, dw = dwet_;
    float dcx = dcx_, dcy = dcy_;
    const float dcr = dc_r_;
    float go = gout_;
    const float dgo = dgout_;
    for (int i = 0; i < m; ++i) {
        ph += dt;
        if (ph >= 1.0f) ph -= 1.0f;
        const float b0 = polyBlep(ph, dt, idt);
        float x = ph + ph - 1.0f - b0;   // saw
        if (SQ) {
            float ph2 = ph + 0.5f;
            if (ph2 >= 1.0f) ph2 -= 1.0f;
            const float sq = (ph < 0.5f ? kSquareGain : -kSquareGain) + kSquareGain * (b0 - polyBlep(ph2, dt, idt));
            rnd += rc * (sq - rnd);   // rounded edges
            x += wm * (rnd - x);
            wm += dwm;
        }
        dt += ddt;

        // ladder: closed-form input of the linear loop (4 x TPT LP, highpass in the feedback),
        // soft clipped, then the four stages
        const float S = B * (((s1 * G + s2) * G + s3) * G + s4);
        const float u = softClip(IX * x - IS * (S - sh));
        float v = (u - s1) * G;
        const float y1 = v + s1;
        s1 = y1 + v;
        v = (y1 - s2) * G;
        const float y2 = v + s2;
        s2 = y2 + v;
        v = (y2 - s3) * G;
        const float y3 = v + s3;
        s3 = y3 + v;
        v = (y3 - s4) * G;
        const float y4 = v + s4;
        s4 = y4 + v;
        v = (y4 - sh) * Gh;   // resonance-loop highpass = y4 - lh
        const float lh = v + sh;
        sh = lh + v;

        // passband makeup above the loop highpass only (the lows were not attenuated), drive,
        // DC blocker, then the VCA (exact silence once the VEG ends)
        float y = (y4 + mk * (y4 - lh)) * kOutTrim;
        if (drive) y += dw * (softClip(y * dg) * dcm - y);
        const float d = y - dcx + dcr * dcy;
        dcx = y;
        dcy = d;

        ex += ec * (et - ex);
        if (ex > ehi || ex < elo) {
            veg_x_ = ex;
            vegSegmentEnd();
            ex = veg_x_;
            ec = veg_c_;
            et = veg_t_;
            elo = veg_lo_;
            ehi = veg_hi_;
        }
        bst += bc * (bt - bst);
        out[i] += d * ((ex * bst) * go);
        go += dgo;
    }
    ph_ = ph;
    dt_ = dt;
    s1_ = s1;
    s2_ = s2;
    s3_ = s3;
    s4_ = s4;
    sh_ = sh;
    rnd_ = rnd;
    if (SQ) wave_m_ = wm;
    veg_x_ = ex;
    veg_c_ = ec;
    veg_t_ = et;
    veg_lo_ = elo;
    veg_hi_ = ehi;
    boost_ = bst;
    dcx_ = dcx;
    dcy_ = dcy;
    gout_ = go;
}

// Renders [from, to) in pieces that end at control-period boundaries and at the gate end.
ORBIT_HOT void AcidVoice::run(float* out, int from, int to)
{
    int i = from;
    while (i < to) {
        if (idle_) return;
        if (ctl_left_ <= 0) {
            control();
            ctl_left_ = kCtl;
        }
        int m = to - i;
        if (m > ctl_left_) m = ctl_left_;
        if (gate_timed_ && (uint32_t)m > gate_left_) m = (int)gate_left_;
        if (dwave_ == 0.0f && wave_m_ <= 0.0f) chunk<false>(out + i, m);
        else chunk<true>(out + i, m);
        ctl_left_ -= m;
        i += m;
        if (gate_timed_) {
            gate_left_ -= (uint32_t)m;
            if (gate_left_ == 0) closeGate();
        }
        if (ctl_left_ > 0) continue;
        // period boundary (block-size independent): exact crossfade end, panic end, VEG end
        wave_m_ = wave_end_;
        if (panic_) {
            if (fade_ <= 0.0f) {
                goIdle();
                if (pending_) {
                    pending_ = false;
                    noteOn(pend_note_, pend_acc_ != 0, false, pend_gate_);
                }
            }
        } else if (veg_stage_ == VEG_DONE) {
            goIdle();   // the VCA is last: the output is exactly 0 from here on
        }
    }
}

}  // namespace orbit
