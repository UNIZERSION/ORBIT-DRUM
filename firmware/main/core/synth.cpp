#include "synth.h"

#include <string.h>

#include "dsp/fastmath.h"

namespace orbit {

using namespace dsp;

// Per-sample helpers must be inlined into the voice loop (their state lives in registers there);
// the two passes of a voice stay separate functions (each loop gets the whole register file).
#define SYNTH_AI inline __attribute__((always_inline))
#define SYNTH_NOINLINE __attribute__((noinline))

namespace {

// Oscillators
constexpr float kDtMax = 0.45f;            // phase increment limit (at most one edge per half wave and sample)
constexpr float kDtMin = 1.0e-7f;
constexpr float kSawPairCents = 40.0f;     // SAW COLOR 1: the two saws 40 cents apart (+-20 around the note)
constexpr float kSawPairLevel = 4.0f;      // partner level = min(1, 4 COLOR)
constexpr float kSawPairNorm = 0.4f;       // pair level compensation: / (1 + 0.4 partner level)
constexpr float kPwMin = 0.03f;            // narrowest pulse (COLOR 1, and the LFO -> PW limit)
constexpr float kFoldMax = 4.0f;           // TRIANGLE COLOR 1: fold gain
constexpr float kXmodDepth = 2.0f;         // XMOD 1: dt1 x (1 + 2 osc2)
constexpr float kOsc2EnvSemis = 24.0f;
// Filter
constexpr float kCutLog2Min = 4.32192809f;   // log2(20 Hz), CUTOFF 0
constexpr float kCutRange = 10.0f;           // octaves, CUTOFF 1 = 20.48 kHz
constexpr float kEnvOct = 8.0f;
constexpr float kLfoOct = 4.0f;
constexpr float kVelOct = 3.0f;
constexpr float kKeyRef = 60.0f;             // key follow reference (C4)
constexpr float kFcMinHz = 16.0f;
constexpr float kFcMaxNorm = 0.45f;
constexpr float kFilterIn = 0.5f;            // mixer -> filter input: one oscillator stays almost linear
constexpr float kLadderK = 4.3f;             // ladder feedback at RESO 1 (oscillates above 4)
constexpr float kLadderMakeup = 0.5f;        // makes up half of the passband loss 1 / (1 + k)
constexpr float kSvfDampMin = 0.06f;         // SVF damping at RESO 1 (Q 17)
constexpr float kSvfCompExp = 0.4f;          // SVF output x (k / 1.41)^0.4 above Q 0.71
// Envelopes (dsp::kLnAttack / kLnSegEnd: attack aims at 1.3 and reaches 1 after T, decay / release
// land on their end value after T)
constexpr float kAtt = -kLnAttack;
constexpr float kSeg = -kLnSegEnd;
constexpr float kAmpFloor = 1.0e-5f;         // -100 dB: amp envelope done
// LFO / voices / output
constexpr float kLfoPitchSemis = 12.0f;
constexpr float kLfoPw = 0.45f;
constexpr float kUniSemis = 0.40f;           // UNISON outer voices at UNI DETUNE 1
constexpr float kUniPos[kSynthVoices] = {-1.0f, -1.0f / 3.0f, 1.0f / 3.0f, 1.0f};
constexpr float kPolyPan[kSynthVoices] = {-1.0f, 1.0f, -1.0f / 3.0f, 1.0f / 3.0f};
constexpr float kPolySpread = 0.35f;
constexpr float kUniGain = 0.5f;             // per voice in UNISON
constexpr float kOutGain = 1.5f;            // calibration: one default saw voice peaks ~0.5 at LEVEL 0.7
constexpr float kDriveGain = 5.0f;
constexpr float kDcHz = 5.0f;
constexpr float kPanicS = 0.004f;
constexpr float kKnobTau = 0.015f;
constexpr float kInvCtl = 1.0f / (float)Synth::kCtl;
constexpr float kSqrt2 = 1.41421356f;

enum AmpStage : uint8_t { AS_DONE = 0, AS_ATTACK, AS_DECAY, AS_SUSTAIN, AS_RELEASE };
enum FiltStage : uint8_t { FS_DONE = 0, FS_ATTACK, FS_DECAY, FS_SUSTAIN, FS_RELEASE };

struct LfoDiv {
    const char* name;
    float beats;
};
constexpr LfoDiv kLfoDivs[kSynthLfoDivs] = {
    {"4 BARS", 16.0f}, {"2 BARS", 8.0f},       {"1 BAR", 4.0f}, {"1/2", 2.0f},  {"1/4.", 1.5f},
    {"1/4", 1.0f},     {"1/4T", 2.0f / 3.0f},  {"1/8.", 0.75f}, {"1/8", 0.5f},  {"1/8T", 1.0f / 3.0f},
    {"1/16", 0.25f},   {"1/16T", 1.0f / 6.0f}, {"1/32", 0.125f},
};

// clampf that also maps NaN to lo.
inline float sanitize(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }
inline uint8_t pickEnum(uint8_t x, uint8_t count) { return x < count ? x : 0; }
// Bytewise equality of two SynthParams, inline (libc memcmp runs from flash on the Tab5)
inline bool sameParams(const SynthParams& a, const SynthParams& b)
{
    const unsigned char* x = reinterpret_cast<const unsigned char*>(&a);
    const unsigned char* y = reinterpret_cast<const unsigned char*>(&b);
    size_t i = 0;
    for (; i + 4 <= sizeof(SynthParams); i += 4) {
        uint32_t u, v;
        memcpy(&u, x + i, 4);
        memcpy(&v, y + i, 4);
        if (u != v) return false;
    }
    for (; i < sizeof(SynthParams); ++i)
        if (x[i] != y[i]) return false;
    return true;
}

// 1 - exp(-y), y >= 0, without libm: series for small y (long times: keeps the precision that
// 1 - fastExp2() would lose), fastExp2 above.
inline float oneMinusExpNeg(float y)
{
    if (y < 0.05f) return y * (1.0f - y * (0.5f - y * (1.0f / 6.0f - y * (1.0f / 24.0f))));
    return 1.0f - fastExp2(-y * 1.44269504f);
}

// One-pole knob follower at control rate; lands exactly on the target (no denormal tail).
inline void follow(float& s, float t, float c)
{
    if (s == t) return;
    s += c * (t - s);
    if (absf(t - s) < 1.0e-6f) s = t;
}

// Sub-sample event offset d in [0, 1) (NaN -> 0).
SYNTH_AI float frac01(float d) { return d > 0.0f ? (d < 0.99999f ? d : 0.99999f) : 0.0f; }

// Cubic soft clip: slope 1 at 0, reaches +-1 (with zero slope) at |x| = 1.5. No division; the
// clamp is one fmax.s + one fmin.s on the device (builtins, no libm call) and maps NaN to -1.5.
SYNTH_AI float sat(float x)
{
    x = __builtin_fminf(__builtin_fmaxf(x, -1.5f), 1.5f);
    return x * (1.0f - 0.148148148f * x * x);
}

// Step of height h, d samples before the current sample (0 <= d < 1): PolyBLEP corrections of the
// pending (previous) sample and of the current one.
SYNTH_AI void blep(float& prev, float& cur, float h, float d)
{
    const float hh = 0.5f * h;
    const float e = 1.0f - d;
    prev += hh * d * d;
    cur -= hh * e * e;
}

// Slope change s (value per sample), d samples before the current sample: PolyBLAMP (the integral
// of the PolyBLEP residual) on both sides.
SYNTH_AI void blamp(float& prev, float& cur, float s, float d)
{
    const float k = s * (1.0f / 6.0f);
    const float e = 1.0f - d;
    prev += k * d * d * d;
    cur += k * e * e * e;
}

// One oscillator sample (O = Synth::Osc). dt: phase increment; dtb / lb: SAW partner increment /
// level; pw: pulse width; sd >= 0: hard-sync reset sd samples before this sample. Returns the
// previous sample, band-limited (one sample late, so both sides of every edge are corrected);
// wrap = sub-sample offset of the main phase's wrap in this sample, -1 if none.
template <class O>
SYNTH_AI float oscTick(O& o, uint8_t wave, float dt, float dtb, float lb, float pw, float sd, float& wrap)
{
    float cur = 0.0f;
    wrap = -1.0f;
    if (wave == kSynthSquare) {
        if (sd < 0.0f) {
            o.ph += dt;
            if (o.ph >= 1.0f) {
                o.ph -= 1.0f;
                wrap = frac01(o.ph / dt);
                if (!o.hi) {
                    blep(o.y1, cur, 2.0f, wrap);
                    o.hi = 1;
                }
            }
            if (o.hi && o.ph >= pw) {
                blep(o.y1, cur, -2.0f, frac01((o.ph - pw) / dt));
                o.hi = 0;
            }
        } else {
            float pr = o.ph + (1.0f - sd) * dt;   // phase at the reset
            if (pr >= 1.0f) {
                pr -= 1.0f;
                if (!o.hi) {
                    blep(o.y1, cur, 2.0f, frac01(sd + pr / dt));
                    o.hi = 1;
                }
            }
            if (o.hi && pr >= pw) {
                blep(o.y1, cur, -2.0f, frac01(sd + (pr - pw) / dt));
                o.hi = 0;
            }
            if (!o.hi) {
                blep(o.y1, cur, 2.0f, sd);
                o.hi = 1;
            }
            o.ph = sd * dt;
            if (o.ph >= pw) {
                blep(o.y1, cur, -2.0f, frac01((o.ph - pw) / dt));
                o.hi = 0;
            }
        }
        cur += o.hi ? 2.0f - 2.0f * pw : -2.0f * pw;   // DC-free pulse
    } else if (wave == kSynthTriangle) {
        const float sl = 8.0f * dt;   // slope change at a corner (value per sample)
        if (sd < 0.0f) {
            o.ph += dt;
            if (o.ph >= 1.0f) {
                o.ph -= 1.0f;
                wrap = frac01(o.ph / dt);
                if (!o.hi) {
                    blamp(o.y1, cur, sl, wrap);
                    o.hi = 1;
                }
            }
            if (o.hi && o.ph >= 0.5f) {
                blamp(o.y1, cur, -sl, frac01((o.ph - 0.5f) / dt));
                o.hi = 0;
            }
        } else {
            float pr = o.ph + (1.0f - sd) * dt;
            if (pr >= 1.0f) {
                pr -= 1.0f;
                if (!o.hi) {
                    blamp(o.y1, cur, sl, frac01(sd + pr / dt));
                    o.hi = 1;
                }
            }
            if (o.hi && pr >= 0.5f) {
                blamp(o.y1, cur, -sl, frac01(sd + (pr - 0.5f) / dt));
                o.hi = 0;
            }
            const float vr = o.hi ? 4.0f * pr - 1.0f : 3.0f - 4.0f * pr;
            blep(o.y1, cur, -1.0f - vr, sd);   // jump to -1
            if (!o.hi) blamp(o.y1, cur, sl, sd);   // falling -> rising
            o.hi = 1;
            o.ph = sd * dt;
        }
        cur += o.hi ? 4.0f * o.ph - 1.0f : 3.0f - 4.0f * o.ph;
    } else {   // SAW (+ detuned partner)
        if (sd < 0.0f) {
            o.ph += dt;
            if (o.ph >= 1.0f) {
                o.ph -= 1.0f;
                wrap = frac01(o.ph / dt);
                blep(o.y1, cur, -2.0f, wrap);
            }
            if (lb > 0.0f) {
                o.phb += dtb;
                if (o.phb >= 1.0f) {
                    o.phb -= 1.0f;
                    blep(o.y1, cur, -2.0f * lb, frac01(o.phb / dtb));
                }
            }
        } else {
            float pr = o.ph + (1.0f - sd) * dt;
            if (pr >= 1.0f) {
                pr -= 1.0f;
                blep(o.y1, cur, -2.0f, frac01(sd + pr / dt));
            }
            blep(o.y1, cur, -2.0f * pr, sd);   // 2 pr - 1 -> -1
            o.ph = sd * dt;
            if (lb > 0.0f) {
                float pb = o.phb + (1.0f - sd) * dtb;
                if (pb >= 1.0f) {
                    pb -= 1.0f;
                    blep(o.y1, cur, -2.0f * lb, frac01(sd + pb / dtb));
                }
                blep(o.y1, cur, -2.0f * lb * pb, sd);
                o.phb = sd * dtb;
            }
        }
        cur += (o.ph + o.ph - 1.0f) + lb * (o.phb + o.phb - 1.0f);
    }
    const float out = o.y1;
    o.y1 = cur;
    return out;
}

// TRIANGLE COLOR: sine wavefolder sin(pi/2 g x) x norm. The triangle-shaped reflection of g x into
// [-1, 1] followed by a quarter-wave sine is the full sine of g x, so the fold is smooth.
SYNTH_AI float foldSine(float x, float g, float norm)
{
    float u = x * g;
    u -= 4.0f * (float)floorToInt(u * 0.25f + 0.5f);   // [-2, 2]
    if (u > 1.0f) u = 2.0f - u;
    else if (u < -1.0f) u = -2.0f - u;
    return sinHalfPi(u * kHalfPi) * norm;
}

inline float rndBi(uint32_t& s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return (float)(int32_t)s * 4.656612873e-10f;
}

inline float lfoShape(uint8_t wave, float ph, float r0, float r1)
{
    switch (wave) {
    case kSynthLfoTriangle: return ph < 0.25f ? 4.0f * ph : (ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f);
    case kSynthLfoSawUp: return ph + ph - 1.0f;
    case kSynthLfoSawDown: return 1.0f - ph - ph;
    case kSynthLfoSquare: return ph < 0.5f ? 1.0f : -1.0f;
    case kSynthLfoSampleHold: return r1;
    default: return r0 + (r1 - r0) * (ph * ph * (3.0f - 2.0f * ph));   // smooth random
    }
}

// Half-wave state that matches a phase (after a wave switch).
inline uint8_t waveHi(uint8_t wave, float ph, float pw) { return (wave == kSynthSquare ? ph < pw : ph < 0.5f) ? 1 : 0; }

// COLOR -> oscillator constants.
struct Color {
    float ra = 1.0f;     // main phase increment factor (SAW pair: -half the detune)
    float rb = 1.0f;     // partner / main increment ratio
    float lb = 0.0f;     // partner level
    float norm = 1.0f;   // level compensation
    float pwb = 0.5f;    // pulse width
    float fg = 0.0f;     // fold gain (0: plain triangle)
    float fn = 1.0f;     // fold normalization
};
inline Color oscColor(uint8_t wave, float c)
{
    Color o;
    if (wave == kSynthSaw) {
        const float cents = kSawPairCents * c;
        o.ra = fastExp2(cents * (-0.5f / 1200.0f));
        o.rb = fastExp2(cents * (1.0f / 1200.0f));
        o.lb = minf(1.0f, kSawPairLevel * c);
        o.norm = 1.0f / (1.0f + kSawPairNorm * o.lb);
    } else if (wave == kSynthSquare) {
        o.pwb = 0.5f - (0.5f - kPwMin) * c;
    } else {
        o.fg = kFoldMax * c;
        if (o.fg > 0.0f) o.fn = 1.0f / sinHalfPi(kHalfPi * minf(o.fg, 1.0f));
    }
    return o;
}

// ------------------------------------------------------------------ factory patches
// (constexpr builders: the table is constant-initialized, no code runs at startup)

constexpr SynthParams patchBrass()
{
    SynthParams p{};
    p.osc1_wave = kSynthSaw;
    p.osc2_wave = kSynthSaw;
    p.osc2_level = 0.8f;
    p.osc2_fine = 8.0f;
    p.filter_type = kSynthLp24;
    p.cutoff = 0.33f;
    p.reso = 0.12f;
    p.key_follow = 0.5f;
    p.env_amt = 0.42f;
    p.vel_cutoff = 0.3f;
    p.f_attack = 0.08f;
    p.f_decay = 0.6f;
    p.f_sustain = 0.45f;
    p.f_release = 0.3f;
    p.a_attack = 0.03f;
    p.a_decay = 0.5f;
    p.a_sustain = 0.85f;
    p.a_release = 0.3f;
    p.lfo_wave = kSynthLfoTriangle;
    p.lfo_rate = 5.5f;
    p.lfo_pitch = 0.06f;
    p.lfo_fade = 0.6f;
    p.lfo_key_trig = 1;
    p.spread = 0.4f;
    p.level = 0.5f;
    return p;
}

constexpr SynthParams patchStrings()
{
    SynthParams p{};
    p.osc1_wave = kSynthSaw;
    p.osc1_color = 0.5f;
    p.osc2_wave = kSynthSaw;
    p.osc2_color = 0.4f;
    p.osc2_level = 0.5f;
    p.osc2_coarse = 12.0f;
    p.osc2_fine = -6.0f;
    p.filter_type = kSynthLp12;
    p.cutoff = 0.6f;
    p.reso = 0.1f;
    p.key_follow = 0.4f;
    p.env_amt = 0.1f;
    p.f_attack = 0.8f;
    p.f_decay = 1.0f;
    p.f_sustain = 0.8f;
    p.f_release = 1.2f;
    p.a_attack = 0.45f;
    p.a_decay = 1.0f;
    p.a_sustain = 0.9f;
    p.a_release = 1.3f;
    p.vel_amp = 0.3f;
    p.lfo_wave = kSynthLfoTriangle;
    p.lfo_rate = 4.8f;
    p.lfo_pitch = 0.07f;
    p.lfo_fade = 1.0f;
    p.lfo_key_trig = 1;
    p.spread = 0.8f;
    p.level = 0.4f;
    return p;
}

constexpr SynthParams patchPluck()
{
    SynthParams p{};
    p.osc1_wave = kSynthSquare;
    p.osc1_color = 0.35f;
    p.osc2_wave = kSynthTriangle;
    p.osc2_level = 0.5f;
    p.osc2_coarse = 12.0f;
    p.filter_type = kSynthLp24;
    p.cutoff = 0.2f;
    p.reso = 0.35f;
    p.key_follow = 0.7f;
    p.env_amt = 0.55f;
    p.vel_cutoff = 0.4f;
    p.f_attack = 0.0f;
    p.f_decay = 0.22f;
    p.f_sustain = 0.0f;
    p.f_release = 0.2f;
    p.a_attack = 0.001f;
    p.a_decay = 0.45f;
    p.a_sustain = 0.0f;
    p.a_release = 0.35f;
    p.vel_amp = 0.5f;
    p.spread = 0.5f;
    p.level = 0.45f;
    return p;
}

constexpr SynthParams patchSyncLead()
{
    SynthParams p{};
    p.osc1_wave = kSynthSaw;
    p.osc1_level = 0.5f;
    p.osc2_wave = kSynthSaw;
    p.osc2_level = 1.0f;
    p.osc2_coarse = 7.0f;
    p.osc2_env = 0.45f;
    p.sync = 1;
    p.filter_type = kSynthLp24;
    p.cutoff = 0.62f;
    p.reso = 0.2f;
    p.key_follow = 0.5f;
    p.env_amt = 0.25f;
    p.f_attack = 0.0f;
    p.f_decay = 0.5f;
    p.f_sustain = 0.2f;
    p.f_release = 0.3f;
    p.a_attack = 0.003f;
    p.a_decay = 0.3f;
    p.a_sustain = 0.9f;
    p.a_release = 0.2f;
    p.lfo_wave = kSynthLfoTriangle;
    p.lfo_rate = 5.5f;
    p.lfo_pitch = 0.08f;
    p.lfo_fade = 0.4f;
    p.lfo_key_trig = 1;
    p.voice_mode = kSynthMono;
    p.legato = 1;
    p.glide = 0.03f;
    p.level = 0.6f;
    return p;
}

constexpr SynthParams patchAcidBass()
{
    SynthParams p{};
    p.osc1_wave = kSynthSaw;
    p.sub_level = 0.4f;
    p.sub_oct = 1;
    p.filter_type = kSynthLp24;
    p.cutoff = 0.2f;
    p.reso = 0.78f;
    p.key_follow = 0.3f;
    p.env_amt = 0.55f;
    p.vel_cutoff = 0.3f;
    p.f_attack = 0.0f;
    p.f_decay = 0.28f;
    p.f_sustain = 0.0f;
    p.f_release = 0.12f;
    p.a_attack = 0.002f;
    p.a_decay = 0.4f;
    p.a_sustain = 0.75f;
    p.a_release = 0.06f;
    p.voice_mode = kSynthMono;
    p.legato = 1;
    p.glide = 0.04f;
    p.drive = 0.35f;
    p.level = 0.7f;
    return p;
}

constexpr SynthParams patchUnisonSaw()
{
    SynthParams p{};
    p.osc1_wave = kSynthSaw;
    p.osc1_color = 0.7f;
    p.osc2_wave = kSynthSaw;
    p.osc2_color = 0.7f;
    p.osc2_level = 0.6f;
    p.osc2_coarse = 12.0f;
    p.osc2_fine = 7.0f;
    p.filter_type = kSynthLp12;
    p.cutoff = 0.72f;
    p.reso = 0.15f;
    p.key_follow = 0.3f;
    p.env_amt = 0.15f;
    p.f_attack = 0.01f;
    p.f_decay = 0.8f;
    p.f_sustain = 0.6f;
    p.f_release = 0.4f;
    p.a_attack = 0.01f;
    p.a_decay = 0.5f;
    p.a_sustain = 0.9f;
    p.a_release = 0.4f;
    p.voice_mode = kSynthUnison;
    p.uni_detune = 0.45f;
    p.spread = 1.0f;
    p.level = 0.55f;
    return p;
}

constexpr SynthParams patchBellRing()
{
    SynthParams p{};
    p.osc1_wave = kSynthTriangle;
    p.osc1_color = 0.25f;   // sine
    p.osc1_level = 0.35f;
    p.osc2_wave = kSynthTriangle;
    p.osc2_color = 0.25f;
    p.osc2_level = 0.9f;
    p.osc2_coarse = 16.0f;
    p.osc2_fine = 36.0f;    // ratio ~2.57: inharmonic, bell-like sidebands
    p.ring = 1;
    p.filter_type = kSynthLp12;
    p.cutoff = 0.8f;
    p.reso = 0.0f;
    p.key_follow = 0.5f;
    p.env_amt = 0.2f;
    p.f_attack = 0.0f;
    p.f_decay = 1.2f;
    p.f_sustain = 0.0f;
    p.f_release = 1.0f;
    p.a_attack = 0.001f;
    p.a_decay = 2.0f;
    p.a_sustain = 0.0f;
    p.a_release = 1.5f;
    p.vel_amp = 0.6f;
    p.spread = 0.6f;
    p.level = 0.5f;
    return p;
}

constexpr SynthParams patchPwmPad()
{
    SynthParams p{};
    p.osc1_wave = kSynthSquare;
    p.osc1_color = 0.2f;
    p.osc2_wave = kSynthSquare;
    p.osc2_color = 0.2f;
    p.osc2_level = 0.8f;
    p.osc2_fine = -9.0f;
    p.filter_type = kSynthLp24;
    p.cutoff = 0.45f;
    p.reso = 0.18f;
    p.key_follow = 0.4f;
    p.env_amt = 0.15f;
    p.f_attack = 0.6f;
    p.f_decay = 1.5f;
    p.f_sustain = 0.6f;
    p.f_release = 1.0f;
    p.a_attack = 0.35f;
    p.a_decay = 1.0f;
    p.a_sustain = 0.85f;
    p.a_release = 1.2f;
    p.vel_amp = 0.3f;
    p.lfo_wave = kSynthLfoTriangle;
    p.lfo_rate = 0.45f;
    p.lfo_pw = 0.55f;
    p.spread = 0.7f;
    p.level = 0.28f;
    return p;
}

constexpr SynthPreset kPresets[] = {
    {"BRASS", patchBrass()},         {"STRINGS", patchStrings()},     {"PLUCK", patchPluck()},
    {"SYNC LEAD", patchSyncLead()},  {"ACID BASS", patchAcidBass()},  {"UNISON SAW", patchUnisonSaw()},
    {"BELL RING", patchBellRing()},  {"PWM PAD", patchPwmPad()},
};

}  // namespace

const SynthPreset* synthPresets(int* count)
{
    if (count) *count = (int)(sizeof(kPresets) / sizeof(kPresets[0]));
    return kPresets;
}

float synthCutoffHz(float cutoff01) { return fastExp2(kCutLog2Min + kCutRange * sanitize(cutoff01, 0.0f, 1.0f)); }

float synthLfoDivBeats(int div) { return div >= 1 && div <= kSynthLfoDivs ? kLfoDivs[div - 1].beats : 0.0f; }

const char* synthLfoDivName(int div) { return div >= 1 && div <= kSynthLfoDivs ? kLfoDivs[div - 1].name : "FREE"; }

// ------------------------------------------------------------------ setup / events

void Synth::init(int sample_rate)
{
    *this = Synth();
    for (int i = 0; i < kSynthVoices; ++i) {
        Voice& v = v_[i];
        // distinct free-running phases and noise / random seeds per voice (deterministic)
        v.o1.ph = 0.05f + 0.23f * (float)i;    // 0.05 .. 0.74
        v.o1.phb = 0.95f - 0.27f * (float)i;   // 0.95 .. 0.14
        v.o2.ph = 0.37f + 0.19f * (float)i;    // 0.37 .. 0.94
        v.o2.phb = 0.83f - 0.21f * (float)i;   // 0.83 .. 0.20
        v.o1.hi = waveHi(kSynthSaw, v.o1.ph, 0.5f);
        v.o2.hi = waveHi(kSynthSaw, v.o2.ph, 0.5f);
        v.nrng = 0x9E3779B9u ^ (0x85EBCA6Bu * (uint32_t)(i + 1));
        v.lrng = 0x68E31DA4u + 0x1B873593u * (uint32_t)(i + 1);
        v.lr1 = rndBi(v.lrng);
        v.lr0 = v.lr1;
    }
    glr1_ = rndBi(grng_);
    glr0_ = glr1_;
    setSampleRate(sample_rate);
}

void Synth::setSampleRate(int sample_rate)
{
    const int sr = sample_rate < 8000 ? 8000 : (sample_rate > 192000 ? 192000 : sample_rate);
    fs_ = (float)sr;
    inv_fs_ = 1.0f / fs_;
    knob_c_ = 1.0f - expf(-(float)kCtl / (kKnobTau * fs_));
    dc_r_ = 1.0f - kTwoPi * kDcHz / fs_;
    fade_step_ = 1.0f / (kPanicS * fs_);
    for (float& t : t_in_) t = -1.0f;   // time coefficients follow on the next render()
    derived_ = false;
    pvalid_ = false;
    ctl_left_ = 0;
}

bool Synth::queue(uint8_t type, uint8_t note, uint8_t vel, int offset)
{
    if (nq_ >= kQueue) return false;
    Event& e = q_[nq_++];
    e.type = type;
    e.note = note > 127 ? 127 : note;
    e.vel = vel > 127 ? 127 : vel;
    e.offset = offset > 0 ? (uint32_t)offset : 0u;
    return true;
}

bool Synth::noteOn(uint8_t note, uint8_t velocity, int frame_offset) { return queue(EV_ON, note, velocity, frame_offset); }
bool Synth::noteOff(uint8_t note, int frame_offset) { return queue(EV_OFF, note, 0, frame_offset); }
bool Synth::allNotesOff(int frame_offset) { return queue(EV_ALL_OFF, 0, 0, frame_offset); }

void Synth::panic()
{
    if (nq_ >= kQueue) nq_ = kQueue - 1;   // a panic is never dropped
    queue(EV_PANIC, 0, 0, 0);
}

ORBIT_HOT int Synth::activeVoices() const
{
    int c = 0;
    for (const Voice& v : v_) c += v.active ? 1 : 0;
    return c;
}

ORBIT_HOT bool Synth::anyActive() const
{
    for (const Voice& v : v_)
        if (v.active) return true;
    return false;
}

// ------------------------------------------------------------------ render

// Per block, from flash (0.4.9: keeps internal RAM free; only the per-sample and control-rate code is ORBIT_HOT)
void Synth::readParams(const SynthParams& p, float bpm)
{
    float* t = kt_;
    t[K_COLOR1] = sanitize(p.osc1_color, 0.0f, 1.0f);
    t[K_LEVEL1] = sanitize(p.osc1_level, 0.0f, 1.0f);
    t[K_COLOR2] = sanitize(p.osc2_color, 0.0f, 1.0f);
    t[K_LEVEL2] = sanitize(p.osc2_level, 0.0f, 1.0f);
    t[K_COARSE] = sanitize(p.osc2_coarse, -24.0f, 24.0f);
    t[K_FINE] = sanitize(p.osc2_fine, -50.0f, 50.0f);
    t[K_O2ENV] = sanitize(p.osc2_env, -1.0f, 1.0f);
    t[K_XMOD] = sanitize(p.xmod, 0.0f, 1.0f);
    t[K_SUB] = sanitize(p.sub_level, 0.0f, 1.0f);
    t[K_NOISE] = sanitize(p.noise_level, 0.0f, 1.0f);
    t[K_TUNE] = sanitize(p.tune, -24.0f, 24.0f);
    t[K_BEND] = sanitize(p.bend, -24.0f, 24.0f);
    t[K_CUTOFF] = sanitize(p.cutoff, 0.0f, 1.0f);
    t[K_RESO] = sanitize(p.reso, 0.0f, 1.0f);
    t[K_KEYF] = sanitize(p.key_follow, 0.0f, 1.0f);
    t[K_ENV] = sanitize(p.env_amt, -1.0f, 1.0f);
    t[K_VELCUT] = sanitize(p.vel_cutoff, 0.0f, 1.0f);
    t[K_FSUS] = sanitize(p.f_sustain, 0.0f, 1.0f);
    t[K_ASUS] = sanitize(p.a_sustain, 0.0f, 1.0f);
    t[K_VELAMP] = sanitize(p.vel_amp, 0.0f, 1.0f);
    t[K_LPITCH] = sanitize(p.lfo_pitch, 0.0f, 1.0f);
    t[K_LCUT] = sanitize(p.lfo_cutoff, 0.0f, 1.0f);
    t[K_LAMP] = sanitize(p.lfo_amp, 0.0f, 1.0f);
    t[K_LPW] = sanitize(p.lfo_pw, 0.0f, 1.0f);
    t[K_DETUNE] = sanitize(p.uni_detune, 0.0f, 1.0f);
    t[K_SPREAD] = sanitize(p.spread, 0.0f, 1.0f);
    t[K_DRIVE] = sanitize(p.drive, 0.0f, 1.0f);
    t[K_LEVEL] = sanitize(p.level, 0.0f, 4.0f);

    // switches (a wave change re-derives the half-wave state from the phase)
    const uint8_t w1 = pickEnum(p.osc1_wave, kSynthWaveCount);
    const uint8_t w2 = pickEnum(p.osc2_wave, kSynthWaveCount);
    if (w1 != w1_) {
        w1_ = w1;
        for (Voice& v : v_) v.o1.hi = waveHi(w1, v.o1.ph, v.pw1);
    }
    if (w2 != w2_) {
        w2_ = w2;
        for (Voice& v : v_) v.o2.hi = waveHi(w2, v.o2.ph, v.pw2);
    }
    ft_ = pickEnum(p.filter_type, kSynthFilterCount);
    sync_ = p.sync ? 1 : 0;
    ring_ = p.ring ? 1 : 0;
    subdiv_ = p.sub_oct >= 2 ? 2 : 1;
    lfo_wave_ = pickEnum(p.lfo_wave, kSynthLfoWaveCount);
    lfo_key_ = p.lfo_key_trig ? 1 : 0;
    legato_ = p.legato ? 1 : 0;
    const uint8_t mode = pickEnum(p.voice_mode, kSynthModeCount);
    if (mode != mode_) {   // voices of the old mode release, the note stack starts empty
        releaseAll();
        nheld_ = 0;
        mode_ = mode;
    }

    // times -> coefficients, only when they change
    const float tm[8] = {sanitize(p.a_attack, 0.0005f, 20.0f), sanitize(p.a_decay, 0.001f, 20.0f),
                         sanitize(p.a_release, 0.001f, 20.0f), sanitize(p.f_attack, 0.0f, 20.0f),
                         sanitize(p.f_decay, 0.001f, 20.0f),   sanitize(p.f_release, 0.001f, 20.0f),
                         sanitize(p.glide, 0.0f, 5.0f),        sanitize(p.lfo_fade, 0.0f, 20.0f)};
    for (int i = 0; i < 8; ++i)
        if (tm[i] != t_in_[i]) timeChanged(i, tm[i]);
    float hz;
    if (p.lfo_sync >= 1 && p.lfo_sync <= kSynthLfoDivs)
        hz = sanitize(bpm, 20.0f, 400.0f) * (1.0f / 60.0f) / kLfoDivs[p.lfo_sync - 1].beats;
    else
        hz = sanitize(p.lfo_rate, 0.05f, 30.0f);
    lfo_inc_ = hz * (float)kCtl * inv_fs_;
}

// A time knob changed: its coefficient (no libm: oneMinusExpNeg / fastExp2). Rare, so not in IRAM.
void Synth::timeChanged(int i, float t)
{
    t_in_[i] = t;
    const float ctl = (float)kCtl;
    const float n = maxf(t * fs_, 1.0e-6f);   // segment length in samples
    switch (i) {
    case 0: att_c_ = oneMinusExpNeg(kAtt / n); break;
    case 1: dec_c_ = oneMinusExpNeg(kSeg / n); break;
    case 2: rel_c_ = oneMinusExpNeg(kSeg / n); break;
    case 3: fatt_c_ = oneMinusExpNeg(kAtt * ctl / n); break;
    case 4: fdec_c_ = oneMinusExpNeg(kSeg * ctl / n); break;
    case 5: frel_c_ = oneMinusExpNeg(kSeg * ctl / n); break;
    case 6: glide_c_ = t > 0.0f ? oneMinusExpNeg(ctl / n) : 1.0f; break;
    default: fade_inc_ = t > 0.0f ? ctl / n : 1.0f; break;
    }
}

ORBIT_HOT void Synth::render(float* outL, float* outR, int n, const SynthParams& p, float bpm)
{
    if (n <= 0 || !outL || !outR) {
        nq_ = 0;
        return;
    }
    // the knobs as bytes (memcpy keeps the padding, so an unchanged SynthParams compares equal)
    if (!pvalid_ || bpm != last_bpm_ || !sameParams(p, last_p_)) {
        readParams(p, bpm);
        memcpy(&last_p_, &p, sizeof(SynthParams));
        last_bpm_ = bpm;
        pvalid_ = true;
    }
    if (!panic_ && !anyActive()) {
        // nothing audible: knobs jump to their values (no glide at the next note start)
        for (int i = 0; i < K_COUNT; ++i) ks_[i] = kt_[i];
        if (nq_ == 0) return;
        derive();
    }

    // stable insertion sort by offset (equal offsets keep the queue order)
    for (int i = 1; i < nq_; ++i) {
        const Event e = q_[i];
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
            run(outL, outR, pos, off);
            pos = off;
        }
        applyEvent(q_[i]);
    }
    nq_ = 0;
    if (pos < n) run(outL, outR, pos, n);

    for (Voice& v : v_) {
        v.s1 = flushDenormal(v.s1);
        v.s2 = flushDenormal(v.s2);
        v.s3 = flushDenormal(v.s3);
        v.s4 = flushDenormal(v.s4);
        v.dcx = flushDenormal(v.dcx);
        v.dcy = flushDenormal(v.dcy);
    }
}

// Note events (and the allocation / envelope starts below) are rare, so they run from flash;
// the per-block and per-sample path is ORBIT_HOT.
void Synth::applyEvent(const Event& e)
{
    switch (e.type) {
    case EV_ON:
        if (e.vel == 0) {   // velocity 0 = NoteOff
            Event off = e;
            off.type = EV_OFF;
            applyEvent(off);
            break;
        }
        if (panic_) {   // starts once the fade is done
            for (int i = 0; i < npend_; ++i)
                if (pend_[i].note == e.note) {
                    pend_[i] = pend_[--npend_];
                    break;
                }
            if (npend_ < kSynthVoices) pend_[npend_++] = {e.note, e.vel};
            break;
        }
        if (mode_ == kSynthPoly) polyOn(e.note, e.vel);
        else monoOn(e.note, e.vel);
        break;
    case EV_OFF:
        if (panic_) {
            for (int i = 0; i < npend_; ++i)
                if (pend_[i].note == e.note) {
                    pend_[i] = pend_[--npend_];
                    break;
                }
            break;
        }
        noteOffNow(e.note);
        break;
    case EV_ALL_OFF:
        npend_ = 0;
        nheld_ = 0;
        releaseAll();
        break;
    default:   // EV_PANIC
        npend_ = 0;
        nheld_ = 0;
        if (!anyActive()) break;
        releaseAll();
        panic_ = true;
        ctl_left_ = 0;   // the fade starts at this frame
        break;
    }
}

// ------------------------------------------------------------------ voice allocation

void Synth::polyOn(uint8_t note, uint8_t vel)
{
    int pick = -1;
    for (int k = 0; k < kSynthVoices; ++k)   // a sounding note retriggers its voice
        if (v_[k].active && v_[k].note == note) {
            pick = k;
            break;
        }
    if (pick < 0) {   // free voice, the one released longest ago
        for (int k = 0; k < kSynthVoices; ++k)
            if (!v_[k].active && (pick < 0 || v_[k].off_age < v_[pick].off_age)) pick = k;
    }
    if (pick < 0) {   // oldest released voice
        for (int k = 0; k < kSynthVoices; ++k)
            if (!v_[k].gate && (pick < 0 || v_[k].off_age < v_[pick].off_age)) pick = k;
    }
    if (pick < 0) {   // oldest held voice
        pick = 0;
        for (int k = 1; k < kSynthVoices; ++k)
            if (v_[k].on_age < v_[pick].on_age) pick = k;
    }
    startVoice(pick, note, vel, glide_c_ < 1.0f);
}

void Synth::monoOn(uint8_t note, uint8_t vel)
{
    const bool overlapped = nheld_ > 0;
    for (int i = 0; i < nheld_; ++i)
        if (held_[i].note == note) {
            for (int j = i; j + 1 < nheld_; ++j) held_[j] = held_[j + 1];
            --nheld_;
            break;
        }
    if (nheld_ >= kHeld) {   // full: forget the oldest
        for (int j = 0; j + 1 < nheld_; ++j) held_[j] = held_[j + 1];
        --nheld_;
    }
    held_[nheld_++] = {note, vel};
    const int nv = mode_ == kSynthUnison ? kSynthVoices : 1;
    const bool slide = legato_ && overlapped && v_[0].active && v_[0].gate;
    for (int k = 0; k < nv; ++k) {
        if (slide) slideVoice(k, note);
        else startVoice(k, note, vel, !legato_ && glide_c_ < 1.0f);
    }
}

void Synth::monoOff(uint8_t note)
{
    int idx = -1;
    for (int i = 0; i < nheld_; ++i)
        if (held_[i].note == note) idx = i;
    if (idx < 0) return;
    const bool top = idx == nheld_ - 1;
    for (int j = idx; j + 1 < nheld_; ++j) held_[j] = held_[j + 1];
    --nheld_;
    if (!top) return;
    const int nv = mode_ == kSynthUnison ? kSynthVoices : 1;
    if (nheld_ > 0) {   // back to the previous held note
        const Held h = held_[nheld_ - 1];
        for (int k = 0; k < nv; ++k) {
            if (legato_ && v_[k].active && v_[k].gate) slideVoice(k, h.note);
            else startVoice(k, h.note, h.vel, !legato_ && glide_c_ < 1.0f);
        }
    } else {
        for (int k = 0; k < nv; ++k)
            if (v_[k].gate) releaseVoice(v_[k]);
    }
}

void Synth::noteOffNow(uint8_t note)
{
    if (mode_ != kSynthPoly) {
        monoOff(note);
        return;
    }
    for (Voice& v : v_)
        if (v.gate && v.note == note) releaseVoice(v);
}

// Starts (or retriggers) a voice: envelopes attack from their current level, pitch jumps or glides.
void Synth::startVoice(int idx, uint8_t note, uint8_t vel, bool glide)
{
    Voice& v = v_[idx];
    const bool was = v.active != 0;
    const bool slide = glide && v.has_pitch;
    v.note = note;
    v.semis_t = (float)note;
    if (!slide) v.semis = v.semis_t;
    v.has_pitch = 1;
    v.vel = (float)vel * (1.0f / 127.0f);
    v.gate = 1;
    v.on_age = ++age_;
    v.astage = AS_ATTACK;
    v.ac = att_c_;
    v.at = 1.3f;
    v.alo = -1.0f;
    v.ahi = 1.0f;
    v.fstage = FS_ATTACK;
    v.lfade = fade_inc_ >= 1.0f ? 1.0f : 0.0f;
    if (lfo_key_) {
        v.lph = 0.0f;
        v.lr0 = v.lr1;
        v.lr1 = rndBi(v.lrng);
    }
    if (!was) {
        v.active = 1;
        v.s1 = v.s2 = v.s3 = v.s4 = 0.0f;
        v.dcx = v.dcy = 0.0f;
        v.o2_last = 0.0f;
    }
    voiceControl(idx, false, !slide || !was, !was);
}

// Legato: new pitch without retriggering the envelopes (glides when GLIDE > 0).
void Synth::slideVoice(int idx, uint8_t note)
{
    Voice& v = v_[idx];
    v.note = note;
    v.semis_t = (float)note;
    if (glide_c_ >= 1.0f) {
        v.semis = v.semis_t;
        voiceControl(idx, false, true, false);
    }
}

void Synth::releaseVoice(Voice& v)
{
    v.gate = 0;
    v.off_age = ++age_;
    if (v.astage != AS_DONE) {
        v.astage = AS_RELEASE;
        v.ac = rel_c_;
        v.at = -0.01f * v.ax;
        v.alo = kAmpFloor;
        v.ahi = 2.0f;
        if (v.ax < kAmpFloor) ampFinish(v);
    }
    if (v.fstage != FS_DONE) {
        v.fstage = FS_RELEASE;
        v.ftgt = -0.01f * v.fx;
        if (v.fx <= 0.0f) {
            v.fx = 0.0f;
            v.fstage = FS_DONE;
        }
    }
}

void Synth::releaseAll()
{
    for (Voice& v : v_)
        if (v.gate) releaseVoice(v);
}

ORBIT_HOT void Synth::freeVoice(Voice& v)
{
    v.active = 0;
    v.gate = 0;
    ampFinish(v);
    v.fstage = FS_DONE;
    v.fx = 0.0f;
    v.s1 = v.s2 = v.s3 = v.s4 = 0.0f;
    v.dcx = v.dcy = 0.0f;
    v.o2_last = 0.0f;
    v.gl = v.gr = v.dgl = v.dgr = 0.0f;
}

void Synth::killAll()
{
    for (Voice& v : v_) freeVoice(v);
    nheld_ = 0;
}

// ------------------------------------------------------------------ amp envelope segments

// (inlined: a call inside the voice loop would force its registers into the few callee-saved ones)
SYNTH_AI void Synth::ampFinish(Voice& v)
{
    v.astage = AS_DONE;
    v.ax = v.ac = v.at = 0.0f;
    v.alo = -1.0f;
    v.ahi = 2.0f;
}

SYNTH_AI void Synth::ampDecayTarget(Voice& v)
{
    const float s = ks_[K_ASUS];
    if (v.ax > s) {
        v.at = s - 0.01f * (1.0f - s);
        v.alo = s;
        v.ahi = 2.0f;
    } else {   // sustain raised above the level: rise to it
        v.at = s + 0.01f;
        v.alo = -1.0f;
        v.ahi = s;
    }
}

SYNTH_AI void Synth::ampEnterDecay(Voice& v)
{
    if (ks_[K_ASUS] >= 1.0f && v.ax >= 1.0f) {
        v.astage = AS_SUSTAIN;
        v.ax = 1.0f;
        v.ac = 0.0f;
        v.at = 1.0f;
        v.alo = -1.0f;
        v.ahi = 2.0f;
        return;
    }
    v.astage = AS_DECAY;
    v.ac = dec_c_;
    ampDecayTarget(v);
}

SYNTH_AI void Synth::ampSegmentEnd(Voice& v)
{
    switch (v.astage) {
    case AS_ATTACK:
        v.ax = 1.0f;
        ampEnterDecay(v);
        break;
    case AS_DECAY:
        v.ax = ks_[K_ASUS];
        v.astage = AS_SUSTAIN;
        v.ac = 0.0f;
        v.at = v.ax;
        v.alo = -1.0f;
        v.ahi = 2.0f;
        if (v.ax < kAmpFloor) ampFinish(v);
        break;
    case AS_RELEASE: ampFinish(v); break;
    default:
        v.alo = -1.0f;
        v.ahi = 2.0f;
        break;
    }
}

// ------------------------------------------------------------------ control rate

// Values derived from the smoothed knobs; the costly ones (deriveSlow) only when their inputs change.
ORBIT_HOT void Synth::derive()
{
    const float* k = ks_;
    const bool all = !derived_;
    if (all || w1_ != dw1_ || w2_ != dw2_ || k[K_COLOR1] != kd_[K_COLOR1] || k[K_COLOR2] != kd_[K_COLOR2] ||
        k[K_RESO] != kd_[K_RESO] || mode_ != dmode_ || k[K_DETUNE] != kd_[K_DETUNE] || k[K_SPREAD] != kd_[K_SPREAD])
        deriveSlow();
    l1_ = k[K_LEVEL1] * norm1_;
    l2_ = k[K_LEVEL2] * norm2_;
    ls_ = k[K_SUB];
    ln_ = k[K_NOISE];
    xm_ = kXmodDepth * k[K_XMOD];

    cut_l2_ = kCutLog2Min + kCutRange * k[K_CUTOFF];
    const float drive = k[K_DRIVE];
    dg_ = 1.0f + kDriveGain * drive;
    dcm_ = (1.0f + 0.2f * (dg_ - 1.0f)) / dg_;
    dw_ = minf(1.0f, 20.0f * drive);
    lpitch_ = kLfoPitchSemis * k[K_LPITCH] * k[K_LPITCH];
    out_gain_ = kOutGain * k[K_LEVEL] * (mode_ == kSynthUnison ? kUniGain : 1.0f);
}

// COLOR, RESO and voice-mode / detune / spread constants (exp, log, sin approximations). Runs only
// while one of those moves, so it stays out of IRAM.
void Synth::deriveSlow()
{
    const float* k = ks_;
    Color c = oscColor(w1_, k[K_COLOR1]);
    ra1_ = c.ra;
    rb1_ = c.rb;
    lb1_ = c.lb;
    pwb1_ = c.pwb;
    fg1_ = c.fg;
    fn1_ = c.fn;
    norm1_ = c.norm;
    c = oscColor(w2_, k[K_COLOR2]);
    ra2_ = c.ra;
    rb2_ = c.rb;
    lb2_ = c.lb;
    pwb2_ = c.pwb;
    fg2_ = c.fg;
    fn2_ = c.fn;
    norm2_ = c.norm;

    const float r = k[K_RESO];
    klad_ = kLadderK * r;
    mklad_ = 1.0f + kLadderMakeup * klad_;
    ksvf_ = 2.0f - (2.0f - kSvfDampMin) * r;
    svfcomp_ = minf(1.0f, fastExp2(kSvfCompExp * fastLog2(ksvf_ * 0.70710678f)));

    const bool uni = mode_ == kSynthUnison;
    for (int i = 0; i < kSynthVoices; ++i) {
        uni_[i] = uni ? kUniPos[i] * kUniSemis * k[K_DETUNE] : 0.0f;
        const float pos = uni ? kUniPos[i] : (mode_ == kSynthPoly ? kPolyPan[i] * kPolySpread : 0.0f);
        const float pan = pos * k[K_SPREAD];
        if (pan == 0.0f) {
            panl_[i] = panr_[i] = 1.0f;
        } else {
            float l, rr;
            panConstantPower(pan, l, rr);
            panl_[i] = l * kSqrt2;
            panr_[i] = rr * kSqrt2;
        }
    }

    dw1_ = w1_;
    dw2_ = w2_;
    dmode_ = mode_;
    kd_[K_COLOR1] = k[K_COLOR1];
    kd_[K_COLOR2] = k[K_COLOR2];
    kd_[K_RESO] = r;
    kd_[K_DETUNE] = k[K_DETUNE];
    kd_[K_SPREAD] = k[K_SPREAD];
    derived_ = true;
}

ORBIT_HOT void Synth::control()
{
    if (panic_) {
        if (fade_ <= 0.0f) {   // the fade ended with the last period: reset, then the pending notes
            killAll();
            panic_ = false;
            fade_ = 1.0f;
            const int np = npend_;
            npend_ = 0;
            for (int i = 0; i < np; ++i) {
                if (mode_ == kSynthPoly) polyOn(pend_[i].note, pend_[i].vel);
                else monoOn(pend_[i].note, pend_[i].vel);
            }
        } else {
            fade_ = maxf(0.0f, fade_ - fade_step_ * (float)kCtl);
        }
    }
    for (int i = 0; i < K_COUNT; ++i) follow(ks_[i], kt_[i], knob_c_);
    derive();

    glph_ += lfo_inc_;   // global LFO (KEY TRIG off)
    if (glph_ >= 1.0f) {
        glph_ -= (float)floorToInt(glph_);
        glr0_ = glr1_;
        glr1_ = rndBi(grng_);
    }
    glfo_ = lfoShape(lfo_wave_, glph_, glr0_, glr1_);

    for (int i = 0; i < kSynthVoices; ++i)
        if (v_[i].active) voiceControl(i, true, false, false);
}

// One voice's control period: advance (glide, LFO, envelopes) when `advance`, then pitch, pulse
// width, filter coefficients and output gains for the next kCtl samples (values at the end of the
// period; jump_* set them at once instead of ramping).
ORBIT_HOT void Synth::voiceControl(int idx, bool advance, bool jump_pitch, bool jump_gain)
{
    Voice& v = v_[idx];
    if (advance) {
        if (v.semis != v.semis_t) {
            v.semis += glide_c_ * (v.semis_t - v.semis);
            if (absf(v.semis - v.semis_t) < 1.0e-4f) v.semis = v.semis_t;
        }
        if (lfo_key_) {
            v.lph += lfo_inc_;
            if (v.lph >= 1.0f) {
                v.lph -= (float)floorToInt(v.lph);
                v.lr0 = v.lr1;
                v.lr1 = rndBi(v.lrng);
            }
        }
        v.lfade = minf(1.0f, v.lfade + fade_inc_);
        switch (v.astage) {   // amp envelope coefficients follow the knobs
        case AS_ATTACK: v.ac = att_c_; break;
        case AS_DECAY:
            v.ac = dec_c_;
            ampDecayTarget(v);
            break;
        case AS_SUSTAIN:
            if (v.ax != ks_[K_ASUS]) ampEnterDecay(v);   // sustain knob moved: glide there
            break;
        case AS_RELEASE: v.ac = rel_c_; break;
        default: break;
        }
        const float fs = ks_[K_FSUS];
        switch (v.fstage) {   // filter envelope
        case FS_ATTACK:
            v.fx += fatt_c_ * (1.3f - v.fx);
            if (v.fx >= 1.0f) {
                v.fx = 1.0f;
                v.fstage = FS_DECAY;
            }
            break;
        case FS_DECAY:
            if (v.fx > fs) {
                v.fx += fdec_c_ * (fs - 0.01f * (1.0f - fs) - v.fx);
                if (v.fx <= fs) {
                    v.fx = fs;
                    v.fstage = FS_SUSTAIN;
                }
            } else {
                v.fx += fdec_c_ * (fs + 0.01f - v.fx);
                if (v.fx >= fs) {
                    v.fx = fs;
                    v.fstage = FS_SUSTAIN;
                }
            }
            break;
        case FS_SUSTAIN: v.fx = fs; break;
        case FS_RELEASE:
            v.fx += frel_c_ * (v.ftgt - v.fx);
            if (v.fx <= 0.0f) {
                v.fx = 0.0f;
                v.fstage = FS_DONE;
            }
            break;
        default: break;
        }
    }
    const float* k = ks_;
    float lfo = lfo_key_ ? lfoShape(lfo_wave_, v.lph, v.lr0, v.lr1) : glfo_;
    lfo *= v.lfade;
    v.lfo = lfo;

    // pitch (phase increments ramp across the period)
    const float pitch = v.semis + k[K_TUNE] + k[K_BEND] + uni_[idx] + lfo * lpitch_;
    const float base = 440.0f * inv_fs_ * fastExp2((pitch - 69.0f) * (1.0f / 12.0f));
    const float d1 = clampf(base * ra1_, kDtMin, kDtMax);
    const float o2 = k[K_COARSE] + 0.01f * k[K_FINE] + kOsc2EnvSemis * k[K_O2ENV] * v.fx;
    const float d2 = clampf(base * fastExp2(o2 * (1.0f / 12.0f)) * ra2_, kDtMin, kDtMax);
    const float pwl = kLfoPw * k[K_LPW] * lfo;
    const float p1 = clampf(pwb1_ + pwl, kPwMin, 1.0f - kPwMin);
    const float p2 = clampf(pwb2_ + pwl, kPwMin, 1.0f - kPwMin);
    if (jump_pitch) {
        v.dt1 = d1;
        v.dt2 = d2;
        v.pw1 = p1;
        v.pw2 = p2;
        v.ddt1 = v.ddt2 = v.dpw1 = v.dpw2 = 0.0f;
    } else {
        v.ddt1 = (d1 - v.dt1) * kInvCtl;
        v.ddt2 = (d2 - v.dt2) * kInvCtl;
        v.dpw1 = (p1 - v.pw1) * kInvCtl;
        v.dpw2 = (p2 - v.pw2) * kInvCtl;
    }

    // filter
    const float oct = cut_l2_ + k[K_KEYF] * (v.semis + k[K_TUNE] - kKeyRef) * (1.0f / 12.0f) + kEnvOct * k[K_ENV] * v.fx +
                      kLfoOct * k[K_LCUT] * lfo + kVelOct * k[K_VELCUT] * (v.vel - 1.0f);
    const float fcn = clampf(fastExp2(oct) * inv_fs_, kFcMinHz * inv_fs_, kFcMaxNorm);
    v.fc_hz = fcn * fs_;
    const float g = fastTanPi(fcn);
    v.ft = ft_;
    if (ft_ == kSynthLp24) {
        const float G = g / (1.0f + g);
        const float G2 = G * G;
        const float inv = 1.0f / (1.0f + klad_ * G2 * G2);
        v.c0 = G;
        v.c1 = 0.0f;   // (unused by the ladder)
        v.c2 = kFilterIn * inv;
        v.c3 = klad_ * inv * (1.0f - G);   // (1 - G) of S folded in: one multiply less in the loop
        v.c4 = mklad_;
    } else {
        const float gk = g + ksvf_;
        v.c0 = g;
        v.c1 = gk;
        v.c2 = 1.0f / (1.0f + g * gk);
        v.c3 = kFilterIn;
        v.c4 = svfcomp_;
    }

    // VCA gain x level x tremolo x panic fade x pan
    const float trem = 1.0f - 0.5f * k[K_LAMP] * (v.lfade - lfo);
    const float amp = out_gain_ * (1.0f - k[K_VELAMP] * (1.0f - v.vel)) * trem * fade_;
    const float tl = amp * panl_[idx], tr = amp * panr_[idx];
    if (jump_gain) {
        v.gl = tl;
        v.gr = tr;
        v.dgl = v.dgr = 0.0f;
    } else {
        v.dgl = (tl - v.gl) * kInvCtl;
        v.dgr = (tr - v.gr) * kInvCtl;
    }
}

// ------------------------------------------------------------------ audio

// Renders [from, to) in pieces that end at control-period boundaries. While nothing sounds the
// grid stops; the next event starts a new control period at its frame.
ORBIT_HOT void Synth::run(float* outL, float* outR, int from, int to)
{
    int i = from;
    while (i < to) {
        if (!panic_ && !anyActive()) {
            ctl_left_ = 0;
            return;
        }
        if (ctl_left_ <= 0) {
            control();
            ctl_left_ = kCtl;
            continue;   // a panic may just have ended
        }
        int m = to - i;
        if (m > ctl_left_) m = ctl_left_;
        renderChunk(outL + i, outR + i, m);
        ctl_left_ -= m;
        i += m;
    }
}

// m <= kCtl samples (at most one control period) of every active voice, in passes so that each
// loop's state fits in the FPU registers: 1) oscillators + mixer per voice into buf[]; 2) filter in
// place: LPF -24 voices two at a time (their ladder chains interleave, which hides the FPU latency
// on the in-order core), LPF / HPF -12 per voice; 3) drive -> DC blocker -> VCA -> pan, added to
// out in voice order.
ORBIT_HOT void Synth::renderChunk(float* outL, float* outR, int m)
{
    float buf[kSynthVoices][kCtl];
    int act[kSynthVoices], lad[kSynthVoices];
    int na = 0, nl = 0;
    for (int k = 0; k < kSynthVoices; ++k) {
        Voice& v = v_[k];
        if (!v.active) continue;
        oscillators(v, buf[na], m);
        if (v.ft == kSynthLp24) lad[nl++] = na;
        else svf(v, buf[na], m);
        act[na++] = k;
    }
    int j = 0;
    for (; j + 1 < nl; j += 2) ladder2(v_[act[lad[j]]], buf[lad[j]], v_[act[lad[j + 1]]], buf[lad[j + 1]], m);
    if (j < nl) ladder1(v_[act[lad[j]]], buf[lad[j]], m);
    for (int k = 0; k < na; ++k) {
        Voice& v = v_[act[k]];
        outPass(v, buf[k], outL, outR, m);
        if (v.astage == AS_DONE) freeVoice(v);   // the VCA is last: exactly 0 from here on
    }
}

// OSC1 (wave W; XM: frequency x (1 + XMOD osc2[i - 1]) from b[]) -> a[], wrap offsets -> wr[].
template <uint8_t W, bool XM>
ORBIT_HOT void Synth::osc1Pass(Voice& v, float* __restrict a, float* __restrict wr, const float* __restrict b,
                               float o2_prev, int m)
{
    Osc o = v.o1;
    float dt = v.dt1, pw = v.pw1;
    const float ddt = v.ddt1, dpw = v.dpw1;
    const float rb = rb1_, lb = W == kSynthSaw ? lb1_ : 0.0f, xm = xm_;
    float o2l = o2_prev;
    for (int i = 0; i < m; ++i) {
        float d = dt;
        if (XM) {
            d = dt * (1.0f + xm * o2l);
            d = d > 0.0f ? (d < kDtMax ? d : kDtMax) : 0.0f;
            o2l = b[i];
        }
        float w;
        a[i] = oscTick(o, W, d, d * rb, lb, pw, -1.0f, w);
        wr[i] = w;
        dt += ddt;
        pw += dpw;
    }
    v.o1 = o;
    v.dt1 = dt;
    v.pw1 = pw;
}

// OSC2 (wave W; wr: OSC1's wrap offsets for SYNC, or null) -> b[].
template <uint8_t W>
ORBIT_HOT void Synth::osc2Pass(Voice& v, float* __restrict b, const float* __restrict wr, int m)
{
    Osc o = v.o2;
    float dt = v.dt2, pw = v.pw2;
    const float ddt = v.ddt2, dpw = v.dpw2;
    const float rb = rb2_, lb = W == kSynthSaw ? lb2_ : 0.0f;
    for (int i = 0; i < m; ++i) {
        float w;
        b[i] = oscTick(o, W, dt, dt * rb, lb, pw, wr ? wr[i] : -1.0f, w);
        dt += ddt;
        pw += dpw;
    }
    v.o2 = o;
    v.dt2 = dt;
    v.pw2 = pw;
}

// Oscillators + mixer of one voice into x[] (m <= kCtl). Each oscillator runs in its own loop,
// specialized for its wave (no per-sample wave dispatch, few live values): OSC1 also records its
// wrap offsets (SUB, SYNC), OSC2 reads them for SYNC; with XMOD (no SYNC) OSC2 runs first and
// OSC1 reads OSC2's previous sample. SYNC + XMOD together need both per sample: oscCoupled().
// (0.4.9: defined after osc1Pass / osc2Pass: a template first used before its ORBIT_HOT definition is
// instantiated without the section attribute, so those passes landed in flash.)
ORBIT_HOT void Synth::oscillators(Voice& v, float* __restrict x, int m)
{
    const bool xm = xm_ > 0.0f;
    if (xm && sync_) {
        oscCoupled(v, x, m);
        return;
    }
    float a[kCtl], b[kCtl], wr[kCtl];
    const float o2_prev = v.o2_last;
    if (xm) {
        switch (w2_) {
        case kSynthSquare: osc2Pass<kSynthSquare>(v, b, nullptr, m); break;
        case kSynthTriangle: osc2Pass<kSynthTriangle>(v, b, nullptr, m); break;
        default: osc2Pass<kSynthSaw>(v, b, nullptr, m); break;
        }
        switch (w1_) {
        case kSynthSquare: osc1Pass<kSynthSquare, true>(v, a, wr, b, o2_prev, m); break;
        case kSynthTriangle: osc1Pass<kSynthTriangle, true>(v, a, wr, b, o2_prev, m); break;
        default: osc1Pass<kSynthSaw, true>(v, a, wr, b, o2_prev, m); break;
        }
    } else {
        switch (w1_) {
        case kSynthSquare: osc1Pass<kSynthSquare, false>(v, a, wr, nullptr, o2_prev, m); break;
        case kSynthTriangle: osc1Pass<kSynthTriangle, false>(v, a, wr, nullptr, o2_prev, m); break;
        default: osc1Pass<kSynthSaw, false>(v, a, wr, nullptr, o2_prev, m); break;
        }
        const float* sw = sync_ ? wr : nullptr;
        switch (w2_) {
        case kSynthSquare: osc2Pass<kSynthSquare>(v, b, sw, m); break;
        case kSynthTriangle: osc2Pass<kSynthTriangle>(v, b, sw, m); break;
        default: osc2Pass<kSynthSaw>(v, b, sw, m); break;
        }
    }
    v.o2_last = b[m - 1];
    mixPass(v, a, b, wr, x, m);
}

// TRIANGLE folds, RING, SUB (toggles on OSC1's wraps, same one-sample latency), NOISE -> mixer.
ORBIT_HOT void Synth::mixPass(Voice& v, const float* __restrict a, const float* __restrict b, const float* __restrict wr,
                              float* __restrict x, int m)
{
    const float fg1 = w1_ == kSynthTriangle ? fg1_ : 0.0f, fg2 = w2_ == kSynthTriangle ? fg2_ : 0.0f;
    const float fn1 = fn1_, fn2 = fn2_;
    const bool ring = ring_ != 0;
    const float l1 = l1_, l2 = l2_, ls = ls_, ln = ln_;
    const uint8_t subdiv = subdiv_;
    float sub_y1 = v.sub_y1;
    uint8_t sub_hi = v.sub_hi, sub_cnt = v.sub_cnt;
    uint32_t nr = v.nrng;
    for (int i = 0; i < m; ++i) {
        float av = a[i], bv = b[i];
        if (fg1 > 0.0f) av = foldSine(av, fg1, fn1);
        if (fg2 > 0.0f) bv = foldSine(bv, fg2, fn2);
        float s = l1 * av + l2 * (ring ? av * bv : bv);
        if (ls > 0.0f) {
            float sc = 0.0f;
            const float wrap = wr[i];
            if (wrap >= 0.0f && ++sub_cnt >= subdiv) {
                sub_cnt = 0;
                blep(sub_y1, sc, sub_hi ? -2.0f : 2.0f, wrap);
                sub_hi ^= 1;
            }
            sc += sub_hi ? 1.0f : -1.0f;
            s += ls * sub_y1;
            sub_y1 = sc;
        }
        if (ln > 0.0f) {
            nr ^= nr << 13;
            nr ^= nr >> 17;
            nr ^= nr << 5;
            s += ln * ((float)(int32_t)nr * 4.656612873e-10f);
        }
        x[i] = s;
    }
    v.sub_y1 = sub_y1;
    v.sub_hi = sub_hi;
    v.sub_cnt = sub_cnt;
    v.nrng = nr;
}

// SYNC + XMOD: both oscillators per sample in one generic loop (same math as the passes above).
// A rare combination, so it runs from flash and keeps IRAM for the common paths.
void Synth::oscCoupled(Voice& v, float* __restrict xo, int m)
{
    Osc o1 = v.o1, o2 = v.o2;
    const uint8_t w1 = w1_, w2 = w2_;
    float dt1 = v.dt1, dt2 = v.dt2, pw1 = v.pw1, pw2 = v.pw2;
    const float ddt1 = v.ddt1, ddt2 = v.ddt2, dpw1 = v.dpw1, dpw2 = v.dpw2;
    const float rb1 = rb1_, rb2 = rb2_;
    const float lb1 = w1 == kSynthSaw ? lb1_ : 0.0f, lb2 = w2 == kSynthSaw ? lb2_ : 0.0f;
    const float fg1 = w1 == kSynthTriangle ? fg1_ : 0.0f, fg2 = w2 == kSynthTriangle ? fg2_ : 0.0f;
    const float fn1 = fn1_, fn2 = fn2_;
    const float xm = xm_;
    const bool sync = sync_ != 0, ring = ring_ != 0;
    const float l1 = l1_, l2 = l2_, ls = ls_, ln = ln_;
    const uint8_t subdiv = subdiv_;
    float sub_y1 = v.sub_y1;
    uint8_t sub_hi = v.sub_hi, sub_cnt = v.sub_cnt;
    uint32_t nr = v.nrng;
    float o2l = v.o2_last;
    for (int i = 0; i < m; ++i) {
        float d1 = dt1;
        if (xm > 0.0f) {
            d1 = dt1 * (1.0f + xm * o2l);
            d1 = d1 > 0.0f ? (d1 < kDtMax ? d1 : kDtMax) : 0.0f;
        }
        float wrap, wrap2;
        float a = oscTick(o1, w1, d1, d1 * rb1, lb1, pw1, -1.0f, wrap);
        float b = oscTick(o2, w2, dt2, dt2 * rb2, lb2, pw2, sync ? wrap : -1.0f, wrap2);
        o2l = b;
        dt1 += ddt1;
        dt2 += ddt2;
        pw1 += dpw1;
        pw2 += dpw2;
        if (fg1 > 0.0f) a = foldSine(a, fg1, fn1);
        if (fg2 > 0.0f) b = foldSine(b, fg2, fn2);
        float x = l1 * a + l2 * (ring ? a * b : b);
        if (ls > 0.0f) {   // sub: toggles on OSC1's wraps, same one-sample latency
            float sc = 0.0f;
            if (wrap >= 0.0f && ++sub_cnt >= subdiv) {
                sub_cnt = 0;
                blep(sub_y1, sc, sub_hi ? -2.0f : 2.0f, wrap);
                sub_hi ^= 1;
            }
            sc += sub_hi ? 1.0f : -1.0f;
            x += ls * sub_y1;
            sub_y1 = sc;
        }
        if (ln > 0.0f) {
            nr ^= nr << 13;
            nr ^= nr >> 17;
            nr ^= nr << 5;
            x += ln * ((float)(int32_t)nr * 4.656612873e-10f);
        }
        xo[i] = x;
    }
    v.o1 = o1;
    v.o2 = o2;
    v.dt1 = dt1;
    v.dt2 = dt2;
    v.pw1 = pw1;
    v.pw2 = pw2;
    v.sub_y1 = sub_y1;
    v.sub_hi = sub_hi;
    v.sub_cnt = sub_cnt;
    v.nrng = nr;
    v.o2_last = o2l;
}

// LPF -24 ladder, one sample: closed-form input of the linear loop (S = the stage states' part of
// the output / (1 - G); IS = k (1 - G) / (1 + k G^4), IX = input gain / (1 + k G^4)), soft clipped,
// then the four TPT stages. Its loop-carried chain (~17 dependent operations) sets the cost.
static SYNTH_AI float ladderTick(float x, float& s1, float& s2, float& s3, float& s4, float G, float IX, float IS,
                                 float MK)
{
    const float S = ((s1 * G + s2) * G + s3) * G + s4;
    const float u = sat(IX * x - IS * S);
    float t = (u - s1) * G;
    const float y1 = t + s1;
    s1 = y1 + t;
    t = (y1 - s2) * G;
    const float y2 = t + s2;
    s2 = y2 + t;
    t = (y2 - s3) * G;
    const float y3 = t + s3;
    s3 = y3 + t;
    t = (y3 - s4) * G;
    const float y4 = t + s4;
    s4 = y4 + t;
    return y4 * MK;
}

// Two LPF -24 voices in one loop (in place: mixer -> ladder output).
ORBIT_HOT SYNTH_NOINLINE void Synth::ladder2(Voice& a, float* __restrict xa, Voice& b, float* __restrict xb, int m)
{
    float a1 = a.s1, a2 = a.s2, a3 = a.s3, a4 = a.s4;
    float b1 = b.s1, b2 = b.s2, b3 = b.s3, b4 = b.s4;
    const float aG = a.c0, aX = a.c2, aS = a.c3, aM = a.c4;
    const float bG = b.c0, bX = b.c2, bS = b.c3, bM = b.c4;
    for (int i = 0; i < m; ++i) {
        const float ya = ladderTick(xa[i], a1, a2, a3, a4, aG, aX, aS, aM);
        const float yb = ladderTick(xb[i], b1, b2, b3, b4, bG, bX, bS, bM);
        xa[i] = ya;
        xb[i] = yb;
    }
    a.s1 = a1;
    a.s2 = a2;
    a.s3 = a3;
    a.s4 = a4;
    b.s1 = b1;
    b.s2 = b2;
    b.s3 = b3;
    b.s4 = b4;
}

ORBIT_HOT SYNTH_NOINLINE void Synth::ladder1(Voice& v, float* __restrict x, int m)
{
    float s1 = v.s1, s2 = v.s2, s3 = v.s3, s4 = v.s4;
    const float G = v.c0, IX = v.c2, IS = v.c3, MK = v.c4;
    for (int i = 0; i < m; ++i) x[i] = ladderTick(x[i], s1, s2, s3, s4, G, IX, IS, MK);
    v.s1 = s1;
    v.s2 = s2;
    v.s3 = s3;
    v.s4 = s4;
}

// LPF / HPF -12: TPT state variable filter with a soft clipped input (in place).
ORBIT_HOT SYNTH_NOINLINE void Synth::svf(Voice& v, float* __restrict x, int m)
{
    const bool high = v.ft == kSynthHp12;
    float s1 = v.s1, s2 = v.s2;
    const float g = v.c0, gk = v.c1, a1 = v.c2, gin = v.c3, comp = v.c4;
    for (int i = 0; i < m; ++i) {
        const float u = sat(gin * x[i]);
        const float hp = (u - gk * s1 - s2) * a1;
        const float t1 = g * hp;
        const float bp = t1 + s1;
        s1 = bp + t1;
        const float t2 = g * bp;
        const float lp = t2 + s2;
        s2 = lp + t2;
        x[i] = (high ? hp : lp) * comp;
    }
    v.s1 = s1;
    v.s2 = s2;
}

// Drive -> DC blocker -> VCA (last: exact silence once the envelope ends) -> pan, added to out.
ORBIT_HOT SYNTH_NOINLINE void Synth::outPass(Voice& v, const float* __restrict yi, float* __restrict outL,
                                             float* __restrict outR, int m)
{
    const bool drive = dw_ > 0.0f;
    const float dg = dg_, dcm = dcm_, dw = dw_;
    float dcx = v.dcx, dcy = v.dcy;
    const float dcr = dc_r_;
    float ex = v.ax, ec = v.ac, et = v.at, elo = v.alo, ehi = v.ahi;
    float gl = v.gl, gr = v.gr;
    const float dgl = v.dgl, dgr = v.dgr;
    for (int i = 0; i < m; ++i) {
        float y = yi[i];
        if (drive) y += dw * (sat(y * dg) * dcm - y);
        const float dc = y - dcx + dcr * dcy;
        dcx = y;
        dcy = dc;
        ex += ec * (et - ex);
        if (ex > ehi || ex < elo) {
            v.ax = ex;
            ampSegmentEnd(v);
            ex = v.ax;
            ec = v.ac;
            et = v.at;
            elo = v.alo;
            ehi = v.ahi;
        }
        const float o = dc * ex;
        outL[i] += o * gl;
        outR[i] += o * gr;
        gl += dgl;
        gr += dgr;
    }
    v.dcx = dcx;
    v.dcy = dcy;
    v.ax = ex;
    v.ac = ec;
    v.at = et;
    v.alo = elo;
    v.ahi = ehi;
    v.gl = gl;
    v.gr = gr;
}

}  // namespace orbit
