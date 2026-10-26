#pragma once
// SYNTH knobs of a project (Project::g[GP_SYN_*], real units: ms, Hz, dB) <-> the synth engine's SynthParams
// (core/synth.h: seconds, normalized cutoff, linear level). 0.4.9.
#include "synth.h"

namespace orbit {

// g = Project::g. The engine calls it once per block: no libm, no allocation; every value is clamped.
void synthFromGlobals(const float* g, SynthParams& out);
// The inverse (UI task): writes every GP_SYN_* sound knob of g from p (a factory preset). Mixer routing
// (GP_SYN_SEND, GP_SYN_BUS, GP_SYN_MUTE) is left as it is.
void synthToGlobals(const SynthParams& p, float* g);
// Factory preset i (0..synthPresetCount()-1) into g; false when i is out of range.
bool synthApplyPreset(int i, float* g);
int synthPresetCount();
const char* synthPresetName(int i);   // "" when out of range

}  // namespace orbit
