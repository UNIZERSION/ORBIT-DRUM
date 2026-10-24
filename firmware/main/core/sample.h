#pragma once
// Sample memory. PCM is int16 (half the PSRAM bandwidth of float), interleaved for stereo,
// at the file's own sample rate (the voice increment absorbs rate differences, so 44.1 kHz
// material plays bit-exact at pitch 0 on a 44.1 kHz engine).
//
// Every buffer has kGuard zero frames before frame 0 and after the last frame, so 4-point
// interpolation (frames i-1 .. i+2) never needs a bounds check in the voice loop.
#include <stdint.h>
#include "config.h"

namespace orbit {

constexpr int kGuard = 4;

struct Sample {
    int16_t* data = nullptr;     // points at frame 0 (guard frames precede it)
    uint32_t frames = 0;
    uint8_t channels = 1;        // 1 or 2
    uint8_t embedded = 0;        // 1: data lives in flash (built-in kit), never freed
    uint8_t _pad[2] = {0, 0};
    uint32_t rate = 44100;       // native rate of data
    uint32_t id = 0;             // unique, increases with every new Sample (UI change detection)
    float peak = 0.0f;           // max |x| / 32768
    float rms = 0.0f;
    int8_t pk_min[kPeakBins];    // waveform overview: per-bin min/max of the mono mix, -127..127
    int8_t pk_max[kPeakBins];
    char name[kNameLen] = {};    // display name ("Kick 01")
    char path[kPathLen] = {};    // source file on the SD card, "" if built-in or edited-unsaved
    void* alloc = nullptr;       // start of the heap block (nullptr when embedded)
};

// Allocates a zeroed sample with guard frames (PSRAM on the device). nullptr if out of memory.
Sample* sampleCreate(uint32_t frames, int channels, uint32_t rate);
// Wraps PCM in flash (must already contain kGuard zero frames on both sides). Not freed.
Sample* sampleWrapEmbedded(const int16_t* frame0, uint32_t frames, int channels, uint32_t rate, const char* name);
void sampleFree(Sample* s);      // nullptr-safe; embedded data is not freed, the struct is
Sample* sampleClone(const Sample* s);
// Recomputes peak, rms and pk_min/pk_max. Call after any edit.
void sampleAnalyze(Sample* s);
uint32_t sampleNextId();
float sampleSeconds(const Sample* s);

}  // namespace orbit
