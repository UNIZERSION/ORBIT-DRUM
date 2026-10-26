#pragma once
// Parameter table: one place that knows every editable continuous/discrete parameter, its range,
// default, knob curve and display format. Knobs, MIDI learn, persistence and the engine all use it.
// Values are stored in real units (ms, Hz, dB, semitones...) as float in Pad::p[] / Project::g[].
#include <stdint.h>
#include "config.h"

namespace orbit {

// Per-pad parameters (Pad::p[]). Grouped by the SAMPLE editor tab that shows them.
enum PadParam : uint8_t {
    // TRIM
    PP_START = 0,     // 0..1 of sample length (play region start)
    PP_END,           // 0..1 (play region end, > start)
    PP_FADE_IN,       // ms 0..1000
    PP_FADE_OUT,      // ms 0..1000 (before region end)
    PP_GAIN,          // dB -24..+24 (sample trim gain)
    PP_LOOP,          // 0..1 loop start inside region (PlayMode::Loop)
    // ENVELOPE (amp)
    PP_ATTACK,        // ms 0..2000
    PP_HOLD,          // ms 0..2000
    PP_DECAY,         // ms 1..8000
    PP_SUSTAIN,       // 0..1
    PP_RELEASE,       // ms 1..8000
    PP_VEL_AMP,       // 0..1 velocity -> amplitude depth
    // FILTER
    PP_CUTOFF,        // Hz 20..20000 (exp)
    PP_RESO,          // 0..1
    PP_DRIVE,         // 0..1 pre-filter saturation
    PP_FTYPE,         // enum FilterType (discrete)
    PP_FENV,          // -1..1 filter envelope depth (x 6 octaves)
    PP_FDECAY,        // ms 1..8000 filter envelope decay
    // PITCH
    PP_TUNE,          // semitones -24..+24
    PP_FINE,          // cents -100..+100
    PP_PENV,          // semitones -48..+48 pitch envelope depth
    PP_PDECAY,        // ms 1..2000 pitch envelope decay
    PP_BITS,          // 4..16 bit crush (16 = off)
    PP_DOWNSAMPLE,    // 1..32 sample-and-hold factor (1 = off)
    // LFO
    PP_LFO_RATE,      // Hz 0.05..30. With PP_LFO_SYNC = 1 the knob position picks a SyncDiv:
                      //   index = round(toNorm(spec, rate) * 11) (0 = 4/1 slowest .. 11 = 1/32)
    PP_LFO_DEPTH,     // 0..1
    PP_LFO_SHAPE,     // enum LfoShape
    PP_LFO_TARGET,    // enum LfoTarget
    PP_LFO_SYNC,      // 0/1 tempo sync
    PP_LFO_FADE,      // ms 0..4000 fade-in after trigger
    // MIX (right macro panel: VOLUME / FX SEND / PAN)
    PP_VOLUME,        // dB -60..+6
    PP_PAN,           // -1..+1
    PP_SEND,          // 0..1 FX send (reverb + delay bus)
    kPadParamCount
};

// Global parameters (Project::g[]): FX view, master, metronome.
enum GlobalParam : uint8_t {
    GP_MASTER = 0,    // dB -60..+6
    GP_REV_SIZE,      // 0..1
    GP_REV_DECAY,     // s 0.2..12
    GP_REV_DAMP,      // 0..1 (high damping)
    GP_REV_PREDELAY,  // ms 0..200
    GP_REV_LEVEL,     // 0..1 return level
    GP_DLY_DIV,       // enum DelayDiv (tempo synced)
    GP_DLY_FEEDBACK,  // 0..0.95
    GP_DLY_TONE,      // 0..1 (dark..bright)
    GP_DLY_LEVEL,     // 0..1 return level
    GP_DLY_PINGPONG,  // 0/1
    GP_LOFI_BITS,     // 4..16
    GP_LOFI_RATE,     // Hz 2000..48000 (exp)
    GP_LOFI_MIX,      // 0..1
    GP_WIDE,          // 0..1 stereo width boost
    GP_PUMP_AMT,      // 0..1 ducking depth
    GP_PUMP_RELEASE,  // ms 20..800
    GP_SPACE_AMT,     // 0..1 extra reverb send on all pads when SPACE is on
    GP_METRO_LEVEL,   // 0..1 metronome click level
    // ACID BASS (303 style, 0.3): the BASS view's knobs. The voice plays into mixer bus GP_BASS_BUS (fader, pan,
    // mute / solo and the bus FX send apply there too).
    GP_BASS_TUNE,     // semitones -12..+12
    GP_BASS_CUTOFF,   // 0..1
    GP_BASS_RESO,     // 0..1
    GP_BASS_ENVMOD,   // 0..1
    GP_BASS_DECAY,    // 0..1
    GP_BASS_ACCENT,   // 0..1
    GP_BASS_DRIVE,    // 0..1 distortion
    GP_BASS_LEVEL,    // dB -60..+6
    GP_BASS_WAVE,     // enum SAW / SQR
    GP_BASS_SEND,     // 0..1 FX send (reverb + delay bus)
    GP_BASS_BUS,      // enum mixer bus 1..8 (default 8)
    GP_BASS_MUTE,     // enum OFF / ON
    // SYNTH (0.4.9): the knobs of the 4-voice synth (core/synth.h), one patch per project; times in ms, the cutoff
    // in Hz. core/synthmap.cpp turns them into SynthParams once per block. Appended: older files keep defaults.
    GP_SYN_OSC1_WAVE,   // enum SAW / SQUARE / TRI
    GP_SYN_OSC1_COLOR,  // 0..1 (core/synth.h: COLOR per wave)
    GP_SYN_OSC1_LEVEL,  // 0..1
    GP_SYN_OSC2_WAVE,   // enum
    GP_SYN_OSC2_COLOR,  // 0..1
    GP_SYN_OSC2_LEVEL,  // 0..1
    GP_SYN_OSC2_COARSE, // semitones -24..+24 (integer)
    GP_SYN_OSC2_FINE,   // cents -50..+50
    GP_SYN_OSC2_ENV,    // -1..1: filter envelope -> OSC2 pitch (+-24 semitones)
    GP_SYN_SYNC,        // enum OFF / ON: OSC2 hard-synced to OSC1
    GP_SYN_RING,        // enum OFF / ON: OSC1 x OSC2 replaces OSC2
    GP_SYN_XMOD,        // 0..1 OSC2 -> OSC1 frequency
    GP_SYN_SUB_OCT,     // enum -1 OCT / -2 OCT
    GP_SYN_SUB_LEVEL,   // 0..1
    GP_SYN_NOISE,       // 0..1
    GP_SYN_TUNE,        // semitones -24..+24
    GP_SYN_FILTER,      // enum LPF 24 / LPF 12 / HPF 12
    GP_SYN_CUTOFF,      // Hz 20..20480 (exp)
    GP_SYN_RESO,        // 0..1
    GP_SYN_KEY_FOLLOW,  // 0..1
    GP_SYN_ENV_AMT,     // -1..1 filter envelope depth (+-8 octaves)
    GP_SYN_VEL_CUTOFF,  // 0..1 velocity -> cutoff
    GP_SYN_F_ATTACK,    // ms, filter envelope
    GP_SYN_F_DECAY,     // ms
    GP_SYN_F_SUSTAIN,   // 0..1
    GP_SYN_F_RELEASE,   // ms
    GP_SYN_A_ATTACK,    // ms, amp envelope
    GP_SYN_A_DECAY,     // ms
    GP_SYN_A_SUSTAIN,   // 0..1
    GP_SYN_A_RELEASE,   // ms
    GP_SYN_VEL_AMP,     // 0..1 velocity -> level
    GP_SYN_LFO_WAVE,    // enum TRI / SAW UP / SAW DN / SQUARE / S&H / RANDOM
    GP_SYN_LFO_RATE,    // Hz 0.05..30 (exp), used while LFO SYNC is FREE
    GP_SYN_LFO_SYNC,    // enum FREE, 4 BARS .. 1/32 (the synth tempo divisions)
    GP_SYN_LFO_FADE,    // ms 0..20000 depth fade-in after a note start
    GP_SYN_LFO_KEY,     // enum OFF / ON: each note restarts its voice LFO
    GP_SYN_LFO_PITCH,   // 0..1 depths
    GP_SYN_LFO_CUTOFF,
    GP_SYN_LFO_AMP,
    GP_SYN_LFO_PW,
    GP_SYN_MODE,        // enum POLY / MONO / UNISON
    GP_SYN_LEGATO,      // enum OFF / ON (MONO / UNISON)
    GP_SYN_GLIDE,       // ms 0..5000
    GP_SYN_DETUNE,      // 0..1 unison detune
    GP_SYN_SPREAD,      // 0..1 stereo spread
    GP_SYN_DRIVE,       // 0..1
    GP_SYN_LEVEL,       // dB -60..+6
    GP_SYN_SEND,        // 0..1 FX send (reverb + delay bus)
    GP_SYN_BUS,         // enum mixer bus 1..8 (default 7, named SYNTH in new projects)
    GP_SYN_MUTE,        // enum OFF / ON: the scene lines stay silent (live notes still play)
    kGlobalParamCount
};

enum class Curve : uint8_t { Lin, Exp, Pow2, Pow3, Int };
enum class Unit : uint8_t {
    Percent,    // 0..1 shown as "72 %"
    Ms,         // "50 ms" / "1.20 s"
    Seconds,    // "2.4 s"
    Hz,         // "4.2 kHz" / "180 Hz"
    Db,         // "-12.9 dB" / "-inf dB"
    Semis,      // "+7 st" / "0.0 st"
    Cents,      // "+12 ct"
    Pan,        // "C" / "L 20" / "R 20"
    Ratio,      // "0.29"
    Bits,       // "12 bit"
    Factor,     // "x4"
    Enum,       // names[]
    Plain,      // integer
};

struct ParamSpec {
    const char* label;   // knob caption, uppercase like the template ("CUTOFF")
    float min, max, def;
    Curve curve;
    Unit unit;
    uint8_t steps;       // 0 = continuous, otherwise number of discrete values (enums, ints)
    const char* const* names;  // for Unit::Enum
};

const ParamSpec& padSpec(PadParam p);
const ParamSpec& globalSpec(GlobalParam p);

// Knob mapping: normalized 0..1 <-> value, honouring the curve; discrete specs snap.
float toNorm(const ParamSpec& s, float value);
float fromNorm(const ParamSpec& s, float norm);
float clampParam(const ParamSpec& s, float value);
// Human-readable value as in the template ("4.2 kHz", "50 ms", "C", "-12.9 dB"). Returns length.
int formatParam(const ParamSpec& s, float value, char* buf, int len);

// Enumerations that live in parameters.
enum class FilterType : uint8_t { LowPass = 0, BandPass, HighPass, Off, Count };
enum class LfoShape : uint8_t { Sine = 0, Triangle, Saw, Square, SampleHold, Count };
enum class LfoTarget : uint8_t { Pitch = 0, Cutoff, Amp, Pan, Count };
// Tempo-synced divisions for LFO rate (sync on) and delay time.
// Ordered from slow to fast (monotonic), so a knob can sweep them.
enum class SyncDiv : uint8_t { D4_1 = 0, D2_1, D1_1, D1_2, D1_4D, D1_4, D1_8D, D1_8, D1_8T, D1_16, D1_16T, D1_32, Count };
float syncDivBeats(SyncDiv d);  // length in quarter notes (D1_4 = 1.0)
const char* syncDivName(SyncDiv d);

}  // namespace orbit
