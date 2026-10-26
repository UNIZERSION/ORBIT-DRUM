#pragma once
// SYNTH: 4-voice virtual-analog synthesizer in the manner of a SYSTEM-1 (our own DSP; user-facing
// name "SYNTH"). Per voice:
//   OSC1 + OSC2 + SUB + NOISE -> mixer -> VCF -> DRIVE -> DC blocker -> VCA -> pan, ADDED to outL / outR.
//   OSC   free-running phase oscillators (phases are not reset by a note), band-limited with a
//         one-sample-delayed PolyBLEP (steps) / PolyBLAMP (corners): every discontinuity, a hard-sync
//         reset included, is corrected on both sides at its exact sub-sample position, so sync and
//         cross-mod stay clean. Waves SAW, SQUARE (pulse), TRIANGLE; COLOR (0..1) per wave:
//           SAW       a second saw is mixed in, the two detuned symmetrically by up to +-20 cents
//                     (COLOR 1); the partner's level reaches the main saw's at COLOR 0.25 (a detuned pair)
//           SQUARE    pulse width 50 % (COLOR 0) .. 3 % (COLOR 1), DC-free; LFO -> PW adds +-45 %
//           TRIANGLE  sine wavefolder sin(pi/2 g tri) / norm with g = 4 COLOR: 0 = triangle,
//                     0.25 = sine, 0.5 = one fold (two humps), 1 = two folds; smooth, so only the
//                     triangle corners need PolyBLAMP
//         OSC2: COARSE -24..+24 semitones, FINE +-50 cents, + filter envelope (OSC2 ENV +-24 semitones,
//         for sync sweeps). SYNC: OSC2 restarts with every OSC1 cycle. RING: OSC1 x OSC2 replaces
//         OSC2 in the mix. XMOD: OSC2 -> OSC1 linear FM, dt1 x (1 + 2 XMOD osc2) (no through-zero,
//         one sample delay). SUB: square 1 or 2 octaves under OSC1 (it toggles on OSC1's cycles, so it
//         follows XMOD). NOISE: white, xorshift per voice.
//   VCF   LPF -24: 4-pole zero-delay-feedback ladder (four TPT one-poles, the loop solved in closed
//         form; the solved input goes through a cubic soft clip, which bounds the resonance: k = 4.3
//         RESO, self-oscillation from RESO ~0.93, never unbounded). Half of the passband loss is
//         made up (x (1 + k / 2)). LPF -12 / HPF -12: TPT state variable filter (Simper), damping
//         2 .. 0.06 (Q 0.5..17: rings, does not self-oscillate), soft clipped input, output trimmed
//         by (k / 1.41)^0.4 at high Q. Cutoff 20 Hz..20.5 kHz exponential (CUTOFF 0..1)
//         + KEY FOLLOW (1 = 1 oct / oct around C4 = 60) + filter env (+-8 oct at ENV +-1)
//         + LFO (+-4 oct) + velocity (VEL > CUTOFF: velocity 127 = as set, 0 = -3 oct), clamped to
//         16 Hz..0.45 fs. Coefficients per control period (kCtl samples).
//   ENV   two ADSRs (filter, amp). Overshoot-target exponential segments that end exactly at their
//         time: attack aims at 1.3 and stops at 1 (concave, RC-like), decay / release fall
//         exponentially and reach the sustain / zero at T. Attack starts from the current level, so
//         a retrigger does not click. Amp env per sample, filter env per control period.
//   LFO   per voice (each voice has its own fade-in and, with KEY TRIG, its own phase and random
//         values); with KEY TRIG off every voice reads one free-running global LFO (all voices move
//         together, classic poly behaviour). Waves TRIANGLE (starts at 0, rising), SAW UP, SAW DOWN,
//         SQUARE, SAMPLE & HOLD, SMOOTH RANDOM (smoothstep between random values); rate 0.05..30 Hz
//         or a tempo division (lfo_sync, from the bpm passed to render()); FADE: depth ramps 0 -> 1
//         over lfo_fade seconds after each (non-legato) note start. Depths: pitch (12 semitones x
//         depth^2, both OSCs), cutoff (+-4 oct), amp (tremolo: gain 1 .. 1 - depth), pulse width.
//   MODES POLY: 4 voices; a NoteOn for a sounding note retriggers that voice; otherwise a free voice
//         (the one released longest ago), else the oldest released voice, else the oldest held one
//         is stolen. MONO: voice 0, last-note priority (a 16-note stack; releasing the top note
//         returns to the previous held one). UNISON: as MONO with all 4 voices on the note, detuned
//         +-40 cents x UNI DETUNE (outer voices; inner +-1/3) and panned +-SPREAD; each voice -6 dB.
//         LEGATO (MONO / UNISON): overlapping notes change pitch without retriggering the envelopes
//         (gliding when GLIDE > 0) and only they glide ("fingered" portamento); a note after all keys
//         were up jumps and retriggers. Without LEGATO every note retriggers and, with GLIDE > 0,
//         glides from the voice's last pitch (POLY too: each voice from its own last note).
//         GLIDE is the time constant of the exponential glide in semitones.
//   OUT   per-voice DRIVE (cubic soft clip x (1 + 5 DRIVE), level compensated, faded in over
//         DRIVE 0..0.05) -> 5 Hz DC blocker -> VCA -> LEVEL -> pan (constant power, centre = unity;
//         POLY spreads voice slots over +-0.35 SPREAD, UNISON over +-SPREAD, MONO is centred).
//         The VCA is last: the output is exactly 0 once every amp envelope has ended.
// Events (noteOn / noteOff / allNotesOff / panic) are queued and applied at their frame offset
// inside the next render(). Knobs are read once per render() and followed with tau 15 ms at control
// rate; switches, waves and times take effect at once (times: coefficients recomputed on change).
// Control work (knob smoothing, envelopes of the filter, LFO, pitch, filter coefficients, gains)
// runs every kCtl samples on one grid that starts at the first event after the synth was idle;
// pitch, pulse width and gains ramp linearly across each control period.
// Audio task only: no allocation, no locks, no I/O, no libm in render() (only in init() and
// setSampleRate()), float only, deterministic: the output does not depend on the block sizes.
// Cost: each control period renders every active voice in passes over 16-sample stack buffers,
// so each loop's state fits the FPU registers: OSC1 and OSC2 in loops specialized for their wave
// (SYNC + XMOD together need both per sample: one generic loop, kept in flash as it is rare),
// the mixer, the filter (LPF -24 voices two at a time: their ladder recursions interleave and hide
// the FPU latency of the in-order core), then drive / DC / VCA / pan. Only the per-block and
// per-sample path is ORBIT_HOT (~12 KB IRAM); note handling and the change-only coefficient math
// (times, COLOR, RESO, pans) run from flash. ~100 RISC-V instructions per voice and sample for
// two saws through LPF -24 (GCC 14 -O2); the real figure is Telemetry::cpu on the Tab5.
#ifndef ORBIT_HOT
#define ORBIT_HOT
#endif

#include <stdint.h>

namespace orbit {

enum SynthWave : uint8_t { kSynthSaw = 0, kSynthSquare, kSynthTriangle, kSynthWaveCount };
enum SynthFilterType : uint8_t { kSynthLp24 = 0, kSynthLp12, kSynthHp12, kSynthFilterCount };
enum SynthLfoWave : uint8_t {
    kSynthLfoTriangle = 0,
    kSynthLfoSawUp,
    kSynthLfoSawDown,
    kSynthLfoSquare,
    kSynthLfoSampleHold,
    kSynthLfoSmoothRandom,
    kSynthLfoWaveCount
};
enum SynthVoiceMode : uint8_t { kSynthPoly = 0, kSynthMono, kSynthUnison, kSynthModeCount };

constexpr int kSynthVoices = 4;
constexpr int kSynthLfoDivs = 13;   // tempo divisions for SynthParams::lfo_sync (1..13)

// All knobs, read by render() once per block. Continuous knobs are 0..1 unless a range is given;
// out-of-range values (and NaN) are clamped. Times are in seconds.
struct SynthParams {
    // OSC 1
    uint8_t osc1_wave = kSynthSaw;   // SynthWave
    float osc1_color = 0.0f;         // 0..1, see COLOR above
    float osc1_level = 1.0f;         // 0..1
    // OSC 2
    uint8_t osc2_wave = kSynthSaw;
    float osc2_color = 0.0f;
    float osc2_level = 0.0f;
    float osc2_coarse = 0.0f;        // semitones -24..+24 (fractions allowed)
    float osc2_fine = 0.0f;          // cents -50..+50
    float osc2_env = 0.0f;           // -1..1: filter envelope -> OSC2 pitch, +-24 semitones
    uint8_t sync = 0;                // 1: OSC2 hard-synced to OSC1
    uint8_t ring = 0;                // 1: OSC1 x OSC2 replaces OSC2 in the mix (at osc2_level)
    float xmod = 0.0f;               // 0..1: OSC2 -> OSC1 frequency (linear FM)
    // SUB / NOISE
    uint8_t sub_oct = 1;             // 1 = one octave under OSC1, 2 = two octaves
    float sub_level = 0.0f;          // 0..1
    float noise_level = 0.0f;        // 0..1
    // PITCH
    float tune = 0.0f;               // semitones -24..+24, added to every note
    float bend = 0.0f;               // semitones -24..+24 (pitch bend; the range is the caller's)
    // FILTER
    uint8_t filter_type = kSynthLp24;   // SynthFilterType
    float cutoff = 0.6f;             // 0..1 -> 20 Hz .. 20.5 kHz exponential (synthCutoffHz)
    float reso = 0.15f;              // 0..1
    float key_follow = 0.5f;         // 0..1
    float env_amt = 0.3f;            // -1..1 filter envelope depth (+-8 octaves)
    float vel_cutoff = 0.0f;         // 0..1 velocity -> cutoff (velocity 0: -3 oct at 1)
    float f_attack = 0.003f;         // filter envelope, s (0..20)
    float f_decay = 0.4f;            // s (0.001..20)
    float f_sustain = 0.3f;          // 0..1
    float f_release = 0.3f;          // s (0.001..20)
    // AMP
    float a_attack = 0.003f;         // s (0.0005..20)
    float a_decay = 0.5f;            // s (0.001..20)
    float a_sustain = 0.8f;          // 0..1
    float a_release = 0.25f;         // s (0.001..20)
    float vel_amp = 0.5f;            // 0..1 velocity -> amp (gain 1 - vel_amp (1 - velocity / 127))
    // LFO
    uint8_t lfo_wave = kSynthLfoTriangle;   // SynthLfoWave
    float lfo_rate = 5.0f;           // Hz 0.05..30 (lfo_sync = 0)
    uint8_t lfo_sync = 0;            // 0 = free (lfo_rate), 1..kSynthLfoDivs = tempo division
    float lfo_fade = 0.0f;           // s (0..20): depth fade-in after the note start
    uint8_t lfo_key_trig = 0;        // 1: per-voice LFO restarted by each note
    float lfo_pitch = 0.0f;          // 0..1 -> 12 x depth^2 semitones (peak)
    float lfo_cutoff = 0.0f;         // 0..1 -> +-4 octaves
    float lfo_amp = 0.0f;            // 0..1 tremolo depth
    float lfo_pw = 0.0f;             // 0..1 -> pulse width +-45 % (SQUARE oscillators)
    // VOICES
    uint8_t voice_mode = kSynthPoly;   // SynthVoiceMode (a change releases every voice)
    uint8_t legato = 0;              // MONO / UNISON: overlapping notes slide without retrigger
    float glide = 0.0f;              // s (0..5): glide time constant, 0 = off
    float uni_detune = 0.3f;         // 0..1 -> +-40 cents (UNISON outer voices)
    float spread = 0.5f;             // 0..1 stereo spread (UNISON +-1, POLY +-0.35)
    // OUTPUT
    float drive = 0.0f;              // 0..1 per-voice saturation
    float level = 0.7f;              // linear master gain 0..4 (the engine converts a dB knob)
};

struct SynthPreset {
    const char* name;   // user-facing (UPPER CASE, no brand names)
    SynthParams p;
};

// Factory patches (constant table, 8 entries); *count receives the number of entries.
const SynthPreset* synthPresets(int* count);

// Display helpers (UI task; no libm).
float synthCutoffHz(float cutoff01);        // CUTOFF knob -> Hz (before key follow / envelope)
float synthLfoDivBeats(int div);            // beats per LFO cycle for lfo_sync 1..kSynthLfoDivs, else 0
const char* synthLfoDivName(int div);       // "1 BAR", "1/16T", ...; "FREE" for 0 / out of range

// Per block: queue the block's note events (frame offset inside the next render()), then render().
// While idle (active() false) render() returns at once; it may also be skipped then (the control
// grid restarts at the next note either way).
class Synth {
public:
    static constexpr int kQueue = 32;   // events per render() call
    static constexpr int kCtl = 16;     // control-rate interval (samples)
    static constexpr int kHeld = 16;    // MONO / UNISON note stack

    void init(int sample_rate);              // resets everything (may use libm)
    void setSampleRate(int sample_rate);     // keeps the playing state, recomputes rate constants
    // Queue an event for the next render(); false when the queue is full (event dropped).
    // velocity 1..127 (0 = NoteOff, MIDI style); frame_offset 0..n-1 (later frames clamp to n-1).
    bool noteOn(uint8_t note, uint8_t velocity, int frame_offset);
    bool noteOff(uint8_t note, int frame_offset);
    bool allNotesOff(int frame_offset);      // release every voice, clear the note stack
    // Silence now: a 4 ms fade from the start of the next render(), then every voice is reset.
    // Notes started during the fade play once it is over.
    void panic();
    // Renders n (1..256 typical, any n >= 1) stereo frames and ADDS them to outL / outR.
    // bpm (20..400) is used by a tempo-synced LFO only.
    void render(float* outL, float* outR, int n, const SynthParams& p, float bpm);

    int activeVoices() const;                // voices with a running amp envelope
    bool active() const { return activeVoices() > 0 || nq_ > 0 || panic_; }

    // Read-only state for meters, the UI and tests (v = 0..kSynthVoices-1).
    bool voiceActive(int v) const { return okv(v) && v_[v].active; }
    bool voiceGate(int v) const { return okv(v) && v_[v].gate; }
    int voiceNote(int v) const { return okv(v) ? v_[v].note : -1; }
    float voiceAmpEnv(int v) const { return okv(v) ? v_[v].ax : 0.0f; }
    float voiceFilterEnv(int v) const { return okv(v) ? v_[v].fx : 0.0f; }
    float voiceCutoffHz(int v) const { return okv(v) ? v_[v].fc_hz : 0.0f; }   // current control period
    float voicePitch(int v) const { return okv(v) ? v_[v].semis : 0.0f; }      // gliding note (MIDI semitones)
    float voiceLfo(int v) const { return okv(v) ? v_[v].lfo : 0.0f; }          // LFO x fade, -1..1

private:
    enum EvType : uint8_t { EV_ON = 0, EV_OFF, EV_ALL_OFF, EV_PANIC };
    struct Event {
        uint8_t type = EV_ON, note = 60, vel = 100;
        uint32_t offset = 0;
    };
    struct Osc {
        float ph = 0.0f, phb = 0.0f;   // phase, detuned partner phase (SAW COLOR)
        float y1 = 0.0f;               // pending (one sample late) output with its corrections
        uint8_t hi = 1;                // SQUARE: high half; TRIANGLE: rising half
    };
    struct Voice {
        uint8_t active = 0, gate = 0, note = 60, has_pitch = 0;
        uint8_t ft = 0;                // filter type the coefficients c0..c4 are for
        uint8_t astage = 0, fstage = 0;
        uint8_t sub_hi = 1, sub_cnt = 0;
        uint32_t on_age = 0, off_age = 0;
        float vel = 0.8f;              // 0..1
        float semis = 60.0f, semis_t = 60.0f;
        // amp envelope (per sample): x += c (t - x), segment ends when x leaves [lo, hi]
        float ax = 0.0f, ac = 0.0f, at = 0.0f, alo = -1.0f, ahi = 2.0f;
        // filter envelope (per control period)
        float fx = 0.0f, ftgt = 0.0f;
        // LFO (own phase / random values used with KEY TRIG)
        float lph = 0.0f, lr0 = 0.0f, lr1 = 0.0f, lfade = 1.0f, lfo = 0.0f;
        uint32_t lrng = 1;
        // per control period
        float dt1 = 0.0f, ddt1 = 0.0f, dt2 = 0.0f, ddt2 = 0.0f;
        float pw1 = 0.5f, dpw1 = 0.0f, pw2 = 0.5f, dpw2 = 0.0f;
        float c0 = 0.0f, c1 = 0.0f, c2 = 0.0f, c3 = 0.0f, c4 = 0.0f;   // filter coefficients
        float gl = 0.0f, gr = 0.0f, dgl = 0.0f, dgr = 0.0f;           // VCA x level x pan ramps
        float fc_hz = 0.0f;
        // audio state
        Osc o1, o2;
        float sub_y1 = 0.0f, o2_last = 0.0f;
        uint32_t nrng = 1;
        float s1 = 0.0f, s2 = 0.0f, s3 = 0.0f, s4 = 0.0f;   // filter (SVF: s1 band, s2 low)
        float dcx = 0.0f, dcy = 0.0f;
    };
    struct Held {
        uint8_t note, vel;
    };
    // smoothed knobs
    enum Knob : uint8_t {
        K_COLOR1 = 0, K_LEVEL1, K_COLOR2, K_LEVEL2, K_COARSE, K_FINE, K_O2ENV, K_XMOD, K_SUB, K_NOISE,
        K_TUNE, K_BEND, K_CUTOFF, K_RESO, K_KEYF, K_ENV, K_VELCUT, K_FSUS, K_ASUS, K_VELAMP,
        K_LPITCH, K_LCUT, K_LAMP, K_LPW, K_DETUNE, K_SPREAD, K_DRIVE, K_LEVEL, K_COUNT
    };

    static bool okv(int v) { return v >= 0 && v < kSynthVoices; }
    bool queue(uint8_t type, uint8_t note, uint8_t vel, int offset);
    void readParams(const SynthParams& p, float bpm);
    void applyEvent(const Event& e);
    void polyOn(uint8_t note, uint8_t vel);
    void monoOn(uint8_t note, uint8_t vel);
    void monoOff(uint8_t note);
    void noteOffNow(uint8_t note);
    void startVoice(int idx, uint8_t note, uint8_t vel, bool glide);
    void slideVoice(int idx, uint8_t note);
    void releaseVoice(Voice& v);
    void releaseAll();
    void killAll();
    void freeVoice(Voice& v);
    void ampSegmentEnd(Voice& v);
    void ampEnterDecay(Voice& v);
    void ampDecayTarget(Voice& v);
    void ampFinish(Voice& v);
    void timeChanged(int i, float t);
    void derive();
    void deriveSlow();
    void control();
    void voiceControl(int idx, bool advance, bool jump_pitch, bool jump_gain);
    void run(float* outL, float* outR, int from, int to);
    void renderChunk(float* outL, float* outR, int m);
    void oscillators(Voice& v, float* x, int m);
    template <uint8_t W, bool XM>
    void osc1Pass(Voice& v, float* a, float* wr, const float* b, float o2_prev, int m);
    template <uint8_t W>
    void osc2Pass(Voice& v, float* b, const float* wr, int m);
    void mixPass(Voice& v, const float* a, const float* b, const float* wr, float* x, int m);
    void oscCoupled(Voice& v, float* x, int m);
    void ladder2(Voice& a, float* xa, Voice& b, float* xb, int m);
    void ladder1(Voice& v, float* x, int m);
    void svf(Voice& v, float* x, int m);
    void outPass(Voice& v, const float* y, float* outL, float* outR, int m);
    bool anyActive() const;

    // rate-dependent constants
    float fs_ = 48000.0f, inv_fs_ = 1.0f / 48000.0f;
    float knob_c_ = 0.0f, dc_r_ = 0.999f, fade_step_ = 0.0f;

    // knob targets (render) and smoothed values (control rate)
    float kt_[K_COUNT] = {};
    float ks_[K_COUNT] = {};
    float kd_[K_COUNT] = {};   // smoothed values derive() last used (change detection)
    bool derived_ = false;

    // switches / discrete values (render)
    uint8_t w1_ = kSynthSaw, w2_ = kSynthSaw, ft_ = kSynthLp24, mode_ = kSynthPoly;
    uint8_t sync_ = 0, ring_ = 0, subdiv_ = 1, lfo_wave_ = kSynthLfoTriangle, lfo_key_ = 0, legato_ = 0;
    // time-derived coefficients (recomputed in render() when a time changes)
    float t_in_[8] = {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
    float att_c_ = 1.0f, dec_c_ = 1.0f, rel_c_ = 1.0f;       // amp env, per sample
    float fatt_c_ = 1.0f, fdec_c_ = 1.0f, frel_c_ = 1.0f;    // filter env, per kCtl
    float glide_c_ = 1.0f;                                   // per kCtl (1 = no glide)
    float lfo_inc_ = 0.0f, fade_inc_ = 1.0f;                 // per kCtl

    // derived per control period (derive())
    float ra1_ = 1.0f, ra2_ = 1.0f, rb1_ = 1.0f, rb2_ = 1.0f, lb1_ = 0.0f, lb2_ = 0.0f;
    float pwb1_ = 0.5f, pwb2_ = 0.5f;
    float fg1_ = 0.0f, fn1_ = 1.0f, fg2_ = 0.0f, fn2_ = 1.0f;   // triangle fold gain / normalization
    float norm1_ = 1.0f, norm2_ = 1.0f;                         // SAW pair level compensation
    float l1_ = 1.0f, l2_ = 0.0f, ls_ = 0.0f, ln_ = 0.0f, xm_ = 0.0f;
    float cut_l2_ = 0.0f, klad_ = 0.0f, mklad_ = 1.0f, ksvf_ = 2.0f, svfcomp_ = 1.0f;
    float dg_ = 1.0f, dcm_ = 1.0f, dw_ = 0.0f;
    float lpitch_ = 0.0f, out_gain_ = 1.0f;
    float uni_[kSynthVoices] = {}, panl_[kSynthVoices] = {}, panr_[kSynthVoices] = {};
    uint8_t dmode_ = 0xFF, dw1_ = 0xFF, dw2_ = 0xFF;

    // global LFO (KEY TRIG off)
    float glph_ = 0.0f, glr0_ = 0.0f, glr1_ = 0.0f, glfo_ = 0.0f;
    uint32_t grng_ = 0x2545F491u;

    // voices, note stack, events
    Voice v_[kSynthVoices];
    Held held_[kHeld] = {};
    int nheld_ = 0;
    uint32_t age_ = 0;
    int32_t ctl_left_ = 0;
    bool panic_ = false;
    float fade_ = 1.0f;
    Held pend_[kSynthVoices] = {};   // NoteOns that arrived during a panic fade
    int npend_ = 0;
    Event q_[kQueue];
    int nq_ = 0;
    // readParams() input of the last render(): unchanged knobs skip it (it runs from flash on the Tab5)
    SynthParams last_p_;
    float last_bpm_ = 0.0f;
    bool pvalid_ = false;
};

}  // namespace orbit
