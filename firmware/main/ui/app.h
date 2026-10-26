#pragma once
// App controller: the single owner of UI state and the bridge between screens, the model
// (Project), the audio engine, the sample pool, the SD library and the loader task.
//
// Threads: every method runs on the LVGL task unless marked [any task]. Screens never touch the
// engine or the Project directly for writes; they call App, which bumps revision counters
// (Revs). Each screen redraws only when a counter it depends on changed (checked in a 30-60 Hz
// LVGL timer), so the UI never rebuilds what did not change.
#include <stdarg.h>
#include <stdint.h>
#include "core/config.h"
#include "core/engine.h"
#include "core/library.h"
#include "core/params.h"
#include "core/project.h"
#include "core/sample.h"
#include "core/settings.h"
#include "hal/hal.h"

namespace orbit::ui {

enum class View : uint8_t { Sample = 0, Sequence, Mixer, Browser, Fx, Bass, Count };   // Bass: 0.3
enum class EditorTab : uint8_t { Trim = 0, Slice, Envelope, Filter, Pitch, Lfo, Count };
// What the SEQUENCE grid edits: trigs, or a value lane of the selected track.
enum class StepLane : uint8_t { Trig = 0, Velocity, Probability, Ratchet, Nudge, Pitch, Cond, Count };
enum class PadMode : uint8_t { Play = 0, Mute, Solo };   // PADS header MUTE / SOLO buttons
enum class SampleOp : uint8_t {
    Trim = 0,      // crop to START..END
    Normalize, Reverse, Duplicate, FadeInOut, TrimSilence, ToMono, PrintCrush, Undo, Count
};

// What a knob/fader edits.
struct ParamRef {
    // OutVolume (appended): the level 0..100 % of the output that plays now (App::outputKind), a setting, not
    // a project value.
    enum class Kind : uint8_t { Pad = 0, Global, BusLevel, BusPan, LayerTune, LayerGain, LayerPan, OutVolume };
    Kind kind = Kind::Pad;
    int8_t index = -1;   // pad / bus / layer; -1 = the selected pad (Pad) or selected layer (Layer*)
    uint8_t id = 0;      // PadParam or GlobalParam
    static ParamRef pad(PadParam p, int pad = -1) { return {Kind::Pad, (int8_t)pad, (uint8_t)p}; }
    static ParamRef global(GlobalParam p) { return {Kind::Global, -1, (uint8_t)p}; }
    static ParamRef busLevel(int b) { return {Kind::BusLevel, (int8_t)b, 0}; }   // b = kBuses: master
    static ParamRef busPan(int b) { return {Kind::BusPan, (int8_t)b, 0}; }
    static ParamRef layer(Kind k, int layer = -1) { return {k, (int8_t)layer, 0}; }
    static ParamRef outVolume() { return {Kind::OutVolume, -1, 0}; }
};

// The output that plays now: Bluetooth headphones (bridge on and a device connected), else the headphone jack
// (Settings > Audio > Output, or HP_DET in Auto) or the built-in speaker. Each keeps its own volume.
enum class OutKind : uint8_t { Speaker = 0, Headphones, Bluetooth, Count };

struct UiState {
    View view = View::Sample;
    EditorTab editor = EditorTab::Trim;
    int pad = 0;                 // selected pad 0..15
    int layer = 0;               // selected layer 0..3
    int seq_page = 0;            // bar shown in SEQUENCE (0..3)
    int seq_bank = 0;            // rows: 0 = pads A1..B4, 1 = C1..D4
    StepLane lane = StepLane::Trig;
    PadMode pad_mode = PadMode::Play;
    Category category = Category::All;
    int lib_sel = -1;            // selected library entry (browser)
    char search[48] = {};
    int settings_tab = 0;        // 0 General .. 6 About
    uint8_t fill = 0;
    uint8_t pad_shown = 1;       // 0 after NEW PROJECT: no pad looks selected until one is touched (selectPad)
};

// Change counters. Screens remember the values they drew and refresh when they differ.
struct Revs {
    uint32_t view = 1;       // view / editor tab / modal changes
    uint32_t selection = 1;  // selected pad / layer
    uint32_t pads = 1;       // pad names, mute/solo, routing, play mode, any pad parameter
    uint32_t params = 1;     // a knob value changed (cheap refresh of knobs only)
    uint32_t pattern = 1;    // steps, scene length/division/swing, scene selection
    uint32_t song = 1;       // song slots, FOLLOW
    uint32_t mixer = 1;      // bus levels / mute / solo / master
    uint32_t fx = 1;         // FX parameters and macros
    uint32_t samples = 1;    // a pad sample changed (load, edit, kit)
    uint32_t library = 1;    // scan finished, filter changed
    uint32_t transport = 1;  // play / stop / record / bpm
    uint32_t settings = 1;
    uint32_t project = 1;    // name, load, new, save state
    uint32_t midi = 1;       // MIDI device connected / gone, learn mode, CC map, external clock tempo
    uint32_t bt = 1;         // Bluetooth: on / off, bridge status, device list, output (appended)
};

class App {
public:
    // ---- lifecycle
    // sd_root: "/sdcard" or nullptr when no card. Creates the engine at sample_rate, the default
    // project (TR-909 built-in kit on pads, demo patterns in scenes A..D) and the loader task.
    bool init(const char* sd_root, int sample_rate);
    void tick();                             // LVGL timer every frame: events, loader results, autosave
    // Audio driver entry (hal::audioStart render callback). [audio task]
    static void render(int16_t* lr, int frames, void* user);

    Project& project();
    Engine& engine();
    Library& library();
    UiState& ui();
    const Revs& revs() const;
    void bump(uint32_t Revs::*field);
    const Settings& settings() const;

    // ---- navigation / selection
    void setView(View v);
    void setEditor(EditorTab t);
    void selectPad(int pad, bool audition);  // audition: plays it when Settings.pad_audition
    void selectLayer(int layer);
    void setPadMode(PadMode m);
    void setSeqPage(int page);
    void setSeqBank(int bank);
    void setLane(StepLane lane);

    // ---- pads (live play)
    // [any task] Touch task / MIDI task call these with their own Producer; never blocks.
    void padDown(int pad, int vel, Producer from);
    void padUp(int pad, Producer from);
    void padTap(int pad);                    // LVGL: honours PadMode (play / toggle mute / solo)
    void toggleMute(int pad);
    void toggleSolo(int pad);
    uint32_t padFlashBits();                 // pads hit since last call (for the pad glow)
    uint32_t takeGridHits();                 // pads padFlashBits() reported since last call (step sparks)

    // ---- parameters
    float paramGet(const ParamRef& r) const;
    void paramSet(const ParamRef& r, float value);
    float paramNorm(const ParamRef& r) const;            // 0..1 knob position
    void paramSetNorm(const ParamRef& r, float norm);
    void paramReset(const ParamRef& r);                  // to the spec default
    const ParamSpec* paramSpec(const ParamRef& r) const; // nullptr for bus kinds (fader law)
    int paramText(const ParamRef& r, char* buf, int len) const;
    const char* paramCaption(const ParamRef& r) const;

    // ---- pad structure (selected pad)
    void setPlayMode(PlayMode m);
    void setReverse(bool on);
    void setChoke(int group);
    void setBus(int bus);
    void setPoly(int voices);
    void setLayerMode(LayerMode m);
    void renamePad(int pad, const char* name);

    // ---- transport
    void play();
    void stop();
    void togglePlay();
    void toggleRecord();
    void setBpm(float bpm);
    void bpmStep(int delta);
    void tap();                              // TAP tempo (average of the last 4 intervals)
    void setMetronome(bool on);
    void setFill(bool on);
    bool playing() const;
    bool recording() const;
    int playStep() const;                    // -1 stopped
    int playScene() const;
    int queuedScene() const;
    int songSlot() const;

    // ---- pattern (current scene = project().scene)
    const Step& step(int pad, int s) const;
    void toggleStep(int pad, int s);
    void setStepOn(int pad, int s, bool on); // drag painting
    void setStepLane(int pad, int s, StepLane lane, int value);
    int stepLaneValue(int pad, int s, StepLane lane) const;
    void clearScene();
    void clearTrack(int pad);
    void randomScene();                      // musically weighted per pad role
    void selectScene(int scene);             // edits + plays (queued while playing)
    void copyScene(int from, int to);
    void setSceneLength(int steps);
    void setDivision(Division d);
    void setSwing(int percent);
    void setTrackLength(int pad, int steps); // 0 = follow scene

    // ---- song (SCENES timeline)
    void setSongSlot(int slot, int scene);   // scene -1 clears; tapping the lit cell clears
    void toggleSongFx(int slot);
    void setFollow(bool on);

    // ---- mixer / fx
    void toggleBusMute(int bus);
    void toggleBusSolo(int bus);
    void toggleMacro(int which);             // 0 LO-FI, 1 WIDE, 2 PUMP, 3 SPACE
    bool macro(int which) const;
    void setLimiter(bool on);
    void setPumpSource(int pad);

    // ---- samples
    const Sample* padSample(int pad, int layer = -1) const;   // -1: first non-empty layer
    const char* padSampleName(int pad) const;
    bool loadFileToPad(int lib_index, int pad, int layer);    // async (loader); toast when done
    bool loadKit(int lib_index);                              // async, whole kit to pads
    void loadBuiltinKit(int kit);                             // 0 TR-808, 1 TR-909
    void previewEntry(int lib_index);
    void previewPad();
    void stopPreview();
    bool applyOp(SampleOp op);                                // selected pad + layer, copy-on-write
    bool canUndo() const;
    void sliceAuto();                                         // transient detection
    void sliceGrid(int count);
    void sliceSelect(int index);                              // region = that slice
    void slicesToPads();                                      // chop to pads from the selected pad on
    void duplicatePadTo(int dst);
    int loadingCount() const;                                 // jobs in the loader queue

    // ---- library (browser)
    void rescan();                                            // async
    void setCategory(Category c);
    void setSearch(const char* text);
    int resultCount() const;                                  // filtered entries
    int result(int i) const;                                  // library index of row i
    void selectEntry(int lib_index);

    // ---- project
    bool save();
    bool saveAs(const char* name);
    bool open(const char* name);
    // NEW PROJECT: a completely empty project (no steps, no song, straight swing, default mixer / FX) with the
    // built-in kit on the pads so they sound; nothing is shown selected until a pad is touched.
    void newProject();
    // Deletes <name>.orb (and its .json export) from the card. The open project stays open; when it was that
    // file it becomes unsaved. revs.project.
    bool deleteProject(const char* name);
    bool exportJson();
    int listProjects(char names[][32], int max);
    bool dirty() const;

    // ---- settings
    Settings& settingsEdit();                // edit a copy...
    void applySettings();                    // ...then apply (brightness, volume, rate, NVS save)

    // ---- status
    bool sdReady() const;
    const char* sdStatus() const;            // "59.5 GB, 12.1 GB wolne" / "Brak karty SD"
    float cpuLoad() const;
    int activeVoices() const;
    float latencyMs() const;
    void toast(const char* fmt, ...);

    // ---- additions (App agent; existing declarations above are unchanged)
    const Sample* previewSample() const;     // browser preview of the selected entry (waveform), may be nullptr
    void setSliceSens(float sens);           // AUTO slice sensitivity 0..1 (LOW 0.25 / MED 0.5 / HIGH 0.8)
    float sliceSens() const;
    void settingsDiscard();                  // drop the settingsEdit() copy (CANCEL)
    void tapAt(uint64_t us);                 // tap() with an explicit timestamp (tests, MIDI)
    void clearPadLayer(int pad, int layer);  // LAYER long press: empty that layer (-1 = selected)

    // ---- additions, wave 2 (App-extras agent; everything above is unchanged)
    // project / pattern
    void renameProject(const char* name);    // file-system safe name (revs.project); the next SAVE writes
                                             // <name>.orb, the file under the old name stays on the card
    void setLaunchQuant(LaunchQuant q);      // scene switch quantization (Settings > General), saved in the
                                             // project (project().quant); revs.settings + revs.transport
    void undoPattern();                      // one level: restores the scene as it was before the last CLEAR,
    bool canUndoPattern() const;             // RANDOM, COPY (the target scene) or CLEAR TRACK (revs.pattern)
    bool saveSampleWav();                    // SAMPLE DETAILS > SAVE WAV: selected pad + layer -> ORBIT/USER/
                                             // <name>.wav (async, toast, library rescan); false = not started
    // Tab5 launcher (the factory app when ORBIT was installed by it)
    bool hasLauncher() const;                // false on the PC and on a standalone flash (ORBIT is the factory app)
    bool exitToLauncher();                   // stop + silence, save first when dirty and Settings.autosave is on
                                             // (blocking), then reboot into the launcher; false (+ toast) when not
                                             // possible. On the Tab5 it does not return on success.
    // MIDI (USB host). Settings.midi_usb / midi_channel / midi_clock_in / midi_clock_out are applied by
    // init() and applySettings(). Notes on the channel play pads (GM/909 map), Start/Continue/Stop drive
    // the transport, clock in sets the BPM, clock out sends 24 PPQN while playing.
    void midiLearn(bool on);                 // learn: move a knob (paramSet*), then a controller: that CC drives
    bool midiLearning() const;               // the knob from then on; until midiLearn(false) or 60 s idle (revs.midi)
    int midiMapCount() const;                // CC bindings (max 32), kept in /sdcard/ORBIT/midimap.bin
    void midiClearMap();
    bool midiConnected() const;              // a USB-MIDI device is open
    const char* midiDeviceName() const;      // "" when none
    float midiClockBpm() const;              // tempo of the incoming MIDI clock (clock in on), 0 = none

    // ---- additions, integration (BROWSER > Kits > PREVIEW): play a kit's first sound without loading the kit
    void previewKit(int lib_index);          // SD kit folder: its first file through the loader (async)
    void previewBuiltinKit(int kit);         // built-in kit 0 / 1: sound 0 from flash

    // ---- additions, review fixes (everything above is unchanged)
    // One-level undo of a pad-set replacement: slicesToPads (TO PADS), loadKit / loadBuiltinKit (LOAD KIT),
    // clearPadLayer (CLEAR LAYER), duplicatePadTo. The replaced pads (samples, edited ones too, parameters,
    // names, routing) come back. Dropped by the next such action, open() and newProject(). revs.pads.
    bool canUndoPads() const;
    bool undoPads();                         // false + toast when there is nothing to undo
    const char* undoPadsLabel() const;       // "TO PADS", "LOAD KIT", ... ("" when none)
    // The current name is the .orb this session opened or saved. A fresh session / NEW starts under a free
    // ORBIT_NN name; SAVE over a different existing <name>.orb needs a second SAVE within 3 s and the
    // autosave never overwrites it. (PROJECT MANAGER: hide the current name from the list only when true.)
    bool projectHasFile() const;
    // exitToLauncher() with unsaved changes it cannot save (Settings.autosave off, no card, save error, name
    // of another project): the first call only warns (false + toast), a second call within 3 s leaves.

    // ---- additions, integration (everything above is unchanged)
    // Pattern undo bookkeeping for the UI: a counter that changes with every new snapshot (RANDOM, CLEAR,
    // CLEAR TRACK, COPY: SEQUENCE turns CLEAR into UNDO when it changes) and the scene undoPattern()
    // restores (-1 none: the timeline "+" menu shows "UNDO <scene>").
    uint32_t patternUndoGen() const;
    int undoPatternScene() const;

    // ---- additions, Bluetooth audio (Aurora Pico bridge, hal::bt*; everything above is unchanged)
    // Changed at once (not through settingsEdit / APPLY) and saved: Settings.bt_enabled / bt_local / bt_volume.
    // On: the bridge starts, its volume and the audio route are set and the engine moves to 44.1 kHz (the
    // rate-change path of applySettings: audio stop -> engine init -> audio start); off: the earlier rate
    // comes back. With Settings.bt_enabled the bridge starts after boot, once app_main's audio start is over (the
    // stream runs, or audioBootDone() was called). exitToLauncher() puts the Pico to sleep first.
    // tick() refreshes btStatus() when the bridge reports a change (revs.bt; toasts on connect / disconnect)
    // and maps the headphone buttons: PLAY -> resume (togglePlay), PAUSE -> stop(), NEXT / PREVIOUS -> scene.
    // Settings > Bluetooth is tab dlg_settings::kTabBluetooth (UiState::settings_tab: 0 General .. 7 About).
    bool btEnabled() const;
    void btSetEnabled(bool on);
    void btSetLocal(bool on);                // speaker / jack keep playing next to Bluetooth
    void btScan();                           // 20 s discovery on the Pico (toast when Bluetooth is off)
    void btSelect(const char* mac);          // connect + remember
    void btReconnect();
    void btTest();                           // 3 s left / silence / right tone on the connected device
    void btSetVolume(int pct);               // Pico volume 0..100, sent at once, saved ~1 s after the last change
    void btAirpodsMode(int mode);            // 1 off, 2 noise cancellation, 3 transparency, 4 adaptive
    const hal::BtStatus& btStatus() const;   // cached copy (tick)
    int btDeviceCount() const;               // saved devices first, then the last scan's results (max 32); 0 while off
    const hal::BtDevice& btDevice(int i) const;
    // app_main, after its audioStart() returned (ok or not): a saved bt_enabled may start now (any task).
    void audioBootDone();

    // ---- additions, 0.3: output volume memory (the main VOLUME knob, ParamRef::outVolume())
    // Settings.vol_speaker / vol_headphones / bt_volume: tick() follows outputKind() and applies the level of
    // the new output by itself (codec volume, or the Pico's volume), with a short toast. setOutputVolume()
    // changes the level of the output that plays now; it is saved ~1.5 s after the last change.
    OutKind outputKind() const;
    int outputVolume(OutKind k) const;       // 0..100
    int outputVolume() const { return outputVolume(outputKind()); }
    void setOutputVolume(int pct);
    static const char* outputName(OutKind k);   // "głośnik" / "słuchawki" / "słuchawki Bluetooth" (toasts)
    static const char* outputTag(OutKind k);    // "SPK" / "HP" / "BT" (knob value)

    // ---- additions, 0.3: restart (Settings > Display > Color theme applies at the start)
    // Like exitToLauncher(): the transport stops, unsaved work is saved first when Settings.autosave is on and
    // the card is there (otherwise a second call within 3 s restarts without saving), Bluetooth parks; then
    // ORBIT restarts and opens the project again (Settings.resume_project). Returns only when it did not
    // restart (false).
    bool restartApp();

    // ---- additions, 0.3: ACID BASS (a 303-style line per scene, the BASS view; knobs: GP_BASS_* globals)
    const BassStep& bassStep(int s) const;         // step s (0..kMaxSteps-1) of the edited scene's line
    void setBassStep(int s, const BassStep& b);    // the playing scene uses it at once (revs.pattern)
    // BASS keyboard: MIDI note now, until bassRelease(); legato glides from the sounding note (KEYS SLIDE)
    void bassAudition(int note, bool accent, bool legato = false);
    void bassRelease();
    // [any task] BASS KEYS with several fingers (0.4.3): the touch task posts straight into the engine like
    // padDown (Producer::Touch, lock-free, a full queue drops the note)
    void bassKeyOn(int note, bool accent, bool legato, Producer from);
    void bassKeyOff(Producer from);
    void bassRandom();                             // RANDOM: a new acid line for the edited scene (UNDO)
    void bassClear();                              // CLEAR: every step rests (UNDO: undoPattern)
    uint32_t bassNotes() const;                    // Telemetry::bass_notes: the BASS view flashes the step

    // ---- additions, 0.4.9: SYNTH (a polyphonic note line per scene, core/project.h SynthLine; knobs: GP_SYN_*)
    const SynthStep& synthStep(int s) const;       // step s (0..kMaxSteps-1) of the edited scene's synth line
    void setSynthStep(int s, const SynthStep& st); // notes are clamped; the playing scene uses it at once
    void synthClear();                             // CLEAR: the edited scene's synth line (UNDO: undoPattern)
    // [any task] live notes (keyboard, MIDI): sound until synthKeyOff with the same note (note < 0: all)
    void synthKeyOn(int note, int vel, Producer from);
    void synthKeyOff(int note, Producer from);
    void synthPreset(int i);                       // factory patch i into the project's SYNTH knobs
    uint32_t synthNotes() const;                   // Telemetry::synth_notes
};

App& app();

}  // namespace orbit::ui
