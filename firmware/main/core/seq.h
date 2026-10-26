#pragma once
// Sample-accurate step sequencer. Runs inside Engine::process() on the audio task.
// It owns transport position, scene switching (quantized), song (FOLLOW) progression, swing,
// micro timing (nudge), ratchets, probability and trig conditions, and pad mute/solo.
// It reads the Project directly (UI-written, see project.h) and never allocates.
#include <stdint.h>
#include "acid.h"
#include "config.h"
#include "project.h"

namespace orbit {

struct SeqTrigger {
    uint8_t pad = 0;
    uint8_t vel = 100;      // 1..127 (step velocity)
    int8_t pitch = 0;       // step pitch (semitones)
    uint8_t _pad = 0;
    uint32_t offset = 0;    // frame inside the current block, 0..n-1
};

struct SeqNotice {          // things the engine turns into events / telemetry
    bool scene_changed = false;
    bool song_slot_changed = false;
    bool transport_changed = false;
    bool metronome_click = false;     // a beat started in this block
    bool metronome_accent = false;    // ...and it is the first beat of a bar
    uint32_t click_offset = 0;
    // ACID BASS (0.3): notes of the playing scene's bass line that start in this block (offsets like
    // SeqTrigger, sorted). NoteOn with the gate length: half a step, a step and a half on SLIDE steps (held
    // into the next note, which then comes with legato = 1).
    static constexpr int kBassMax = 4;
    uint8_t bass_count = 0;
    AcidEvent bass[kBassMax];
    // SYNTH (0.4.9): notes of the playing scene's synth line that start in this block (sorted by offset), each with
    // its gate: its length in steps minus 1/16 step (the engine sends the note-off).
    struct SynthNoteOn {
        uint8_t note = 60, vel = 100;
        uint16_t _pad = 0;
        uint32_t offset = 0;
        uint32_t gate_frames = 0;
    };
    static constexpr int kSynthMax = 12;
    uint8_t synth_count = 0;
    SynthNoteOn synth[kSynthMax];
};

class Sequencer {
public:
    void init(int sample_rate, const Project* project);
    void setSampleRate(int sample_rate);

    void play();                 // from the start (scene step 0 / song slot 0 when FOLLOW)
    void stop();                 // stop; when already stopped: rewind to step 0
    void resume();               // continue from the current position
    void locate(int step);
    // Immediate when stopped, else at the Project::quant boundary. Ignored only while FOLLOW plays a
    // song; after FOLLOW is switched off it works at once (also before the song pass has ended).
    void queueScene(int scene);
    void setFill(bool on);
    // Swing/division/length/bpm are read from the Project every block, so edits apply live.

    // Advances n frames. Writes up to max triggers (sorted by offset) and returns their count.
    // A random generator for probability is internal (xorshift, deterministic per seed).
    int process(int n, SeqTrigger* out, int max, SeqNotice& notice);

    bool playing() const;
    int step() const;            // current step index of the playing scene, -1 when stopped
    int scene() const;           // playing scene
    int queuedScene() const;     // -1 none
    int songSlot() const;        // -1 when FOLLOW is off
    uint32_t loopCount() const;  // passes of the current scene since it started
    double beats() const;        // quarter notes since play() (LFO / delay sync, metronome)
    float beatsF() const;        // the same in float math only (audio task: the P4 has no double FPU)

    // Live recording: where does a hit `offset` frames into the block just processed land?
    // Rounds to the nearest step of the playing scene (late hits move to the next step when
    // closer). nudge = signed remainder in % of a step (-50..50). False when stopped.
    // With a pad (0..kPads-1), step is that pad's TRACK step (polymeter tracks loop on their own
    // length, so it is the step that track plays there); without one (-1) the scene step.
    bool quantize(uint32_t offset, int pad, int& scene, int& step, int& nudge) const;
    bool quantize(uint32_t offset, int& scene, int& step, int& nudge) const
    {
        return quantize(offset, -1, scene, step, nudge);
    }

    void seed(uint32_t s);

private:
    // implementation detail fields live in seq.cpp's Impl to keep the header stable
    struct State;
    alignas(8) uint8_t state_[3584];   // ~3.1 KB: lookahead ring + pending hits and notes (seq.cpp)
    State& st();
    const State& st() const;
};

}  // namespace orbit
