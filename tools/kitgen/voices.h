#pragma once
// Sound definitions of the two built-in kits. Pad order is fixed (firmware/main/core/kits.h).
#include "dsp.h"

namespace kitgen {

struct SoundDef {
    const char* name;       // pad label, <= 8 chars
    const char* long_name;  // descriptive name
    const char* file;       // WAV stem, written as "<NN>_<file>.wav"
    Buf (*render)();        // mono float at kFs (oversampled)
    float level_db;         // short-term RMS target relative to the kit's kick (non-kicks)
    bool kick;              // kicks are peak-normalised to -1 dBFS
    uint8_t choke;          // suggested choke group (1 = hats)
    uint8_t bus;            // 0 kick, 1 snare/clap/rim, 2 hats, 3 perc, 4 toms, 5 cymbals
    float cap_s;            // ROM-like hard length (0 = none), ends with cap_fade_ms
    float cap_fade_ms;
    float embed_cap_s;      // flash copy length limit (0 = same as the WAV), ends with a fade
    const char* recipe;     // short synthesis summary (report)
};

constexpr int kSoundsPerKit = 16;
const SoundDef* kit808();  // kSoundsPerKit entries
const SoundDef* kit909();

// Short-term RMS over [t0, t1) seconds of an oversampled buffer.
float rmsWindow(const Buf& b, float t0, float t1);
// Mixes a and b (both oversampled) after normalising each to unit RMS over its first `win`
// seconds: out = a * dbToLin(ga) / rms(a) + b * dbToLin(gb) / rms(b).
Buf mixNorm(const Buf& a, float ga_db, const Buf& b, float gb_db, float win);

}  // namespace kitgen
