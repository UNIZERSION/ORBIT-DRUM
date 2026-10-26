#include "params.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef ORBIT_HOT
#define ORBIT_HOT
#endif

namespace orbit {

namespace {

const char* const kFilterNames[] = {"LP", "BP", "HP", "OFF"};
const char* const kShapeNames[] = {"SINE", "TRI", "SAW", "SQR", "S&H"};
const char* const kTargetNames[] = {"PITCH", "CUTOFF", "AMP", "PAN"};
const char* const kOnOff[] = {"OFF", "ON"};
const char* const kSyncNames[] = {"4/1", "2/1", "1/1", "1/2", "1/4.", "1/4", "1/8.", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
const float kSyncBeats[] = {16.f, 8.f, 4.f, 2.f, 1.5f, 1.f, 0.75f, 0.5f, 1.f / 3.f, 0.25f, 1.f / 6.f, 0.125f};

// label, min, max, default, curve, unit, steps, names
const ParamSpec kPad[kPadParamCount] = {
    /* PP_START      */ {"START", 0.f, 1.f, 0.f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* PP_END        */ {"END", 0.f, 1.f, 1.f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* PP_FADE_IN    */ {"FADE IN", 0.f, 1000.f, 0.f, Curve::Pow3, Unit::Ms, 0, nullptr},
    /* PP_FADE_OUT   */ {"FADE OUT", 0.f, 1000.f, 0.f, Curve::Pow3, Unit::Ms, 0, nullptr},
    /* PP_GAIN       */ {"GAIN", -24.f, 24.f, 0.f, Curve::Lin, Unit::Db, 0, nullptr},
    /* PP_LOOP       */ {"LOOP", 0.f, 1.f, 0.f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* PP_ATTACK     */ {"ATTACK", 0.f, 2000.f, 0.f, Curve::Pow3, Unit::Ms, 0, nullptr},
    /* PP_HOLD       */ {"HOLD", 0.f, 2000.f, 0.f, Curve::Pow3, Unit::Ms, 0, nullptr},
    /* PP_DECAY      */ {"DECAY", 1.f, 8000.f, 1200.f, Curve::Exp, Unit::Ms, 0, nullptr},
    /* PP_SUSTAIN    */ {"SUSTAIN", 0.f, 1.f, 1.f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* PP_RELEASE    */ {"RELEASE", 1.f, 8000.f, 60.f, Curve::Exp, Unit::Ms, 0, nullptr},
    /* PP_VEL_AMP    */ {"VELOCITY", 0.f, 1.f, 0.8f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* PP_CUTOFF     */ {"CUTOFF", 20.f, 20000.f, 20000.f, Curve::Exp, Unit::Hz, 0, nullptr},
    /* PP_RESO       */ {"RESONANCE", 0.f, 1.f, 0.1f, Curve::Lin, Unit::Ratio, 0, nullptr},
    /* PP_DRIVE      */ {"DRIVE", 0.f, 1.f, 0.f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* PP_FTYPE      */ {"TYPE", 0.f, 3.f, 0.f, Curve::Int, Unit::Enum, 4, kFilterNames},
    /* PP_FENV       */ {"ENV AMT", -1.f, 1.f, 0.f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* PP_FDECAY     */ {"ENV DECAY", 1.f, 8000.f, 300.f, Curve::Exp, Unit::Ms, 0, nullptr},
    /* PP_TUNE       */ {"TUNE", -24.f, 24.f, 0.f, Curve::Lin, Unit::Semis, 0, nullptr},
    /* PP_FINE       */ {"FINE", -100.f, 100.f, 0.f, Curve::Lin, Unit::Cents, 0, nullptr},
    /* PP_PENV       */ {"P.ENV", -48.f, 48.f, 0.f, Curve::Lin, Unit::Semis, 0, nullptr},
    /* PP_PDECAY     */ {"P.DECAY", 1.f, 2000.f, 80.f, Curve::Exp, Unit::Ms, 0, nullptr},
    /* PP_BITS       */ {"BITS", 4.f, 16.f, 16.f, Curve::Int, Unit::Bits, 13, nullptr},
    /* PP_DOWNSAMPLE */ {"DOWNSAMPLE", 1.f, 32.f, 1.f, Curve::Int, Unit::Factor, 32, nullptr},
    /* PP_LFO_RATE   */ {"RATE", 0.05f, 30.f, 2.f, Curve::Exp, Unit::Hz, 0, nullptr},
    /* PP_LFO_DEPTH  */ {"DEPTH", 0.f, 1.f, 0.f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* PP_LFO_SHAPE  */ {"SHAPE", 0.f, 4.f, 0.f, Curve::Int, Unit::Enum, 5, kShapeNames},
    /* PP_LFO_TARGET */ {"TARGET", 0.f, 3.f, 1.f, Curve::Int, Unit::Enum, 4, kTargetNames},
    /* PP_LFO_SYNC   */ {"SYNC", 0.f, 1.f, 0.f, Curve::Int, Unit::Enum, 2, kOnOff},
    /* PP_LFO_FADE   */ {"FADE", 0.f, 4000.f, 0.f, Curve::Pow3, Unit::Ms, 0, nullptr},
    /* PP_VOLUME     */ {"VOLUME", -60.f, 6.f, 0.f, Curve::Lin, Unit::Db, 0, nullptr},
    /* PP_PAN        */ {"PAN", -1.f, 1.f, 0.f, Curve::Lin, Unit::Pan, 0, nullptr},
    /* PP_SEND       */ {"FX SEND", 0.f, 1.f, 0.f, Curve::Pow2, Unit::Db, 0, nullptr},
};

const char* const kWaveNames[] = {"SAW", "SQR"};
const char* const kBusNumbers[] = {"1", "2", "3", "4", "5", "6", "7", "8"};
// SYNTH (0.4.9)
const char* const kSynWaveNames[] = {"SAW", "SQUARE", "TRI"};
const char* const kSynSubNames[] = {"-1 OCT", "-2 OCT"};
const char* const kSynFilterNames[] = {"LPF 24", "LPF 12", "HPF 12"};
const char* const kSynLfoNames[] = {"TRI", "SAW UP", "SAW DN", "SQUARE", "S&H", "RANDOM"};
// FREE + the 13 tempo divisions of core/synth.cpp (kLfoDivs, synthLfoDivName)
const char* const kSynLfoSyncNames[] = {"FREE", "4 BARS", "2 BARS", "1 BAR", "1/2",  "1/4.",  "1/4",
                                        "1/4T", "1/8.",   "1/8",    "1/8T",  "1/16", "1/16T", "1/32"};
const char* const kSynModeNames[] = {"POLY", "MONO", "UNISON"};
const char* const kDelayDivs[] = {"4/1", "2/1", "1/1", "1/2", "1/4.", "1/4", "1/8.", "1/8", "1/8T", "1/16", "1/16T", "1/32"};

const ParamSpec kGlobal[kGlobalParamCount] = {
    /* GP_MASTER       */ {"MASTER", -60.f, 6.f, -1.f, Curve::Lin, Unit::Db, 0, nullptr},
    /* GP_REV_SIZE     */ {"SIZE", 0.f, 1.f, 0.55f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_REV_DECAY    */ {"DECAY", 0.2f, 12.f, 2.2f, Curve::Exp, Unit::Seconds, 0, nullptr},
    /* GP_REV_DAMP     */ {"DAMP", 0.f, 1.f, 0.45f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_REV_PREDELAY */ {"PRE-DLY", 0.f, 200.f, 12.f, Curve::Pow2, Unit::Ms, 0, nullptr},
    /* GP_REV_LEVEL    */ {"LEVEL", 0.f, 1.f, 0.7f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_DLY_DIV      */ {"TIME", 0.f, 11.f, 6.f, Curve::Int, Unit::Enum, 12, kDelayDivs},
    /* GP_DLY_FEEDBACK */ {"FEEDBACK", 0.f, 0.95f, 0.38f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_DLY_TONE     */ {"TONE", 0.f, 1.f, 0.6f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_DLY_LEVEL    */ {"LEVEL", 0.f, 1.f, 0.5f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_DLY_PINGPONG */ {"PING-PONG", 0.f, 1.f, 1.f, Curve::Int, Unit::Enum, 2, kOnOff},
    /* GP_LOFI_BITS    */ {"BITS", 4.f, 16.f, 10.f, Curve::Int, Unit::Bits, 13, nullptr},
    /* GP_LOFI_RATE    */ {"RATE", 2000.f, 48000.f, 16000.f, Curve::Exp, Unit::Hz, 0, nullptr},
    /* GP_LOFI_MIX     */ {"MIX", 0.f, 1.f, 1.f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_WIDE         */ {"WIDTH", 0.f, 1.f, 0.6f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_PUMP_AMT     */ {"AMOUNT", 0.f, 1.f, 0.6f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_PUMP_RELEASE */ {"RELEASE", 20.f, 800.f, 180.f, Curve::Exp, Unit::Ms, 0, nullptr},
    /* GP_SPACE_AMT    */ {"SPACE", 0.f, 1.f, 0.5f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_METRO_LEVEL  */ {"CLICK", 0.f, 1.f, 0.5f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_BASS_TUNE    */ {"TUNE", -12.f, 12.f, 0.f, Curve::Lin, Unit::Semis, 0, nullptr},
    /* GP_BASS_CUTOFF  */ {"CUTOFF", 0.f, 1.f, 0.4f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_BASS_RESO    */ {"RESO", 0.f, 1.f, 0.65f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_BASS_ENVMOD  */ {"ENV MOD", 0.f, 1.f, 0.55f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_BASS_DECAY   */ {"DECAY", 0.f, 1.f, 0.45f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_BASS_ACCENT  */ {"ACCENT", 0.f, 1.f, 0.6f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_BASS_DRIVE   */ {"DRIVE", 0.f, 1.f, 0.15f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_BASS_LEVEL   */ {"LEVEL", -60.f, 6.f, -4.f, Curve::Lin, Unit::Db, 0, nullptr},
    /* GP_BASS_WAVE    */ {"WAVE", 0.f, 1.f, 0.f, Curve::Int, Unit::Enum, 2, kWaveNames},
    /* GP_BASS_SEND    */ {"FX SEND", 0.f, 1.f, 0.12f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_BASS_BUS     */ {"BUS", 0.f, 7.f, 7.f, Curve::Int, Unit::Enum, 8, kBusNumbers},
    /* GP_BASS_MUTE    */ {"MUTE", 0.f, 1.f, 0.f, Curve::Int, Unit::Enum, 2, kOnOff},
    // SYNTH (0.4.9): defaults = SynthParams defaults (core/synth.h)
    /* GP_SYN_OSC1_WAVE   */ {"WAVE", 0.f, 2.f, 0.f, Curve::Int, Unit::Enum, 3, kSynWaveNames},
    /* GP_SYN_OSC1_COLOR  */ {"COLOR", 0.f, 1.f, 0.f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_OSC1_LEVEL  */ {"LEVEL", 0.f, 1.f, 1.f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_OSC2_WAVE   */ {"WAVE", 0.f, 2.f, 0.f, Curve::Int, Unit::Enum, 3, kSynWaveNames},
    /* GP_SYN_OSC2_COLOR  */ {"COLOR", 0.f, 1.f, 0.f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_OSC2_LEVEL  */ {"LEVEL", 0.f, 1.f, 0.f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_OSC2_COARSE */ {"COARSE", -24.f, 24.f, 0.f, Curve::Int, Unit::Semis, 49, nullptr},
    /* GP_SYN_OSC2_FINE   */ {"FINE", -50.f, 50.f, 0.f, Curve::Lin, Unit::Cents, 0, nullptr},
    /* GP_SYN_OSC2_ENV    */ {"ENV>OSC2", -1.f, 1.f, 0.f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_SYNC        */ {"SYNC", 0.f, 1.f, 0.f, Curve::Int, Unit::Enum, 2, kOnOff},
    /* GP_SYN_RING        */ {"RING", 0.f, 1.f, 0.f, Curve::Int, Unit::Enum, 2, kOnOff},
    /* GP_SYN_XMOD        */ {"X-MOD", 0.f, 1.f, 0.f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_SUB_OCT     */ {"SUB OCT", 0.f, 1.f, 0.f, Curve::Int, Unit::Enum, 2, kSynSubNames},
    /* GP_SYN_SUB_LEVEL   */ {"SUB", 0.f, 1.f, 0.f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_NOISE       */ {"NOISE", 0.f, 1.f, 0.f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_TUNE        */ {"TUNE", -24.f, 24.f, 0.f, Curve::Lin, Unit::Semis, 0, nullptr},
    /* GP_SYN_FILTER      */ {"FILTER", 0.f, 2.f, 0.f, Curve::Int, Unit::Enum, 3, kSynFilterNames},
    /* GP_SYN_CUTOFF      */ {"CUTOFF", 20.f, 20480.f, 1280.f, Curve::Exp, Unit::Hz, 0, nullptr},
    /* GP_SYN_RESO        */ {"RESO", 0.f, 1.f, 0.15f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_KEY_FOLLOW  */ {"KEY FLW", 0.f, 1.f, 0.5f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_ENV_AMT     */ {"ENV AMT", -1.f, 1.f, 0.3f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_VEL_CUTOFF  */ {"VEL>CUT", 0.f, 1.f, 0.f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_F_ATTACK    */ {"ATTACK", 0.5f, 20000.f, 3.f, Curve::Exp, Unit::Ms, 0, nullptr},
    /* GP_SYN_F_DECAY     */ {"DECAY", 1.f, 20000.f, 400.f, Curve::Exp, Unit::Ms, 0, nullptr},
    /* GP_SYN_F_SUSTAIN   */ {"SUSTAIN", 0.f, 1.f, 0.3f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_F_RELEASE   */ {"RELEASE", 1.f, 20000.f, 300.f, Curve::Exp, Unit::Ms, 0, nullptr},
    /* GP_SYN_A_ATTACK    */ {"ATTACK", 0.5f, 20000.f, 3.f, Curve::Exp, Unit::Ms, 0, nullptr},
    /* GP_SYN_A_DECAY     */ {"DECAY", 1.f, 20000.f, 500.f, Curve::Exp, Unit::Ms, 0, nullptr},
    /* GP_SYN_A_SUSTAIN   */ {"SUSTAIN", 0.f, 1.f, 0.8f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_A_RELEASE   */ {"RELEASE", 1.f, 20000.f, 250.f, Curve::Exp, Unit::Ms, 0, nullptr},
    /* GP_SYN_VEL_AMP     */ {"VEL>AMP", 0.f, 1.f, 0.5f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_LFO_WAVE    */ {"LFO", 0.f, 5.f, 0.f, Curve::Int, Unit::Enum, 6, kSynLfoNames},
    /* GP_SYN_LFO_RATE    */ {"RATE", 0.05f, 30.f, 5.f, Curve::Exp, Unit::Hz, 0, nullptr},
    /* GP_SYN_LFO_SYNC    */ {"SYNC", 0.f, 13.f, 0.f, Curve::Int, Unit::Enum, 14, kSynLfoSyncNames},
    /* GP_SYN_LFO_FADE    */ {"FADE", 0.f, 20000.f, 0.f, Curve::Pow3, Unit::Ms, 0, nullptr},
    /* GP_SYN_LFO_KEY     */ {"KEY TRIG", 0.f, 1.f, 0.f, Curve::Int, Unit::Enum, 2, kOnOff},
    /* GP_SYN_LFO_PITCH   */ {"PITCH", 0.f, 1.f, 0.f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_LFO_CUTOFF  */ {"FILTER", 0.f, 1.f, 0.f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_LFO_AMP     */ {"AMP", 0.f, 1.f, 0.f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_LFO_PW      */ {"PW", 0.f, 1.f, 0.f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_MODE        */ {"MODE", 0.f, 2.f, 0.f, Curve::Int, Unit::Enum, 3, kSynModeNames},
    /* GP_SYN_LEGATO      */ {"LEGATO", 0.f, 1.f, 0.f, Curve::Int, Unit::Enum, 2, kOnOff},
    /* GP_SYN_GLIDE       */ {"GLIDE", 0.f, 5000.f, 0.f, Curve::Pow3, Unit::Ms, 0, nullptr},
    /* GP_SYN_DETUNE      */ {"DETUNE", 0.f, 1.f, 0.3f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_SPREAD      */ {"SPREAD", 0.f, 1.f, 0.5f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_DRIVE       */ {"DRIVE", 0.f, 1.f, 0.f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_LEVEL       */ {"LEVEL", -60.f, 6.f, -3.f, Curve::Lin, Unit::Db, 0, nullptr},
    /* GP_SYN_SEND        */ {"FX SEND", 0.f, 1.f, 0.15f, Curve::Lin, Unit::Percent, 0, nullptr},
    /* GP_SYN_BUS         */ {"BUS", 0.f, 7.f, 6.f, Curve::Int, Unit::Enum, 8, kBusNumbers},
    /* GP_SYN_MUTE        */ {"MUTE", 0.f, 1.f, 0.f, Curve::Int, Unit::Enum, 2, kOnOff},
};

float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

}  // namespace

const ParamSpec& padSpec(PadParam p) { return kPad[p < kPadParamCount ? p : 0]; }
const ParamSpec& globalSpec(GlobalParam p) { return kGlobal[p < kGlobalParamCount ? p : 0]; }

float clampParam(const ParamSpec& s, float v)
{
    if (!(v == v)) v = s.def;  // NaN
    if (v < s.min) v = s.min;
    if (v > s.max) v = s.max;
    if (s.curve == Curve::Int) v = floorf(v + 0.5f);
    return v;
}

float toNorm(const ParamSpec& s, float v)
{
    v = clampParam(s, v);
    const float span = s.max - s.min;
    if (span <= 0.f) return 0.f;
    switch (s.curve) {
    case Curve::Exp:
        if (s.min > 0.f) return clamp01(logf(v / s.min) / logf(s.max / s.min));
        return clamp01((v - s.min) / span);
    case Curve::Pow2: return clamp01(sqrtf((v - s.min) / span));
    case Curve::Pow3: return clamp01(cbrtf((v - s.min) / span));
    case Curve::Lin:
    case Curve::Int:
    default: return clamp01((v - s.min) / span);
    }
}

float fromNorm(const ParamSpec& s, float n)
{
    n = clamp01(n);
    const float span = s.max - s.min;
    float v;
    switch (s.curve) {
    case Curve::Exp: v = s.min > 0.f ? s.min * powf(s.max / s.min, n) : s.min + span * n; break;
    case Curve::Pow2: v = s.min + span * n * n; break;
    case Curve::Pow3: v = s.min + span * n * n * n; break;
    case Curve::Int: v = s.min + floorf(span * n + 0.5f); break;
    case Curve::Lin:
    default: v = s.min + span * n; break;
    }
    return clampParam(s, v);
}

int formatParam(const ParamSpec& s, float v, char* buf, int len)
{
    if (!buf || len <= 0) return 0;
    v = clampParam(s, v);
    int n = 0;
    switch (s.unit) {
    case Unit::Percent: n = snprintf(buf, len, "%d %%", (int)lroundf(v * 100.f)); break;
    case Unit::Ms:
        if (v >= 1000.f) n = snprintf(buf, len, "%.2f s", v / 1000.f);
        else if (v < 10.f) n = snprintf(buf, len, "%.1f ms", v);
        else n = snprintf(buf, len, "%d ms", (int)lroundf(v));
        break;
    case Unit::Seconds: n = snprintf(buf, len, "%.1f s", v); break;
    case Unit::Hz:
        if (v >= 1000.f) n = snprintf(buf, len, "%.1f kHz", v / 1000.f);
        else if (v < 10.f) n = snprintf(buf, len, "%.2f Hz", v);
        else n = snprintf(buf, len, "%d Hz", (int)lroundf(v));
        break;
    case Unit::Db: {
        // FX SEND stores linear gain 0..1 and shows dB, the others store dB directly.
        float db = v;
        if (s.min == 0.f && s.max == 1.f) {
            if (v <= 0.0005f) { n = snprintf(buf, len, "-inf dB"); break; }
            db = 20.f * log10f(v);
        }
        if (db <= -59.95f) n = snprintf(buf, len, "-inf dB");
        else n = snprintf(buf, len, "%.1f dB", fabsf(db) < 0.05f ? 0.f : db);
        break;
    }
    case Unit::Semis:
        if (fabsf(v) < 0.05f) n = snprintf(buf, len, "0.0 st");
        else n = snprintf(buf, len, "%+.1f st", v);
        break;
    case Unit::Cents: n = snprintf(buf, len, "%+d ct", (int)lroundf(v)); break;
    case Unit::Pan: {
        int p = (int)lroundf(v * 100.f);
        if (p == 0) n = snprintf(buf, len, "C");
        else n = snprintf(buf, len, "%c %d", p < 0 ? 'L' : 'R', p < 0 ? -p : p);
        break;
    }
    case Unit::Ratio: n = snprintf(buf, len, "%.2f", v); break;
    case Unit::Bits:
        if (v >= 16.f) n = snprintf(buf, len, "OFF");
        else n = snprintf(buf, len, "%d bit", (int)v);
        break;
    case Unit::Factor:
        if (v <= 1.f) n = snprintf(buf, len, "OFF");
        else n = snprintf(buf, len, "x%d", (int)v);
        break;
    case Unit::Enum: {
        int i = (int)v - (int)s.min;
        if (s.names && i >= 0 && i < s.steps) n = snprintf(buf, len, "%s", s.names[i]);
        else n = snprintf(buf, len, "%d", (int)v);
        break;
    }
    case Unit::Plain:
    default: n = snprintf(buf, len, "%d", (int)lroundf(v)); break;
    }
    if (n < 0) n = 0;
    if (n >= len) n = len - 1;
    return n;
}

ORBIT_HOT float syncDivBeats(SyncDiv d)   // every block (delay time in EngineImpl::master)
{
    const int i = (int)d;
    return (i >= 0 && i < (int)SyncDiv::Count) ? kSyncBeats[i] : 1.f;
}

const char* syncDivName(SyncDiv d)
{
    const int i = (int)d;
    return (i >= 0 && i < (int)SyncDiv::Count) ? kSyncNames[i] : "?";
}

}  // namespace orbit
