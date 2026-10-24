#pragma once
// ACID: monophonic TB-303-style bass voice.
//   VCO  PolyBLEP saw / square (one-pole rounded edges, 5 ms crossfade on a wave switch),
//        free-running phase. Slide: exponential glide in semitones (tau 65 ms) on a legato
//        NoteOn while the gate is open; other notes jump.
//   VCF  4-pole zero-delay-feedback ladder: four TPT one-poles and a 150 Hz one-pole highpass in
//        the resonance loop, solved in closed form for the ladder input, which then goes through
//        a rational soft clip (bounds the resonance; the filter stays stable at any setting).
//        The loop highpass keeps low cutoffs from booming; the feedback k = RESO 4 (1 + 150 / fc)
//        follows the oscillation threshold it raises, so RESO 1 sits at the edge above ~800 Hz.
//        Half of the passband loss above 150 Hz is made up after the ladder (x (1 + k / 2)).
//        Cutoff 90 Hz..4.5 kHz (exponential) + MEG (up to 4 octaves) + accent sweep (2.5),
//        clamped to 0.45 fs; coefficients at control rate (kCtl samples).
//   MEG  filter envelope: restarts at 1 on every non-legato NoteOn, exponential decay with
//        tau 0.15..2.5 s from DECAY (fixed 0.2 s on accented notes).
//   ACC  accent sweep: the accented MEG charges an RC through a "diode" (tau 40 ms), which
//        discharges with tau 0.15..0.35 s (longer at high RESO, as on the 303). Close accents
//        start from what is left, so a run of accents opens the filter further (the "wow"):
//        the 2nd accent of a 16th run sweeps ~15 % higher than the 1st. Accented notes are
//        also louder: VCA x (1 + ACCENT MEG), +6 dB at the note start with ACCENT = 1.
//   VEG  amp envelope: 2.5 ms attack from the current level (no click on a retrigger), tau 3 s
//        decay while the gate is open, tau 8 ms release; a legato NoteOn keeps it running.
//   OUT  ladder -> makeup -> drive (soft clip of x (1 + 8 DRIVE), level compensated, faded in
//        over DRIVE 0..0.05) -> 7 Hz DC blocker -> VCA -> LEVEL (+ panic fade), ADDED to out.
//        The VCA is last, so the output is exactly 0 once the release has ended.
// Events are applied at their frame offset inside render(). Knobs are read once per render()
// and followed with tau 15 ms at control rate (no zipper noise from block-rate steps).
// Audio task only: no allocation, no locks, no I/O, no libm in render() (only in init() and
// setSampleRate()), float only, deterministic: the output does not depend on the block sizes.
#ifndef ORBIT_HOT
#define ORBIT_HOT
#endif

#include <stdint.h>

namespace orbit {

struct AcidParams {          // UI/project values, read by the engine once per block
    float tune = 0.0f;       // semitones -12..+12 added to every note
    float cutoff = 0.45f;    // 0..1 knob
    float reso = 0.6f;       // 0..1 knob
    float envmod = 0.5f;     // 0..1 knob
    float decay = 0.4f;      // 0..1 knob (filter envelope decay)
    float accent = 0.6f;     // 0..1 knob (accent amount)
    float drive = 0.0f;      // 0..1 post-filter saturation ("distortion")
    float level = 1.0f;      // linear output gain (engine converts the dB knob)
    uint8_t wave = 0;        // 0 = saw, 1 = square
};

enum class AcidEvtType : uint8_t { NoteOn = 0, NoteOff, Panic };

// NoteOff closes the gate only when `note` is the sounding note (a NoteOff for an older,
// overlapped key is ignored, MIDI mono style); note = kAcidAnyNote closes it in any case.
constexpr uint8_t kAcidAnyNote = 0xFF;

struct AcidEvent {
    AcidEvtType type = AcidEvtType::NoteOn;
    uint8_t note = 36;        // MIDI note number (24..84 used)
    uint8_t accent = 0;       // 0/1
    uint8_t legato = 0;       // 1: slide into this note from the sounding one (no envelope retrigger, pitch glides)
    uint32_t offset = 0;      // frame inside the next render() call, 0..n-1
    uint32_t gate_frames = 0; // NoteOn: gate stays open this many frames after offset (0 = until NoteOff)
};

// Per block: schedule() the block's events (offset = frame inside this block), then render().
// While idle (active() false) render() returns at once, so it may be skipped; knob values read
// while idle are taken without smoothing.
class AcidVoice {
public:
    static constexpr int kQueue = 16;   // events per render() call
    static constexpr int kCtl = 16;     // control-rate interval (samples)

    void init(int sample_rate);         // resets all state
    void setSampleRate(int sample_rate);   // keeps the playing state, recomputes rate-dependent constants
    bool schedule(const AcidEvent& e);  // queue for the next render() (kQueue events); false when full
    // Renders n (1..256) mono samples and ADDS them to out[] (the engine mixes several sources).
    void render(float* out, int n, const AcidParams& p);
    bool active() const { return !idle_ || nq_ > 0; }   // something audible / envelopes not finished

    // Read-only state for meters, the UI and tests.
    bool gate() const { return gate_; }
    uint8_t note() const { return note_; }        // last NoteOn note (slide target)
    float ampEnv() const { return veg_x_; }       // VEG 0..1
    float filterEnv() const { return meg_; }      // MEG 0..1
    float accentSweep() const { return acc_; }    // accent RC 0..~1
    float cutoffHz() const { return fc_hz_; }     // filter cutoff of the current control period
    float pitchSemis() const { return semis_; }   // gliding pitch without TUNE (MIDI semitones)

private:
    void applyEvent(const AcidEvent& e);
    void noteOn(uint8_t note, bool accent, bool legato, uint32_t gate_frames);
    void closeGate();
    void goIdle();
    void control();
    void run(float* out, int from, int to);
    template <bool SQ>
    void chunk(float* out, int m);
    void vegSegmentEnd();

    // rate-dependent constants (setSampleRate)
    float fs_ = 48000.0f, inv_fs_ = 1.0f / 48000.0f;
    float att_c_ = 0.0f, dec_c_ = 0.0f, rel_c_ = 0.0f;   // VEG per-sample coefficients
    float slide_a_ = 0.0f;           // per kCtl
    float knob_c_ = 0.0f;            // per kCtl
    float acc_ca_ = 0.0f;            // accent RC charge, per kCtl
    float acc_rx_lo_ = 0.0f, acc_rx_hi_ = 0.0f;   // accent RC discharge exponent per kCtl (reso 0 / 1)
    float accmeg_x_ = 0.0f;          // MEG decay exponent per kCtl on accented notes
    float meg_x_ = 0.0f;             // MEG decay exponent per kCtl from DECAY (cached)
    float decay_in_ = -1.0f;         // DECAY value meg_x_ was computed for
    float gh_ = 0.0f, bh_ = 1.0f;    // resonance-loop highpass (TPT one-pole) G and 1 - G
    float rnd_c_ = 1.0f;             // square rounding lowpass
    float boost_c_ = 1.0f;           // accent gain smoothing (per sample)
    float dc_r_ = 0.999f;            // DC blocker pole
    float fade_step_ = 0.0f;         // panic fade per sample
    float wave_step_ = 0.0f;         // wave crossfade per sample

    // knob targets (render) and smoothed values (control rate)
    float t_tune_ = 0.0f, t_cut_ = 0.45f, t_reso_ = 0.6f, t_env_ = 0.5f, t_acc_ = 0.6f, t_drive_ = 0.0f,
          t_level_ = 1.0f;
    float tune_ = 0.0f, cut_ = 0.45f, reso_ = 0.6f, env_ = 0.5f, acck_ = 0.6f, drive_ = 0.0f, level_ = 1.0f;
    float wave_t_ = 0.0f;            // 0 = saw, 1 = square
    float wave_m_ = 0.0f;            // crossfade position
    float wave_end_ = 0.0f;          // crossfade position at the end of the control period

    // note / envelopes
    bool idle_ = true;
    bool gate_ = false, gate_timed_ = false;
    bool accented_ = false;
    bool jump_ = true;               // next control tick sets the pitch without a ramp
    bool panic_ = false;
    bool pending_ = false;           // NoteOn that arrived during a panic fade
    uint8_t note_ = 36;
    uint8_t veg_stage_ = 0;
    uint8_t pend_note_ = 36, pend_acc_ = 0;
    uint32_t pend_gate_ = 0;
    uint32_t gate_left_ = 0;
    int32_t ctl_left_ = 0;
    float semis_ = 36.0f, semis_t_ = 36.0f;
    float meg_ = 0.0f, acc_ = 0.0f;
    float veg_x_ = 0.0f, veg_c_ = 0.0f, veg_t_ = 0.0f, veg_lo_ = -1.0f, veg_hi_ = 2.0f;
    float boost_ = 1.0f, boost_t_ = 1.0f;
    float fade_ = 1.0f;
    float fc_hz_ = 0.0f;

    // per control period (control())
    float dt_ = 0.0f, ddt_ = 0.0f, idt_ = 0.0f;   // phase increment, its ramp, 1 / dt
    float g_ = 0.0f, b_ = 1.0f;      // ladder stage G and 1 - G
    float fb_a_ = 0.0f;              // A = k (1 - G_hp)
    float in_x_ = 0.0f, in_s_ = 0.0f;   // ladder input = in_x VCO - in_s (S - s_hp), / (1 + A G^4) folded in
    float makeup_ = 0.0f;            // passband makeup above the loop highpass
    float dgain_ = 1.0f, dcomp_ = 1.0f, dwet_ = 0.0f;   // drive
    float gout_ = 0.0f, dgout_ = 0.0f;   // output gain ramp (LEVEL * panic fade)
    float dwave_ = 0.0f;             // crossfade ramp per sample

    // audio states
    float ph_ = 0.0f;
    float s1_ = 0.0f, s2_ = 0.0f, s3_ = 0.0f, s4_ = 0.0f, sh_ = 0.0f;
    float rnd_ = 0.0f;
    float dcx_ = 0.0f, dcy_ = 0.0f;

    AcidEvent q_[kQueue];
    int nq_ = 0;
};

}  // namespace orbit
