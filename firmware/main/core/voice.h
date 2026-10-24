#pragma once
// Sampler voice: int16 playback with 4-point Hermite, region / reverse / loop (crossfaded seam),
// AHDSR amp envelope, Simper SVF with drive and envelope, pitch envelope, LFO, crush and
// downsample. Rendering adds into block buffers; control-rate work every kCtlBlock samples.
// Audio task only: no allocation, no locks, no I/O.
#ifndef ORBIT_HOT
#define ORBIT_HOT
#endif

#include <stdint.h>
#include "config.h"
#include "dsp/env.h"
#include "dsp/lfo.h"
#include "dsp/svf.h"
#include "project.h"
#include "sample.h"

namespace orbit {

constexpr int kGhostVoices = 8;                       // extra slots for click-free stealing
constexpr int kVoicePool = kVoices + kGhostVoices;
constexpr int kCtlBlock = 16;                         // control-rate interval (samples)

// Per-pad values derived from Project::pad[] (only when they change). Voices read only this.
struct PadCache {
    float start = 0.0f, end = 1.0f;      // region fractions, start < end
    float loop = 0.0f;                   // loop start, fraction of the region
    float fade_in_s = 0.0f, fade_out_s = 0.0f;
    float gain = 1.0f;                   // PP_GAIN * PP_VOLUME, linear
    dsp::AmpEnvCoefs env;
    float vel_amp = 0.8f;
    float cutoff_norm = 0.4f;            // cutoff / fs
    float cutoff_l2 = -1.3f;             // log2(cutoff_norm)
    float k = 1.8f;                      // SVF damping
    float drive_pre = 0.0f, drive_post = 1.0f;   // drive_pre 0 = off (int16 scale folded in)
    float mix_lp = 1.0f, mix_bp = 0.0f, mix_hp = 0.0f;
    float fenv_oct = 0.0f;               // filter envelope depth in octaves
    float fdec_a = 0.0f;                 // DecayEnv factor per kCtlBlock samples
    float tune = 0.0f;                   // semitones (TUNE + FINE)
    float penv = 0.0f;                   // pitch envelope depth, semitones
    float pdec_a = 0.0f;
    float crush_q = 0.0f, crush_step = 1.0f;     // crush_q 0 = off (int16 domain)
    int32_t downsample = 1;              // 1 = off
    float lfo_inc = 0.0f;                // cycles per sample
    float lfo_depth = 0.0f;
    float lfo_fade_inv = 0.0f;           // 1 / fade samples, 0 = no fade-in
    float pan = 0.0f;
    float send = 0.0f;
    uint8_t filter_on = 1;
    uint8_t lfo_shape = 0, lfo_target = 1;
    uint8_t mode = 0, reverse = 0, bus = 0, choke = 0, poly = 4;
};

enum VoiceState : uint8_t { VS_FREE = 0, VS_ON, VS_FADE };

// Block buffers a voice adds into (already offset to the segment start).
struct VoiceOut {
    float* l;
    float* r;
    float* sl;   // FX send
    float* sr;
};

struct VoiceStart {
    const Sample* smp = nullptr;
    uint8_t pad = 0, layer = 0, vel = 100;
    float pitch = 0.0f;        // layer tune + step pitch + command pitch (semitones)
    float layer_gain = 1.0f;   // linear
    float layer_pan = 0.0f;
    uint32_t hit = 0, age = 0, seed = 1;
};

struct Voice {
    uint8_t state = VS_FREE;
    uint8_t pad = 0, layer = 0, channels = 1;
    uint8_t loop = 0, gate = 0, rev = 0, first_pass = 1;
    uint8_t released = 0, xf_dip = 0, fast = 0, filt = 0;
    uint8_t started = 0, ended = 0, filt_active = 0, has_send = 0;
    uint32_t hit = 0, age = 0;
    const Sample* smp = nullptr;
    const int16_t* base = nullptr;   // region start (forward) or region end - 1 (reverse)
    int32_t reg_start = 0, reg_end = 0;   // absolute frames
    int32_t len = 1, loop_off = 0, loop_len = 1, xf = 0;
    float inv_xf = 0.0f;
    int32_t t_int = 0;               // region position (frames, playback order), 32.32 fixed point
    uint32_t t_fr = 0;
    int32_t inc_i = 1;               // increment, 32.32 fixed point (no float->int chain per sample)
    uint32_t inc_f = 0;
    float inc = 1.0f, rate_ratio = 1.0f, pitch = 0.0f;
    float vel_gain = 1.0f, layer_gain = 1.0f, layer_pan = 0.0f;
    float micro = 0.0f;              // micro fade-out length (source frames), 0 = none
    int32_t ctl_left = 0;
    uint32_t age_samples = 0;
    float fade = 1.0f, fade_step = 0.0f;   // ghost fade (linear, folded into the gain ramps)
    dsp::AmpEnv amp;
    dsp::DecayEnv fenv, penv;
    dsp::Lfo lfo;
    dsp::Svf svf[2];
    // SVF in delta state-space form (coefficients constant per sub-block, see voice.cpp)
    float fb1 = 0.0f, fe11 = 0.0f, fe12 = 0.0f, fb2 = 0.0f, fe21 = 0.0f, fe22 = 0.0f;
    float fd = 1.0f, fc1 = 0.0f, fc2 = 0.0f;
    float f_l2 = 0.0f;               // smoothed log2(fc / fs)
    float f_key = -1.0f;             // k + filter type of the current coefficients
    float f_l2_coef = 100.0f;        // f_l2 the coefficients were computed for
    float last_semis = 1.0e9f;
    float last_pan = 1.0e9f, pl = 0.0f, pr = 0.0f;
    float gl = 0.0f, gr = 0.0f, gsl = 0.0f, gsr = 0.0f;
    float dgl = 0.0f, dgr = 0.0f, dgsl = 0.0f, dgsr = 0.0f;
    float hold[2] = {0.0f, 0.0f};
    float last_in[2] = {0.0f, 0.0f};   // last pre-filter value (filter engage without a step)
    int32_t ds_cnt = 0;

    bool live() const { return state == VS_ON; }
    bool active() const { return state != VS_FREE; }
    // Current loudness estimate for stealing / meters.
    float level() const { return amp.x * fade; }
};

struct VoiceCtx {
    float fs = 48000.0f;
    float inv_fs = 1.0f / 48000.0f;
};

// Sets up v (does not touch the pool). False when the sample is unusable.
bool voiceStart(Voice& v, const VoiceStart& s, const PadCache& pc, const VoiceCtx& ctx);
// Adds frames [0, n) into out. Returns false when the voice finished (it is then VS_FREE).
bool voiceRender(Voice& v, const PadCache& pc, const VoiceOut& out, int n, const VoiceCtx& ctx);
// Gate off: Gate / Loop voices enter release. One-shots ignore it.
void voiceRelease(Voice& v, const PadCache& pc);
// Ghost fade to silence over `samples` (steal, choke, retire, panic).
void voiceFadeOut(Voice& v, float samples);
// Playhead as 0..1 of the whole sample.
float voicePosition(const Voice& v);
// Flush denormal-prone states (once per block).
void voiceFlush(Voice& v);

}  // namespace orbit
