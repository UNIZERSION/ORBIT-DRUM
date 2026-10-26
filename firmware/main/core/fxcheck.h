#pragma once
// 0.4.9: the block-wise stereo delay against its per-sample reference (StereoDelay::processRef). Both get the same
// input and the same parameter moves over `blocks` blocks of mixed lengths; the result counts the output samples
// that differ in any bit (0 = identical). Allocates its own lines (bigAlloc, 1 MB) and frees them; on the Tab5 it
// pauses 1 ms every 16 blocks, so its memory traffic stays small next to the audio task's. Not for the audio task.
// Used by test_engine on the PC and by the test console's "fxcheck" on the Tab5 (the device's compiler fuses
// multiply-adds, the PC build does not).
#include <stdint.h>

namespace orbit {

struct FxCheck {
    bool ran = false;           // the memory was there
    int64_t samples = 0;        // output samples compared (both channels)
    int64_t delay_diff = 0;     // samples that differ
    int64_t delay_block = 0;    // blocks that took the block-wise path
    int64_t blocks = 0;
    double delay_rms = 0.0;     // output level of the reference (the check had signal)
};

FxCheck fxSelfCheck(int rate, int blocks);

}  // namespace orbit
