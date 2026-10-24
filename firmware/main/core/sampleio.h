#pragma once
// Sample files and sample processing. Runs on the loader task / UI task, never on the audio task.
// Uses only stdio (fopen/fread), so the same code reads /sdcard on the Tab5 and folders on the PC.
#include <stdint.h>
#include "config.h"
#include "sample.h"

namespace orbit {

// ---------------------------------------------------------------- WAV
struct WavInfo {
    uint32_t rate = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;          // 8, 16, 24, 32
    uint8_t is_float = 0;       // IEEE float (32/64)
    uint32_t frames = 0;
    // optional chunks
    uint8_t has_loop = 0;       // smpl chunk loop
    uint32_t loop_start = 0, loop_end = 0;
    uint8_t cue_count = 0;      // cue chunk markers (used as slice points)
    uint32_t cues[kMaxSlices] = {};
};

enum class WavError : uint8_t { Ok = 0, Open, NotRiff, NotWave, NoFmt, NoData, Unsupported, TooLarge, NoMemory, Io };
const char* wavErrorText(WavError e);   // Polish, short ("Nieobsługiwany format")

// Reads only the header (fast, for the browser).
WavError wavProbe(const char* path, WavInfo& info);
// Loads PCM 8/16/24/32-bit int, 32/64-bit float, mono/stereo (more channels: first two),
// WAVE_FORMAT_EXTENSIBLE, odd chunk padding, LIST/smpl/cue chunks. Converts to int16 at the
// native rate. max_seconds caps memory (longer files are truncated, info.frames tells).
// Multichannel >2 keeps the first two. Calls sampleAnalyze(). Name = file stem.
Sample* wavLoad(const char* path, WavInfo* info, WavError* err, float max_seconds = 60.0f);
// Writes 16-bit PCM WAV (+ cue chunk with the given slice points if count > 0).
WavError wavSave(const char* path, const Sample* s, const float* slices = nullptr, int slice_count = 0);
// Loads from memory (embedded kits / tests).
Sample* wavLoadMem(const uint8_t* data, uint32_t size, const char* name, WavInfo* info, WavError* err);

// ---------------------------------------------------------------- processing (destructive)
// Every op returns a NEW sample (copy-on-write) or nullptr on failure; the input is untouched,
// so the engine can keep playing the old one until it is retired. Regions are 0..1 fractions.
Sample* opCrop(const Sample* s, float start, float end);                 // TRIM
Sample* opNormalize(const Sample* s, float target_db = -0.3f);           // peak normalize
Sample* opReverse(const Sample* s);
Sample* opFade(const Sample* s, float fade_in_ms, float fade_out_ms);    // linear-in-power fades
Sample* opGain(const Sample* s, float db);
Sample* opTrimSilence(const Sample* s, float threshold_db = -50.0f);     // auto trim start/end
Sample* opToMono(const Sample* s);
Sample* opResample(const Sample* s, uint32_t new_rate);                  // windowed-sinc, offline
Sample* opPitch(const Sample* s, float semitones);                       // resample (changes length)
Sample* opBitcrush(const Sample* s, int bits, int downsample);           // print the crush
Sample* opDuplicate(const Sample* s);                                    // plain copy (DUPLICATE)
// Zero-crossing snap: nearest frame position (as 0..1) where the signal crosses zero.
float snapToZeroCrossing(const Sample* s, float pos, float window_ms = 5.0f);

// ---------------------------------------------------------------- slicing
// Equal grid: count slices -> positions[0..count-1] (positions[0] = start).
int sliceGrid(float start, float end, int count, float* positions);
// Transient detection (spectral-flux / energy onset), sensitivity 0..1 (higher = more slices),
// limited to max slices inside [start, end]. Returns count, positions ascending, first = start.
int sliceTransients(const Sample* s, float start, float end, float sensitivity, int max, float* positions);

}  // namespace orbit
