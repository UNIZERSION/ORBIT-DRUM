#pragma once
// ORBIT DRUM audio engine: sampler voices, sequencer clock, mixer buses, FX and master chain.
//
// Real-time rules inside process(): no allocation, no locks, no file or log I/O, no unbounded
// loops. All communication with other tasks goes through per-producer SPSC command queues,
// one SPSC event queue back to the UI, and the atomic Telemetry block.
//
// The engine reads the Project directly (see the threading contract in project.h) and owns the
// sample slot table: layers name a slot; slot contents change only via CmdType::SetSample.
// A replaced or cleared sample is "retired": voices still playing it fade out (<= 5 ms) and
// then the engine emits EvtType::SampleRetired with the pointer, after which the UI may free it.
// Each SetSample install of a pointer is answered by exactly one SampleRetired: installing a pointer
// that already sits in another slot, or whose retirement is still pending, answers the earlier
// install at once; the pointer is freeable only after the answer to its last install.
#include <atomic>
#include <stdint.h>
#include "config.h"
#include "project.h"
#include "sample.h"

namespace orbit {

enum class CmdType : uint8_t {
    Trigger = 0,   // pad, vel 1..127, value = extra pitch in semitones (live/MIDI note offset)
    Release,       // pad: gate off (PlayMode::Gate / Loop voices enter release)
    Play,          // start from the beginning (scene step 0, or song slot 0 when FOLLOW is on)
    Stop,          // stop the sequencer; voices keep ringing; a second Stop = rewind
    Continue,      // resume from the current position
    Locate,        // value = step in the current scene
    QueueScene,    // pad = scene 0..3; switches now when stopped, else at Project::quant
    SetSample,     // value = slot 0..kMaxSamples-1, sample = new content (nullptr clears)
    Preview,       // sample = pointer owned by the caller until EvtType::PreviewDone; vel
    StopPreview,
    Panic,         // every voice fades out within 5 ms
    SetFill,       // value 0/1: FILL held (TrigCond::Fill / NotFill)
    SetRecord,     // value 0/1: live record arm; while playing, Trigger produces RecordHit events
    Metronome,     // value 0/1 runtime override (Project::metronome is the saved default)
    // ACID BASS (0.3) live note (BASS keyboard): value = MIDI note, vel bit 0 = accented, bit 1 = legato (slide
    // from the sounding note, 0.4.2), pad = gate in 10 ms steps (0: until BassOff). The sequencer plays the bass
    // line by itself; while a held live note (pad 0) sounds, the line's notes are skipped (the player has the voice).
    BassNote,
    BassOff,       // closes the live note's gate
    // SYNTH (0.4.9) live notes (keyboard, MIDI): value = MIDI note, vel 1..127; held until SynthOff with the same
    // note (value < 0: every live note). The sequencer plays the scene's synth line by itself.
    SynthNote,
    SynthOff,
    Count
};

struct Command {
    CmdType type = CmdType::Trigger;
    uint8_t pad = 0;
    uint8_t vel = 100;
    uint8_t _pad = 0;
    int32_t value = 0;
    const Sample* sample = nullptr;
};

enum class EvtType : uint8_t {
    PadHit = 0,        // pad, vel: a voice started (sequencer or live): flash the pad
    SampleRetired,     // sample: no voice uses it any more, the owner may free it
    PreviewDone,       // sample: preview voice finished or was replaced
    SceneChanged,      // scene: the playing scene changed (queued switch happened)
    RecordHit,         // pad, vel, scene, step (the pad's track step), nudge: write this into the pattern (UI does it)
    TransportChanged,  // playing / stopped / rewound
    SongSlotChanged,   // step = song slot now playing
    Count
};

struct Event {
    EvtType type = EvtType::PadHit;
    uint8_t pad = 0;
    uint8_t vel = 0;
    int8_t scene = 0;
    int8_t nudge = 0;
    int16_t step = 0;
    const Sample* sample = nullptr;
};

// Written by the audio task, read by the UI (relaxed atomics, lock-free on RV32 with "A").
struct Telemetry {
    std::atomic<uint8_t> playing{0};
    std::atomic<uint8_t> recording{0};
    std::atomic<int16_t> step{-1};          // current step of the playing scene, -1 when stopped
    std::atomic<int8_t> scene{0};           // playing scene
    std::atomic<int8_t> queued_scene{-1};   // pending switch, -1 none
    std::atomic<int8_t> song_slot{-1};      // playing song slot when FOLLOW is on
    std::atomic<uint32_t> loop_count{0};    // completed passes of the playing scene
    std::atomic<float> beat{0.0f};          // position in quarter notes since Play
    std::atomic<float> bus_peak[kBuses];    // post-fader peak of the last block, linear
    std::atomic<float> master_peak[2];      // L, R
    std::atomic<float> gain_reduction{0.0f};// limiter, dB (>= 0)
    std::atomic<uint32_t> pad_hits{0};      // bit per pad, set on each hit; UI exchange(0)s it
    std::atomic<float> pad_env[kPads];      // loudest current amp envelope per pad, 0..1
    std::atomic<int8_t> watch_pad{0};       // UI writes: pad whose playhead is reported
    std::atomic<float> play_pos{-1.0f};     // newest voice of watch_pad: position 0..1 of the sample, <0 none
    std::atomic<float> cpu{0.0f};           // smoothed render time / block time, 0..1+
    std::atomic<uint32_t> xruns{0};         // driver-reported underruns
    std::atomic<uint8_t> voices{0};         // active voices
    // Polyphony allowed right now: min(Engine::setVoiceLimit, CPU governor). The governor lowers
    // it when Telemetry::cpu stays above ~0.8 and gives voices back slowly below ~0.6.
    std::atomic<uint8_t> voice_cap{(uint8_t)kVoices};
    std::atomic<uint32_t> bass_notes{0};    // ACID BASS notes started (sequencer and live): the BASS view flashes
    std::atomic<uint32_t> synth_notes{0};   // SYNTH notes started (line and live), 0.4.9
    std::atomic<uint8_t> synth_voices{0};   // SYNTH voices sounding
    // Profile (0.4.9): average CPU cycles per block of each part of a block on the audio core, over the last 256
    // blocks (0 on the PC, which has no cycle counter here): before the voices (commands, sequencer), pad voices
    // (with their hits), ACID BASS, SYNTH (its notes, knobs and mix plus, in dual mode, the wait for the helper core,
    // else its whole render), preview, master (buses, FX, limiter), after (telemetry, retire, outbox).
    enum ProfPart { kProfPre = 0, kProfVoices, kProfBass, kProfSynth, kProfPreview, kProfMaster, kProfPost, kProfParts };
    std::atomic<uint32_t> prof[kProfParts];
    std::atomic<uint32_t> prof_job{0};           // the SYNTH render alone (on the helper core in dual mode)
    std::atomic<uint32_t> prof_wait{0};          // the audio core waiting for the helper (part of kProfSynth)
    std::atomic<uint32_t> prof_fx{0};            // FX bus: reverb + delay (part of kProfMaster)
    std::atomic<uint32_t> prof_dly{0};           // of that: the stereo delay
    std::atomic<uint32_t> prof_rev{0};           // of that: the plate reverb
    std::atomic<uint32_t> prof_voices_x100{0};   // average sounding pad voices per block x 100
    std::atomic<uint32_t> prof_block{0};         // frames per block of that average
    std::atomic<uint32_t> helper_stalls{0};      // blocks whose SYNTH the helper core did not finish in 5 ms (skipped)
    std::atomic<uint8_t> dual{0};                // 1: the SYNTH renders on the helper core
    Telemetry()
    {
        for (auto& p : prof) p.store(0u, std::memory_order_relaxed);
        for (auto& b : bus_peak) b.store(0.0f, std::memory_order_relaxed);
        for (auto& m : master_peak) m.store(0.0f, std::memory_order_relaxed);
        for (auto& e : pad_env) e.store(0.0f, std::memory_order_relaxed);
    }
};

class EngineImpl;

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Before the audio task starts (or while it is stopped). project must outlive the engine.
    bool init(int sample_rate, const Project* project);
    int sampleRate() const;

    // ---- audio task only ----
    // Renders n (1..kMaxBlock) stereo frames into left/right (overwritten).
    void process(float* left, float* right, int n);
    // Renders n frames as interleaved stereo int16 (TPDF-dithered, clipped). For drivers.
    void processI16(int16_t* lr, int n);
    // Drivers report measured timing so Telemetry::cpu is real: call after each block.
    void reportTiming(uint32_t render_us, uint32_t block_us);
    void reportXrun();

    // ---- other tasks ----
    bool post(Producer from, const Command& c);   // false when that queue is full
    bool poll(Event& e);                          // UI task only (single consumer)
    Telemetry& telemetry();
    const Sample* slot(int i) const;              // current slot content as last seen (may lag a block)
    // Upper polyphony limit (Settings > Audio), kMinVoiceLimit..kVoices; any task. Default kVoices.
    // Voices above a lowered limit are faded out (4 ms) on the next block.
    void setVoiceLimit(int n);
    int voiceLimit() const;
    static constexpr int kMinVoiceLimit = 8;
    // SYNTH on the second core (0.4.9, default on): it renders on the helper core (platform.h) while the pads render
    // on the audio core. Off, or without a helper, it renders inline. The output is the same either way. Any task.
    void setDualCore(bool on);
    bool dualCore() const;
    // Stereo delay code (0.4.9): false = block-wise (default), true = the per-sample reference code (same output;
    // for A/B measurements). Any task.
    void setFxReference(bool on);
    bool fxReference() const;
    // Diagnostics only (0.4.9): the FX bus silent and skipped (memory-contention tests). Default off. Any task.
    void setFxBypass(bool on);
    bool fxLinesInternal() const;   // the reverb's lines are in the internal block of FxBus::reserveLines

private:
    EngineImpl* impl_;
};

}  // namespace orbit
