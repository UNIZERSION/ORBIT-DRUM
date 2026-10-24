#pragma once
// ORBIT DRUM — compile-time limits shared by the engine, the model and the UI.
// Portable: no ESP-IDF or LVGL headers here (the host tests build this on the PC).
#include <stdint.h>

namespace orbit {

constexpr int kPads = 16;          // A1..D4
constexpr int kLayers = 4;         // LAYER 1..4 per pad
constexpr int kScenes = 4;         // scenes A..D (= patterns)
constexpr int kMaxSteps = 64;      // 4 bars of 1/16
constexpr int kStepsPerPage = 16;  // one bar shown at a time in SEQUENCE
constexpr int kBuses = 8;          // mixer channels (pads are routed to a bus)
constexpr int kSongSlots = 16;     // song arranger slots in the SCENES timeline
constexpr int kMaxSlices = 16;
constexpr int kMaxSamples = 128;   // engine sample slot table
constexpr int kVoices = 32;        // global polyphony
constexpr int kMaxBlock = 256;     // largest audio block the engine accepts
constexpr int kMaxPadPoly = 8;     // per-pad voice limit
constexpr int kPeakBins = 1024;    // waveform overview resolution (min/max pairs)
constexpr int kNameLen = 24;
constexpr int kPathLen = 160;

// Supported engine rates (Settings > Audio > Sample rate). The engine never assumes one.
constexpr int kRate44 = 44100;
constexpr int kRate48 = 48000;

// Producers that may post commands to the engine. Each has its own single-producer queue,
// so no queue ever has two writers (UI task, touch task, MIDI task, loader task).
enum class Producer : uint8_t { Ui = 0, Touch = 1, Midi = 2, Loader = 3, Count = 4 };

}  // namespace orbit
