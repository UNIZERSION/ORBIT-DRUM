#include "synthmap.h"

#include "dsp/fastmath.h"
#include "params.h"

namespace orbit {

using dsp::clampf;
using dsp::dbToLin;
using dsp::fastLog2;

namespace {

constexpr float kCutLog2Min = 4.32192809f;   // log2(20 Hz): the synth's CUTOFF 0 (core/synth.cpp)
constexpr float kCutRange = 10.0f;           // octaves: CUTOFF 1 = 20.48 kHz

inline float knob(const float* g, GlobalParam p, float lo, float hi)
{
    const float v = g[p];
    return v >= lo ? (v <= hi ? v : hi) : lo;   // NaN -> lo
}
inline uint8_t choice(const float* g, GlobalParam p, int count)
{
    const float v = g[p];
    if (!(v >= 0.0f)) return 0;
    const int i = (int)(v + 0.5f);
    return (uint8_t)(i < count ? i : count - 1);
}
inline float ms(const float* g, GlobalParam p, float lo_ms, float hi_ms) { return knob(g, p, lo_ms, hi_ms) * 0.001f; }

}  // namespace

// Once per block: runs from flash (it is not per sample; internal RAM is scarce)
void synthFromGlobals(const float* g, SynthParams& o)
{
    o.osc1_wave = choice(g, GP_SYN_OSC1_WAVE, kSynthWaveCount);
    o.osc1_color = knob(g, GP_SYN_OSC1_COLOR, 0.0f, 1.0f);
    o.osc1_level = knob(g, GP_SYN_OSC1_LEVEL, 0.0f, 1.0f);
    o.osc2_wave = choice(g, GP_SYN_OSC2_WAVE, kSynthWaveCount);
    o.osc2_color = knob(g, GP_SYN_OSC2_COLOR, 0.0f, 1.0f);
    o.osc2_level = knob(g, GP_SYN_OSC2_LEVEL, 0.0f, 1.0f);
    o.osc2_coarse = knob(g, GP_SYN_OSC2_COARSE, -24.0f, 24.0f);
    o.osc2_fine = knob(g, GP_SYN_OSC2_FINE, -50.0f, 50.0f);
    o.osc2_env = knob(g, GP_SYN_OSC2_ENV, -1.0f, 1.0f);
    o.sync = choice(g, GP_SYN_SYNC, 2);
    o.ring = choice(g, GP_SYN_RING, 2);
    o.xmod = knob(g, GP_SYN_XMOD, 0.0f, 1.0f);
    o.sub_oct = (uint8_t)(choice(g, GP_SYN_SUB_OCT, 2) + 1);
    o.sub_level = knob(g, GP_SYN_SUB_LEVEL, 0.0f, 1.0f);
    o.noise_level = knob(g, GP_SYN_NOISE, 0.0f, 1.0f);
    o.tune = knob(g, GP_SYN_TUNE, -24.0f, 24.0f);
    o.bend = 0.0f;
    o.filter_type = choice(g, GP_SYN_FILTER, kSynthFilterCount);
    const float hz = knob(g, GP_SYN_CUTOFF, 20.0f, 20480.0f);
    o.cutoff = clampf((fastLog2(hz) - kCutLog2Min) * (1.0f / kCutRange), 0.0f, 1.0f);
    o.reso = knob(g, GP_SYN_RESO, 0.0f, 1.0f);
    o.key_follow = knob(g, GP_SYN_KEY_FOLLOW, 0.0f, 1.0f);
    o.env_amt = knob(g, GP_SYN_ENV_AMT, -1.0f, 1.0f);
    o.vel_cutoff = knob(g, GP_SYN_VEL_CUTOFF, 0.0f, 1.0f);
    o.f_attack = ms(g, GP_SYN_F_ATTACK, 0.0f, 20000.0f);
    o.f_decay = ms(g, GP_SYN_F_DECAY, 1.0f, 20000.0f);
    o.f_sustain = knob(g, GP_SYN_F_SUSTAIN, 0.0f, 1.0f);
    o.f_release = ms(g, GP_SYN_F_RELEASE, 1.0f, 20000.0f);
    o.a_attack = ms(g, GP_SYN_A_ATTACK, 0.5f, 20000.0f);
    o.a_decay = ms(g, GP_SYN_A_DECAY, 1.0f, 20000.0f);
    o.a_sustain = knob(g, GP_SYN_A_SUSTAIN, 0.0f, 1.0f);
    o.a_release = ms(g, GP_SYN_A_RELEASE, 1.0f, 20000.0f);
    o.vel_amp = knob(g, GP_SYN_VEL_AMP, 0.0f, 1.0f);
    o.lfo_wave = choice(g, GP_SYN_LFO_WAVE, kSynthLfoWaveCount);
    o.lfo_rate = knob(g, GP_SYN_LFO_RATE, 0.05f, 30.0f);
    o.lfo_sync = choice(g, GP_SYN_LFO_SYNC, kSynthLfoDivs + 1);
    o.lfo_fade = ms(g, GP_SYN_LFO_FADE, 0.0f, 20000.0f);
    o.lfo_key_trig = choice(g, GP_SYN_LFO_KEY, 2);
    o.lfo_pitch = knob(g, GP_SYN_LFO_PITCH, 0.0f, 1.0f);
    o.lfo_cutoff = knob(g, GP_SYN_LFO_CUTOFF, 0.0f, 1.0f);
    o.lfo_amp = knob(g, GP_SYN_LFO_AMP, 0.0f, 1.0f);
    o.lfo_pw = knob(g, GP_SYN_LFO_PW, 0.0f, 1.0f);
    o.voice_mode = choice(g, GP_SYN_MODE, kSynthModeCount);
    o.legato = choice(g, GP_SYN_LEGATO, 2);
    o.glide = ms(g, GP_SYN_GLIDE, 0.0f, 5000.0f);
    o.uni_detune = knob(g, GP_SYN_DETUNE, 0.0f, 1.0f);
    o.spread = knob(g, GP_SYN_SPREAD, 0.0f, 1.0f);
    o.drive = knob(g, GP_SYN_DRIVE, 0.0f, 1.0f);
    const float ldb = knob(g, GP_SYN_LEVEL, -60.0f, 6.0f);
    o.level = ldb <= -59.9f ? 0.0f : dbToLin(ldb);
}

void synthToGlobals(const SynthParams& p, float* g)
{
    auto put = [g](GlobalParam k, float v) { g[k] = clampParam(globalSpec(k), v); };
    put(GP_SYN_OSC1_WAVE, (float)p.osc1_wave);
    put(GP_SYN_OSC1_COLOR, p.osc1_color);
    put(GP_SYN_OSC1_LEVEL, p.osc1_level);
    put(GP_SYN_OSC2_WAVE, (float)p.osc2_wave);
    put(GP_SYN_OSC2_COLOR, p.osc2_color);
    put(GP_SYN_OSC2_LEVEL, p.osc2_level);
    put(GP_SYN_OSC2_COARSE, p.osc2_coarse);
    put(GP_SYN_OSC2_FINE, p.osc2_fine);
    put(GP_SYN_OSC2_ENV, p.osc2_env);
    put(GP_SYN_SYNC, p.sync ? 1.0f : 0.0f);
    put(GP_SYN_RING, p.ring ? 1.0f : 0.0f);
    put(GP_SYN_XMOD, p.xmod);
    put(GP_SYN_SUB_OCT, p.sub_oct >= 2 ? 1.0f : 0.0f);
    put(GP_SYN_SUB_LEVEL, p.sub_level);
    put(GP_SYN_NOISE, p.noise_level);
    put(GP_SYN_TUNE, p.tune);
    put(GP_SYN_FILTER, (float)p.filter_type);
    put(GP_SYN_CUTOFF, synthCutoffHz(p.cutoff));
    put(GP_SYN_RESO, p.reso);
    put(GP_SYN_KEY_FOLLOW, p.key_follow);
    put(GP_SYN_ENV_AMT, p.env_amt);
    put(GP_SYN_VEL_CUTOFF, p.vel_cutoff);
    put(GP_SYN_F_ATTACK, p.f_attack * 1000.0f);
    put(GP_SYN_F_DECAY, p.f_decay * 1000.0f);
    put(GP_SYN_F_SUSTAIN, p.f_sustain);
    put(GP_SYN_F_RELEASE, p.f_release * 1000.0f);
    put(GP_SYN_A_ATTACK, p.a_attack * 1000.0f);
    put(GP_SYN_A_DECAY, p.a_decay * 1000.0f);
    put(GP_SYN_A_SUSTAIN, p.a_sustain);
    put(GP_SYN_A_RELEASE, p.a_release * 1000.0f);
    put(GP_SYN_VEL_AMP, p.vel_amp);
    put(GP_SYN_LFO_WAVE, (float)p.lfo_wave);
    put(GP_SYN_LFO_RATE, p.lfo_rate);
    put(GP_SYN_LFO_SYNC, (float)p.lfo_sync);
    put(GP_SYN_LFO_FADE, p.lfo_fade * 1000.0f);
    put(GP_SYN_LFO_KEY, p.lfo_key_trig ? 1.0f : 0.0f);
    put(GP_SYN_LFO_PITCH, p.lfo_pitch);
    put(GP_SYN_LFO_CUTOFF, p.lfo_cutoff);
    put(GP_SYN_LFO_AMP, p.lfo_amp);
    put(GP_SYN_LFO_PW, p.lfo_pw);
    put(GP_SYN_MODE, (float)p.voice_mode);
    put(GP_SYN_LEGATO, p.legato ? 1.0f : 0.0f);
    put(GP_SYN_GLIDE, p.glide * 1000.0f);
    put(GP_SYN_DETUNE, p.uni_detune);
    put(GP_SYN_SPREAD, p.spread);
    put(GP_SYN_DRIVE, p.drive);
    put(GP_SYN_LEVEL, p.level > 0.001f ? 6.02059991f * fastLog2(p.level) : -60.0f);
}

int synthPresetCount()
{
    int n = 0;
    synthPresets(&n);
    return n;
}

const char* synthPresetName(int i)
{
    int n = 0;
    const SynthPreset* t = synthPresets(&n);
    return t && i >= 0 && i < n ? t[i].name : "";
}

bool synthApplyPreset(int i, float* g)
{
    int n = 0;
    const SynthPreset* t = synthPresets(&n);
    if (!t || i < 0 || i >= n) return false;
    synthToGlobals(t[i].p, g);
    return true;
}

}  // namespace orbit
