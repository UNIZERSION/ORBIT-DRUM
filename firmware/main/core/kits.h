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

struct BuiltinSound {
    const char* name;        // "BD", "SD", ... (pad label)
    const char* long_name;   // "Bass Drum", ...
    const int16_t* pcm;      // frame 0 (kGuard zero frames on both sides in the array)
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

}  // namespace orbit
