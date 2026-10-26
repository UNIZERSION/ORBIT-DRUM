#pragma once
// Built-in "808 STYLE" / "909 STYLE" kits, synthesized offline by tools/kitgen from published
// circuit descriptions of the classic analogue drum machines (our own synthesis, no Roland
// samples; "TR-808"/"TR-909" are Roland trademarks, so the UI says "808 STYLE"/"909 STYLE").
// Compiled into the firmware so the machine plays without an SD card; the same kits are also
// written as WAV files to sdcard/ORBIT/KITS/ for editing.
// Pad order (both kits): 0 Kick 1 Snare 2 Cl Hat 3 Op Hat 4 Clap 5 Rim 6 Lo Tom 7 Mid Tom 8 Hi Tom
// 9 Cymbal/Crash 10 Cowbell/Ride, 11..15 kit specific (see tools/kitgen).
#include <stdint.h>
#include "config.h"

namespace orbit {

// 0.4.9: the PCM is stored packed (lossless KPK1, tools/kitgen/kitpack.py: 2.24 MB -> 0.83 MB of flash) and
// unpacked into PSRAM on first use: builtinPcm().
struct BuiltinSound {
    const char* name;        // "BD", "SD", ... (pad label)
    const char* long_name;   // "Bass Drum", ...
    const uint8_t* packed;   // KPK1 stream
    uint32_t packed_bytes;
    uint32_t frames;
    uint8_t choke;           // suggested choke group (hats)
    uint8_t bus;             // suggested mixer bus
};

struct BuiltinKit {
    const char* name;        // "808 STYLE", "909 STYLE"
    uint32_t rate;           // 48000
    const BuiltinSound* sounds;
    int count;               // 16
};

int builtinKitCount();
const BuiltinKit& builtinKit(int i);
// The PCM of sound `sound` of kit `kit` (frame 0; kGuard zero frames on both sides), unpacked into PSRAM on the first
// call and kept for good (~10 ms per kit on the Tab5). nullptr when out of memory or the stream is damaged. Not
// thread safe: call it from one task (the UI task).
const int16_t* builtinPcm(int kit, int sound);
// The unpacked PCM if builtinPcm() already ran for it, else nullptr (never unpacks).
const int16_t* builtinPcmIfUnpacked(int kit, int sound);
// Unpacks a KPK1 stream (exposed for the tests). False when the stream ends early or is malformed.
bool kitUnpack(const uint8_t* src, uint32_t bytes, int16_t* dst, uint32_t frames);

}  // namespace orbit
