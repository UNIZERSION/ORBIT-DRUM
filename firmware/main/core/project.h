#pragma once
// The project model: everything that is saved in a project file.
//
// Threading contract (important):
//  * The UI task is the ONLY writer of a Project. Other tasks never write it.
//  * The audio task reads it directly every block (no copy). Every field is a naturally aligned
//    scalar of <= 32 bits, written whole by the UI, so a read can never be torn. A multi-field edit
//    (e.g. copying a scene while it plays) may be observed half-done for one block; that is harmless.
//  * Sample data is NOT referenced by pointer here: layers hold an engine slot index; slot contents
//    change only through Engine commands (see engine.h), which also retire old samples safely.
#include <stdint.h>
#include "config.h"
#include "params.h"

namespace orbit {

enum class PlayMode : uint8_t { OneShot = 0, Gate, Loop, Count };
enum class LayerMode : uint8_t { Single = 0, Velocity, RoundRobin, Random, Stack, Count };
// Step length of a scene (GRID select / PATTERN SETTINGS "1/16 ▾").
enum class Division : uint8_t { D1_8 = 0, D1_16, D1_32, D1_8T, D1_16T, Count };
// Scene switch quantization (Settings > General > Launch quantization).
enum class LaunchQuant : uint8_t { Bar = 0, Beat, Eighth, Off, Count };
// Trig conditions (Elektron-like). Always = plain step.
enum class TrigCond : uint8_t {
    Always = 0,
    P1of2, P2of2,              // play on 1st / 2nd of every 2 loops
    P1of4, P2of4, P3of4, P4of4,
    Fill, NotFill,             // only while FILL is held / only while not held
    First,                     // only on the first loop after the scene started
    Count
};

struct Layer {
    int16_t slot = -1;          // engine sample slot (-1 = empty)
    int16_t _pad0 = 0;
    float tune = 0.0f;          // semitones added for this layer
    float gain_db = 0.0f;
    float pan = 0.0f;           // -1..1, added to the pad pan
    uint8_t vel_lo = 0;         // LayerMode::Velocity range (inclusive)
    uint8_t vel_hi = 127;
    uint8_t _pad1[2] = {0, 0};
};

struct Pad {
    char name[kNameLen] = {};
    Layer layer[kLayers];
    LayerMode layer_mode = LayerMode::Single;
    PlayMode mode = PlayMode::OneShot;
    uint8_t reverse = 0;        // play the region backwards (non-destructive)
    uint8_t bus = 0;            // mixer channel 0..kBuses-1
    uint8_t choke = 0;          // 0 = none, 1..8: a new hit stops other pads of the same group
    uint8_t poly = 4;           // voices this pad may hold (1 = mono retrigger)
    uint8_t mute = 0;
    uint8_t solo = 0;
    uint8_t slice_count = 0;    // markers in slices[] (0 = not sliced)
    uint8_t slice_sel = 0;      // selected slice in the SLICE editor
    uint8_t _pad[2] = {0, 0};
    float slices[kMaxSlices] = {};  // slice start positions 0..1, ascending, slices[0] = 0
    float slice_end = 1.0f;     // end of the last slice (the region END when sliced; 16 slices leave no
                                // room for it in slices[])
    float p[kPadParamCount];    // see PadParam
};

struct Step {
    uint8_t on = 0;
    uint8_t vel = 100;          // 1..127
    uint8_t prob = 100;         // 0..100 %
    uint8_t ratchet = 1;        // 1..4 hits inside the step
    int8_t nudge = 0;           // -50..+50 % of a step (micro timing)
    int8_t pitch = 0;           // -24..+24 semitones on top of the pad tune
    TrigCond cond = TrigCond::Always;
    uint8_t flam = 0;           // 0 = off, 1..8: grace note (60 % velocity) 16 + 4*flam ms early
};

struct Track {
    Step step[kMaxSteps];
    uint8_t length = 0;         // 0 = follow Scene::length, otherwise 1..64 (polymeter)
    uint8_t _pad[3] = {0, 0, 0};
};

// ACID BASS (303 style, 0.3): one monophonic line per scene, on the scene's step grid (length, division, swing).
// A step plays its note for half a step; SLIDE holds it into the next note, which then glides instead of starting
// again (and gets no new envelope). Notes are semitones from C2 (MIDI 36): -24..+36 = C0..C5 (0.4.6: an octave more
// down and up, in place of the TUNE knob; it was -12..+24 = C1..C4).
constexpr int kBassRootNote = 36;
constexpr int kBassNoteMin = -24, kBassNoteMax = 36;
struct BassStep {
    uint8_t gate = 0;           // 1: a note, 0: rest
    int8_t note = 0;            // kBassNoteMin..kBassNoteMax (kept while the step rests)
    uint8_t accent = 0;         // louder, brighter, the accent sweep builds up over close accents
    uint8_t slide = 0;          // held into the next step's note, which glides
};
struct BassLine {
    BassStep step[kMaxSteps];
};

struct Scene {
    Track track[kPads];
    uint8_t length = 16;        // steps 1..64 (LENGTH: 1 Bar = 16, 2 Bars = 32, 4 Bars = 64)
    Division div = Division::D1_16;
    uint8_t swing = 50;         // 50 (straight) .. 75 (%), delays every second step
    uint8_t _pad = 0;
    BassLine bass;              // appended (0.3)
};

// Song arranger shown in the SCENES timeline: each slot plays one full pass of a scene.
struct Song {
    int8_t slot[kSongSlots];    // -1 = empty (song ends at the first empty slot), else scene 0..3
    uint8_t fx[kSongSlots];     // FX lane: 1 = "FX throw" during that slot (sends + SPACE boosted)
    uint8_t follow = 0;         // FOLLOW: 1 = play the song chain, 0 = loop the current scene
    uint8_t _pad[3] = {0, 0, 0};
};

struct Bus {
    char name[8] = {};
    float level = 0.8f;         // fader 0..1 (gain = level^2 * 2, i.e. 0.707 -> 0 dB, see busGain())
    float pan = 0.0f;
    uint8_t mute = 0;
    uint8_t solo = 0;
    uint8_t _pad[2] = {0, 0};
};

struct Project {
    char name[32] = "ORBIT_01";
    float bpm = 120.0f;         // 30..300
    uint8_t scene = 0;          // scene being edited / looped (A..D)
    uint8_t beats_per_bar = 4;  // 4/4 shown in the top bar
    LaunchQuant quant = LaunchQuant::Bar;
    uint8_t metronome = 0;
    uint8_t count_in = 0;       // bars of count-in before recording (0..2)
    // Performance macros (FX view): master LO-FI, WIDE, PUMP, SPACE
    uint8_t lofi_on = 0, wide_on = 0, pump_on = 0, space_on = 0;
    uint8_t limiter_on = 1;
    uint8_t pump_src = 0;       // pad whose hits duck the mix (PUMP)
    uint8_t _pad[1] = {0};
    Pad pad[kPads];
    Scene scene_data[kScenes];
    Song song;
    Bus bus[kBuses];
    float g[kGlobalParamCount]; // see GlobalParam
};

// Fills a project with defaults: parameter defaults from params.h, bus names, pad names and the demo (0.4.6: one
// track in the manner of the 303 + 909 sound of "Da Funk", 111 BPM, scenes A..D = intro, groove, break, full, and
// a song chain of them), so PLAY makes sound at once. Every boot starts with it.
void projectDefaults(Project& p);
// NEW PROJECT: the defaults without the demo — every step of every scene off, straight 1/16 bars of 16 steps, an
// empty song, 120 BPM and default knobs.
void projectEmpty(Project& p);
// Bus fader position -> linear gain (0 at 0, 1.0 at ~0.707, +6 dB at 1.0).
float busGain(float level);
float dbToGain(float db);
float gainToDb(float g);
// Number of steps of track t in scene s (track length or scene length).
int trackLength(const Scene& s, int t);
// Length of one step in quarter notes for a division (1/16 = 0.25).
float divisionBeats(Division d);
const char* divisionName(Division d);   // "1/16", "1/8T"...
const char* trigCondName(TrigCond c);   // "1:2", "FILL"...

}  // namespace orbit
