// App controller (see app.h): UI state, model edits, engine bridge, sample pool and loader task.
//
// Ownership of samples (the subtle part):
//  * The engine owns the slot table; this file keeps a mirror (State::slots) with the Sample*
//    last posted to each slot, a reference count (pad layers + undo entries using the slot) and
//    the SampleRef used when saving.
//  * Every heap Sample this file adopts is listed in a registry with two counters:
//      engine = installs (SetSample / Preview) not yet answered by SampleRetired / PreviewDone,
//      holds  = undo entries and loader jobs that still read it.
//    maybeFree() frees a sample only when both are 0 and no slot / browser preview shows it.
//  * Embedded (built-in kit) samples are wrapped once, cached and never freed.
//
// MIDI (USB host, hal::midiStart): midiHook() runs on the USB client task. Notes play pads at once
// (Producer::Midi, that engine queue has no other writer), Start / Continue / Stop are posted to the
// engine the same way, clock ticks feed a tempo estimator owned by that task; CCs and transport
// changes reach the LVGL task through State::midi_q (SPSC) and are handled in tick() (MIDI learn,
// CC -> paramSetNorm). Clock out is sent by clockOutService() on the hal service timer (1 ms).
//
// This file deliberately does not include LVGL headers: it only needs ui::toast() from
// widgets.h, declared below, so the controller builds in host tests without LVGL.
#include "ui/app.h"

#include <atomic>
#include <math.h>
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "core/kits.h"
#include "core/platform.h"
#include "core/sampleio.h"
#include "core/spsc.h"
#include "hal/hal.h"

namespace orbit::ui {

void toast(const char* text);  // ui/widgets.h

namespace {

// ------------------------------------------------------------------------------- constants
constexpr int kJobQ = 32;
constexpr int kResQ = 32;
constexpr int kDeferMax = 256;
constexpr int kRegMax = 512;
constexpr int kBuiltinKits = 4;
constexpr uint64_t kAutosaveUs = 300ull * 1000000ull;  // 5 min
constexpr uint64_t kTapResetUs = 2000000ull;           // TAP resets after 2 s
constexpr float kBpmMin = 30.0f, kBpmMax = 300.0f;
constexpr float kPreviewSeconds = 60.0f;
constexpr int kMidiMapMax = 32;                  // CC bindings (MIDI learn)
constexpr int kMidiQ = 256;                      // USB task -> LVGL task (CC, transport)
constexpr int kClockWin = 48;                    // clock-in intervals averaged for the tempo (2 beats)
constexpr int kClockMinIntervals = 24;           // one beat before a tempo is reported
constexpr uint32_t kClockLostMs = 1000;          // no clock tick for this long: external tempo gone
constexpr uint32_t kMidiMapMagic = 0x314D4D4Fu;  // "OMM1" (midimap.bin)
constexpr uint64_t kConfirmUs = 3000000ull;      // second tap within 3 s confirms (overwrite, exit unsaved)
constexpr uint64_t kOverloadNoticeUs = 30000000ull;   // "CPU audio przeciążone" at most every 30 s
constexpr uint64_t kLearnIdleUs = 60000000ull;   // MIDI learn switches itself off after 60 s without activity
// Deferred NVS writes stall both cores (flash cache off): the HAL may do them only when the output has been
// silent this long (no transport, no voice, master peak below kSilentPeak: FX tails, previews, the click and
// the limiter latency all show in the master peak) and no hit was posted meanwhile.
constexpr uint64_t kIdleHoldUs = 3000000ull;
constexpr float kSilentPeak = 2.0f / 32768.0f;
constexpr uint32_t kAuditionMs = 200;            // RecordHit of an audition (waveform tap) arrives within this
constexpr int kBtDevMax = 32;                    // the Pico's LIST: 8 saved + 24 found (hal/bt_protocol.h kMaxDevices)
constexpr uint64_t kBtNameWaitUs = 1500000ull;   // "połączono z <name>": wait this long for the name
constexpr uint64_t kBtSaveUs = 1000000ull;       // volume saved this long after the last change
constexpr uint64_t kBtQuietUs = 5000000ull;      // SCAN / SELECT / RECONNECT restart the Pico: no "rozłączono" toast

// ------------------------------------------------------------------------------- strings
void copyStr(char* dst, int cap, const char* src)
{
    if (!dst || cap <= 0) return;
    int i = 0;
    if (src)
        for (; i < cap - 1 && src[i]; ++i) dst[i] = src[i];
    dst[i] = 0;
}

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
int sfmt(char* dst, int cap, const char* fmt, ...)
{
    if (!dst || cap <= 0) return 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, (size_t)cap, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n >= cap) n = cap - 1;
    return n;
}

const char* baseName(const char* path)
{
    if (!path) return "";
    const char* b = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
bool isAlpha(char c) { c = lower(c); return c >= 'a' && c <= 'z'; }

bool startsWith(const char* s, const char* prefix)
{
    if (!s || !prefix) return false;
    while (*prefix)
        if (*s++ != *prefix++) return false;
    return true;
}

bool containsCi(const char* hay, const char* needle)
{
    if (!hay || !needle || !*needle) return false;
    for (const char* h = hay; *h; ++h) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b && lower(*a) == lower(*b)) ++a, ++b;
        if (!*b) return true;
    }
    return false;
}

// Whole-word match ("sd" matches "SD" and "SD 2", not "ASDF").
bool hasWord(const char* hay, const char* word)
{
    const int n = (int)strlen(word);
    for (const char* h = hay; *h; ++h) {
        if (h != hay && isAlpha(h[-1])) continue;
        int i = 0;
        while (i < n && h[i] && lower(h[i]) == lower(word[i])) ++i;
        if (i == n && !isAlpha(h[n])) return true;
    }
    return false;
}

bool eqCi(const char* a, const char* b)
{
    if (!a || !b) return false;
    while (*a && *b)
        if (lower(*a++) != lower(*b++)) return false;
    return *a == *b;
}

void padLabel(int pad, char* out, int cap)
{
    sfmt(out, cap, "%c%d", (char)('A' + (pad / 4) % 4), pad % 4 + 1);
}

int makeDir(const char* path)
{
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0775);
#endif
}

// Removes the files directly inside dir (not sub-folders), then dir itself when it is empty.
void removeDirFiles(const char* dir)
{
    DIR* d = opendir(dir);
    if (!d) return;
    char path[kPathLen + 64];
    struct dirent* de;
    struct stat st;
    while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] == '.') continue;
        sfmt(path, (int)sizeof path, "%s/%s", dir, de->d_name);
        if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode)) remove(path);
    }
    closedir(d);
#if defined(_WIN32)
    _rmdir(dir);
#else
    rmdir(dir);
#endif
}

// Keeps project names file-system safe.
void sanitizeName(char* dst, int cap, const char* src)
{
    int o = 0;
    for (int i = 0; src && src[i] && o < cap - 1; ++i) {
        const unsigned char c = (unsigned char)src[i];
        const bool bad = c < 32 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
                         c == '<' || c == '>' || c == '|';
        dst[o++] = bad ? '_' : (char)c;
    }
    while (o > 0 && (dst[o - 1] == ' ' || dst[o - 1] == '.')) --o;  // FAT: no trailing dot/space
    dst[o] = 0;
    if (!o) copyStr(dst, cap, "ORBIT_01");
}

// ------------------------------------------------------------------------------- loader jobs
enum class JobType : uint8_t { LoadWav = 0, LoadKit, Probe, Scan, Save, Load, Op, Export, Slice, SaveWav };

struct KitJob {
    char name[kNameLen + 8];
    int count;
    char paths[kPads][kPathLen];
    Sample* out[kPads];
    WavError err[kPads];
};

struct SaveWrite {
    const Sample* s;
    int16_t slot;
    uint8_t ok;
    uint8_t _pad;
    uint32_t id;
    char path[kPathLen];
};

struct SaveJob {
    Project proj;
    SampleRef refs[kPads][kLayers];
    char path[kPathLen];
    char dir[kPathLen];
    int nwrite;
    SaveWrite w[kMaxSamples];
    uint32_t gen;
    uint8_t ok;
    char err[96];
};

struct LoadJob {
    Project proj;
    SampleRef refs[kPads][kLayers];
    Sample* smp[kPads][kLayers];
    char path[kPathLen];
    char name[32];
    int missing;
    uint8_t ok;
    char err[96];
};

struct Job {
    JobType type = JobType::LoadWav;
    int8_t pad = 0;
    int8_t layer = 0;
    uint8_t flag = 0;            // Probe: preview when loaded; Save: autosave
    SampleOp op = SampleOp::Trim;
    int32_t index = -1;          // library entry / back library index (Scan)
    uint32_t src_id = 0;         // Op: id of the sample the op started from
    const Sample* src = nullptr; // Op: source (held while the job runs)
    float pp[kPadParamCount];    // Op / Slice: pad parameters when the job was started
    float fval = 0.0f;           // Slice: sensitivity
    void* payload = nullptr;     // KitJob / SaveJob / LoadJob
    const Library* lib = nullptr;// LoadKit / kit preview: the library the entry index belongs to (kitFiles on the loader)
    char path[kPathLen] = {};
};

struct Result {
    Job job;
    Sample* sample = nullptr;
    WavError err = WavError::Ok;
    int count = 0;
    uint64_t free_bytes = 0, total_bytes = 0;
    float pos[kMaxSlices] = {};  // Slice: detected positions (count)
    WavInfo info;                // Probe: header of the decoded file (browser details without a second read)
    uint32_t bytes = 0;          // Probe: file size
    uint8_t have_info = 0;
};

// ------------------------------------------------------------------------------- state
struct SlotInfo {
    Sample* s = nullptr;   // content of the slot (posted to the engine unless `wait`)
    uint16_t refs = 0;     // pad layers + undo entries naming this slot
    uint8_t edited = 0;    // content is not the file in ref.path (save writes a WAV)
    uint8_t wait = 0;      // s not posted yet: its previous install is still being retired (postSetSample)
    SampleRef ref;
};

struct Owned {
    Sample* s = nullptr;
    int16_t engine = 0;    // slot installs (SetSample) not yet answered by SampleRetired
    int16_t holds = 0;     // undo entries / loader jobs reading it
    int16_t pv = 0;        // previews (CmdType::Preview) not yet answered by PreviewDone
};

// One-level undo of a pad-set replacement (TO PADS, LOAD KIT, built-in kit, CLEAR LAYER): the Pad structs
// as they were; every layer slot in them keeps a reference, so replaced and edited samples stay alive.
struct PadUndo {
    uint8_t valid = 0;
    uint32_t mask = 0;            // pads saved
    char what[24] = {};
    Pad pad[kPads];
};

struct Undo {
    uint8_t valid = 0;
    uint8_t other_slot = 0;   // result went to a new slot because the source slot was shared
    uint8_t reverse = 0;
    uint8_t slice_count = 0;
    uint8_t prev_edited = 0;
    int16_t slot = -1;        // slot holding the result
    int16_t prev_slot = -1;   // other_slot: the shared slot to return to (a reference is held)
    Sample* prev = nullptr;   // !other_slot: previous content (held)
    uint32_t result_id = 0;
    SampleRef prev_ref;
    float p[kPadParamCount];
    float slices[kMaxSlices];
    float slice_end = 1.0f;   // Pad::slice_end when project.h has it
};

// ------------------------------------------------------------------------------- MIDI types
enum class MidiEv : uint8_t { Cc = 0, Start, Continue, Stop };

struct MidiIn {                // USB task -> LVGL task
    MidiEv type = MidiEv::Cc;
    uint8_t ch = 0;            // 1..16
    uint8_t a = 0;             // CC number
    uint8_t b = 0;             // CC value
};

struct MidiBinding {           // MIDI learn: controller (channel + CC) -> knob
    uint8_t cc = 0;
    uint8_t ch = 1;
    ParamRef ref;
};

// Shared with the USB task and the service timer (lock-free).
struct MidiShared {
    std::atomic<uint8_t> on{0};               // Settings.midi_usb and the USB host is running
    std::atomic<uint8_t> channel{10};         // note channel 1..16, 0 = omni
    std::atomic<uint8_t> clock_in{0};
    std::atomic<uint8_t> clock_out{0};
    std::atomic<uint8_t> clock_reset{0};      // LVGL -> USB task: forget the tempo estimate
    std::atomic<uint32_t> clock_bpm_milli{0}; // estimated external tempo * 1000, 0 = none yet
    std::atomic<uint32_t> clock_last_ms{0};   // hal::micros() / 1000 of the newest clock tick
    std::atomic<uint8_t> clock_settled{0};    // the estimate spans a full window (48 intervals)
    std::atomic<uint32_t> latency_us{0};      // audio output latency (clock out lines up with the sound)
    std::atomic<uint32_t> rx{0};              // messages received
    std::atomic<uint32_t> dropped{0};         // midi_q full
};

struct alignas(64) State {
    Project* project = nullptr;
    UiState ui;
    Revs revs;
    Settings settings;
    Settings settings_edit;
    bool inited = false;
    bool ui_ready = false;
    bool sd = false;
    bool playing = false;
    bool recording = false;
    bool master_muted = false;
    bool scan_busy = false;
    bool scan_again = false;
    bool save_busy = false;
    bool autosave_next = false;   // the next saveAs() is the 5-minute autosave (quiet toast)
    bool autosave_warned = false; // autosave skipped once because <name>.orb belongs to another project
    bool audio_idle = true;       // last value given to hal::setAudioIdle (the HAL starts idle)
    uint64_t loud_us = 0;         // last tick() that was not silent (kIdleHoldUs)
    std::atomic<uint32_t> live_ms{0};   // last live hit / audition posted (any task), ms | 1
    char file_name[32] = {};      // the .orb this session opened or saved ("" = none): SAVE may overwrite it
    char confirm_key[40] = {};    // App-level second-tap confirmations (confirmTwice)
    uint64_t confirm_us = 0;
    bool any_wait = false;        // some slot has wait = 1 (flushWaiting)
    bool worker = false;
    float master_unmute_db = 0.0f;
    int rate = 48000;
    char sd_root[kPathLen] = {};
    char sd_status[64] = {};
    char pending_toast[128] = {};

    Library libs[2];
    int front = 0;
    int results[kMaxLibraryFiles];
    int result_count = 0;

    SlotInfo slots[kMaxSamples];
    Owned reg[kRegMax];
    Undo undo[kPads][kLayers];
    Sample* builtin[kBuiltinKits][kPads] = {};

    Sample* bp = nullptr;          // browser preview sample (waveform + PREVIEW)
    int bp_index = -1;
    char bp_path[kPathLen] = {};

    Spsc<Job, kJobQ> jobs;
    Spsc<Result, kResQ> res;
    uint32_t jobs_posted = 0;
    uint32_t jobs_done = 0;

    Command defer[kDeferMax];
    int defer_head = 0;
    int defer_count = 0;

    uint32_t flash = 0;
    uint32_t grid_hits = 0;   // what padFlashBits() handed out, kept for takeGridHits() (step sparks)
    std::atomic<uint32_t> touch_ms[kPads] = {};   // last padDown(Touch) per pad, written by the touch task
    uint32_t dirty_gen = 0;
    uint32_t saved_gen = 0;
    uint64_t last_save_us = 0;
    uint64_t taps[5] = {};
    int tap_count = 0;
    uint32_t rnd = 0x9E3779B9u;
    float slice_sens = 0.5f;

    float cpu = 0.0f;
    int voices = 0;
    float latency = 0.0f;

    // MIDI
    MidiShared midi;
    Spsc<MidiIn, kMidiQ> midi_q;
    bool midi_started = false;     // hal::midiStart() succeeded (the USB host stays on afterwards)
    bool clock_timer = false;      // clock out sender running on the service timer
    bool midi_conn = false;        // last hal::midiConnected() seen by tick()
    bool learn = false;            // MIDI learn mode
    bool learn_armed = false;      // a knob moved in learn mode: the next CC binds to learn_ref
    bool cc_applying = false;      // paramSet called by a bound CC (does not re-arm learn)
    uint64_t learn_us = 0;         // hal::micros() of the last learn activity (start, knob armed, binding)
    ParamRef learn_ref;
    MidiBinding map[kMidiMapMax];
    int map_count = 0;
    float ext_bpm = 0.0f;          // external clock tempo shown (0 = none)
    bool ext_tracking = false;     // the project BPM follows the estimate until it settles

    // pattern undo (one level)
    Scene pat_undo;
    int8_t pat_undo_scene = -1;
    uint32_t pat_undo_gen = 0;     // ++ per snapshot (patternUndoGen: the SEQUENCE view arms UNDO on a new one)
    uint64_t overload_us = 0;      // last "CPU overload" notice (governor below the voice limit)
    bool launcher = false;         // hal::launcherAvailable() (cached at init)

    PadUndo pad_undo;              // TO PADS / LOAD KIT / CLEAR LAYER (undoPads)
    uint32_t audition_ms[kPads] = {};   // last audition Trigger per pad while recording (its RecordHit is dropped)

    // Bluetooth (Aurora Pico bridge)
    hal::BtStatus bt = {};         // last hal::btStatus() with a new revision
    hal::BtDevice bt_dev[kBtDevMax] = {};
    int bt_dev_count = 0;
    bool bt_on = false;            // the bridge is enabled (Settings.bt_enabled applied)
    bool bt_boot = false;          // Settings.bt_enabled at init: start once the boot audio start is over
    bool bt_conn_toast = false;    // connected, the toast waits for the device name (kBtNameWaitUs)
    uint64_t bt_conn_us = 0;
    bool bt_save = false;          // a volume change waits to be saved (bt_save_us)
    uint64_t bt_save_us = 0;       // hal::micros() of the last volume change
    int bt_prev_rate = 0;          // engine rate to restore when Bluetooth goes off (0: none)
    uint64_t bt_quiet_us = 0;      // hal::micros() of the last SCAN / SELECT / RECONNECT (kBtQuietUs)
    std::atomic<bool> audio_boot_done{false};   // app_main: its audioStart() returned (audioBootDone)

    char resume_name[32] = {};           // restartApp(): open this project at the first tick
    // Output volume memory (outputKind / setOutputVolume)
    OutKind out_kind = OutKind::Count;   // output whose level is applied (Count: nothing applied yet)
    int local_vol = -1;                  // codec volume last sent (speaker / jack)
    bool vol_save = false;               // a level change waits to be saved (vol_save_us)
    uint64_t vol_save_us = 0;
};

State* g_state = nullptr;

State& S()
{
    if (!g_state) {
        // PSRAM on the Tab5 (the state is ~100 KB); bigAlloc aligns to 16, State wants 64.
        void* raw = bigAlloc(sizeof(State) + 64);
        if (!raw) {
            logf("app: out of memory for state");
            for (;;) sleepMs(1000);
        }
        uintptr_t a = ((uintptr_t)raw + 63) & ~(uintptr_t)63;
        g_state = new ((void*)a) State();
        settingsDefaults(g_state->settings);
        g_state->settings_edit = g_state->settings;
    }
    return *g_state;
}

Engine& eng()
{
    static Engine e;
    return e;
}

Project& P() { return *S().project; }
Library& front() { return S().libs[S().front]; }

void markDirty() { ++S().dirty_gen; }

// ------------------------------------------------------------------------------- project files
// <name>.orb exists on the card.
bool projectFileExists(const char* name)
{
    if (!S().sd || !name || !name[0]) return false;
    char path[kPathLen];
    struct stat st;
    return front().projectPath(name, path, sizeof path) && stat(path, &st) == 0;
}

// <name>.orb is the file this session opened or saved (SAVE / autosave may write it without asking).
bool ownsFile(const char* name) { return S().file_name[0] && name && strcmp(S().file_name, name) == 0; }

// First ORBIT_NN without a .orb on the card (every boot and NEW start under a free name).
void pickFreeName(char* out, int cap)
{
    for (int k = 1; k < 100; ++k) {
        char cand[32];
        snprintf(cand, sizeof cand, "ORBIT_%02d", k);
        if (!projectFileExists(cand)) {
            copyStr(out, cap, cand);
            return;
        }
    }
    // ORBIT_01..99 all taken: the name stays, SAVE asks before overwriting.
}

// App-level "tap again within 3 s" confirmation; true on the confirming (second) call with the same key.
bool confirmTwice(const char* key)
{
    State& s = S();
    const uint64_t now = nowUs();
    if (s.confirm_key[0] && strcmp(s.confirm_key, key) == 0 && now - s.confirm_us < kConfirmUs) {
        s.confirm_key[0] = 0;
        return true;
    }
    copyStr(s.confirm_key, sizeof s.confirm_key, key);
    s.confirm_us = now;
    return false;
}

void say(const char* text)
{
    State& s = S();
    if (!s.ui_ready) {
        copyStr(s.pending_toast, sizeof(s.pending_toast), text);
        return;
    }
    ::orbit::ui::toast(text);
}

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
void sayf(const char* fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    say(buf);
}

uint32_t rnd32()
{
    State& s = S();
    uint32_t x = s.rnd;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s.rnd = x;
    return x;
}
float rndf() { return (float)(rnd32() >> 8) * (1.0f / 16777216.0f); }
int rndi(int lo, int hi) { return lo + (int)(rnd32() % (uint32_t)(hi - lo + 1)); }

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Settings > Display colours (the UI's theme.h lives on the LVGL side; core/settings.h has the counts):
// a base or accent out of range is the default, the CUSTOM hue 0..359, its saturation 0 (never set) or
// kAccentSatMin..100.
void sanitizeColors(Settings& t)
{
    if (t.base >= kBaseCount) t.base = 0;
    if (t.theme > kAccentCustom) t.theme = 0;
    t.accent_hue = (uint16_t)clampi(t.accent_hue, 0, 359);
    if (t.accent_sat) t.accent_sat = (uint8_t)clampi(t.accent_sat, kAccentSatMin, 100);
}

// ------------------------------------------------------------------------------- engine posting
void flushDeferred()
{
    State& s = S();
    while (s.defer_count > 0 && eng().post(Producer::Ui, s.defer[s.defer_head])) {
        s.defer_head = (s.defer_head + 1) % kDeferMax;
        --s.defer_count;
    }
}

// UI-task commands keep their order: when the engine queue is full they wait in a local FIFO
// that tick() flushes. Slot changes must never be lost (the mirror would diverge).
void postUi(const Command& c)
{
    State& s = S();
    if (s.defer_count == 0 && eng().post(Producer::Ui, c)) return;
    if (s.defer_count < kDeferMax) {
        s.defer[(s.defer_head + s.defer_count) % kDeferMax] = c;
        ++s.defer_count;
        return;
    }
    logf("app: command queue overflow (type %d)", (int)c.type);
}

void postSimple(CmdType t, int value = 0, int pad = 0, int vel = 100)
{
    Command c;
    c.type = t;
    c.value = value;
    c.pad = (uint8_t)pad;
    c.vel = (uint8_t)vel;
    postUi(c);
}

// [any task] A hit or preview was just posted: the output is about to sound, so no deferred flash
// write may start now (tick() keeps the idle flag off for kIdleHoldUs after the newest one).
void noteSound()
{
    State* st = g_state;
    if (!st) return;
    st->live_ms.store((uint32_t)(nowUs() / 1000u) | 1u, std::memory_order_relaxed);
    hal::setAudioIdle(false);
}

// LVGL task: a pad hit posted by the UI. audition = listening only (waveform tap, pad selection): while
// REC records, its RecordHit is dropped in tick() (the engine records every live Trigger).
void postTrigger(int pad, int vel, bool audition)
{
    State& s = S();
    if (pad < 0 || pad >= kPads) return;
    postSimple(CmdType::Trigger, 0, pad, vel);
    noteSound();
    if (audition) s.audition_ms[pad] = s.recording && s.playing ? ((uint32_t)(nowUs() / 1000u) | 1u) : 0u;
}

// ------------------------------------------------------------------------------- registry
int regFind(const Sample* p)
{
    if (!p) return -1;
    State& s = S();
    for (int i = 0; i < kRegMax; ++i)
        if (s.reg[i].s == p) return i;
    return -1;
}

// Adopts a heap sample (loader result). Embedded samples are never registered.
void regAdd(Sample* p)
{
    if (!p || p->embedded || regFind(p) >= 0) return;
    State& s = S();
    for (int i = 0; i < kRegMax; ++i)
        if (!s.reg[i].s) {
            s.reg[i] = Owned{p, 0, 0, 0};
            return;
        }
    logf("app: sample registry full, %s is never freed", p->name);
}

void regEngine(const Sample* p, int d)
{
    const int i = regFind(p);
    if (i < 0) return;
    Owned& o = S().reg[i];
    o.engine = (int16_t)(o.engine + d);
    if (o.engine < 0) o.engine = 0;
}

void regHold(const Sample* p, int d)
{
    const int i = regFind(p);
    if (i < 0) return;
    Owned& o = S().reg[i];
    o.holds = (int16_t)(o.holds + d);
    if (o.holds < 0) o.holds = 0;
}

void regPreview(const Sample* p, int d)
{
    const int i = regFind(p);
    if (i < 0) return;
    Owned& o = S().reg[i];
    o.pv = (int16_t)(o.pv + d);
    if (o.pv < 0) o.pv = 0;
}

int regEngineCount(const Sample* p)
{
    const int i = regFind(p);
    return i < 0 ? 0 : S().reg[i].engine;
}

bool inAnySlot(const Sample* p)
{
    State& s = S();
    for (int i = 0; i < kMaxSamples; ++i)
        if (s.slots[i].s == p) return true;
    return false;
}

void maybeFree(Sample* p)
{
    if (!p || p->embedded) return;
    State& s = S();
    const int i = regFind(p);
    if (i < 0) return;
    const Owned& o = s.reg[i];
    if (o.engine > 0 || o.pv > 0 || o.holds > 0 || p == s.bp || inAnySlot(p)) return;
    s.reg[i] = Owned{};
    sampleFree(p);
}

// Engine answer (tick / drainEngine): SampleRetired answers one slot install, PreviewDone one preview.
void engineAnswered(const Event& e)
{
    Sample* smp = const_cast<Sample*>(e.sample);
    if (e.type == EvtType::SampleRetired) regEngine(smp, -1);
    else regPreview(smp, -1);
    maybeFree(smp);
}

// A sample that never reached the registry (discarded loader result).
void freeUnowned(Sample* p)
{
    if (!p || p->embedded || regFind(p) >= 0) return;
    sampleFree(p);
}

// ------------------------------------------------------------------------------- slots
// Install protocol. Owned::engine counts slot installs the engine has not answered yet; the engine sends
// one SampleRetired when a pointer leaves its LAST slot and no voice / preview uses it any more. So:
//  * a heap Sample sits in at most one slot (assignSample / undoApply share that slot instead), and
//  * a Sample whose previous install is still being retired (counted, but in no posted slot: voices
//    fading, a browser preview still playing it, or SampleRetired not polled yet) is not posted again
//    before that answer: the engine would cancel the retirement without answering it (a leak). The
//    slot shows it at once (wait = 1); flushWaiting() posts it when the answer came.
// Together they give exactly one answer per counted install.
void postRaw(int slot, const Sample* smp)
{
    Command c;
    c.type = CmdType::SetSample;
    c.value = slot;
    c.sample = smp;
    postUi(c);
}

bool inPostedSlot(const Sample* p)
{
    const State& s = S();
    for (int i = 0; i < kMaxSamples; ++i)
        if (s.slots[i].s == p && !s.slots[i].wait) return true;
    return false;
}

// An earlier install of p is still being retired by the engine.
bool retiring(const Sample* p) { return p && regEngineCount(p) > 0 && !inPostedSlot(p); }

void postSetSample(int slot, Sample* smp)
{
    State& s = S();
    SlotInfo& si = s.slots[slot];
    Sample* const sent = si.wait ? nullptr : si.s;   // what the engine slot holds
    si.wait = 0;
    if (smp == sent) {   // nothing changes for the engine (it would not answer a repeated install)
        si.s = smp;
        return;
    }
    if (smp && retiring(smp)) {
        if (sent) postRaw(slot, nullptr);   // the old content leaves now (its own SampleRetired follows)
        si.s = smp;
        si.wait = 1;
        s.any_wait = true;
        // A preview still playing it holds the retirement: stop it so the pad sounds within a few ms.
        const int i = regFind(smp);
        if (i >= 0 && s.reg[i].pv > 0) postSimple(CmdType::StopPreview);
        return;
    }
    postRaw(slot, smp);
    if (smp) regEngine(smp, +1);
    si.s = smp;
}

// tick(): posts the slots that waited for an earlier retirement of their sample.
void flushWaiting()
{
    State& s = S();
    if (!s.any_wait) return;
    bool left = false;
    for (int i = 0; i < kMaxSamples; ++i) {
        SlotInfo& si = s.slots[i];
        if (!si.wait) continue;
        if (!si.s) {
            si.wait = 0;
            continue;
        }
        if (retiring(si.s)) {
            left = true;
            continue;
        }
        si.wait = 0;
        postRaw(i, si.s);
        regEngine(si.s, +1);
    }
    s.any_wait = left;
}

// The slot already holding p (refs > 0), -1 none.
int slotOf(const Sample* p)
{
    if (!p) return -1;
    const State& s = S();
    for (int i = 0; i < kMaxSamples; ++i)
        if (s.slots[i].s == p && s.slots[i].refs > 0) return i;
    return -1;
}

int slotAlloc()
{
    State& s = S();
    for (int i = 0; i < kMaxSamples; ++i)
        if (s.slots[i].refs == 0 && !s.slots[i].s) return i;
    return -1;
}

void slotRetain(int slot)
{
    if (slot >= 0 && slot < kMaxSamples) ++S().slots[slot].refs;
}

void slotRelease(int slot)
{
    if (slot < 0 || slot >= kMaxSamples) return;
    SlotInfo& si = S().slots[slot];
    if (si.refs > 0) --si.refs;
    if (si.refs) return;
    Sample* old = si.s;
    if (old) postSetSample(slot, nullptr);   // engine retires `old` once no voice uses it
    si.ref = SampleRef{};
    si.edited = 0;
    maybeFree(old);
}

bool padUndoDrop();

// Opens a fresh slot with content smp (refs = 1). -1 when the table is full (the pad-set undo gives
// its slots back first).
int slotOpen(Sample* smp, const SampleRef& ref, bool edited)
{
    int slot = slotAlloc();
    if (slot < 0 && padUndoDrop()) slot = slotAlloc();
    if (slot < 0) return -1;
    SlotInfo& si = S().slots[slot];
    si.refs = 1;
    si.ref = ref;
    si.edited = edited ? 1 : 0;
    postSetSample(slot, smp);
    return slot;
}

Sample* slotSample(int slot) { return (slot >= 0 && slot < kMaxSamples) ? S().slots[slot].s : nullptr; }

// ------------------------------------------------------------------------------- undo
void undoDrop(int pad, int layer)
{
    Undo& u = S().undo[pad][layer];
    if (!u.valid) return;
    u.valid = 0;
    if (u.other_slot) {
        const int ps = u.prev_slot;
        u.prev_slot = -1;
        slotRelease(ps);
    } else {
        Sample* prev = u.prev;
        u.prev = nullptr;
        regHold(prev, -1);
        maybeFree(prev);
    }
}

void undoDropPad(int pad)
{
    for (int l = 0; l < kLayers; ++l) undoDrop(pad, l);
}

// ------------------------------------------------------------------------------- pads
int padIndex(int pad) { return pad < 0 ? S().ui.pad : clampi(pad, 0, kPads - 1); }

void resetRegion(Pad& p)
{
    p.p[PP_START] = 0.0f;
    p.p[PP_END] = 1.0f;
    p.slice_count = 0;
    p.slice_sel = 0;
}

void setLayerSlot(int pad, int layer, int slot)
{
    Layer& L = P().pad[pad].layer[layer];
    const int old = L.slot;
    L.slot = (int16_t)slot;
    if (old >= 0) slotRelease(old);
}

void clearLayer(int pad, int layer)
{
    undoDrop(pad, layer);
    setLayerSlot(pad, layer, -1);
}

// Puts smp on a pad layer through a new slot, or through the slot that already holds it (the same
// decoded file loaded again, e.g. the browser preview onto a second pad). Takes ownership (registers
// heap samples).
bool assignSample(int pad, int layer, Sample* smp, const SampleRef& ref, bool edited)
{
    if (!smp) return false;
    regAdd(smp);
    undoDrop(pad, layer);
    const int shared = slotOf(smp);
    if (shared >= 0) {
        slotRetain(shared);
        setLayerSlot(pad, layer, shared);
        return true;
    }
    const int slot = slotOpen(smp, ref, edited);
    if (slot < 0) {
        say("Brak wolnych slotów na sample");
        maybeFree(smp);
        return false;
    }
    setLayerSlot(pad, layer, slot);
    return true;
}

bool padEmpty(const Pad& p)
{
    for (int l = 0; l < kLayers; ++l)
        if (p.layer[l].slot >= 0) return false;
    return true;
}

// Copies pad src over dst, sharing its sample slots (reference counted).
void copyPad(int src, int dst)
{
    if (src == dst) return;
    undoDropPad(dst);
    for (int l = 0; l < kLayers; ++l) setLayerSlot(dst, l, -1);
    Project& pr = P();
    Pad copy = pr.pad[src];
    for (int l = 0; l < kLayers; ++l) slotRetain(copy.layer[l].slot);
    copy.mute = 0;
    copy.solo = 0;
    pr.pad[dst] = copy;
}

// ------------------------------------------------------------------------------- pad-set undo
// Gives the saved pads' slot references back (next snapshot, project open / new, a full slot table).
bool padUndoDrop()
{
    State& s = S();
    PadUndo& u = s.pad_undo;
    if (!u.valid) return false;
    u.valid = 0;
    for (int i = 0; i < kPads; ++i) {
        if (!(u.mask & (1u << i))) continue;
        for (int l = 0; l < kLayers; ++l) {
            const int sl = u.pad[i].layer[l].slot;
            u.pad[i].layer[l].slot = -1;
            slotRelease(sl);
        }
    }
    u.mask = 0;
    ++s.revs.pads;   // canUndoPads()
    return true;
}

// Before TO PADS / LOAD KIT / CLEAR LAYER ...: remembers the pads in mask (one level).
void padUndoTake(uint32_t mask, const char* what)
{
    padUndoDrop();
    State& s = S();
    PadUndo& u = s.pad_undo;
    mask &= (1u << kPads) - 1u;
    if (!mask) return;
    for (int i = 0; i < kPads; ++i) {
        if (!(mask & (1u << i))) continue;
        u.pad[i] = P().pad[i];
        for (int l = 0; l < kLayers; ++l) slotRetain(u.pad[i].layer[l].slot);
    }
    u.mask = mask;
    copyStr(u.what, sizeof u.what, what);
    u.valid = 1;
    ++s.revs.pads;
}

// ------------------------------------------------------------------------------- built-in kits
Sample* builtinSample(int kit, int i)
{
    if (kit < 0 || kit >= kBuiltinKits || kit >= builtinKitCount() || i < 0 || i >= kPads) return nullptr;
    const BuiltinKit& k = builtinKit(kit);
    if (!k.sounds || i >= k.count) return nullptr;
    State& s = S();
    if (!s.builtin[kit][i]) {
        const BuiltinSound& snd = k.sounds[i];
        if (!snd.pcm || !snd.frames) return nullptr;
        s.builtin[kit][i] = sampleWrapEmbedded(snd.pcm, snd.frames, 1, k.rate, snd.name);
    }
    return s.builtin[kit][i];
}

void builtinRef(int kit, int i, SampleRef& r)
{
    const BuiltinKit& k = builtinKit(kit);
    r = SampleRef{};
    sfmt(r.path, sizeof r.path, "builtin:%s/%s", k.name ? k.name : "KIT", k.sounds[i].name ? k.sounds[i].name : "?");
    copyStr(r.name, sizeof r.name, k.sounds[i].name);
}

// "builtin:909 STYLE/BD" (also accepts "builtin:TR-909/BD") -> cached embedded sample.
Sample* resolveBuiltin(const char* path)
{
    if (!startsWith(path, "builtin:")) return nullptr;
    const char* spec = path + 8;
    const char* slash = strrchr(spec, '/');
    if (!slash) return nullptr;
    char kitname[48];
    int n = (int)(slash - spec);
    if (n >= (int)sizeof kitname) n = (int)sizeof kitname - 1;
    memcpy(kitname, spec, (size_t)n);
    kitname[n] = 0;
    const char* snd = slash + 1;
    const int count = builtinKitCount();
    for (int pass = 0; pass < 2; ++pass) {
        for (int k = 0; k < count && k < kBuiltinKits; ++k) {
            const BuiltinKit& kit = builtinKit(k);
            if (!kit.name) continue;
            bool match = pass == 0 ? eqCi(kit.name, kitname)
                                   : ((containsCi(kitname, "808") && containsCi(kit.name, "808")) ||
                                      (containsCi(kitname, "909") && containsCi(kit.name, "909")));
            if (!match) continue;
            for (int i = 0; i < kit.count && i < kPads; ++i)
                if (kit.sounds[i].name && eqCi(kit.sounds[i].name, snd)) return builtinSample(k, i);
        }
    }
    return nullptr;
}

// ------------------------------------------------------------------------------- library
void refilter()
{
    State& s = S();
    s.result_count = s.sd ? front().filter(s.ui.category, s.ui.search, s.results, kMaxLibraryFiles) : 0;
    if (s.result_count < 0) s.result_count = 0;
    ++s.revs.library;
}

void setBrowserPreview(Sample* smp, int index, const char* path)
{
    State& s = S();
    Sample* old = s.bp;
    if (smp) regAdd(smp);
    s.bp = smp;
    s.bp_index = smp ? index : -1;
    copyStr(s.bp_path, sizeof s.bp_path, smp ? path : "");
    maybeFree(old);
}

void postPreview(Sample* smp)
{
    if (!smp) return;
    Command c;
    c.type = CmdType::Preview;
    c.vel = 100;
    c.sample = smp;
    postUi(c);
    regPreview(smp, +1);
    noteSound();
}

void formatSdStatus(uint64_t total, uint64_t freeb)
{
    State& s = S();
    if (!s.sd) {
        copyStr(s.sd_status, sizeof s.sd_status, "Brak karty SD");
        return;
    }
    if (!total) {
        copyStr(s.sd_status, sizeof s.sd_status, "Karta SD gotowa");
        return;
    }
    const float gib = 1073741824.0f;
    sfmt(s.sd_status, sizeof s.sd_status, "%.1f GB, %.1f GB wolne", (double)((float)total / gib),
         (double)((float)freeb / gib));
}

// ------------------------------------------------------------------------------- loader task
bool postJob(Job& j)
{
    State& s = S();
    if (!s.worker) {
        say("Loader nie działa");
        return false;
    }
    if (!s.jobs.push(j)) {
        say("Kolejka ładowania jest pełna");
        return false;
    }
    ++s.jobs_posted;
    workerSignal();
    return true;
}

void runSave(SaveJob& sj, bool export_only)
{
    sj.ok = 0;
    if (export_only) {
        sj.ok = projectExportJson(sj.path, sj.proj, sj.refs) ? 1 : 0;
        if (!sj.ok) copyStr(sj.err, sizeof sj.err, "Nie udało się zapisać JSON");
        return;
    }
    if (sj.nwrite > 0) makeDir(sj.dir);
    sj.err[0] = 0;
    int failed = 0;
    for (int i = 0; i < sj.nwrite; ++i) {
        SaveWrite& w = sj.w[i];
        w.ok = wavSave(w.path, w.s) == WavError::Ok ? 1 : 0;
        if (!w.ok) {
            ++failed;
            if (!sj.err[0]) sfmt(sj.err, sizeof sj.err, "nie zapisano sampla %s", baseName(w.path));
        }
        // A failed WAV is still referenced (intended path + name): opening the project then reports it
        // as a missing sample instead of silently dropping the layer.
        for (int p = 0; p < kPads; ++p)
            for (int l = 0; l < kLayers; ++l)
                if (sj.proj.pad[p].layer[l].slot == w.slot) {
                    copyStr(sj.refs[p][l].path, kPathLen, w.path);
                    if (!sj.refs[p][l].name[0]) copyStr(sj.refs[p][l].name, kNameLen, w.s->name);
                }
    }
    char wav_err[96];
    copyStr(wav_err, sizeof wav_err, sj.err);
    sj.err[0] = 0;
    sj.ok = projectSave(sj.path, sj.proj, sj.refs, sj.err, sizeof sj.err) ? 1 : 0;
    if (!sj.ok && !sj.err[0]) copyStr(sj.err, sizeof sj.err, "Błąd zapisu");
    if (sj.ok && failed) {   // the JSON is on the card, an edited sample is not: partial, stays dirty
        sj.ok = 2;
        copyStr(sj.err, sizeof sj.err, wav_err);
    }
}

void runLoad(LoadJob& lj)
{
    lj.missing = 0;
    lj.err[0] = 0;
    memset(lj.smp, 0, sizeof lj.smp);
    for (int p = 0; p < kPads; ++p)
        for (int l = 0; l < kLayers; ++l) lj.refs[p][l] = SampleRef{};
    lj.ok = projectLoad(lj.path, lj.proj, lj.refs, lj.err, sizeof lj.err) ? 1 : 0;
    if (!lj.ok) {
        if (!lj.err[0]) copyStr(lj.err, sizeof lj.err, "Nie można otworzyć projektu");
        return;
    }
    for (int p = 0; p < kPads; ++p)
        for (int l = 0; l < kLayers; ++l) {
            const char* path = lj.refs[p][l].path;
            if (!path[0] || startsWith(path, "builtin:")) continue;
            // Layers naming the same file share one Sample (and later one slot).
            Sample* found = nullptr;
            for (int q = 0; q <= p && !found; ++q)
                for (int m = 0; m < kLayers; ++m) {
                    if (q == p && m >= l) break;
                    if (lj.smp[q][m] && strcmp(lj.refs[q][m].path, path) == 0) {
                        found = lj.smp[q][m];
                        break;
                    }
                }
            if (!found) {
                WavError err = WavError::Ok;
                found = wavLoad(path, nullptr, &err, kPreviewSeconds);
                if (!found) ++lj.missing;
            }
            lj.smp[p][l] = found;
        }
}

// SAVE WAV: j.path = ".../USER/<name>" without extension; writes the first free <name>.wav,
// <name>_2.wav ... and returns the path written in r.job.path.
void runSaveWav(Result& r)
{
    const Job& j = r.job;
    char dir[kPathLen];
    copyStr(dir, sizeof dir, j.path);
    if (char* slash = strrchr(dir, '/')) {
        *slash = 0;
        makeDir(dir);   // USER/ may not exist yet
    }
    char path[kPathLen] = {};
    WavError err = WavError::Io;
    for (int k = 1; k < 100; ++k) {
        if (k == 1) sfmt(path, sizeof path, "%s.wav", j.path);
        else sfmt(path, sizeof path, "%s_%d.wav", j.path, k);
        struct stat st;
        if (stat(path, &st) == 0) continue;
        err = wavSave(path, j.src);
        break;
    }
    r.err = err;
    copyStr(r.job.path, sizeof r.job.path, path);
}

// FADE with a play region or REVERSE: prints PP_FADE_IN / PP_FADE_OUT where the voice applies them
// (voice.cpp regionGain: linear gain, counted from the region start in play direction), so the pad
// sounds the same after the parameters are zeroed. Frames outside the region are unchanged.
Sample* fadeRegion(const Sample* s, float fi_ms, float fo_ms, float start, float end, bool reverse)
{
    const uint32_t n = s->frames;
    int32_t a = (int32_t)(clampf(start, 0.0f, 1.0f) * (float)n);   // the voice's region rounding
    int32_t b = (int32_t)(clampf(end, 0.0f, 1.0f) * (float)n);
    if (a >= (int32_t)n) a = (int32_t)n - 1;
    if (a < 0) a = 0;
    if (b > (int32_t)n) b = (int32_t)n;
    if (b <= a) b = a + 1;
    const uint32_t len = (uint32_t)(b - a);
    float fi = fi_ms > 0.0f ? fi_ms * 0.001f * (float)s->rate : 0.0f;
    float fo = fo_ms > 0.0f ? fo_ms * 0.001f * (float)s->rate : 0.0f;
    if (fi + fo > (float)len) {   // longer than the region: shrink both proportionally
        const float k = (float)len / (fi + fo);
        fi *= k;
        fo *= k;
    }
    Sample* r = sampleClone(s);
    if (!r) return nullptr;
    r->path[0] = 0;
    const int ch = r->channels == 2 ? 2 : 1;
    const uint32_t nin = fi > 0.0f ? (uint32_t)ceilf(fi) : 0u, nout = fo > 0.0f ? (uint32_t)ceilf(fo) : 0u;
    auto apply = [&](uint32_t t) {   // t = frames from the region start in play order
        float g = 1.0f;
        if (fi > 0.0f && (float)t < fi) g = (float)t / fi;
        const float left = (float)(len - t);
        if (fo > 0.0f && left < fo) g = fminf(g, left / fo);
        if (g >= 1.0f) return;
        if (g < 0.0f) g = 0.0f;
        const uint32_t i = reverse ? (uint32_t)b - 1u - t : (uint32_t)a + t;
        int16_t* f = r->data + (size_t)i * ch;
        for (int c = 0; c < ch; ++c) f[c] = (int16_t)lrintf((float)f[c] * g);
    };
    for (uint32_t t = 0; t < nin && t < len; ++t) apply(t);
    for (uint32_t t = len > nout ? len - nout : 0u; t < len; ++t)
        if (t >= nin) apply(t);
    sampleAnalyze(r);
    return r;
}

Sample* runOp(const Job& j)
{
    const Sample* s = j.src;
    if (!s) return nullptr;
    const float* pp = j.pp;
    switch (j.op) {
    case SampleOp::Trim: return opCrop(s, pp[PP_START], pp[PP_END]);
    case SampleOp::Normalize: return opNormalize(s);
    case SampleOp::Reverse: return opReverse(s);
    case SampleOp::FadeInOut: {
        float fi = pp[PP_FADE_IN], fo = pp[PP_FADE_OUT];
        if (fi <= 0.0f && fo <= 0.0f) fi = 2.0f, fo = 20.0f;   // nothing set: de-click fades
        const bool reverse = j.flag != 0;
        const float a = fminf(pp[PP_START], pp[PP_END]), b = fmaxf(pp[PP_START], pp[PP_END]);
        if (a <= 0.0f && b >= 1.0f && !reverse) return opFade(s, fi, fo);   // whole file forward: file ends
        return fadeRegion(s, fi, fo, a, b, reverse);
    }
    case SampleOp::TrimSilence: return opTrimSilence(s);
    case SampleOp::ToMono: return opToMono(s);
    case SampleOp::PrintCrush: return opBitcrush(s, (int)pp[PP_BITS], (int)pp[PP_DOWNSAMPLE]);
    default: return nullptr;
    }
}

void loaderMain(void*)
{
    State& s = S();
    for (;;) {
        Job j;
        if (!s.jobs.pop(j)) {
            workerWait(100);
            continue;
        }
        Result r;
        r.job = j;
        switch (j.type) {
        case JobType::LoadWav: {
            WavError err = WavError::Ok;
            r.sample = wavLoad(j.path, nullptr, &err, kPreviewSeconds);
            r.err = err;
            break;
        }
        case JobType::Probe: {
            if (j.flag == 2 && !j.path[0] && j.lib) {   // kit preview: the kit's first file (folder read here)
                char first[1][kPathLen];
                if (j.lib->kitFiles((int)j.index, first, 1) > 0) copyStr(r.job.path, sizeof r.job.path, first[0]);
            }
            if (!r.job.path[0]) {
                r.err = WavError::Open;
                break;
            }
            WavError err = WavError::Ok;
            r.sample = wavLoad(r.job.path, &r.info, &err, kPreviewSeconds);
            r.err = err;
            r.have_info = r.info.rate > 0 ? 1 : 0;
            struct stat st;
            if (stat(r.job.path, &st) == 0) {
                const uint64_t sz = st.st_size > 0 ? (uint64_t)st.st_size : 0;
                r.bytes = sz > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)sz;
            }
            break;
        }
        case JobType::LoadKit: {
            KitJob* kj = static_cast<KitJob*>(j.payload);
            if (kj->count < 0 && j.lib) {   // folder listing / kit.txt on the loader, not on the LVGL task
                kj->count = j.lib->kitFiles((int)j.index, kj->paths, kPads);
                if (kj->count < 0) kj->count = 0;
            }
            for (int i = 0; i < kj->count; ++i) {
                kj->err[i] = WavError::Ok;
                // kit.txt may leave pads unmapped (empty path): keep those pads, do not try to open "".
                kj->out[i] = kj->paths[i][0] ? wavLoad(kj->paths[i], nullptr, &kj->err[i], kPreviewSeconds) : nullptr;
            }
            break;
        }
        case JobType::Scan: {
            Library& back = s.libs[j.index];
            r.count = back.scan();
            r.free_bytes = back.freeBytes();
            r.total_bytes = back.totalBytes();
            break;
        }
        case JobType::Save: runSave(*static_cast<SaveJob*>(j.payload), false); break;
        case JobType::Export: runSave(*static_cast<SaveJob*>(j.payload), true); break;
        case JobType::Load: runLoad(*static_cast<LoadJob*>(j.payload)); break;
        case JobType::Op: r.sample = runOp(j); break;
        case JobType::Slice: r.count = sliceTransients(j.src, j.pp[PP_START], j.pp[PP_END], j.fval, kMaxSlices, r.pos); break;
        case JobType::SaveWav: runSaveWav(r); break;
        }
        while (!s.res.push(r)) sleepMs(5);
    }
}

// ------------------------------------------------------------------------------- result handlers (LVGL task)
void applyLoadWav(const Result& r)
{
    State& s = S();
    const Job& j = r.job;
    if (!r.sample) {
        sayf("%s: %s", baseName(j.path), wavErrorText(r.err));
        return;
    }
    SampleRef ref;
    copyStr(ref.path, sizeof ref.path, j.path);
    copyStr(ref.name, sizeof ref.name, r.sample->name);
    const int pad = clampi(j.pad, 0, kPads - 1), layer = clampi(j.layer, 0, kLayers - 1);
    if (!assignSample(pad, layer, r.sample, ref, false)) return;
    Pad& p = P().pad[pad];
    if (layer == 0) {
        copyStr(p.name, kNameLen, r.sample->name);
        resetRegion(p);
    }
    char lbl[8];
    padLabel(pad, lbl, sizeof lbl);
    sayf("%s \xE2\x86\x92 %s", baseName(j.path), lbl);
    ++s.revs.samples;
    ++s.revs.pads;
    markDirty();
}

void applyKit(const Result& r)
{
    State& s = S();
    KitJob* kj = static_cast<KitJob*>(r.job.payload);
    if (kj->count <= 0) {
        say("Zestaw jest pusty");
        bigFree(kj);
        return;
    }
    uint32_t mask = 0;   // pads this kit replaces: one-level undo (undoPads)
    for (int i = 0; i < kj->count && i < kPads; ++i)
        if (kj->out[i]) mask |= 1u << i;
    if (mask) padUndoTake(mask, "LOAD KIT");
    int ok = 0, mapped = 0;
    for (int i = 0; i < kj->count && i < kPads; ++i) {
        if (kj->paths[i][0]) ++mapped;
        Sample* smp = kj->out[i];
        if (!smp) continue;
        SampleRef ref;
        copyStr(ref.path, sizeof ref.path, kj->paths[i]);
        copyStr(ref.name, sizeof ref.name, smp->name);
        if (!assignSample(i, 0, smp, ref, false)) continue;
        for (int l = 1; l < kLayers; ++l) clearLayer(i, l);
        Pad& p = P().pad[i];
        copyStr(p.name, kNameLen, smp->name);
        resetRegion(p);
        ++ok;
    }
    if (ok) sayf("Załadowano zestaw %s (%d/%d)", kj->name, ok, mapped);
    else say("Nie udało się załadować zestawu");
    bigFree(kj);
    ++s.revs.samples;
    ++s.revs.pads;
    if (ok) markDirty();
}

void applyProbe(const Result& r)
{
    State& s = S();
    const Job& j = r.job;
    if (j.flag == 2) {   // App::previewKit: play the kit's first sound; the browser preview is unchanged
        const bool still = s.sd && j.index >= 0 && j.index < front().count() && s.ui.lib_sel == j.index;
        if (!j.path[0]) {
            say("Zestaw jest pusty");
        } else if (!r.sample) {
            sayf("%s: %s", baseName(j.path), wavErrorText(r.err));
        } else if (!still) {
            freeUnowned(r.sample);
        } else {
            regAdd(r.sample);
            postPreview(r.sample);   // freed by maybeFree() when the engine reports PreviewDone
        }
        return;
    }
    const bool current = s.sd && j.index >= 0 && j.index < front().count() &&
                         strcmp(front().at(j.index).path, j.path) == 0 && s.ui.lib_sel == j.index;
    if (!current) {
        freeUnowned(r.sample);
        return;
    }
    if (!front().at(j.index).probed) {
        if (r.have_info) {
            // The loader read this header while decoding: fill the entry without a second card read on
            // the LVGL task (Library::probe() would do the same: header fields, file size, probed).
            LibEntry& x = const_cast<LibEntry&>(front().at(j.index));
            x.rate = r.info.rate;
            x.channels = r.info.channels;
            x.bits = r.info.bits;
            x.seconds = r.info.rate ? (float)r.info.frames / (float)r.info.rate : 0.0f;
            x.bytes = r.bytes;
            x.probed = 1;
        } else {
            front().probe(j.index);
        }
    }
    if (!r.sample) {
        sayf("%s: %s", baseName(j.path), wavErrorText(r.err));
        setBrowserPreview(nullptr, -1, "");
    } else {
        setBrowserPreview(r.sample, j.index, j.path);
        if (j.flag) postPreview(r.sample);
    }
    ++s.revs.library;
}

void applyScan(const Result& r)
{
    State& s = S();
    s.scan_busy = false;
    s.front = r.job.index;   // the freshly scanned library becomes the visible one
    setBrowserPreview(nullptr, -1, "");
    s.ui.lib_sel = -1;
    formatSdStatus(r.total_bytes, r.free_bytes);
    refilter();
    // The library holds at most 4096 entries (USER first, then SAMPLES): say when files were left out.
    const bool cut = front().truncated();
    if (cut && s.sd) {
        const int used = (int)strlen(s.sd_status);
        sfmt(s.sd_status + used, (int)sizeof s.sd_status - used, " \xE2\x80\xA2 lista obcięta");
    }
    if (s.ui_ready && r.job.flag) {
        if (cut) sayf("Skanowanie SD: %d plików (lista obcięta)", r.count);
        else sayf("Skanowanie SD: %d plików", r.count);
    }
    if (s.scan_again) {
        s.scan_again = false;
        app().rescan();
    }
}

void applySave(const Result& r, bool export_only)
{
    State& s = S();
    SaveJob* sj = static_cast<SaveJob*>(r.job.payload);
    if (!export_only) {
        s.save_busy = false;
        for (int i = 0; i < sj->nwrite; ++i) {
            const SaveWrite& w = sj->w[i];
            SlotInfo& si = s.slots[w.slot];
            if (w.ok && si.s && si.s->id == w.id) {
                copyStr(si.ref.path, sizeof si.ref.path, w.path);
                si.edited = 0;
            }
            Sample* held = const_cast<Sample*>(w.s);
            regHold(held, -1);
            maybeFree(held);
        }
    }
    if (!export_only && sj->ok) copyStr(s.file_name, sizeof s.file_name, sj->proj.name);   // this .orb is ours now
    if (sj->ok == 1) {
        if (!export_only) s.saved_gen = sj->gen;
        if (export_only) sayf("Wyeksportowano %s", baseName(sj->path));
        else if (r.job.flag) sayf("Autozapis: %s", sj->proj.name);
        else sayf("Zapisano projekt %s", sj->proj.name);
    } else if (sj->ok == 2) {
        // The JSON is written but an edited sample is not: the project stays dirty (autosave retries).
        sayf("Zapis częściowy: %s", sj->err);
    } else {
        sayf("Błąd zapisu: %s", sj->err);
    }
    bigFree(sj);
    ++s.revs.project;
}

void bumpAll()
{
    Revs& v = S().revs;
    ++v.view, ++v.selection, ++v.pads, ++v.params, ++v.pattern, ++v.song, ++v.mixer, ++v.fx, ++v.samples,
        ++v.transport, ++v.project;
}

void releaseAllPads()
{
    padUndoDrop();   // the pad-set undo belonged to the previous project
    for (int p = 0; p < kPads; ++p) {
        undoDropPad(p);
        for (int l = 0; l < kLayers; ++l) setLayerSlot(p, l, -1);
    }
}

void applyLoad(const Result& r)
{
    State& s = S();
    LoadJob* lj = static_cast<LoadJob*>(r.job.payload);
    if (!lj->ok) {
        for (int p = 0; p < kPads; ++p)
            for (int l = 0; l < kLayers; ++l) {
                Sample* x = lj->smp[p][l];
                if (!x) continue;
                for (int q = p; q < kPads; ++q)   // clear duplicates before freeing
                    for (int m = 0; m < kLayers; ++m)
                        if (lj->smp[q][m] == x) lj->smp[q][m] = nullptr;
                sampleFree(x);
            }
        sayf("Błąd otwarcia: %s", lj->err);
        bigFree(lj);
        return;
    }
    // Stop while playing only pauses (position and expanded hits of the old pattern stay for Continue);
    // a second Stop, posted once the new project is in place, rewinds (below).
    postSimple(CmdType::Stop);
    postSimple(CmdType::Panic);
    s.playing = false;
    if (s.recording) {
        s.recording = false;
        postSimple(CmdType::SetRecord, 0);
    }
    releaseAllPads();
    s.master_muted = false;
    s.pat_undo_scene = -1;   // the pattern undo belonged to the previous project

    Sample* seen[kPads * kLayers];
    int seen_slot[kPads * kLayers];
    int nseen = 0;
    int missing = lj->missing;
    for (int p = 0; p < kPads; ++p)
        for (int l = 0; l < kLayers; ++l) {
            Layer& L = lj->proj.pad[p].layer[l];
            L.slot = -1;
            const SampleRef& ref = lj->refs[p][l];
            Sample* smp = lj->smp[p][l];
            if (!smp && ref.path[0] && startsWith(ref.path, "builtin:")) {
                smp = resolveBuiltin(ref.path);
                if (!smp) ++missing;
            }
            if (!smp) continue;
            int slot = -1;
            for (int k = 0; k < nseen; ++k)
                if (seen[k] == smp) slot = seen_slot[k];
            if (slot >= 0) {
                slotRetain(slot);
            } else {
                regAdd(smp);
                slot = slotOpen(smp, ref, false);
                if (slot < 0) {
                    ++missing;
                    continue;
                }
                seen[nseen] = smp;
                seen_slot[nseen++] = slot;
            }
            L.slot = (int16_t)slot;
        }
    // Loaded samples that found no slot (table full) are not registered: free them once.
    for (int p = 0; p < kPads; ++p)
        for (int l = 0; l < kLayers; ++l) {
            Sample* x = lj->smp[p][l];
            if (!x || regFind(x) >= 0) continue;
            for (int q = p; q < kPads; ++q)
                for (int m = 0; m < kLayers; ++m)
                    if (lj->smp[q][m] == x) lj->smp[q][m] = nullptr;
            sampleFree(x);
        }
    // The project is the file it was opened from: a copy or rename made on a PC (BEAT.orb -> BEAT_v2.orb)
    // keeps the "name" of the original inside, and SAVE must not write over BEAT.orb.
    {
        char stem[32];
        sanitizeName(stem, sizeof stem, lj->name);
        copyStr(lj->proj.name, sizeof lj->proj.name, stem);
        copyStr(s.file_name, sizeof s.file_name, stem);
        s.autosave_warned = false;
    }
    if (lj->proj.scene >= kScenes) lj->proj.scene = 0;
    *s.project = lj->proj;
    postSimple(CmdType::Stop);   // rewind (FOLLOW / song of the new project): PLAY starts it from the top
    postSimple(CmdType::QueueScene, 0, s.project->scene);
    postSimple(CmdType::Metronome, s.project->metronome ? 1 : 0);
    s.ui.pad = 0;
    s.ui.layer = 0;
    s.ui.seq_page = 0;
    s.ui.pad_shown = 1;
    eng().telemetry().watch_pad.store(0, std::memory_order_relaxed);
    s.saved_gen = s.dirty_gen;
    s.last_save_us = nowUs();
    if (missing) sayf("Otwarto %s (brak sampli: %d)", s.project->name, missing);
    else sayf("Otwarto projekt %s", s.project->name);
    bigFree(lj);
    bumpAll();
}

void applyOpResult(const Result& r);
void applySlice(const Result& r);

// Library rescan without the "Skanowanie SD" toast (after SAVE WAV).
void rescanQuiet()
{
    State& s = S();
    if (!s.sd) return;
    if (s.scan_busy) {
        s.scan_again = true;
        return;
    }
    Job j;
    j.type = JobType::Scan;
    j.index = 1 - s.front;
    j.flag = 0;
    s.scan_busy = postJob(j);
}

void applySaveWav(const Result& r)
{
    State& s = S();
    const Job& j = r.job;
    Sample* src = const_cast<Sample*>(j.src);
    regHold(src, -1);
    if (r.err != WavError::Ok) {
        sayf("SAVE WAV: %s", wavErrorText(r.err));
        maybeFree(src);
        return;
    }
    // An edited sample now has a file: the project refers to it and a save does not write a copy.
    const int slot = j.index;
    if (slot >= 0 && slot < kMaxSamples && s.slots[slot].s && s.slots[slot].s->id == j.src_id && s.slots[slot].edited) {
        copyStr(s.slots[slot].ref.path, sizeof s.slots[slot].ref.path, j.path);
        s.slots[slot].edited = 0;
    }
    maybeFree(src);
    sayf("Zapisano %s (USER)", baseName(j.path));
    rescanQuiet();
}

void applyResult(const Result& r)
{
    switch (r.job.type) {
    case JobType::LoadWav: applyLoadWav(r); break;
    case JobType::LoadKit: applyKit(r); break;
    case JobType::Probe: applyProbe(r); break;
    case JobType::Scan: applyScan(r); break;
    case JobType::Save: applySave(r, false); break;
    case JobType::Export: applySave(r, true); break;
    case JobType::Load: applyLoad(r); break;
    case JobType::Op: applyOpResult(r); break;
    case JobType::Slice: applySlice(r); break;
    case JobType::SaveWav: applySaveWav(r); break;
    }
}

// ------------------------------------------------------------------------------- sample ops
const char* opName(SampleOp op)
{
    switch (op) {
    case SampleOp::Trim: return "TRIM";
    case SampleOp::Normalize: return "NORMALIZE";
    case SampleOp::Reverse: return "REVERSE";
    case SampleOp::Duplicate: return "DUPLICATE";
    case SampleOp::FadeInOut: return "FADE";
    case SampleOp::TrimSilence: return "TRIM SILENCE";
    case SampleOp::ToMono: return "MONO";
    case SampleOp::PrintCrush: return "PRINT CRUSH";
    case SampleOp::Undo: return "UNDO";
    default: return "OP";
    }
}

// Region end of the last slice. Kept in Pad::slice_end when core/project.h has that field (16 slices
// leave no room behind the markers); otherwise after the markers when there is room (see sliceGrid).
template <typename PadT>
auto padSliceEnd(const PadT& p, int) -> decltype(p.slice_end, float())
{
    return p.slice_end;
}
template <typename PadT>
float padSliceEnd(const PadT&, long)
{
    return -1.0f;   // no field
}
template <typename PadT>
auto setPadSliceEnd(PadT& p, float end, int) -> decltype(p.slice_end, void())
{
    p.slice_end = end;
}
template <typename PadT>
void setPadSliceEnd(PadT&, float, long)
{
}

float sliceEnd(const Pad& p, int i)
{
    if (i + 1 < p.slice_count) return p.slices[i + 1];
    if (p.slice_count <= 0) return 1.0f;
    const float last = p.slices[p.slice_count - 1];
    const float field = padSliceEnd(p, 0);
    if (field > last) return field;
    if (p.slice_count < kMaxSlices && p.slices[p.slice_count] > last) return p.slices[p.slice_count];
    return 1.0f;
}

void storeSlices(Pad& p, const float* pos, int count, float end)
{
    if (count > kMaxSlices) count = kMaxSlices;
    for (int i = 0; i < kMaxSlices; ++i) p.slices[i] = 0.0f;
    for (int i = 0; i < count; ++i) p.slices[i] = clampf(pos[i], 0.0f, 1.0f);
    if (count > 0 && count < kMaxSlices) p.slices[count] = clampf(end, 0.0f, 1.0f);
    setPadSliceEnd(p, count > 0 ? clampf(end, 0.0f, 1.0f) : 1.0f, 0);
    p.slice_count = (uint8_t)(count < 0 ? 0 : count);
    p.slice_sel = 0;
}

// AUTO slice result (transient detection ran on the loader).
void applySlice(const Result& r)
{
    State& s = S();
    const Job& j = r.job;
    Sample* src = const_cast<Sample*>(j.src);
    regHold(src, -1);
    Pad& p = P().pad[j.pad];
    const Sample* now = slotSample(p.layer[j.layer].slot);
    if (!now || now->id != j.src_id) {
        say("Sampel zmienił się - SLICE anulowany");
    } else if (r.count <= 1) {
        say("Nie wykryto uderzeń");
    } else {
        storeSlices(p, r.pos, r.count, j.pp[PP_END]);
        ++s.revs.pads;
        ++s.revs.params;
        markDirty();
        sayf("SLICE: %d", r.count);
    }
    maybeFree(src);
}

void applyOpResult(const Result& r)
{
    State& s = S();
    const Job& j = r.job;
    Sample* src = const_cast<Sample*>(j.src);
    regHold(src, -1);
    Sample* out = r.sample;
    const int pad = j.pad, layer = j.layer;
    Pad& p = P().pad[pad];
    Layer& L = p.layer[layer];
    const int S0 = L.slot;
    if (!out) {
        sayf("%s: operacja nie powiodła się", opName(j.op));
        maybeFree(src);
        return;
    }
    if (S0 < 0 || !s.slots[S0].s || s.slots[S0].s->id != j.src_id) {
        sampleFree(out);
        say("Sampel zmienił się - operacja anulowana");
        maybeFree(src);
        return;
    }
    regAdd(out);
    undoDrop(pad, layer);
    Undo& u = s.undo[pad][layer];
    memcpy(u.p, p.p, sizeof u.p);
    u.p[PP_START] = j.pp[PP_START];
    u.p[PP_END] = j.pp[PP_END];
    u.reverse = p.reverse;
    u.slice_count = p.slice_count;
    memcpy(u.slices, p.slices, sizeof u.slices);
    u.slice_end = padSliceEnd(p, 0);
    u.prev = nullptr;
    u.prev_slot = -1;
    if (s.slots[S0].refs > 1) {
        // Shared slot (duplicated / sliced pads): copy-on-write into a slot of our own.
        SampleRef ref;
        copyStr(ref.name, sizeof ref.name, s.slots[S0].ref.name);
        const int N = slotOpen(out, ref, true);
        if (N < 0) {
            say("Brak wolnych slotów na sample");
            maybeFree(out);
            maybeFree(src);
            return;
        }
        u.other_slot = 1;
        u.prev_slot = (int16_t)S0;   // the layer's reference on S0 now belongs to the undo entry
        L.slot = (int16_t)N;
        u.slot = (int16_t)N;
    } else {
        u.other_slot = 0;
        u.prev = s.slots[S0].s;
        regHold(u.prev, +1);
        u.prev_ref = s.slots[S0].ref;
        u.prev_edited = s.slots[S0].edited;
        postSetSample(S0, out);
        s.slots[S0].ref.path[0] = 0;
        s.slots[S0].edited = 1;
        u.slot = (int16_t)S0;
    }
    u.result_id = out->id;
    u.valid = 1;

    switch (j.op) {
    case SampleOp::Trim: {
        const float a = j.pp[PP_START], b = j.pp[PP_END];
        const float span = b - a;
        float pos[kMaxSlices];
        int n = 0;
        for (int i = 0; i < p.slice_count && span > 1e-6f; ++i) {
            const float x = (p.slices[i] - a) / span;
            if (x >= 0.0f && x < 1.0f) pos[n++] = x;
        }
        // START cut through a slice: its tail is now the head of the sample and slices[0] must be 0.
        if (n > 0 && pos[0] > 1e-4f) {
            if (n == kMaxSlices) --n;
            for (int i = n; i > 0; --i) pos[i] = pos[i - 1];
            pos[0] = 0.0f;
            ++n;
        }
        p.p[PP_START] = 0.0f;
        p.p[PP_END] = 1.0f;
        storeSlices(p, pos, n, 1.0f);
        break;
    }
    case SampleOp::Reverse: {
        const float a = p.p[PP_START], b = p.p[PP_END];
        p.p[PP_START] = clampf(1.0f - b, 0.0f, 1.0f);
        p.p[PP_END] = clampf(1.0f - a, 0.0f, 1.0f);
        p.slice_count = 0;
        p.slice_sel = 0;
        break;
    }
    case SampleOp::TrimSilence: resetRegion(p); break;
    case SampleOp::FadeInOut:
        p.p[PP_FADE_IN] = 0.0f;   // printed into the sample
        p.p[PP_FADE_OUT] = 0.0f;
        break;
    case SampleOp::PrintCrush:
        p.p[PP_BITS] = 16.0f;
        p.p[PP_DOWNSAMPLE] = 1.0f;
        break;
    default: break;
    }
    maybeFree(src);
    char lbl[8];
    padLabel(pad, lbl, sizeof lbl);
    sayf("%s: %s", opName(j.op), lbl);
    ++s.revs.samples;
    ++s.revs.pads;
    ++s.revs.params;
    markDirty();
}

bool undoApply(int pad, int layer)
{
    State& s = S();
    Undo& u = s.undo[pad][layer];
    if (!u.valid) {
        say("Brak operacji do cofnięcia");
        return false;
    }
    Pad& p = P().pad[pad];
    Layer& L = p.layer[layer];
    if (L.slot != u.slot || !slotSample(u.slot) || slotSample(u.slot)->id != u.result_id) {
        undoDrop(pad, layer);
        say("Nie można cofnąć");
        return false;
    }
    memcpy(p.p, u.p, sizeof p.p);
    p.reverse = u.reverse;
    p.slice_count = u.slice_count;
    memcpy(p.slices, u.slices, sizeof p.slices);
    if (u.slice_end >= 0.0f) setPadSliceEnd(p, u.slice_end, 0);
    p.slice_sel = 0;
    const int R = u.slot;
    if (u.other_slot) {
        L.slot = u.prev_slot;   // the undo reference moves back to the layer
        u.valid = 0;
        u.prev_slot = -1;
        slotRelease(R);
    } else {
        Sample* prev = u.prev;
        u.prev = nullptr;
        u.valid = 0;
        const int M = slotOf(prev);   // prev was loaded onto another pad meanwhile: share that slot
        if (M >= 0 && M != R) {
            slotRetain(M);
            L.slot = (int16_t)M;
            slotRelease(R);
        } else if (s.slots[R].refs > 1) {
            // The result was shared meanwhile (DUPLICATE): only this layer goes back.
            const int N = slotOpen(prev, u.prev_ref, u.prev_edited != 0);
            if (N < 0) {
                regHold(prev, -1);
                maybeFree(prev);
                say("Brak wolnych slotów na sample");
                return false;
            }
            L.slot = (int16_t)N;
            slotRelease(R);
        } else {
            postSetSample(R, prev);
            s.slots[R].ref = u.prev_ref;
            s.slots[R].edited = u.prev_edited;
        }
        regHold(prev, -1);
        maybeFree(prev);
    }
    char lbl[8];
    padLabel(pad, lbl, sizeof lbl);
    sayf("Cofnięto: %s", lbl);
    ++s.revs.samples;
    ++s.revs.pads;
    ++s.revs.params;
    markDirty();
    return true;
}

// ------------------------------------------------------------------------------- random scene
enum class Role : uint8_t { Kick, Snare, Clap, ClosedHat, OpenHat, Tom, Rim, Crash, Ride, Perc, Other };

Role padRole(const Pad& p, int index)
{
    const char* n = p.name;
    if (containsCi(n, "kick") || hasWord(n, "bd") || containsCi(n, "bass drum")) return Role::Kick;
    if (containsCi(n, "snare") || hasWord(n, "sd")) return Role::Snare;
    if (containsCi(n, "clap") || hasWord(n, "cp")) return Role::Clap;
    if (containsCi(n, "open") || containsCi(n, "op hat") || hasWord(n, "oh")) return Role::OpenHat;
    if (containsCi(n, "hat") || hasWord(n, "hh") || hasWord(n, "ch")) return Role::ClosedHat;
    if (containsCi(n, "tom") || hasWord(n, "lt") || hasWord(n, "mt") || hasWord(n, "ht")) return Role::Tom;
    if (containsCi(n, "rim") || hasWord(n, "rs") || containsCi(n, "stick")) return Role::Rim;
    if (containsCi(n, "crash") || containsCi(n, "cymbal") || hasWord(n, "cy") || hasWord(n, "cr")) return Role::Crash;
    if (containsCi(n, "ride") || hasWord(n, "rd")) return Role::Ride;
    if (containsCi(n, "cow") || hasWord(n, "cb") || containsCi(n, "perc") || containsCi(n, "conga") ||
        containsCi(n, "clave") || hasWord(n, "cl") || containsCi(n, "shak") || containsCi(n, "tamb") ||
        hasWord(n, "ma") || containsCi(n, "marac") || containsCi(n, "bongo"))
        return Role::Perc;
    // Canonical built-in layout (core/kits.h).
    static const Role kDefault[kPads] = {Role::Kick,  Role::Snare, Role::ClosedHat, Role::OpenHat,
                                         Role::Clap,  Role::Rim,   Role::Tom,       Role::Tom,
                                         Role::Tom,   Role::Crash, Role::Ride,      Role::Kick,
                                         Role::Snare, Role::OpenHat, Role::Clap,    Role::Kick};
    return index >= 0 && index < kPads ? kDefault[index] : Role::Other;
}

void hit(Track& t, int step, int vlo, int vhi)
{
    Step& x = t.step[step];
    x.on = 1;
    x.vel = (uint8_t)clampi(rndi(vlo, vhi), 1, 127);
}

// One bar (16 steps at offset o) for a role. style: 0 four-on-the-floor, 1 broken, 2 half-time.
// weight < 1 thins out secondary pads of the same role.
void genBar(Track& t, Role role, int o, int len, int style, bool last_bar, float weight)
{
    auto on = [&](int st, float prob, int vlo, int vhi) {
        if (o + st < len && rndf() < prob * weight) hit(t, o + st, vlo, vhi);
    };
    switch (role) {
    case Role::Kick:
        if (style == 0) {
            for (int b = 0; b < 16; b += 4) on(b, b == 0 ? 1.0f : 0.97f, 112, 127);
            on(3, 0.10f, 80, 100);
            on(11, 0.12f, 80, 100);
            on(14, 0.15f, 85, 105);
        } else if (style == 1) {
            on(0, 1.0f, 115, 127);
            on(10, 0.75f, 105, 120);
            on(rndf() < 0.5f ? 6 : 7, 0.45f, 95, 115);
            on(rndf() < 0.5f ? 2 : 3, 0.25f, 90, 110);
            on(13, 0.2f, 90, 110);
        } else {
            on(0, 1.0f, 115, 127);
            on(7, 0.5f, 100, 118);
            on(10, 0.6f, 100, 120);
            on(3, 0.2f, 90, 105);
            on(14, 0.2f, 90, 105);
        }
        break;
    case Role::Snare:
    case Role::Clap: {
        const float main = role == Role::Snare ? 0.97f : (style == 0 ? 0.85f : 0.45f);
        if (style == 2) {
            on(8, main, 105, 122);
        } else {
            on(4, main, 105, 122);
            on(12, main, 105, 122);
        }
        if (role == Role::Snare)
            for (int st = 1; st < 16; st += 2) on(st, 0.07f, 35, 60);   // ghost notes
        if (last_bar)
            for (int st = 13; st < 16; ++st) on(st, 0.3f, 70, 110);     // small fill
        break;
    }
    case Role::ClosedHat:
        for (int st = 0; st < 16; ++st) {
            if (st % 2 == 0) on(st, 0.95f, st % 4 == 0 ? 88 : 72, st % 4 == 0 ? 104 : 90);
            else on(st, style == 2 ? 0.3f : 0.5f, 50, 72);
        }
        if (o + 15 < len && t.step[o + 15].on && rndf() < 0.15f) t.step[o + 15].ratchet = 2;
        break;
    case Role::OpenHat:
        for (int st = 2; st < 16; st += 4) on(st, style == 0 ? 0.8f : 0.3f, 80, 100);
        break;
    case Role::Tom:
        for (int st = 0; st < 16; ++st) on(st, last_bar && st >= 10 ? 0.22f : 0.03f, 80, 112);
        break;
    case Role::Rim:
        on(3, 0.2f, 70, 95);
        on(6, 0.2f, 70, 95);
        on(11, 0.2f, 70, 95);
        on(14, 0.2f, 70, 95);
        break;
    case Role::Crash:
        if (o == 0) on(0, 0.45f, 95, 115);
        break;
    case Role::Ride:
        if (style != 0)
            for (int st = 0; st < 16; st += 2) on(st, 0.45f, 70, 95);
        break;
    case Role::Perc:
        for (int st = 0; st < 16; ++st) on(st, st % 2 ? 0.12f : 0.05f, 70, 100);
        break;
    case Role::Other:
        for (int st = 0; st < 16; ++st) on(st, 0.05f, 70, 100);
        break;
    }
}

// ------------------------------------------------------------------------------- params
struct Target {
    float* v = nullptr;
    const ParamSpec* spec = nullptr;
};

Target target(const ParamRef& r)
{
    Target t;
    Project& pr = P();
    switch (r.kind) {
    case ParamRef::Kind::Pad:
        if (r.id < kPadParamCount) {
            t.v = &pr.pad[padIndex(r.index)].p[r.id];
            t.spec = &padSpec((PadParam)r.id);
        }
        break;
    case ParamRef::Kind::Global:
        if (r.id < kGlobalParamCount) {
            t.v = &pr.g[r.id];
            t.spec = &globalSpec((GlobalParam)r.id);
        }
        break;
    case ParamRef::Kind::LayerTune:
    case ParamRef::Kind::LayerGain:
    case ParamRef::Kind::LayerPan: {
        const int l = r.index < 0 ? S().ui.layer : clampi(r.index, 0, kLayers - 1);
        Layer& L = pr.pad[S().ui.pad].layer[l];
        if (r.kind == ParamRef::Kind::LayerTune) t.v = &L.tune, t.spec = &padSpec(PP_TUNE);
        else if (r.kind == ParamRef::Kind::LayerGain) t.v = &L.gain_db, t.spec = &padSpec(PP_GAIN);
        else t.v = &L.pan, t.spec = &padSpec(PP_PAN);
        break;
    }
    default: break;
    }
    return t;
}

// Master fader: the same position law as the buses (busGain), stored as GP_MASTER dB.
float masterPos()
{
    const float db = P().g[GP_MASTER];
    const ParamSpec& sp = globalSpec(GP_MASTER);
    if (db <= sp.min) return 0.0f;
    const float pos = sqrtf(dbToGain(db) * 0.5f);
    return clampf(pos, 0.0f, 1.0f);
}

float posToMasterDb(float pos)
{
    const ParamSpec& sp = globalSpec(GP_MASTER);
    const float g = busGain(clampf(pos, 0.0f, 1.0f));
    return clampParam(sp, g <= 0.0f ? sp.min : gainToDb(g));
}

void bumpFor(const ParamRef& r)
{
    Revs& v = S().revs;
    ++v.params;
    switch (r.kind) {
    case ParamRef::Kind::Pad:
    case ParamRef::Kind::LayerTune:
    case ParamRef::Kind::LayerGain:
    case ParamRef::Kind::LayerPan: ++v.pads; break;
    case ParamRef::Kind::Global:
        ++v.fx;
        if (r.id == GP_MASTER) ++v.mixer;
        break;
    default: ++v.mixer; break;
    }
}

int busIndex(const ParamRef& r) { return clampi(r.index, 0, kBuses); }

// [audio task] hal::audioSetTimingHook: real render time / block period -> Telemetry::cpu, and
// new driver underruns -> Telemetry::xruns. Lock-free, no I/O.
void audioTiming(uint32_t render_us, uint32_t block_us, void* user)
{
    Engine* e = user ? static_cast<Engine*>(user) : &eng();
    e->reportTiming(render_us, block_us);
    static uint32_t seen = 0;   // audio task only
    const uint32_t x = hal::audioXruns();
    uint32_t d = x - seen;
    seen = x;
    if (d > 16) d = 16;         // a burst is reported as a few, never a long loop
    while (d--) e->reportXrun();
}

// ------------------------------------------------------------------------------- MIDI in (USB task)
// GM / TR-909 drum map -> pads (core/kits.h layout: BD SD CH OH CP RS LT MT HT CR RD ...).
// 35 (acoustic BD), 40 (electric SD) and 44 (pedal HH) are GM aliases of 36, 38 and 42.
int noteToPad(int note)
{
    switch (note) {
    case 35: case 36: return 0;    // kick
    case 37: return 5;             // rim
    case 38: case 40: return 1;    // snare
    case 39: return 4;             // clap
    case 41: case 43: return 6;    // low tom
    case 42: case 44: return 2;    // closed hat
    case 45: case 47: return 7;    // mid tom
    case 46: return 3;             // open hat
    case 48: case 50: return 8;    // high tom
    case 49: return 9;             // crash
    case 51: return 10;            // ride
    default: return note >= 52 && note <= 56 ? 11 + (note - 52) : -1;
    }
}

// Clock-in tempo estimator. Only the USB task touches it (midiHook); the scratch arrays live here,
// not on that task's small stack. Single-precision math (the P4 has no double FPU).
struct ClockIn {
    uint64_t t[kClockWin + 1] = {};   // ring of tick times (us)
    int head = 0;                     // next write
    int count = 0;                    // valid stamps
    float slope = 0.0f;               // us per tick of the last fit, 0 = none
    uint32_t published = 0;           // last value stored in MidiShared::clock_bpm_milli
    float y[kClockWin + 1] = {};      // fit scratch
    float off[kClockWin + 1] = {};
    float sorted[kClockWin + 1] = {};
    bool in[kClockWin + 1] = {};
};
ClockIn g_clock_in;

uint64_t clockStamp(const ClockIn& c, int i)   // 0 = oldest .. count-1 = newest
{
    constexpr int n = kClockWin + 1;
    return c.t[(c.head - c.count + i + 2 * n) % n];
}

template <typename T>
void insertionSort(T* v, int n)
{
    for (int i = 1; i < n; ++i) {
        const T x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) {
            v[j + 1] = v[j];
            --j;
        }
        v[j + 1] = x;
    }
}

// Robust line fit of tick index -> arrival time: least squares, then twice: the offsets of the stamps
// from the fitted slope, their median as the grid, stamps farther than max(2 ms, 10 %) from it
// (late USB packets, a busy task) dropped, refit. Slope in us per tick; 0 when too few stamps agree.
float clockFit(ClockIn& c)
{
    const int n = c.count;
    const uint64_t t0 = clockStamp(c, 0);
    for (int i = 0; i < n; ++i) {
        c.y[i] = (float)(clockStamp(c, i) - t0);
        c.in[i] = true;
    }
    float slope = 0.0f;
    for (int pass = 0; pass < 3; ++pass) {
        float sx = 0.0f, sy = 0.0f;
        int m = 0;
        for (int i = 0; i < n; ++i)
            if (c.in[i]) sx += (float)i, sy += c.y[i], ++m;
        if (m < kClockMinIntervals + 1) return 0.0f;
        const float xm = sx / (float)m, ym = sy / (float)m;
        float sxx = 0.0f, sxy = 0.0f;
        for (int i = 0; i < n; ++i)
            if (c.in[i]) {
                const float dx = (float)i - xm;
                sxx += dx * dx;
                sxy += dx * (c.y[i] - ym);
            }
        if (sxx <= 0.0f) return 0.0f;
        slope = sxy / sxx;
        if (pass == 2) break;
        for (int i = 0; i < n; ++i) c.off[i] = c.sorted[i] = c.y[i] - slope * (float)i;
        insertionSort(c.sorted, n);
        const float grid = c.sorted[n / 2];
        const float thr = slope * 0.1f > 2000.0f ? slope * 0.1f : 2000.0f;
        for (int i = 0; i < n; ++i) c.in[i] = fabsf(c.off[i] - grid) <= thr;
    }
    return slope;
}

// One 0xF8 at time now (us). A gap > 500 ms or > 3 intervals (stop, lost ticks) restarts the window;
// when the median of the last 6 intervals is > 4 % away from the fitted interval the tempo changed
// and the window restarts from those stamps. Publishes BPM * 1000 once 24 intervals agree.
void clockTick(MidiShared& x, uint64_t now)
{
    ClockIn& c = g_clock_in;
    if (x.clock_reset.exchange(0, std::memory_order_acq_rel)) {
        c.count = 0;
        c.slope = 0.0f;
        c.published = 0;
    }
    if (c.count > 0) {
        const uint64_t last = clockStamp(c, c.count - 1);
        const uint64_t gap = now >= last ? now - last : 0;
        if (now < last || gap > 500000u || (c.slope > 0.0f && (float)gap > 3.0f * c.slope)) {
            c.count = 0;
            c.slope = 0.0f;
        }
    }
    c.t[c.head] = now;
    c.head = (c.head + 1) % (kClockWin + 1);
    if (c.count < kClockWin + 1) ++c.count;
    x.clock_last_ms.store((uint32_t)(now / 1000u), std::memory_order_relaxed);
    if (c.count >= 8 && c.slope > 0.0f) {
        uint32_t r6[6];
        for (int k = 0; k < 6; ++k) r6[k] = (uint32_t)(clockStamp(c, c.count - 1 - k) - clockStamp(c, c.count - 2 - k));
        insertionSort(r6, 6);
        const float recent = 0.5f * (float)(r6[2] + r6[3]);
        if (fabsf(recent - c.slope) > 0.04f * c.slope) {
            c.count = 7;   // the ring keeps the newest stamps
            c.slope = 0.0f;
        }
    }
    const float slope = c.count - 1 >= kClockMinIntervals ? clockFit(c) : 0.0f;
    const float bpm = slope > 0.0f ? 60000000.0f / (slope * 24.0f) : 0.0f;
    if (bpm < 20.0f || bpm > 400.0f) {
        x.clock_settled.store(0, std::memory_order_release);
        return;
    }
    c.slope = slope;
    const uint32_t milli = (uint32_t)(bpm * 1000.0f + 0.5f);
    const uint32_t d = milli > c.published ? milli - c.published : c.published - milli;
    if (d >= 20) {   // 0.02 BPM
        c.published = milli;
        x.clock_bpm_milli.store(milli, std::memory_order_relaxed);
    }
    x.clock_settled.store(c.count == kClockWin + 1 ? 1 : 0, std::memory_order_release);
}

// [USB task] hal::midiStart hook.
void midiHook(const hal::MidiMsg& m, void*)
{
    State* st = g_state;
    if (!st) return;
    MidiShared& x = st->midi;
    if (!x.on.load(std::memory_order_acquire)) return;
    x.rx.fetch_add(1, std::memory_order_relaxed);
    const uint8_t status = m.status;
    if (status >= 0xF8) {   // real time
        Command c;
        MidiIn ev;
        switch (status) {
        case 0xF8:
            if (x.clock_in.load(std::memory_order_relaxed)) clockTick(x, hal::micros());
            return;
        case 0xFA: c.type = CmdType::Play, ev.type = MidiEv::Start; break;
        case 0xFB: c.type = CmdType::Continue, ev.type = MidiEv::Continue; break;
        case 0xFC: c.type = CmdType::Stop, ev.type = MidiEv::Stop; break;
        default: return;   // active sensing, reset, undefined
        }
        eng().post(Producer::Midi, c);
        if (!st->midi_q.push(ev)) x.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint8_t type = status & 0xF0;
    const uint8_t ch = (uint8_t)((status & 0x0F) + 1);
    if (type == 0x90 || type == 0x80) {
        const uint8_t want = x.channel.load(std::memory_order_relaxed);
        if (want && want != ch) return;
        const int pad = noteToPad(m.d1 & 0x7F);
        if (pad < 0) return;
        if (type == 0x90 && (m.d2 & 0x7F)) app().padDown(pad, m.d2 & 0x7F, Producer::Midi);
        else app().padUp(pad, Producer::Midi);   // note off, or note on with velocity 0
    } else if (type == 0xB0) {
        MidiIn ev;
        ev.type = MidiEv::Cc;
        ev.ch = ch;
        ev.a = m.d1 & 0x7F;
        ev.b = m.d2 & 0x7F;
        if (!st->midi_q.push(ev)) x.dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

// ------------------------------------------------------------------------------- MIDI clock out (service timer)
struct ClockOut {
    bool playing = false;
    int64_t sent = 0;          // ticks sent since Start (Continue: from the song position)
    float beat = -1.0f;        // last Telemetry::beat seen
    float play_beat = 0.0f;    // beat at the previous call while playing (restart detection)
    uint64_t t_beat = 0;       // when `beat` was first seen (extrapolation between audio blocks)
};
ClockOut g_clock_out;          // service timer task only (reset by the LVGL task while it is stopped)

void midiOut(uint8_t status, uint8_t d1 = 0, uint8_t d2 = 0)
{
    hal::MidiMsg m = {status, d1, d2, 0};
    hal::midiSend(m);
}

// [service timer, 1 ms] 24 PPQN from Telemetry::beat (one value per audio block), extrapolated with
// the BPM between blocks (at most 10 ms) and delayed by the output latency, so the ticks line up with
// what is heard. Start (FA), Song Position + Continue (F2 + FB) and Stop (FC) follow the transport.
void clockOutService(void*)
{
    State* st = g_state;
    if (!st || !st->project || !st->midi.clock_out.load(std::memory_order_acquire)) return;
    ClockOut& c = g_clock_out;
    Telemetry& tel = eng().telemetry();
    const bool playing = tel.playing.load(std::memory_order_relaxed) != 0;
    const float beat = tel.beat.load(std::memory_order_relaxed);
    const uint64_t now = hal::micros();
    if (beat != c.beat) {
        c.beat = beat;
        c.t_beat = now;
    }
    if (!playing) {
        if (c.playing) midiOut(0xFC);
        c.playing = false;
        return;
    }
    if (!c.playing || beat + 1e-3f < c.play_beat) {
        if (beat < 1.0f / 24.0f || c.playing) {   // from the top (or Play again while playing)
            midiOut(0xFA);
            c.sent = 0;
        } else {                                  // resumed: song position in 16ths, then Continue
            const int spp = (int)(beat * 4.0f) & 0x3FFF;
            midiOut(0xF2, (uint8_t)(spp & 0x7F), (uint8_t)(spp >> 7));
            midiOut(0xFB);
            c.sent = (int64_t)spp * 6;
        }
        c.playing = true;
    }
    c.play_beat = beat;
    const float bpm = st->project->bpm;
    uint64_t dt = now - c.t_beat;
    if (dt > 10000u) dt = 10000u;
    const double lat = (double)st->midi.latency_us.load(std::memory_order_relaxed);
    const double pos = (double)beat + ((double)dt - lat) * (double)bpm / 60000000.0;
    const int64_t due = (int64_t)floor(pos * 24.0) + 1;   // tick k is due at pos k / 24
    for (int k = 0; k < 4 && c.sent < due; ++k) {         // catch-up bursts are spread over calls
        midiOut(0xF8);
        ++c.sent;
    }
}

// ------------------------------------------------------------------------------- MIDI (LVGL task)
bool sameRef(const ParamRef& a, const ParamRef& b) { return a.kind == b.kind && a.index == b.index && a.id == b.id; }

void refLabel(const ParamRef& r, char* out, int cap)
{
    const char* c = app().paramCaption(r);
    if (!c || !c[0]) c = "?";
    if (r.kind == ParamRef::Kind::Pad && r.index >= 0) {
        char lbl[8];
        padLabel(r.index, lbl, sizeof lbl);
        sfmt(out, cap, "%s %s", c, lbl);
    } else if (r.kind == ParamRef::Kind::BusPan) {
        const int b = clampi(r.index, 0, kBuses);
        sfmt(out, cap, "PAN %s", b < kBuses ? P().bus[b].name : "MASTER");
    } else {
        sfmt(out, cap, "%s", c);
    }
}

bool midiMapPath(char* out, int cap)
{
    const State& s = S();
    if (!s.sd || !s.sd_root[0]) return false;
    sfmt(out, cap, "%s/ORBIT/midimap.bin", s.sd_root);
    return true;
}

bool refValid(const ParamRef& r)
{
    switch (r.kind) {
    case ParamRef::Kind::Pad: return r.index >= -1 && r.index < kPads && r.id < kPadParamCount;
    case ParamRef::Kind::Global: return r.id < kGlobalParamCount;
    case ParamRef::Kind::BusLevel:
    case ParamRef::Kind::BusPan: return r.index >= 0 && r.index <= kBuses;
    case ParamRef::Kind::LayerTune:
    case ParamRef::Kind::LayerGain:
    case ParamRef::Kind::LayerPan: return r.index >= -1 && r.index < kLayers;
    case ParamRef::Kind::OutVolume: return true;
    default: return false;
    }
}

// midimap.bin: "OMM1", count (u32 LE), count x {cc, channel, kind, index, id, 0, 0, 0}.
void midiMapSave()
{
    State& s = S();
    char path[kPathLen];
    if (!midiMapPath(path, sizeof path)) return;
    uint8_t buf[8 + kMidiMapMax * 8];
    memset(buf, 0, sizeof buf);
    const uint32_t n = (uint32_t)s.map_count;
    for (int i = 0; i < 4; ++i) {
        buf[i] = (uint8_t)(kMidiMapMagic >> (8 * i));
        buf[4 + i] = (uint8_t)(n >> (8 * i));
    }
    for (int i = 0; i < s.map_count; ++i) {
        uint8_t* e = buf + 8 + 8 * i;
        const MidiBinding& b = s.map[i];
        e[0] = b.cc;
        e[1] = b.ch;
        e[2] = (uint8_t)b.ref.kind;
        e[3] = (uint8_t)b.ref.index;
        e[4] = b.ref.id;
    }
    const size_t len = 8 + 8 * (size_t)s.map_count;
    FILE* f = fopen(path, "wb");
    bool ok = f != nullptr;
    if (f) {
        ok = fwrite(buf, 1, len, f) == len;
        ok = fclose(f) == 0 && ok;
    }
    if (!ok) say("Nie udało się zapisać mapy MIDI");
}

void midiMapLoad()
{
    State& s = S();
    s.map_count = 0;
    char path[kPathLen];
    if (!midiMapPath(path, sizeof path)) return;
    FILE* f = fopen(path, "rb");
    if (!f) return;
    uint8_t buf[8 + kMidiMapMax * 8];
    const size_t got = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (got < 8) return;
    uint32_t magic = 0, n = 0;
    for (int i = 0; i < 4; ++i) {
        magic |= (uint32_t)buf[i] << (8 * i);
        n |= (uint32_t)buf[4 + i] << (8 * i);
    }
    if (magic != kMidiMapMagic || n > (uint32_t)kMidiMapMax || got < 8 + 8 * (size_t)n) return;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* e = buf + 8 + 8 * i;
        MidiBinding b;
        b.cc = e[0];
        b.ch = e[1];
        b.ref.kind = (ParamRef::Kind)e[2];
        b.ref.index = (int8_t)e[3];
        b.ref.id = e[4];
        if (b.cc > 127 || b.ch < 1 || b.ch > 16 || !refValid(b.ref)) continue;
        s.map[s.map_count++] = b;
    }
}

// Binds (channel, cc) to r: one controller per knob and one knob per controller.
void midiBind(int ch, int cc, const ParamRef& r)
{
    State& s = S();
    int w = 0;
    for (int i = 0; i < s.map_count; ++i) {
        const MidiBinding& b = s.map[i];
        if ((b.cc == cc && b.ch == ch) || sameRef(b.ref, r)) continue;
        s.map[w++] = b;
    }
    s.map_count = w;
    if (s.map_count >= kMidiMapMax) {
        sayf("MIDI LEARN: pełna tablica (%d przypisań)", kMidiMapMax);
        return;
    }
    MidiBinding nb;
    nb.cc = (uint8_t)cc;
    nb.ch = (uint8_t)ch;
    nb.ref = r;
    s.map[s.map_count++] = nb;
    s.learn_us = hal::micros();
    midiMapSave();
    ++s.revs.midi;
    char what[48];
    refLabel(r, what, sizeof what);
    sayf("MIDI LEARN: CC %d (kanał %d) \xE2\x86\x92 %s", cc, ch, what);
}

// paramSet*() hook: in learn mode the knob moved last is the one the next CC binds to.
void learnTouched(const ParamRef& r)
{
    State& s = S();
    if (!s.learn || s.cc_applying) return;
    const bool changed = !s.learn_armed || !sameRef(s.learn_ref, r);
    s.learn_ref = r;
    s.learn_armed = true;
    s.learn_us = hal::micros();
    if (!changed) return;
    ++s.revs.midi;
    char what[48];
    refLabel(r, what, sizeof what);
    sayf("MIDI LEARN: %s - teraz rusz kontrolerem", what);
}

void ccHandle(int ch, int cc, int val)
{
    State& s = S();
    if (s.learn && s.learn_armed) {
        midiBind(ch, cc, s.learn_ref);
        s.learn_armed = false;
        return;
    }
    for (int i = 0; i < s.map_count; ++i) {
        const MidiBinding& b = s.map[i];
        if (b.cc != cc || b.ch != ch) continue;
        s.cc_applying = true;
        app().paramSetNorm(b.ref, (float)val * (1.0f / 127.0f));
        s.cc_applying = false;
    }
}

// Applies Settings.midi_* (init, applySettings).
void midiSync()
{
    State& s = S();
    const Settings& n = s.settings;
    MidiShared& x = s.midi;
    x.channel.store(n.midi_channel, std::memory_order_relaxed);
    const bool cin = n.midi_usb && n.midi_clock_in;
    if (!cin && x.clock_in.load(std::memory_order_relaxed)) {
        x.clock_reset.store(1, std::memory_order_release);
        x.clock_bpm_milli.store(0, std::memory_order_relaxed);
    }
    x.clock_in.store(cin ? 1 : 0, std::memory_order_relaxed);
    if (n.midi_usb && !s.midi_started) {
        s.midi_started = hal::midiStart(&midiHook, nullptr);
        if (!s.midi_started) say("Nie można uruchomić USB MIDI");
    }
    // The USB host cannot be stopped: with midi_usb off the hook ignores everything.
    x.on.store(n.midi_usb && s.midi_started ? 1 : 0, std::memory_order_release);
    const bool out = n.midi_usb && n.midi_clock_out && s.midi_started;
    if (out && !s.clock_timer) {
        g_clock_out = ClockOut{};   // the timer is stopped: no other user
        x.latency_us.store((uint32_t)(hal::audioLatencyMs() * 1000.0f), std::memory_order_relaxed);
        x.clock_out.store(1, std::memory_order_release);
        s.clock_timer = hal::serviceTimerStart(&clockOutService, nullptr, 1000);
        if (!s.clock_timer) {
            x.clock_out.store(0, std::memory_order_release);
            say("MIDI clock out: brak timera");
        }
    } else if (!out && s.clock_timer) {
        x.clock_out.store(0, std::memory_order_release);
        hal::serviceTimerStart(nullptr, nullptr, 0);
        s.clock_timer = false;
        if (g_clock_out.playing) midiOut(0xFC);   // receivers must not keep running without a clock
        g_clock_out.playing = false;
    }
    ++s.revs.midi;
}

// tick(): CCs and transport from the USB task, device changes, external tempo.
void midiTick()
{
    State& s = S();
    MidiShared& x = s.midi;
    MidiIn batch[64];
    int n = 0;
    while (n < 64 && s.midi_q.pop(batch[n])) ++n;
    for (int i = 0; i < n; ++i) {
        const MidiIn& e = batch[i];
        switch (e.type) {
        case MidiEv::Start:
        case MidiEv::Continue:
            s.playing = true;
            ++s.revs.transport;
            break;
        case MidiEv::Stop:
            s.playing = false;
            ++s.revs.transport;
            break;
        case MidiEv::Cc: {
            // A turned knob sends many values: only the newest of this frame is applied.
            bool later = false;
            for (int k = i + 1; k < n && !later; ++k)
                later = batch[k].type == MidiEv::Cc && batch[k].ch == e.ch && batch[k].a == e.a;
            if (!later || (s.learn && s.learn_armed)) ccHandle(e.ch, e.a, e.b);
            break;
        }
        }
    }

    const bool conn = hal::midiConnected();
    if (conn != s.midi_conn) {
        s.midi_conn = conn;
        ++s.revs.midi;
        if (conn) sayf("MIDI: %s", hal::midiDeviceName());
        else say("MIDI: urządzenie odłączone");
    }

    float ext = 0.0f;
    if (x.clock_in.load(std::memory_order_relaxed)) {
        const uint32_t now_ms = (uint32_t)(hal::micros() / 1000u);
        const uint32_t last = x.clock_last_ms.load(std::memory_order_relaxed);
        const uint32_t milli = x.clock_bpm_milli.load(std::memory_order_relaxed);
        if (milli && now_ms - last <= kClockLostMs) ext = (float)milli * 0.001f;
    }
    if ((ext > 0.0f) != (s.ext_bpm > 0.0f) || fabsf(ext - s.ext_bpm) > 0.05f) {
        s.ext_bpm = ext;
        ++s.revs.midi;
    }
    // A change > 0.3 BPM starts tracking: the project follows the (0.1-rounded) estimate until the
    // estimator spans a full window; then small drift (< 0.3 BPM) is ignored.
    if (ext > 0.0f) {
        if (fabsf(ext - P().bpm) > 0.3f) s.ext_tracking = true;
        if (s.ext_tracking) {
            const float target = roundf(ext * 10.0f) * 0.1f;
            if (fabsf(target - P().bpm) > 0.01f) app().setBpm(target);
            if (x.clock_settled.load(std::memory_order_acquire)) s.ext_tracking = false;
        }
    } else {
        s.ext_tracking = false;
    }
    x.latency_us.store((uint32_t)(s.latency * 1000.0f), std::memory_order_relaxed);
}

void btTick(uint64_t now);   // Bluetooth section at the end of the file
bool btPark();               // ... (exitToLauncher)
void btUnpark();
void volTick();              // output volume memory, end of the file

}  // namespace

// =============================================================================== App

App& app()
{
    static App a;
    return a;
}

bool App::init(const char* sd_root, int sample_rate)
{
    State& s = S();
    if (s.inited) return true;
#ifdef ESP_PLATFORM
    // Boot profile on the device: time of each init step
    uint64_t t_step = nowUs();
    auto mark = [&](const char* what) {
        const uint64_t now = nowUs();
        logf("init %-14s %5u ms", what, (unsigned)((now - t_step) / 1000u));
        t_step = now;
    };
#else
    auto mark = [](const char*) {};
#endif
    s.project = static_cast<Project*>(bigAlloc(sizeof(Project)));
    if (!s.project) return false;
    new (s.project) Project();
    projectDefaults(*s.project);
    mark("project");

    settingsDefaults(s.settings);
    hal::settingsLoad(s.settings);
    {
        // bt_* start after the 36 bytes of older settings files (settings.h), which therefore keep the defaults.
        Settings& t = s.settings;
        t.bt_enabled = t.bt_enabled ? 1 : 0;
        t.bt_local = t.bt_local ? 1 : 0;
        t.bt_volume = (uint8_t)clampi(t.bt_volume, 0, 100);
        s.bt_boot = t.bt_enabled != 0;   // started by tick() once app_main's audio start is over (btTick)
        // Output volume memory (0.3): files of older builds had one codec volume, now every output keeps its own.
        if (!t.vol_init) {
            t.vol_speaker = t.vol_headphones = (uint8_t)clampi(t.volume, 0, 100);
            t.vol_init = 1;
        }
        t.vol_speaker = (uint8_t)clampi(t.vol_speaker, 0, 100);
        t.vol_headphones = (uint8_t)clampi(t.vol_headphones, 0, 100);
        sanitizeColors(t);
        // restartApp(): the project to open again, once (read and cleared now, opened by the first tick)
        if (t.resume_project[0]) {
            t.resume_project[sizeof t.resume_project - 1] = 0;
            copyStr(s.resume_name, sizeof s.resume_name, t.resume_project);
            t.resume_project[0] = 0;
            hal::settingsSave(t);
        }
    }
    s.settings_edit = s.settings;
    s.project->limiter_on = s.settings.master_limiter;
    mark("settings");

    s.rate = sample_rate > 0 ? sample_rate : (int)s.settings.sample_rate;
    if (!eng().init(s.rate, s.project)) {
        logf("app: engine init failed");
        return false;
    }
    eng().setVoiceLimit(s.settings.voice_limit);   // Settings > Audio > Voice limit
    mark("engine");

    s.sd = false;
    if (sd_root && sd_root[0]) {
        copyStr(s.sd_root, sizeof s.sd_root, sd_root);
        char root[kPathLen];
        sfmt(root, sizeof root, "%s/ORBIT", sd_root);
        s.sd = s.libs[0].open(root) && s.libs[1].open(root);
    }
    mark("library");
    formatSdStatus(0, 0);
    // A fresh session is a new, unsaved project: never the name of a project already on the card
    // (SAVE / autosave of "ORBIT_01" would replace the one saved yesterday).
    s.file_name[0] = 0;
    if (s.sd) pickFreeName(s.project->name, (int)sizeof s.project->name);
    mark("sd status/name");

    // Built-in 909-style kit on the pads (falls back to the first kit).
    if (builtinKitCount() > 1) loadBuiltinKit(1);
    else if (builtinKitCount() > 0) loadBuiltinKit(0);
    mark("builtin kit");
    eng().telemetry().watch_pad.store(0, std::memory_order_relaxed);
    s.rnd ^= (uint32_t)nowUs() | 1u;

    hal::audioSetTimingHook(&audioTiming, &eng());
    s.worker = startWorker(&loaderMain, nullptr, "loader", 16384, 2);
    if (!s.worker) logf("app: loader task failed to start");
    s.inited = true;
    if (s.sd) {
        s.scan_busy = true;
        Job j;
        j.type = JobType::Scan;
        j.index = 1 - s.front;
        j.flag = 0;
        if (!postJob(j)) s.scan_busy = false;
    }
    s.saved_gen = s.dirty_gen;
    s.last_save_us = nowUs();
    mark("loader+scan job");
    s.launcher = hal::launcherAvailable();
    mark("launcher check");
    midiMapLoad();
    mark("midi map");
    midiSync();   // Settings.midi_usb: USB host + hook; clock out timer
    mark("midi/usb");
    return true;
}

void App::render(int16_t* lr, int frames, void* user)
{
    Engine* e = user ? static_cast<Engine*>(user) : &eng();
    // Timing (CPU load) comes from the driver through audioTiming() (hal::audioSetTimingHook).
    int done = 0;
    while (done < frames) {
        const int n = frames - done < kMaxBlock ? frames - done : kMaxBlock;
        e->processI16(lr + done * 2, n);
        done += n;
    }
}

void App::tick()
{
    State& s = S();
    if (!s.inited) return;
    if (!s.ui_ready) {
        s.ui_ready = true;
        hal::setBrightness(s.settings.brightness);
        hal::setFrameCap(s.settings.low_power ? 30 : 60);
        hal::audioSetOutput(s.settings.output);   // the codec volume follows from volTick() below
        if (s.resume_name[0]) {   // back from restartApp(): the project that was open
            char name[32];
            copyStr(name, sizeof name, s.resume_name);
            s.resume_name[0] = 0;
            if (s.sd && projectFileExists(name)) open(name);
        }
        if (s.pending_toast[0]) {
            ::orbit::ui::toast(s.pending_toast);
            s.pending_toast[0] = 0;
        }
    }
    flushDeferred();

    Telemetry& tel = eng().telemetry();
    Event e;
    for (int n = 0; n < 256 && eng().poll(e); ++n) {
        switch (e.type) {
        case EvtType::PadHit:
            if (e.pad < kPads) s.flash |= 1u << e.pad;
            break;
        case EvtType::SampleRetired:
        case EvtType::PreviewDone: engineAnswered(e); break;
        case EvtType::SceneChanged: ++s.revs.pattern, ++s.revs.transport; break;
        case EvtType::TransportChanged:
            s.playing = tel.playing.load(std::memory_order_relaxed) != 0;
            s.recording = tel.recording.load(std::memory_order_relaxed) != 0;
            ++s.revs.transport;
            break;
        case EvtType::SongSlotChanged: ++s.revs.song, ++s.revs.transport; break;
        case EvtType::RecordHit:
            if (e.pad < kPads && s.audition_ms[e.pad]) {
                // An audition (waveform tap) is listening, not playing: not written into the pattern.
                const uint32_t t = s.audition_ms[e.pad];
                s.audition_ms[e.pad] = 0;
                if ((int32_t)((uint32_t)(nowUs() / 1000u) - t) <= (int32_t)kAuditionMs) break;   // stamps are ms | 1
            }
            if (e.scene >= 0 && e.scene < kScenes && e.pad < kPads && e.step >= 0 && e.step < kMaxSteps) {
                // e.step: the pad's track step (Sequencer::quantize with the pad), so a polymeter track
                // records where it plays; anything beyond the track length is wrapped defensively.
                const Scene& scn = s.project->scene_data[e.scene];
                const int tlen = trackLength(scn, e.pad);
                const int at = tlen > 0 && e.step >= tlen ? e.step % tlen : e.step;
                Step& st = s.project->scene_data[e.scene].track[e.pad].step[at];
                st.on = 1;
                st.vel = (uint8_t)clampi(e.vel, 1, 127);
                st.nudge = (int8_t)clampi(e.nudge, -50, 50);
                ++s.revs.pattern;
                markDirty();
            }
            break;
        default: break;
        }
    }

    flushWaiting();   // slots whose sample's earlier retirement was just answered

    Result r;
    for (int n = 0; n < 8 && s.res.pop(r); ++n) {
        ++s.jobs_done;
        applyResult(r);
    }

    midiTick();

    s.flash |= tel.pad_hits.exchange(0, std::memory_order_relaxed);

    // Autosave every 5 min while there are unsaved changes (Settings > General > Autosave). Never over a
    // <name>.orb this session did not open or save (another project of the same name): SAVE asks first.
    const uint64_t now = nowUs();
    if (s.settings.autosave && s.sd && s.dirty_gen != s.saved_gen && !s.save_busy &&
        now - s.last_save_us > kAutosaveUs) {
        s.last_save_us = now;   // a failed attempt retries in 5 min, not every frame
        if (!ownsFile(s.project->name) && projectFileExists(s.project->name)) {
            if (!s.autosave_warned) sayf("Autozapis wstrzymany: %s już istnieje (zapisz ręcznie)", s.project->name);
            s.autosave_warned = true;
        } else {
            s.autosave_next = true;
            saveAs(s.project->name);
            s.autosave_next = false;
        }
    }

    // MIDI learn ends by itself after a minute without a knob move or a binding.
    if (s.learn && hal::micros() > s.learn_us + kLearnIdleUs) {
        app().midiLearn(false);
        say("MIDI LEARN wyłączony (brak aktywności)");
    }

    btTick(now);
    volTick();

    s.cpu = tel.cpu.load(std::memory_order_relaxed);
    s.voices = tel.voices.load(std::memory_order_relaxed);
    s.latency = hal::audioLatencyMs();
    // The engine's CPU governor took voices away (render time near the block period): say why notes get
    // cut and what helps, at most every 30 s.
    if ((int)tel.voice_cap.load(std::memory_order_relaxed) < eng().voiceLimit() &&
        (s.overload_us == 0 || now - s.overload_us > kOverloadNoticeUs)) {
        s.overload_us = now;
        sayf("CPU audio przeciążone: %d głosów \xE2\x80\x94 zwiększ bufor (Settings > Audio)",
             (int)tel.voice_cap.load(std::memory_order_relaxed));
    }
    // Deferred flash (NVS) writes stall both cores: allowed only after kIdleHoldUs of real silence.
    const uint32_t live0 = s.live_ms.load(std::memory_order_relaxed);
    const float peak = fmaxf(tel.master_peak[0].load(std::memory_order_relaxed),
                             tel.master_peak[1].load(std::memory_order_relaxed));
    const bool quiet = tel.playing.load(std::memory_order_relaxed) == 0 && s.voices == 0 && !s.playing &&
                       peak < kSilentPeak && s.defer_count == 0 &&
                       (live0 == 0 || (int32_t)((uint32_t)(now / 1000u) - live0) > (int32_t)(kIdleHoldUs / 1000u));
    if (!quiet || s.loud_us == 0) s.loud_us = now;
    bool idle = quiet && now - s.loud_us >= kIdleHoldUs;
    if (idle != s.audio_idle) {
        hal::setAudioIdle(idle);
        // A hit posted by another task while this was computed: back to "not idle" at once.
        if (idle && s.live_ms.load(std::memory_order_relaxed) != live0) {
            hal::setAudioIdle(false);
            idle = false;
            s.loud_us = now;
        }
        s.audio_idle = idle;
    }
}

Project& App::project() { return P(); }
Engine& App::engine() { return eng(); }
Library& App::library() { return front(); }
UiState& App::ui() { return S().ui; }
const Revs& App::revs() const { return S().revs; }
void App::bump(uint32_t Revs::*field) { ++(S().revs.*field); }
const Settings& App::settings() const { return S().settings; }

// ---------------------------------------------------------------- navigation / selection
void App::setView(View v)
{
    if ((int)v >= (int)View::Count) return;
    S().ui.view = v;
    ++S().revs.view;
}

void App::setEditor(EditorTab t)
{
    if ((int)t >= (int)EditorTab::Count) return;
    S().ui.editor = t;
    ++S().revs.view;
}

void App::selectPad(int pad, bool audition)
{
    State& s = S();
    pad = clampi(pad, 0, kPads - 1);
    if (pad != s.ui.pad) {
        s.ui.pad = pad;
        s.ui.layer = 0;
    }
    s.ui.pad_shown = 1;
    eng().telemetry().watch_pad.store((int8_t)pad, std::memory_order_relaxed);
    ++s.revs.selection;
    if (audition && s.settings.pad_audition) postTrigger(pad, s.settings.fixed_velocity, true);
}

void App::selectLayer(int layer)
{
    S().ui.layer = clampi(layer, 0, kLayers - 1);
    ++S().revs.selection;
}

void App::setPadMode(PadMode m)
{
    S().ui.pad_mode = m;
    ++S().revs.pads;
}

void App::setSeqPage(int page)
{
    S().ui.seq_page = clampi(page, 0, kMaxSteps / kStepsPerPage - 1);
    ++S().revs.view;
    ++S().revs.pattern;
}

void App::setSeqBank(int bank)
{
    S().ui.seq_bank = clampi(bank, 0, 1);
    ++S().revs.view;
    ++S().revs.pattern;
}

void App::setLane(StepLane lane)
{
    if ((int)lane >= (int)StepLane::Count) return;
    S().ui.lane = lane;
    ++S().revs.view;
    ++S().revs.pattern;
}

// ---------------------------------------------------------------- pads (live play)
void App::padDown(int pad, int vel, Producer from)
{
    if (pad < 0 || pad >= kPads) return;
    if (from == Producer::Touch && g_state) {
        // MUTE / SOLO pad modes: a touch toggles (padTap on the LVGL task) instead of playing.
        if (g_state->ui.pad_mode != PadMode::Play) return;
        g_state->touch_ms[pad].store((uint32_t)(nowUs() / 1000u) | 1u, std::memory_order_relaxed);
    }
    Command c;
    c.type = CmdType::Trigger;
    c.pad = (uint8_t)pad;
    c.vel = (uint8_t)clampi(vel, 1, 127);
    noteSound();           // before the post: no flash write may start under this hit
    eng().post(from, c);   // a dropped live hit is better than blocking the touch task
}

void App::padUp(int pad, Producer from)
{
    if (pad < 0 || pad >= kPads) return;
    Command c;
    c.type = CmdType::Release;
    c.pad = (uint8_t)pad;
    eng().post(from, c);
}

void App::padTap(int pad)
{
    if (pad < 0 || pad >= kPads) return;
    switch (S().ui.pad_mode) {
    case PadMode::Mute: toggleMute(pad); break;
    case PadMode::Solo: toggleSolo(pad); break;
    default: {
        selectPad(pad, false);
        // The pad grid's touch hook already played it on finger-down: no second hit.
        const uint32_t t = S().touch_ms[pad].load(std::memory_order_relaxed);
        const uint32_t now = (uint32_t)(nowUs() / 1000u);
        if (t == 0 || (int32_t)(now - t) > 400) postTrigger(pad, S().settings.fixed_velocity, false);
        break;
    }
    }
}

void App::toggleMute(int pad)
{
    if (pad < 0 || pad >= kPads) return;
    Pad& p = P().pad[pad];
    p.mute = p.mute ? 0 : 1;
    ++S().revs.pads;
    markDirty();
}

void App::toggleSolo(int pad)
{
    if (pad < 0 || pad >= kPads) return;
    Pad& p = P().pad[pad];
    p.solo = p.solo ? 0 : 1;
    ++S().revs.pads;
    markDirty();
}

uint32_t App::padFlashBits()
{
    State& s = S();
    uint32_t b = s.flash | eng().telemetry().pad_hits.exchange(0, std::memory_order_relaxed);
    s.flash = 0;
    s.grid_hits |= b;
    return b;
}

uint32_t App::takeGridHits()
{
    State& s = S();
    const uint32_t b = s.grid_hits;
    s.grid_hits = 0;
    return b;
}

// ---------------------------------------------------------------- parameters
float App::paramGet(const ParamRef& r) const
{
    switch (r.kind) {
    case ParamRef::Kind::BusLevel: {
        const int b = busIndex(r);
        return b == kBuses ? masterPos() : P().bus[b].level;
    }
    case ParamRef::Kind::BusPan: {
        const int b = busIndex(r);
        return b == kBuses ? 0.0f : P().bus[b].pan;
    }
    case ParamRef::Kind::OutVolume: return (float)outputVolume() * 0.01f;
    default: {
        Target t = target(r);
        return t.v ? *t.v : 0.0f;
    }
    }
}

void App::paramSet(const ParamRef& r, float value)
{
    learnTouched(r);
    if (r.kind == ParamRef::Kind::OutVolume) {   // a device setting, not part of the project
        setOutputVolume((int)lroundf(clampf(value, 0.0f, 1.0f) * 100.0f));
        return;
    }
    switch (r.kind) {
    case ParamRef::Kind::BusLevel: {
        const int b = busIndex(r);
        if (b == kBuses) {
            P().g[GP_MASTER] = posToMasterDb(value);
            S().master_muted = false;
        } else {
            P().bus[b].level = clampf(value, 0.0f, 1.0f);
        }
        break;
    }
    case ParamRef::Kind::BusPan: {
        const int b = busIndex(r);
        if (b == kBuses) return;
        P().bus[b].pan = clampf(value, -1.0f, 1.0f);
        break;
    }
    default: {
        Target t = target(r);
        if (!t.v) return;
        float v = clampParam(*t.spec, value);
        if (r.kind == ParamRef::Kind::Pad) {
            // Keep the play region valid (START < END).
            Pad& p = P().pad[padIndex(r.index)];
            if (r.id == PP_START && v > p.p[PP_END] - 0.001f) v = clampf(p.p[PP_END] - 0.001f, 0.0f, 1.0f);
            if (r.id == PP_END && v < p.p[PP_START] + 0.001f) v = clampf(p.p[PP_START] + 0.001f, 0.0f, 1.0f);
        }
        *t.v = v;
        break;
    }
    }
    bumpFor(r);
    markDirty();
}

float App::paramNorm(const ParamRef& r) const
{
    switch (r.kind) {
    case ParamRef::Kind::BusLevel:
    case ParamRef::Kind::OutVolume: return paramGet(r);
    case ParamRef::Kind::BusPan: return (paramGet(r) + 1.0f) * 0.5f;
    default: {
        const ParamSpec* sp = paramSpec(r);
        return sp ? toNorm(*sp, paramGet(r)) : 0.0f;
    }
    }
}

void App::paramSetNorm(const ParamRef& r, float norm)
{
    norm = clampf(norm, 0.0f, 1.0f);
    switch (r.kind) {
    case ParamRef::Kind::BusLevel:
    case ParamRef::Kind::OutVolume: paramSet(r, norm); break;
    case ParamRef::Kind::BusPan: paramSet(r, norm * 2.0f - 1.0f); break;
    default: {
        const ParamSpec* sp = paramSpec(r);
        if (sp) paramSet(r, fromNorm(*sp, norm));
        break;
    }
    }
}

void App::paramReset(const ParamRef& r)
{
    learnTouched(r);
    switch (r.kind) {
    case ParamRef::Kind::BusLevel:
        if (busIndex(r) == kBuses) {
            P().g[GP_MASTER] = globalSpec(GP_MASTER).def;
            S().master_muted = false;
            bumpFor(r);
            markDirty();
        } else {
            paramSet(r, 0.7071f);
        }
        break;
    case ParamRef::Kind::BusPan: paramSet(r, 0.0f); break;
    case ParamRef::Kind::LayerTune:
    case ParamRef::Kind::LayerGain:
    case ParamRef::Kind::LayerPan: paramSet(r, 0.0f); break;
    case ParamRef::Kind::OutVolume: setOutputVolume(outputKind() == OutKind::Headphones ? 50 : 70); break;
    default: {
        const ParamSpec* sp = paramSpec(r);
        if (sp) paramSet(r, sp->def);
        break;
    }
    }
}

const ParamSpec* App::paramSpec(const ParamRef& r) const
{
    switch (r.kind) {
    case ParamRef::Kind::Pad: return r.id < kPadParamCount ? &padSpec((PadParam)r.id) : nullptr;
    case ParamRef::Kind::Global: return r.id < kGlobalParamCount ? &globalSpec((GlobalParam)r.id) : nullptr;
    case ParamRef::Kind::LayerTune: return &padSpec(PP_TUNE);
    case ParamRef::Kind::LayerGain: return &padSpec(PP_GAIN);
    case ParamRef::Kind::LayerPan: return &padSpec(PP_PAN);
    default: return nullptr;
    }
}

int App::paramText(const ParamRef& r, char* buf, int len) const
{
    if (!buf || len <= 0) return 0;
    switch (r.kind) {
    case ParamRef::Kind::BusLevel: {
        const int b = busIndex(r);
        if (b == kBuses) return formatParam(globalSpec(GP_MASTER), P().g[GP_MASTER], buf, len);
        const float g = busGain(P().bus[b].level);
        if (g <= 0.001f) return sfmt(buf, len, "-inf dB");
        const float db = gainToDb(g);
        return sfmt(buf, len, "%.1f dB", (double)(fabsf(db) < 0.05f ? 0.0f : db));
    }
    case ParamRef::Kind::BusPan: return formatParam(padSpec(PP_PAN), paramGet(r), buf, len);
    case ParamRef::Kind::OutVolume: {
        const OutKind k = outputKind();
        return sfmt(buf, len, "%d%% \xC2\xB7 %s", outputVolume(k), outputTag(k));
    }
    default: break;
    }
    const ParamSpec* sp = paramSpec(r);
    if (!sp) return sfmt(buf, len, "-");
    const float v = paramGet(r);
    if (r.kind == ParamRef::Kind::Pad && r.id == PP_LFO_RATE && P().pad[padIndex(r.index)].p[PP_LFO_SYNC] >= 0.5f) {
        const int idx = clampi((int)lroundf(toNorm(*sp, v) * 11.0f), 0, (int)SyncDiv::Count - 1);
        return sfmt(buf, len, "%s", syncDivName((SyncDiv)idx));
    }
    return formatParam(*sp, v, buf, len);
}

const char* App::paramCaption(const ParamRef& r) const
{
    switch (r.kind) {
    case ParamRef::Kind::BusLevel: {
        const int b = busIndex(r);
        return b == kBuses ? "MASTER" : P().bus[b].name;
    }
    case ParamRef::Kind::BusPan: return "PAN";
    case ParamRef::Kind::OutVolume: return "VOLUME";
    default: {
        const ParamSpec* sp = paramSpec(r);
        return sp ? sp->label : "";
    }
    }
}

// ---------------------------------------------------------------- pad structure (selected pad)
void App::setPlayMode(PlayMode m)
{
    if ((int)m >= (int)PlayMode::Count) return;
    P().pad[S().ui.pad].mode = m;
    ++S().revs.pads;
    markDirty();
}

void App::setReverse(bool on)
{
    P().pad[S().ui.pad].reverse = on ? 1 : 0;
    ++S().revs.pads;
    markDirty();
}

void App::setChoke(int group)
{
    P().pad[S().ui.pad].choke = (uint8_t)clampi(group, 0, 8);
    ++S().revs.pads;
    markDirty();
}

void App::setBus(int bus)
{
    P().pad[S().ui.pad].bus = (uint8_t)clampi(bus, 0, kBuses - 1);
    ++S().revs.pads;
    ++S().revs.mixer;
    markDirty();
}

void App::setPoly(int voices)
{
    P().pad[S().ui.pad].poly = (uint8_t)clampi(voices, 1, kMaxPadPoly);
    ++S().revs.pads;
    markDirty();
}

void App::setLayerMode(LayerMode m)
{
    if ((int)m >= (int)LayerMode::Count) return;
    P().pad[S().ui.pad].layer_mode = m;
    ++S().revs.pads;
    markDirty();
}

void App::renamePad(int pad, const char* name)
{
    if (pad < 0 || pad >= kPads || !name) return;
    copyStr(P().pad[pad].name, kNameLen, name);
    ++S().revs.pads;
    markDirty();
}

// ---------------------------------------------------------------- transport
void App::play()
{
    postSimple(CmdType::Play);
    S().playing = true;
    ++S().revs.transport;
}

void App::stop()
{
    postSimple(CmdType::Stop);
    S().playing = false;
    ++S().revs.transport;
}

void App::togglePlay()
{
    if (S().playing) {
        stop();
    } else {
        postSimple(CmdType::Continue);
        S().playing = true;
        ++S().revs.transport;
    }
}

void App::toggleRecord()
{
    State& s = S();
    s.recording = !s.recording;
    postSimple(CmdType::SetRecord, s.recording ? 1 : 0);
    ++s.revs.transport;
    say(s.recording ? "REC: nagrywanie uzbrojone" : "REC wyłączony");
}

void App::setBpm(float bpm)
{
    if (!(bpm == bpm)) return;
    P().bpm = clampf(bpm, kBpmMin, kBpmMax);
    ++S().revs.transport;
    markDirty();
}

void App::bpmStep(int delta) { setBpm(roundf(P().bpm) + (float)delta); }

void App::tap() { tapAt(nowUs()); }

void App::tapAt(uint64_t t)
{
    State& s = S();
    if (s.tap_count > 0 && (t < s.taps[s.tap_count - 1] || t - s.taps[s.tap_count - 1] > kTapResetUs)) s.tap_count = 0;
    if (s.tap_count == 5) {
        for (int i = 0; i < 4; ++i) s.taps[i] = s.taps[i + 1];
        s.tap_count = 4;
    }
    s.taps[s.tap_count++] = t;
    if (s.tap_count < 2) return;
    const int n = s.tap_count - 1;   // intervals, at most 4
    const uint64_t span = s.taps[s.tap_count - 1] - s.taps[0];
    if (!span) return;
    const float avg_us = (float)span / (float)n;
    float bpm = 60000000.0f / avg_us;
    bpm = clampf(roundf(bpm * 10.0f) * 0.1f, kBpmMin, kBpmMax);
    setBpm(bpm);   // the top bar and the macros BPM show it live: no toast over the SCENES matrix
}

void App::setMetronome(bool on)
{
    P().metronome = on ? 1 : 0;
    postSimple(CmdType::Metronome, on ? 1 : 0);
    ++S().revs.transport;
    markDirty();
}

void App::setFill(bool on)
{
    S().ui.fill = on ? 1 : 0;
    postSimple(CmdType::SetFill, on ? 1 : 0);
    ++S().revs.transport;
}

bool App::playing() const { return S().playing; }
bool App::recording() const { return S().recording; }
int App::playStep() const { return eng().telemetry().step.load(std::memory_order_relaxed); }
int App::playScene() const { return eng().telemetry().scene.load(std::memory_order_relaxed); }
int App::queuedScene() const { return eng().telemetry().queued_scene.load(std::memory_order_relaxed); }
int App::songSlot() const { return eng().telemetry().song_slot.load(std::memory_order_relaxed); }

// ---------------------------------------------------------------- pattern
namespace {
Scene& curScene() { return P().scene_data[P().scene < kScenes ? P().scene : 0]; }
bool stepValid(int pad, int s) { return pad >= 0 && pad < kPads && s >= 0 && s < kMaxSteps; }
void patternEdited()
{
    ++S().revs.pattern;
    markDirty();
}
// Pattern undo (one level): the scene as it was before a destructive edit.
void patternSnapshot(int scene)
{
    State& s = S();
    if (scene < 0 || scene >= kScenes) return;
    s.pat_undo = P().scene_data[scene];
    s.pat_undo_scene = (int8_t)scene;
    ++s.pat_undo_gen;
}
}  // namespace

const Step& App::step(int pad, int s) const
{
    static const Step kNone{};
    if (!stepValid(pad, s)) return kNone;
    return curScene().track[pad].step[s];
}

void App::toggleStep(int pad, int s)
{
    if (!stepValid(pad, s)) return;
    Step& x = curScene().track[pad].step[s];
    x.on = x.on ? 0 : 1;
    patternEdited();
}

void App::setStepOn(int pad, int s, bool on)
{
    if (!stepValid(pad, s)) return;
    Step& x = curScene().track[pad].step[s];
    if ((x.on != 0) == on) return;
    x.on = on ? 1 : 0;
    patternEdited();
}

void App::setStepLane(int pad, int s, StepLane lane, int value)
{
    if (!stepValid(pad, s)) return;
    Step& x = curScene().track[pad].step[s];
    switch (lane) {
    case StepLane::Trig: x.on = value ? 1 : 0; break;
    case StepLane::Velocity: x.vel = (uint8_t)clampi(value, 1, 127); break;
    case StepLane::Probability: x.prob = (uint8_t)clampi(value, 0, 100); break;
    case StepLane::Ratchet: x.ratchet = (uint8_t)clampi(value, 1, 4); break;
    case StepLane::Nudge: x.nudge = (int8_t)clampi(value, -50, 50); break;
    case StepLane::Pitch: x.pitch = (int8_t)clampi(value, -24, 24); break;
    case StepLane::Cond: x.cond = (TrigCond)clampi(value, 0, (int)TrigCond::Count - 1); break;
    default: return;
    }
    patternEdited();
}

int App::stepLaneValue(int pad, int s, StepLane lane) const
{
    const Step& x = step(pad, s);
    switch (lane) {
    case StepLane::Trig: return x.on;
    case StepLane::Velocity: return x.vel;
    case StepLane::Probability: return x.prob;
    case StepLane::Ratchet: return x.ratchet;
    case StepLane::Nudge: return x.nudge;
    case StepLane::Pitch: return x.pitch;
    case StepLane::Cond: return (int)x.cond;
    default: return 0;
    }
}

void App::clearScene()
{
    patternSnapshot(P().scene);
    Scene& sc = curScene();
    for (int t = 0; t < kPads; ++t)
        for (int i = 0; i < kMaxSteps; ++i) sc.track[t].step[i] = Step{};
    patternEdited();
    say("Wyczyszczono scenę");
}

void App::clearTrack(int pad)
{
    if (pad < 0 || pad >= kPads) return;
    patternSnapshot(P().scene);
    Track& t = curScene().track[pad];
    for (int i = 0; i < kMaxSteps; ++i) t.step[i] = Step{};
    patternEdited();
}

void App::randomScene()
{
    patternSnapshot(P().scene);
    Scene& sc = curScene();
    const int len = clampi(sc.length, 1, kMaxSteps);
    const int bars = (len + 15) / 16;
    const int style = rndi(0, 2);
    int seen[(int)Role::Other + 1] = {};
    for (int t = 0; t < kPads; ++t) {
        Track& tr = sc.track[t];
        for (int i = 0; i < kMaxSteps; ++i) tr.step[i] = Step{};
        const Role role = padRole(P().pad[t], t);
        const int nth = seen[(int)role]++;
        // First pad of a role carries the groove, a second one adds sparse color, more stay empty.
        const float weight = nth == 0 ? 1.0f : (nth == 1 ? 0.2f : 0.0f);
        if (weight <= 0.0f) continue;
        for (int b = 0; b < bars; ++b) genBar(tr, role, b * 16, len, style, b == bars - 1, weight);
        if (role == Role::Kick && nth == 0) tr.step[0].on = 1, tr.step[0].vel = 124;   // anchor
    }
    // An open hat replaces the closed hat on the same step (they choke each other anyway).
    for (int t = 0; t < kPads; ++t) {
        if (padRole(P().pad[t], t) != Role::OpenHat) continue;
        for (int h = 0; h < kPads; ++h) {
            if (padRole(P().pad[h], h) != Role::ClosedHat) continue;
            for (int i = 0; i < len; ++i)
                if (sc.track[t].step[i].on) sc.track[h].step[i].on = 0;
        }
    }
    patternEdited();
    say("Wylosowano wzór");
}

void App::selectScene(int scene)
{
    scene = clampi(scene, 0, kScenes - 1);
    P().scene = (uint8_t)scene;
    postSimple(CmdType::QueueScene, 0, scene);
    // A shorter scene has fewer bars: stay on its last one (bar tabs are hidden for a 1-bar scene).
    const int pages = (clampi(P().scene_data[scene].length, 1, kMaxSteps) + kStepsPerPage - 1) / kStepsPerPage;
    if (S().ui.seq_page >= pages) {
        S().ui.seq_page = pages - 1;
        ++S().revs.view;
    }
    ++S().revs.pattern;
    ++S().revs.transport;
}

void App::copyScene(int from, int to)
{
    if (from < 0 || from >= kScenes || to < 0 || to >= kScenes || from == to) return;
    patternSnapshot(to);
    P().scene_data[to] = P().scene_data[from];
    patternEdited();
    sayf("Skopiowano scenę %c \xE2\x86\x92 %c", (char)('A' + from), (char)('A' + to));
}

void App::setSceneLength(int steps)
{
    curScene().length = (uint8_t)clampi(steps, 1, kMaxSteps);
    const int pages = (curScene().length + kStepsPerPage - 1) / kStepsPerPage;
    if (S().ui.seq_page >= pages) S().ui.seq_page = pages - 1;
    patternEdited();
}

void App::setDivision(Division d)
{
    if ((int)d >= (int)Division::Count) return;
    curScene().div = d;
    patternEdited();
}

void App::setSwing(int percent)
{
    curScene().swing = (uint8_t)clampi(percent, 50, 75);
    patternEdited();
}

void App::setTrackLength(int pad, int steps)
{
    if (pad < 0 || pad >= kPads) return;
    curScene().track[pad].length = (uint8_t)clampi(steps, 0, kMaxSteps);
    patternEdited();
}

// ---------------------------------------------------------------- song
void App::setSongSlot(int slot, int scene)
{
    if (slot < 0 || slot >= kSongSlots) return;
    Song& sg = P().song;
    if (scene < 0 || scene >= kScenes || sg.slot[slot] == scene) sg.slot[slot] = -1;
    else sg.slot[slot] = (int8_t)scene;
    ++S().revs.song;
    markDirty();
}

void App::toggleSongFx(int slot)
{
    if (slot < 0 || slot >= kSongSlots) return;
    P().song.fx[slot] = P().song.fx[slot] ? 0 : 1;
    ++S().revs.song;
    markDirty();
}

void App::setFollow(bool on)
{
    P().song.follow = on ? 1 : 0;
    // FOLLOW off: the edited scene plays again (the sequencer takes the request at once when stopped,
    // else at the launch quantization or the song slot's pass end, whichever comes first).
    if (!on) postSimple(CmdType::QueueScene, 0, P().scene);
    ++S().revs.song;
    markDirty();
}

// ---------------------------------------------------------------- mixer / fx
void App::toggleBusMute(int bus)
{
    State& s = S();
    if (bus == kBuses) {
        // MASTER M: silence the output, remember the level (not a project setting).
        if (s.master_muted) {
            P().g[GP_MASTER] = s.master_unmute_db;
            s.master_muted = false;
        } else {
            s.master_unmute_db = P().g[GP_MASTER];
            P().g[GP_MASTER] = globalSpec(GP_MASTER).min;
            s.master_muted = true;
        }
        ++s.revs.mixer;
        ++s.revs.params;
        return;
    }
    if (bus < 0 || bus >= kBuses) return;
    P().bus[bus].mute = P().bus[bus].mute ? 0 : 1;
    ++s.revs.mixer;
    markDirty();
}

void App::toggleBusSolo(int bus)
{
    if (bus < 0 || bus >= kBuses) return;
    P().bus[bus].solo = P().bus[bus].solo ? 0 : 1;
    ++S().revs.mixer;
    markDirty();
}

namespace {
uint8_t* macroField(int which)
{
    Project& p = P();
    switch (which) {
    case 0: return &p.lofi_on;
    case 1: return &p.wide_on;
    case 2: return &p.pump_on;
    case 3: return &p.space_on;
    default: return nullptr;
    }
}
}  // namespace

void App::toggleMacro(int which)
{
    uint8_t* f = macroField(which);
    if (!f) return;
    *f = *f ? 0 : 1;
    static const char* const kNames[4] = {"LO-FI", "WIDE", "PUMP", "SPACE"};
    sayf("%s: %s", kNames[which], *f ? "ON" : "OFF");
    ++S().revs.fx;
    markDirty();
}

bool App::macro(int which) const
{
    const uint8_t* f = macroField(which);
    return f && *f;
}

void App::setLimiter(bool on)
{
    P().limiter_on = on ? 1 : 0;
    ++S().revs.fx;
    ++S().revs.mixer;
    markDirty();
}

void App::setPumpSource(int pad)
{
    P().pump_src = (uint8_t)clampi(pad, 0, kPads - 1);
    ++S().revs.fx;
    markDirty();
}

// ---------------------------------------------------------------- samples
const Sample* App::padSample(int pad, int layer) const
{
    if (pad < 0 || pad >= kPads) return nullptr;
    const Pad& p = P().pad[pad];
    if (layer >= 0) return layer < kLayers ? slotSample(p.layer[layer].slot) : nullptr;
    for (int l = 0; l < kLayers; ++l)
        if (Sample* s = slotSample(p.layer[l].slot)) return s;
    return nullptr;
}

const char* App::padSampleName(int pad) const
{
    if (pad < 0 || pad >= kPads) return "";
    const Pad& p = P().pad[pad];
    for (int l = 0; l < kLayers; ++l) {
        const int sl = p.layer[l].slot;
        if (sl < 0 || !S().slots[sl].s) continue;
        const SlotInfo& si = S().slots[sl];
        return si.ref.name[0] ? si.ref.name : si.s->name;
    }
    return "";
}

bool App::loadFileToPad(int lib_index, int pad, int layer)
{
    State& s = S();
    if (!s.sd || lib_index < 0 || lib_index >= front().count()) return false;
    const LibEntry& e = front().at(lib_index);
    if (e.is_kit) return loadKit(lib_index);
    pad = padIndex(pad);
    layer = layer < 0 ? s.ui.layer : clampi(layer, 0, kLayers - 1);
    // Already decoded for the browser preview: no second read.
    if (s.bp && s.bp_index == lib_index && strcmp(s.bp_path, e.path) == 0) {
        Result r;
        r.job.type = JobType::LoadWav;
        r.job.pad = (int8_t)pad;
        r.job.layer = (int8_t)layer;
        copyStr(r.job.path, sizeof r.job.path, e.path);
        r.sample = s.bp;
        applyLoadWav(r);
        return true;
    }
    Job j;
    j.type = JobType::LoadWav;
    j.pad = (int8_t)pad;
    j.layer = (int8_t)layer;
    j.index = lib_index;
    copyStr(j.path, sizeof j.path, e.path);
    return postJob(j);
}

bool App::loadKit(int lib_index)
{
    State& s = S();
    if (!s.sd || lib_index < 0 || lib_index >= front().count()) return false;
    const LibEntry& e = front().at(lib_index);
    if (!e.is_kit) return false;
    KitJob* kj = static_cast<KitJob*>(bigAlloc(sizeof(KitJob)));
    if (!kj) return false;
    kj->count = -1;   // the loader lists the folder (kit.txt / WAV names): no card I/O on the LVGL task
    copyStr(kj->name, sizeof kj->name, e.name);
    Job j;
    j.type = JobType::LoadKit;
    j.index = lib_index;
    j.lib = &front();
    j.payload = kj;
    if (!postJob(j)) {
        bigFree(kj);
        return false;
    }
    return true;
}

void App::loadBuiltinKit(int kit)
{
    State& s = S();
    if (kit < 0 || kit >= builtinKitCount()) return;
    const BuiltinKit& k = builtinKit(kit);
    if (s.inited) {   // a user action (not the start-up / NEW kit): one-level undo of the pads it replaces
        uint32_t mask = 0;
        for (int i = 0; i < k.count && i < kPads; ++i)
            if (builtinSample(kit, i)) mask |= 1u << i;
        if (mask) padUndoTake(mask, "ZESTAW");
    }
    int ok = 0;
    for (int i = 0; i < k.count && i < kPads; ++i) {
        Sample* smp = builtinSample(kit, i);
        if (!smp) continue;
        SampleRef ref;
        builtinRef(kit, i, ref);
        if (!assignSample(i, 0, smp, ref, false)) continue;
        for (int l = 1; l < kLayers; ++l) clearLayer(i, l);
        Pad& p = P().pad[i];
        const BuiltinSound& snd = k.sounds[i];
        copyStr(p.name, kNameLen, snd.name);
        p.choke = (uint8_t)clampi(snd.choke, 0, 8);
        p.bus = (uint8_t)clampi(snd.bus, 0, kBuses - 1);
        resetRegion(p);
        ++ok;
    }
    ++s.revs.samples;
    ++s.revs.pads;
    if (!ok) {
        say("Wbudowany zestaw jest pusty");
        return;
    }
    if (s.inited) {
        markDirty();
        sayf("Zestaw %s", k.name ? k.name : "");
    }
}

void App::previewEntry(int lib_index)
{
    State& s = S();
    if (!s.sd || lib_index < 0 || lib_index >= front().count()) return;
    if (front().at(lib_index).is_kit) return;
    if (s.bp && s.bp_index == lib_index) {
        postPreview(s.bp);
        return;
    }
    s.ui.lib_sel = lib_index;
    ++s.revs.library;
    Job j;
    j.type = JobType::Probe;
    j.index = lib_index;
    j.flag = 1;
    copyStr(j.path, sizeof j.path, front().at(lib_index).path);
    postJob(j);
}

void App::previewKit(int lib_index)
{
    State& s = S();
    if (!s.sd || lib_index < 0 || lib_index >= front().count()) return;
    if (!front().at(lib_index).is_kit) return;
    Job j;
    j.type = JobType::Probe;
    j.index = lib_index;
    j.flag = 2;          // applyProbe: preview only
    j.lib = &front();    // the loader finds the kit's first file (empty kit: error toast from applyProbe)
    j.path[0] = 0;
    postJob(j);
}

void App::previewBuiltinKit(int kit)
{
    if (kit < 0 || kit >= builtinKitCount()) return;
    if (Sample* smp = builtinSample(kit, 0)) postPreview(smp);   // embedded: never freed
}

// Waveform taps (slice pick, dialog): listening, so REC does not write them into the pattern.
void App::previewPad() { postTrigger(S().ui.pad, S().settings.fixed_velocity, true); }

void App::stopPreview() { postSimple(CmdType::StopPreview); }

bool App::applyOp(SampleOp op)
{
    State& s = S();
    const int pad = s.ui.pad, layer = s.ui.layer;
    if (op == SampleOp::Undo) return undoApply(pad, layer);
    if (op == SampleOp::Duplicate) {
        for (int k = 1; k < kPads; ++k) {
            const int dst = (pad + k) % kPads;
            if (padEmpty(P().pad[dst])) {
                duplicatePadTo(dst);
                return true;
            }
        }
        say("Brak wolnego padu");
        return false;
    }
    if ((int)op >= (int)SampleOp::Count) return false;
    const Pad& p = P().pad[pad];
    const int slot = p.layer[layer].slot;
    Sample* src = slotSample(slot);
    if (!src) {
        say("Brak sampla na tej warstwie");
        return false;
    }
    if (op == SampleOp::ToMono && src->channels == 1) {
        say("Sampel jest już mono");
        return false;
    }
    if (op == SampleOp::PrintCrush && p.p[PP_BITS] >= 16.0f && p.p[PP_DOWNSAMPLE] <= 1.0f) {
        say("Brak efektu BITS / DOWNSAMPLE do zapisania");
        return false;
    }
    Job j;
    j.type = JobType::Op;
    j.op = op;
    j.pad = (int8_t)pad;
    j.layer = (int8_t)layer;
    j.src = src;
    j.src_id = src->id;
    j.flag = p.reverse ? 1 : 0;   // FADE: the play direction decides which end is the fade-in
    memcpy(j.pp, p.p, sizeof j.pp);
    regHold(src, +1);
    if (!postJob(j)) {
        regHold(src, -1);
        return false;
    }
    return true;
}

bool App::canUndo() const
{
    const State& s = S();
    const Undo& u = s.undo[s.ui.pad][s.ui.layer];
    return u.valid && P().pad[s.ui.pad].layer[s.ui.layer].slot == u.slot;
}

void App::sliceAuto()
{
    // Spectral-flux onset detection takes ~0.1-1 s on long samples: it runs on the loader,
    // the markers arrive in tick() (applySlice).
    State& s = S();
    const Pad& p = P().pad[s.ui.pad];
    int layer = s.ui.layer;
    if (!slotSample(p.layer[layer].slot))
        for (layer = 0; layer < kLayers && !slotSample(p.layer[layer].slot); ++layer) {}
    Sample* smp = layer < kLayers ? slotSample(p.layer[layer].slot) : nullptr;
    if (!smp) {
        say("Brak sampla do pocięcia");
        return;
    }
    Job j;
    j.type = JobType::Slice;
    j.pad = (int8_t)s.ui.pad;
    j.layer = (int8_t)layer;
    j.src = smp;
    j.src_id = smp->id;
    j.fval = s.slice_sens;
    memcpy(j.pp, p.p, sizeof j.pp);
    regHold(smp, +1);
    if (!postJob(j)) regHold(smp, -1);
}

void App::sliceGrid(int count)
{
    State& s = S();
    Pad& p = P().pad[s.ui.pad];
    count = clampi(count, 1, kMaxSlices);
    float pos[kMaxSlices];
    const float a = p.p[PP_START], b = p.p[PP_END];
    const int n = ::orbit::sliceGrid(a, b, count, pos);
    if (n <= 0) return;
    storeSlices(p, pos, n, b);
    ++s.revs.pads;
    ++s.revs.params;
    markDirty();
}

void App::sliceSelect(int index)
{
    State& s = S();
    Pad& p = P().pad[s.ui.pad];
    if (!p.slice_count) return;
    index = clampi(index, 0, p.slice_count - 1);
    const float start = p.slices[index], end = sliceEnd(p, index);
    p.slice_sel = (uint8_t)index;
    p.p[PP_START] = clampf(start, 0.0f, 1.0f);
    p.p[PP_END] = clampf(end > start ? end : 1.0f, 0.0f, 1.0f);
    ++s.revs.pads;
    ++s.revs.params;
    markDirty();
}

void App::slicesToPads()
{
    State& s = S();
    const int src = s.ui.pad;
    const Pad& sp = P().pad[src];
    const int count = sp.slice_count;
    if (!count) {
        say("Najpierw potnij sampel (AUTO / GRID)");
        return;
    }
    float starts[kMaxSlices], ends[kMaxSlices];
    for (int i = 0; i < count; ++i) starts[i] = sp.slices[i], ends[i] = sliceEnd(sp, i);
    char base[kNameLen];
    copyStr(base, sizeof base, sp.name);
    if ((int)strlen(base) > kNameLen - 5) base[kNameLen - 5] = 0;
    uint32_t mask = 0;   // the source pad (its region / slices change) and the pads it overwrites
    for (int i = 0; i < count && src + i < kPads; ++i) mask |= 1u << (src + i);
    padUndoTake(mask, "TO PADS");
    int made = 0;
    for (int i = 0; i < count && src + i < kPads; ++i) {
        const int dst = src + i;
        if (dst != src) copyPad(src, dst);
        Pad& d = P().pad[dst];
        d.p[PP_START] = clampf(starts[i], 0.0f, 1.0f);
        d.p[PP_END] = clampf(ends[i] > starts[i] ? ends[i] : 1.0f, 0.0f, 1.0f);
        d.slice_count = 0;
        d.slice_sel = 0;
        sfmt(d.name, kNameLen, "%s %d", base, i + 1);
        ++made;
    }
    ++s.revs.pads;
    ++s.revs.samples;
    ++s.revs.params;
    markDirty();
    // Polish plural: 1 pad, 2-4 pady (not 12-14), else padów.
    const int m10 = made % 10, m100 = made % 100;
    const char* word = made == 1 ? "pad" : (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) ? "pady" : "padów";
    sayf("Pocięto na %d %s", made, word);
}

void App::duplicatePadTo(int dst)
{
    State& s = S();
    const int src = s.ui.pad;
    if (dst < 0 || dst >= kPads || dst == src) return;
    padUndoTake(1u << dst, "DUPLICATE");
    copyPad(src, dst);
    char a[8], b[8];
    padLabel(src, a, sizeof a);
    padLabel(dst, b, sizeof b);
    sayf("Skopiowano %s \xE2\x86\x92 %s", a, b);
    ++s.revs.pads;
    ++s.revs.samples;
    markDirty();
}

void App::clearPadLayer(int pad, int layer)
{
    State& s = S();
    pad = padIndex(pad);
    layer = layer < 0 ? s.ui.layer : clampi(layer, 0, kLayers - 1);
    if (P().pad[pad].layer[layer].slot < 0) return;
    padUndoTake(1u << pad, "CLEAR LAYER");
    clearLayer(pad, layer);
    if (padEmpty(P().pad[pad])) P().pad[pad].slice_count = 0;
    ++s.revs.samples;
    ++s.revs.pads;
    markDirty();
}

int App::loadingCount() const { return (int)(S().jobs_posted - S().jobs_done); }

const Sample* App::previewSample() const { return S().bp; }

void App::setSliceSens(float sens) { S().slice_sens = clampf(sens, 0.0f, 1.0f); ++S().revs.params; }

float App::sliceSens() const { return S().slice_sens; }

// ---------------------------------------------------------------- library
void App::rescan()
{
    State& s = S();
    if (!s.sd) {
        say("Brak karty SD");
        return;
    }
    if (s.scan_busy) {
        s.scan_again = true;
        return;
    }
    Job j;
    j.type = JobType::Scan;
    j.index = 1 - s.front;
    j.flag = 1;
    s.scan_busy = postJob(j);
}

void App::setCategory(Category c)
{
    if ((int)c >= (int)Category::Count) return;
    S().ui.category = c;
    refilter();
}

void App::setSearch(const char* text)
{
    copyStr(S().ui.search, sizeof S().ui.search, text ? text : "");
    refilter();
}

int App::resultCount() const { return S().result_count; }

int App::result(int i) const { return (i >= 0 && i < S().result_count) ? S().results[i] : -1; }

void App::selectEntry(int lib_index)
{
    State& s = S();
    if (!s.sd || lib_index < 0 || lib_index >= front().count()) return;
    s.ui.lib_sel = lib_index;
    ++s.revs.library;
    if (front().at(lib_index).is_kit) {
        setBrowserPreview(nullptr, -1, "");
        return;
    }
    if (s.bp && s.bp_index == lib_index) return;
    Job j;
    j.type = JobType::Probe;
    j.index = lib_index;
    j.flag = 0;
    copyStr(j.path, sizeof j.path, front().at(lib_index).path);
    postJob(j);
}

// ---------------------------------------------------------------- project
namespace {
SaveJob* snapshot(bool for_save)
{
    State& s = S();
    SaveJob* sj = static_cast<SaveJob*>(bigAlloc(sizeof(SaveJob)));
    if (!sj) return nullptr;
    sj->proj = *s.project;
    if (s.master_muted) sj->proj.g[GP_MASTER] = s.master_unmute_db;
    sj->gen = s.dirty_gen;
    sj->nwrite = 0;
    for (int p = 0; p < kPads; ++p)
        for (int l = 0; l < kLayers; ++l) {
            const int sl = s.project->pad[p].layer[l].slot;
            sj->refs[p][l] = (sl >= 0 && s.slots[sl].s) ? s.slots[sl].ref : SampleRef{};
        }
    if (!for_save) return sj;
    copyStr(sj->path, sizeof sj->path, "");
    // Samples edited in this project are written as WAV into its folder before the JSON.
    for (int sl = 0; sl < kMaxSamples; ++sl) {
        SlotInfo& si = s.slots[sl];
        if (!si.s || !si.refs || !si.edited || si.s->embedded) continue;
        int fp = -1, fl = -1;
        for (int p = 0; p < kPads && fp < 0; ++p)
            for (int l = 0; l < kLayers; ++l)
                if (s.project->pad[p].layer[l].slot == sl) {
                    fp = p, fl = l;
                    break;
                }
        if (fp < 0) continue;   // only an undo entry uses it
        SaveWrite& w = sj->w[sj->nwrite++];
        w.s = si.s;
        w.slot = (int16_t)sl;
        w.id = si.s->id;
        w.ok = 0;
        char lbl[8], nm[kNameLen];
        padLabel(fp, lbl, sizeof lbl);
        sanitizeName(nm, sizeof nm, si.ref.name[0] ? si.ref.name : si.s->name);
        for (char* c = nm; *c; ++c)
            if (*c == ' ') *c = '_';
        w.path[0] = 0;   // the directory is prefixed by the caller
        sfmt(w.path, sizeof w.path, "%s_L%d_%s_%u.wav", lbl, fl + 1, nm, (unsigned)(w.id % 100000u));
        regHold(si.s, +1);
    }
    return sj;
}

void releaseSaveJob(SaveJob* sj)
{
    for (int i = 0; i < sj->nwrite; ++i) {
        Sample* held = const_cast<Sample*>(sj->w[i].s);
        regHold(held, -1);
        maybeFree(held);
    }
    bigFree(sj);
}

// Save job for <name> (sanitized; the project takes that name). nullptr when it cannot be built.
SaveJob* prepareSave(const char* name)
{
    State& s = S();
    char clean[32];
    sanitizeName(clean, sizeof clean, name ? name : P().name);
    if (strcmp(clean, P().name) != 0) {
        copyStr(P().name, sizeof P().name, clean);
        ++s.revs.project;
    }
    SaveJob* sj = snapshot(true);
    if (!sj) return nullptr;
    const bool okp = front().projectPath(clean, sj->path, sizeof sj->path);
    const bool okd = front().projectDir(clean, sj->dir, sizeof sj->dir);
    if (!okp || !okd) {
        releaseSaveJob(sj);
        say("Błędna ścieżka projektu");
        return nullptr;
    }
    // Strip a trailing separator, then prefix the WAV names with the project folder.
    int dl = (int)strlen(sj->dir);
    while (dl > 0 && (sj->dir[dl - 1] == '/' || sj->dir[dl - 1] == '\\')) sj->dir[--dl] = 0;
    for (int i = 0; i < sj->nwrite; ++i) {
        char file[kPathLen];
        copyStr(file, sizeof file, sj->w[i].path);
        sfmt(sj->w[i].path, sizeof sj->w[i].path, "%s/%s", sj->dir, file);
    }
    return sj;
}

// Saves on the calling (LVGL) task, for exitToLauncher(): the loader may not finish before a restart.
bool saveBlocking()
{
    State& s = S();
    SaveJob* sj = prepareSave(P().name);
    if (!sj) return false;
    s.save_busy = true;
    runSave(*sj, false);
    const bool ok = sj->ok == 1;   // 2 = partial (an edited sample was not written)
    Result r;
    r.job.type = JobType::Save;
    r.job.payload = sj;
    r.job.flag = 0;
    applySave(r, false);   // slots, saved generation, toast; frees sj
    s.last_save_us = nowUs();
    return ok;
}
}  // namespace

bool App::save() { return saveAs(P().name); }

bool App::saveAs(const char* name)
{
    State& s = S();
    if (!s.sd) {
        say("Brak karty SD");
        return false;
    }
    if (s.save_busy) {
        say("Zapis w toku");
        return false;
    }
    // <name>.orb of another project (not opened / saved in this session): the second SAVE within 3 s
    // overwrites it; the autosave never does.
    char clean[32];
    sanitizeName(clean, sizeof clean, name ? name : P().name);
    if (!ownsFile(clean) && projectFileExists(clean)) {
        if (s.autosave_next) return false;
        char key[48];
        sfmt(key, sizeof key, "save:%s", clean);
        if (!confirmTwice(key)) {
            sayf("Projekt %s już istnieje - zapisz ponownie, aby go nadpisać", clean);
            return false;
        }
    }
    SaveJob* sj = prepareSave(name);
    if (!sj) return false;
    Job j;
    j.type = JobType::Save;
    j.payload = sj;
    j.flag = s.autosave_next ? 1 : 0;
    if (!postJob(j)) {
        releaseSaveJob(sj);
        return false;
    }
    s.save_busy = true;
    s.last_save_us = nowUs();
    return true;
}

bool App::open(const char* name)
{
    State& s = S();
    if (!s.sd || !name || !name[0]) return false;
    LoadJob* lj = static_cast<LoadJob*>(bigAlloc(sizeof(LoadJob)));
    if (!lj) return false;
    if (!front().projectPath(name, lj->path, sizeof lj->path)) {
        bigFree(lj);
        return false;
    }
    copyStr(lj->name, sizeof lj->name, name);
    Job j;
    j.type = JobType::Load;
    j.payload = lj;
    if (!postJob(j)) {
        bigFree(lj);
        return false;
    }
    return true;
}

void App::newProject()
{
    State& s = S();
    postSimple(CmdType::Stop);   // pauses while playing; the second Stop below rewinds
    postSimple(CmdType::Panic);
    s.playing = false;
    if (s.recording) {
        s.recording = false;
        postSimple(CmdType::SetRecord, 0);
    }
    releaseAllPads();
    s.master_muted = false;
    s.pat_undo_scene = -1;   // the pattern undo belonged to the previous project
    projectEmpty(*s.project);   // no demo: every step off, empty song (the boot project keeps the demo)
    s.project->limiter_on = s.settings.master_limiter;
    // First free ORBIT_NN name on the card (checked per file, not against a listing that may be cut short).
    s.file_name[0] = 0;
    s.autosave_warned = false;
    if (s.sd) pickFreeName(s.project->name, (int)sizeof s.project->name);
    const bool was = s.inited;
    s.inited = false;   // no "dirty" / toast from the kit load
    if (builtinKitCount() > 1) loadBuiltinKit(1);
    else if (builtinKitCount() > 0) loadBuiltinKit(0);
    s.inited = was;
    postSimple(CmdType::Stop);   // rewind with the new project in place: PLAY starts at the top
    postSimple(CmdType::QueueScene, 0, 0);
    postSimple(CmdType::Metronome, 0);
    s.ui.pad = 0;
    s.ui.layer = 0;
    s.ui.seq_page = 0;
    s.ui.pad_shown = 0;   // nothing looks selected until a pad is touched
    eng().telemetry().watch_pad.store(0, std::memory_order_relaxed);
    s.saved_gen = s.dirty_gen;
    s.last_save_us = nowUs();
    bumpAll();
    sayf("Nowy projekt %s", s.project->name);
}

bool App::exportJson()
{
    State& s = S();
    if (!s.sd) {
        say("Brak karty SD");
        return false;
    }
    SaveJob* sj = snapshot(false);
    if (!sj) return false;
    char proj[kPathLen];
    if (!front().projectPath(P().name, proj, sizeof proj)) {
        bigFree(sj);
        return false;
    }
    // <PROJECTS>/<name>.orb -> <PROJECTS>/<name>.json
    char* dot = strrchr(proj, '.');
    if (dot && !strchr(dot, '/')) *dot = 0;
    sfmt(sj->path, sizeof sj->path, "%s.json", proj);
    Job j;
    j.type = JobType::Export;
    j.payload = sj;
    if (!postJob(j)) {
        bigFree(sj);
        return false;
    }
    return true;
}

int App::listProjects(char names[][32], int max)
{
    if (!S().sd || !names || max <= 0) return 0;
    return front().listProjects(names, max);
}

bool App::deleteProject(const char* name)
{
    State& s = S();
    if (!s.sd || !name || !name[0]) {
        say("Brak karty SD");
        return false;
    }
    if (s.save_busy) {
        say("Poczekaj, trwa zapis projektu");
        return false;
    }
    char path[kPathLen];
    if (!front().projectPath(name, path, sizeof path)) return false;
    if (remove(path) != 0) {
        sayf("Nie udało się usunąć projektu %s", name);
        return false;
    }
    // EXPORT JSON wrote <PROJECTS>/<name>.json next to it, SAVE the project's own samples into <PROJECTS>/<name>/:
    // they go too.
    char* dot = strrchr(path, '.');
    if (dot && !strchr(dot, '/')) {
        *dot = 0;
        char js[kPathLen + 8];
        sfmt(js, sizeof js, "%s.json", path);
        remove(js);
    }
    char dir[kPathLen];
    if (front().projectDir(name, dir, sizeof dir)) {
        const int n = (int)strlen(dir);
        if (n > 1 && (dir[n - 1] == '/' || dir[n - 1] == '\\')) dir[n - 1] = 0;
        removeDirFiles(dir);
    }
    if (ownsFile(name)) {   // the open project was that file: it stays open, unsaved
        s.file_name[0] = 0;
        markDirty();
    }
    ++s.revs.project;
    sayf("Usunięto projekt %s", name);
    return true;
}

bool App::dirty() const { return S().dirty_gen != S().saved_gen; }

// ---------------------------------------------------------------- settings
namespace {
// Only while the audio stream is stopped (the UI task stands in for the audio task): applies
// every queued command, silences all voices and handles the resulting retire / preview events,
// so no Sample handed to the engine is still pending when it is re-initialised.
void drainEngine()
{
    State& s = S();
    postSimple(CmdType::StopPreview);
    postSimple(CmdType::Panic);
    int16_t scratch[2 * 128];
    for (int i = 0; i < 64 && (i < 8 || s.defer_count > 0); ++i) {   // >= 8 blocks: Panic fades <= 5 ms
        flushDeferred();
        eng().processI16(scratch, 128);
    }
    Event e;
    while (eng().poll(e))
        if (e.type == EvtType::SampleRetired || e.type == EvtType::PreviewDone) engineAnswered(e);
    s.defer_count = 0;
    s.defer_head = 0;
}

// Rate change: stop the stream, re-create the engine at the new rate, give it every slot again, restart the
// stream (block frames per DMA block). false: the engine cannot run at new_rate and stays at the old one.
// MIDI posts stop meanwhile: the caller runs midiSync() afterwards.
bool restartEngine(int new_rate, int block)
{
    State& s = S();
    const bool was = hal::audioRunning();
    if (was) hal::audioStop();
    s.midi.on.store(0, std::memory_order_release);   // no MIDI posts while the engine is re-created
    drainEngine();
    const bool ok = eng().init(new_rate, s.project);
    if (ok) {
        s.rate = new_rate;
    } else {
        eng().init(s.rate, s.project);
        say("Nie można zmienić częstotliwości");
    }
    // drainEngine() answered every retirement, so the fresh engine owes nothing for those; every slot
    // (waiting ones too: their retirement is answered now) is installed again and counted once.
    for (int i = 0; i < kRegMax; ++i) s.reg[i].engine = 0;
    for (int sl = 0; sl < kMaxSamples; ++sl) {
        SlotInfo& si = s.slots[sl];
        si.wait = 0;
        if (!si.s) continue;
        postRaw(sl, si.s);
        regEngine(si.s, +1);
    }
    s.any_wait = false;
    for (int i = 0; i < kRegMax; ++i) maybeFree(s.reg[i].s);
    postSimple(CmdType::QueueScene, 0, P().scene);
    if (P().metronome) postSimple(CmdType::Metronome, 1);
    // The engine keeps its REC arm and FILL across init: line them up with what the UI shows.
    postSimple(CmdType::SetRecord, 0);
    postSimple(CmdType::SetFill, s.ui.fill ? 1 : 0);
    s.playing = false;
    s.recording = false;
    if (was && !hal::audioStart((uint32_t)s.rate, block, &App::render, &eng())) say("Błąd uruchomienia audio");
    ++s.revs.transport;
    return ok;
}
}  // namespace

Settings& App::settingsEdit() { return S().settings_edit; }

void App::settingsDiscard() { S().settings_edit = S().settings; }

void App::applySettings()
{
    State& s = S();
    Settings n = s.settings_edit;
    const Settings old = s.settings;
    n.brightness = (uint8_t)clampi(n.brightness, 10, 100);
    // Output levels belong to the VOLUME knob (setOutputVolume, saved by itself): the edit copy never changes
    // them; `volume` is only the pre-0.3 level the memory was seeded from.
    n.volume = old.volume;
    n.vol_speaker = old.vol_speaker;
    n.vol_headphones = old.vol_headphones;
    n.vol_init = old.vol_init;
    n.resume_project[0] = 0;          // only restartApp() sets it, right before it restarts
    sanitizeColors(n);
    if (n.sample_rate != (uint32_t)kRate44 && n.sample_rate != (uint32_t)kRate48) n.sample_rate = kRate48;
    if (n.buffer_frames != 64 && n.buffer_frames != 128 && n.buffer_frames != 256) n.buffer_frames = 128;
    n.knob_sensitivity = (uint8_t)clampi(n.knob_sensitivity, 1, 100);
    n.fixed_velocity = (uint8_t)clampi(n.fixed_velocity, 1, 127);
    n.midi_channel = (uint8_t)clampi(n.midi_channel, 0, 16);

    if (n.brightness != old.brightness) hal::setBrightness(n.brightness);
    if (n.output != old.output) hal::audioSetOutput(n.output);   // volTick() then applies that output's level
    n.voice_limit = (uint8_t)clampi(n.voice_limit, Engine::kMinVoiceLimit, kVoices);
    if (n.voice_limit != old.voice_limit) eng().setVoiceLimit(n.voice_limit);   // any task; voices above fade out
    if (n.low_power != old.low_power) hal::setFrameCap(n.low_power ? 30 : 60);
    if (n.master_limiter != old.master_limiter) {
        P().limiter_on = n.master_limiter;
        ++s.revs.fx;
        ++s.revs.mixer;
    }
    // Bluetooth fields belong to the bt* methods (applied and saved at once): the edit copy never changes them.
    n.bt_enabled = old.bt_enabled;
    n.bt_local = old.bt_local;
    n.bt_volume = old.bt_volume;
    // While Bluetooth holds the engine at 44.1 kHz a new rate is only remembered: it applies when Bluetooth goes off.
    if (s.bt_on && n.sample_rate != old.sample_rate) s.bt_prev_rate = (int)n.sample_rate;
    const bool rate_later = s.bt_on && n.sample_rate != old.sample_rate && (int)n.sample_rate != s.rate;
    if (n.sample_rate != old.sample_rate && !s.bt_on) {
        if (!restartEngine((int)n.sample_rate, n.buffer_frames)) n.sample_rate = old.sample_rate;
    } else if (n.buffer_frames != old.buffer_frames && hal::audioRunning()) {
        // Block size only: restart the stream, the engine keeps running state.
        hal::audioStop();
        if (!hal::audioStart((uint32_t)s.rate, n.buffer_frames, &App::render, &eng())) say("Błąd uruchomienia audio");
    }
    s.settings = n;
    s.settings_edit = n;
    midiSync();
    if (!hal::settingsSave(n)) say("Nie udało się zapisać ustawień");
    else say(rate_later ? "Zapisano ustawienia \xE2\x80\x94 częstotliwość zmieni się po wyłączeniu Bluetooth"
                        : "Zapisano ustawienia");
    ++s.revs.settings;
}

// ---------------------------------------------------------------- status
bool App::sdReady() const { return S().sd; }
const char* App::sdStatus() const { return S().sd_status[0] ? S().sd_status : "Brak karty SD"; }
float App::cpuLoad() const { return S().cpu; }
int App::activeVoices() const { return S().voices; }
float App::latencyMs() const { return S().latency; }

void App::toast(const char* fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    say(buf);
}

// ---------------------------------------------------------------- additions, wave 2
void App::renameProject(const char* name)
{
    State& s = S();
    if (!name) return;
    while (*name == ' ') ++name;
    if (!name[0]) {
        say("Nazwa projektu nie może być pusta");
        return;
    }
    char clean[32];
    sanitizeName(clean, sizeof clean, name);
    if (strcmp(clean, P().name) == 0) return;
    copyStr(P().name, sizeof P().name, clean);
    ++s.revs.project;
    markDirty();
    char path[kPathLen];
    struct stat st;
    if (s.sd && front().projectPath(clean, path, sizeof path) && stat(path, &st) == 0)
        sayf("Projekt %s już istnieje - SAVE go nadpisze", clean);
    else
        sayf("Nazwa projektu: %s", clean);
}

void App::setLaunchQuant(LaunchQuant q)
{
    if ((int)q >= (int)LaunchQuant::Count || P().quant == q) return;
    P().quant = q;   // the engine reads it at the next queued scene switch
    ++S().revs.settings;
    ++S().revs.transport;
    markDirty();
}

void App::undoPattern()
{
    State& s = S();
    if (s.pat_undo_scene < 0 || s.pat_undo_scene >= kScenes) {
        say("Brak zmiany wzoru do cofnięcia");
        return;
    }
    const int sc = s.pat_undo_scene;
    P().scene_data[sc] = s.pat_undo;
    s.pat_undo_scene = -1;
    if (sc == P().scene) {
        const int pages = (P().scene_data[sc].length + kStepsPerPage - 1) / kStepsPerPage;
        if (s.ui.seq_page >= pages) s.ui.seq_page = pages > 0 ? pages - 1 : 0;
    }
    patternEdited();
    sayf("Cofnięto zmianę sceny %c", (char)('A' + sc));
}

// ---------------------------------------------------------------- ACID BASS (0.3)
const BassStep& App::bassStep(int s) const
{
    static const BassStep kNone{};
    if (s < 0 || s >= kMaxSteps) return kNone;
    return P().scene_data[P().scene < kScenes ? P().scene : 0].bass.step[s];
}

void App::setBassStep(int s, const BassStep& b)
{
    if (s < 0 || s >= kMaxSteps) return;
    BassStep& d = P().scene_data[P().scene < kScenes ? P().scene : 0].bass.step[s];
    BassStep n = b;
    n.gate = n.gate ? 1 : 0;
    n.note = (int8_t)clampi(n.note, kBassNoteMin, kBassNoteMax);
    n.accent = n.accent ? 1 : 0;
    n.slide = n.slide ? 1 : 0;
    if (memcmp(&d, &n, sizeof n) == 0) return;
    d = n;
    patternEdited();
}

void App::bassAudition(int note, bool accent, bool legato)
{
    noteSound();
    postSimple(CmdType::BassNote, clampi(note, 0, 127), 0, (accent ? 1 : 0) | (legato ? 2 : 0));
}

void App::bassRelease() { postSimple(CmdType::BassOff); }

void App::bassKeyOn(int note, bool accent, bool legato, Producer from)
{
    Command c;
    c.type = CmdType::BassNote;
    c.value = clampi(note, 0, 127);
    c.vel = (uint8_t)((accent ? 1 : 0) | (legato ? 2 : 0));
    noteSound();   // before the post: no flash write may start under this note
    eng().post(from, c);
}

void App::bassKeyOff(Producer from)
{
    Command c;
    c.type = CmdType::BassOff;
    eng().post(from, c);
}

// RANDOM: notes of C minor pentatonic over two octaves around the root, ~70 % of the steps play, about a
// quarter accented, a fifth sliding; an octave jump now and then. Undoable like the drum RANDOM.
void App::bassRandom()
{
    const int sc = P().scene < kScenes ? P().scene : 0;
    patternSnapshot(sc);
    Scene& scene = P().scene_data[sc];
    static uint32_t seed = 0x1234567u;
    seed ^= (uint32_t)nowUs();
    auto rnd = [&](int n) {
        seed = seed * 1664525u + 1013904223u;
        return (int)((seed >> 8) % (uint32_t)n);
    };
    static const int8_t kScale[] = {0, 3, 5, 7, 10, 12, 15, 17, 19, 22};
    const int len = clampi(scene.length, 1, kMaxSteps);
    for (int i = 0; i < kMaxSteps; ++i) {
        BassStep& b = scene.bass.step[i];
        b = BassStep{};
        if (i >= len) continue;
        b.gate = (i % 4 == 0) || rnd(100) < 62 ? 1 : 0;
        b.note = kScale[rnd(i % 4 == 0 ? 3 : (int)(sizeof kScale))];
        if (rnd(100) < 12) b.note = (int8_t)(b.note - 12);   // a low octave now and then
        b.accent = rnd(100) < 26 ? 1 : 0;
        b.slide = b.gate && rnd(100) < 20 ? 1 : 0;
    }
    patternEdited();
    sayf("Nowa linia basu w scenie %c", (char)('A' + sc));
}

void App::bassClear()
{
    const int sc = P().scene < kScenes ? P().scene : 0;
    patternSnapshot(sc);
    P().scene_data[sc].bass = BassLine{};
    patternEdited();
    sayf("Wyczyszczono bas sceny %c", (char)('A' + sc));
}

uint32_t App::bassNotes() const { return eng().telemetry().bass_notes.load(std::memory_order_relaxed); }

bool App::canUndoPattern() const { return S().pat_undo_scene >= 0; }
uint32_t App::patternUndoGen() const { return S().pat_undo_gen; }
int App::undoPatternScene() const { return S().pat_undo_scene; }

bool App::saveSampleWav()
{
    State& s = S();
    if (!s.sd) {
        say("Brak karty SD");
        return false;
    }
    const int pad = s.ui.pad, layer = s.ui.layer;
    const int slot = P().pad[pad].layer[layer].slot;
    Sample* smp = slotSample(slot);
    if (!smp) {
        say("Brak sampla na tej warstwie");
        return false;
    }
    char nm[kNameLen];
    sanitizeName(nm, sizeof nm, s.slots[slot].ref.name[0] ? s.slots[slot].ref.name : smp->name);
    for (char* c = nm; *c; ++c)
        if (*c == ' ') *c = '_';
    Job j;
    j.type = JobType::SaveWav;
    j.pad = (int8_t)pad;
    j.layer = (int8_t)layer;
    j.index = slot;
    j.src = smp;
    j.src_id = smp->id;
    if (!front().userPath(nm, j.path, sizeof j.path)) {
        say("Błędna ścieżka USER");
        return false;
    }
    regHold(smp, +1);
    if (!postJob(j)) {
        regHold(smp, -1);
        return false;
    }
    return true;
}

bool App::hasLauncher() const { return S().launcher; }

bool App::exitToLauncher()
{
    State& s = S();
    if (!s.launcher) s.launcher = hal::launcherAvailable();
    if (!s.launcher) {
        say("Brak launchera Tab5 na tym urządzeniu");
        return false;
    }
    if (s.save_busy) {
        say("Zapis w toku - spróbuj za chwilę");
        return false;
    }
    postSimple(CmdType::Stop);
    postSimple(CmdType::StopPreview);
    postSimple(CmdType::Panic);   // every voice fades out within 5 ms
    s.playing = false;
    if (s.recording) {
        s.recording = false;
        postSimple(CmdType::SetRecord, 0);
    }
    ++s.revs.transport;
    if (dirty()) {
        // Unsaved work: saved first when Settings.autosave is on and the card is there (still in the slot,
        // and the name is not another project's file); otherwise, or when that save fails, the exit needs
        // a second tap within 3 s and then leaves without saving.
        char probe[kPathLen];
        struct stat st;
        sfmt(probe, sizeof probe, "%s/ORBIT", s.sd_root);
        const bool card = s.sd && stat(probe, &st) == 0;
        const bool foreign = !ownsFile(P().name) && projectFileExists(P().name);
        const bool can_save = s.settings.autosave && card && !foreign;
        if (!(can_save && saveBlocking())) {
            if (!confirmTwice("exit")) {
                if (can_save) say("Błąd zapisu - dotknij ponownie, aby wyjść bez zapisu");
                else if (foreign) sayf("%s już istnieje - zapisz projekt lub dotknij ponownie, aby wyjść bez zapisu", P().name);
                else say("Niezapisane zmiany - dotknij ponownie, aby wyjść bez zapisu");
                return false;
            }
        }
    }
    if (s.clock_timer) {
        s.midi.clock_out.store(0, std::memory_order_release);
        hal::serviceTimerStart(nullptr, nullptr, 0);
        s.clock_timer = false;
        if (g_clock_out.playing) midiOut(0xFC);
        g_clock_out.playing = false;
    }
    const bool audio = hal::audioRunning();
    if (audio) {
        sleepMs(20);   // let the Panic fade play out
        hal::audioStop();
    }
    const bool bt = btPark();   // Bluetooth on: the Pico gets STOP + SLEEP first
    if (hal::restartToLauncher()) return true;   // the Tab5 restarts and never gets here
    if (bt) btUnpark();
    if (audio && !hal::audioStart((uint32_t)s.rate, s.settings.buffer_frames, &App::render, &eng()))
        say("Błąd uruchomienia audio");
    midiSync();
    say("Nie udało się uruchomić launchera");
    return false;
}

bool App::restartApp()
{
    State& s = S();
    if (s.save_busy) {
        say("Zapis w toku - spróbuj za chwilę");
        return false;
    }
    postSimple(CmdType::Stop);
    postSimple(CmdType::StopPreview);
    postSimple(CmdType::Panic);
    s.playing = false;
    if (s.recording) {
        s.recording = false;
        postSimple(CmdType::SetRecord, 0);
    }
    ++s.revs.transport;
    if (dirty()) {   // as exitToLauncher(): saved first when it can be, else a second tap restarts without saving
        char probe[kPathLen];
        struct stat st;
        sfmt(probe, sizeof probe, "%s/ORBIT", s.sd_root);
        const bool card = s.sd && stat(probe, &st) == 0;
        const bool foreign = !ownsFile(P().name) && projectFileExists(P().name);
        const bool can_save = s.settings.autosave && card && !foreign;
        if (!(can_save && saveBlocking())) {
            if (!confirmTwice("restart")) {
                if (can_save) say("Błąd zapisu - dotknij ponownie, aby uruchomić ponownie bez zapisu");
                else if (foreign) sayf("%s już istnieje - zapisz projekt lub dotknij ponownie", P().name);
                else say("Niezapisane zmiany - zapisz projekt lub dotknij ponownie, aby uruchomić bez zapisu");
                return false;
            }
        }
    }
    if (s.clock_timer) {
        s.midi.clock_out.store(0, std::memory_order_release);
        hal::serviceTimerStart(nullptr, nullptr, 0);
        s.clock_timer = false;
        if (g_clock_out.playing) midiOut(0xFC);
        g_clock_out.playing = false;
    }
    if (hal::audioRunning()) {
        sleepMs(20);   // let the Panic fade play out
        hal::audioStop();
    }
    // The project to open again after the start: only one whose file is on the card. Saved with the audio
    // stopped, so the NVS mirror is written now too (no card: NVS is the only copy of the new theme).
    copyStr(s.settings.resume_project, sizeof s.settings.resume_project, ownsFile(P().name) ? P().name : "");
    s.settings_edit.resume_project[0] = 0;
    hal::settingsSave(s.settings);
    btPark();
    hal::restart();   // does not return on the Tab5 (the host only records it)
    return false;
}

void App::midiLearn(bool on)
{
    State& s = S();
    if (s.learn == on) return;
    s.learn = on;
    s.learn_armed = false;
    s.learn_us = hal::micros();
    ++s.revs.midi;
    ++s.revs.params;
    if (!on) sayf("MIDI LEARN wyłączony (przypisania: %d)", s.map_count);
    else if (!s.settings.midi_usb) say("MIDI LEARN: najpierw włącz USB MIDI w ustawieniach");
    else say("MIDI LEARN: rusz pokrętłem, potem kontrolerem MIDI");
}

bool App::midiLearning() const { return S().learn; }

int App::midiMapCount() const { return S().map_count; }

void App::midiClearMap()
{
    State& s = S();
    s.map_count = 0;
    s.learn_armed = false;
    midiMapSave();
    ++s.revs.midi;
    say("Usunięto przypisania MIDI");
}

bool App::midiConnected() const { return S().midi_conn; }

const char* App::midiDeviceName() const { return S().midi_conn ? hal::midiDeviceName() : ""; }

float App::midiClockBpm() const { return S().ext_bpm; }

// ---------------------------------------------------------------- additions, review fixes
bool App::canUndoPads() const { return S().pad_undo.valid != 0; }

const char* App::undoPadsLabel() const { return S().pad_undo.valid ? S().pad_undo.what : ""; }

bool App::undoPads()
{
    State& s = S();
    PadUndo& u = s.pad_undo;
    if (!u.valid) {
        say("Brak zmiany padów do cofnięcia");
        return false;
    }
    u.valid = 0;
    for (int i = 0; i < kPads; ++i) {
        if (!(u.mask & (1u << i))) continue;
        undoDropPad(i);
        for (int l = 0; l < kLayers; ++l) setLayerSlot(i, l, -1);
        P().pad[i] = u.pad[i];   // the snapshot's slot references move to the pad
        for (int l = 0; l < kLayers; ++l) u.pad[i].layer[l].slot = -1;
    }
    u.mask = 0;
    sayf("Cofnięto: %s", u.what);
    ++s.revs.samples;
    ++s.revs.pads;
    ++s.revs.params;
    ++s.revs.mixer;   // routing (bus) of the restored pads
    markDirty();
    return true;
}

bool App::projectHasFile() const { return ownsFile(P().name); }

// ---------------------------------------------------------------- Bluetooth (Aurora Pico bridge, hal::bt*)
// LVGL-task code only, built for size (the 4 MB image limit).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("Os")
#endif
namespace {
const hal::BtDevice kNoBtDevice = {};

// bt_* are changed outside APPLY: the dialog's edit copy follows, the settings file is written now.
void btSaveNow()
{
    State& s = S();
    s.bt_save = false;
    s.vol_save = false;   // one settings file: waiting output levels are written with it
    s.settings_edit.vol_speaker = s.settings.vol_speaker;
    s.settings_edit.vol_headphones = s.settings.vol_headphones;
    s.settings_edit.bt_enabled = s.settings.bt_enabled;
    s.settings_edit.bt_local = s.settings.bt_local;
    s.settings_edit.bt_volume = s.settings.bt_volume;
    if (!hal::settingsSave(s.settings)) say("Nie udało się zapisać ustawień");
}

// Bridge on: start it, the engine to 44.1 kHz (the Pico takes 16-bit stereo at 44.1 kHz), volume, audio route.
// Off: route back to the codec, bridge off, the earlier engine rate again. false: the bridge did not start.
bool btApply(bool on, bool quiet)
{
    State& s = S();
    if (on == s.bt_on) return true;
    const int block = s.settings.buffer_frames;
    bool moved = false;
    if (on) {
        if (!hal::btEnable(true)) {
            say("Bluetooth: nie można uruchomić mostka Pico");
            return false;
        }
        s.bt_on = true;
        if (s.rate != kRate44) {
            s.bt_prev_rate = s.rate;
            moved = restartEngine(kRate44, block);
            midiSync();
        }
        hal::btSetVolume(s.settings.bt_volume);
        hal::btSetAudioRoute(true, s.settings.bt_local != 0);
        if (!quiet) say(moved ? "Bluetooth włączony \xE2\x80\x94 silnik audio 44,1 kHz" : "Bluetooth włączony");
        return true;
    }
    hal::btSetAudioRoute(false, true);
    hal::btEnable(false);
    s.bt_on = false;
    s.bt_conn_toast = false;
    s.bt_dev_count = 0;   // no stale rows while off (btTick copies the list only while on)
    ++s.revs.bt;
    const int back = s.bt_prev_rate ? s.bt_prev_rate : (int)s.settings.sample_rate;
    s.bt_prev_rate = 0;
    if (back != s.rate) {
        moved = restartEngine(back, block);
        midiSync();
    }
    if (quiet) return true;
    if (moved) sayf("Bluetooth wyłączony \xE2\x80\x94 silnik audio %d kHz", s.rate / 1000);
    else say("Bluetooth wyłączony");
    return true;
}

bool btReady()
{
    if (!S().bt_on) say("Najpierw włącz Bluetooth (Settings > Bluetooth)");
    return S().bt_on;
}

// Leaving for the launcher (the Tab5 restarts): STOP + SLEEP reach the Pico before that, so it does not keep the
// headphones on a dead stream until its keepalive runs out. The App state stays (btUnpark when the exit fails).
bool btPark()
{
    if (!S().bt_on) return false;
    hal::btSetAudioRoute(false, true);
    hal::btEnable(false);
    for (int i = 0; i < 50; ++i) {   // the bridge task's next step (20 ms) queues STOP + SLEEP
        hal::BtStatus st;
        hal::btStatus(st);
        if (!st.enabled && !st.awake) break;
        sleepMs(20);
    }
    sleepMs(60);   // ... and its UART sends them (11 bytes at 115200 baud)
    return true;
}

void btUnpark()
{
    const Settings& t = S().settings;
    if (!hal::btEnable(true)) return;
    hal::btSetVolume(t.bt_volume);
    hal::btSetAudioRoute(true, t.bt_local != 0);
}

// tick(): start at boot, deferred volume save, status refresh + toasts, headphone buttons.
void btTick(uint64_t now)
{
    State& s = S();
    // At boot only once app_main's audioStart() is over (the stream runs, or audioBootDone() says it failed):
    // the rate switch then stops and restarts that stream, never races it, and the boot path stays as it was.
    if (s.bt_boot && (hal::audioRunning() || s.audio_boot_done.load(std::memory_order_acquire))) {
        s.bt_boot = false;
        if (!btApply(true, true)) {
            s.settings.bt_enabled = 0;
            btSaveNow();
        }
        ++s.revs.bt;
    }
    if (s.bt_save && hal::micros() - s.bt_save_us >= kBtSaveUs) btSaveNow();
    if (!s.bt_on && !s.bt.enabled && !s.bt.connected) return;   // off and the last status says so
    hal::BtStatus st;
    hal::btStatus(st);
    if (st.revision != s.bt.revision) {
        const bool was = s.bt.connected != 0;
        s.bt = st;
        const int n = s.bt_on ? hal::btDevices(s.bt_dev, kBtDevMax) : 0;
        s.bt_dev_count = n < 0 ? 0 : (n > kBtDevMax ? kBtDevMax : n);
        ++s.revs.bt;
        if (st.connected && !was) {
            s.bt_conn_toast = true;   // the name may follow in the next status
            s.bt_conn_us = now;
        } else if (!st.connected && was) {
            s.bt_conn_toast = false;
            // SCAN / SELECT / RECONNECT restart the Pico (the link drops on purpose): keep their hint on screen
            if (s.bt_on && hal::micros() - s.bt_quiet_us > kBtQuietUs) say("Bluetooth: rozłączono");
        }
    }
    if (s.bt_conn_toast && (s.bt.connected_name[0] || now - s.bt_conn_us > kBtNameWaitUs)) {
        s.bt_conn_toast = false;
        if (s.bt.connected_name[0]) sayf("Bluetooth: połączono z %s", s.bt.connected_name);
        else say("Bluetooth: połączono");
    }
    if (!s.bt_on) return;
    for (int i = 0; i < 8; ++i) {
        const hal::BtRemote r = hal::btPopRemote();
        if (r == hal::BtRemote::None) break;
        const int sc = P().scene;
        // PLAY resumes where PAUSE stopped, like the on-screen play button (togglePlay: Continue, not Play)
        if (r == hal::BtRemote::Play && !s.playing) app().togglePlay();
        else if (r == hal::BtRemote::Pause && s.playing) app().stop();
        else if (r == hal::BtRemote::Next) app().selectScene((sc + 1) % kScenes);
        else if (r == hal::BtRemote::Previous) app().selectScene((sc + kScenes - 1) % kScenes);
    }
}
}  // namespace

bool App::btEnabled() const { return S().settings.bt_enabled != 0; }

void App::btSetEnabled(bool on)
{
    State& s = S();
    s.bt_boot = false;
    if (!btApply(on, false)) on = false;
    if ((s.settings.bt_enabled != 0) != on) {
        s.settings.bt_enabled = on ? 1 : 0;
        btSaveNow();
    }
    ++s.revs.bt;
}

void App::btSetLocal(bool on)
{
    State& s = S();
    s.settings.bt_local = on ? 1 : 0;
    if (s.bt_on) hal::btSetAudioRoute(true, on);
    btSaveNow();
    ++s.revs.bt;
}

void App::btScan()
{
    if (!btReady() || !hal::btScan()) return;
    S().bt_quiet_us = hal::micros();
    say("Bluetooth: szukam urządzeń (20 s) \xE2\x80\x94 włącz parowanie w słuchawkach");
}

void App::btSelect(const char* mac)
{
    State& s = S();
    if (!mac || !mac[0] || !btReady()) return;
    const char* name = mac;
    for (int i = 0; i < s.bt_dev_count; ++i)
        if (strcmp(s.bt_dev[i].mac, mac) == 0 && s.bt_dev[i].name[0]) name = s.bt_dev[i].name;
    if (!hal::btSelect(mac)) return;
    s.bt_quiet_us = hal::micros();
    sayf("Bluetooth: łączę z %s\xE2\x80\xA6", name);
}

void App::btReconnect()
{
    if (!btReady() || !hal::btReconnect()) return;
    S().bt_quiet_us = hal::micros();
    say("Bluetooth: ponowne łączenie\xE2\x80\xA6");
}

void App::audioBootDone() { S().audio_boot_done.store(true, std::memory_order_release); }

void App::btTest()
{
    if (!btReady()) return;
    if (!S().bt.connected) say("Bluetooth: brak połączonego urządzenia");
    else if (hal::btTest()) say("Test Bluetooth: lewy kanał, cisza, prawy kanał");
}

void App::btSetVolume(int pct)
{
    State& s = S();
    pct = clampi(pct, 0, 100);
    if (s.settings.bt_volume == (uint8_t)pct) return;
    s.settings.bt_volume = s.settings_edit.bt_volume = (uint8_t)pct;
    if (s.bt_on) hal::btSetVolume(pct);
    s.bt_save = true;   // a slider drag writes the file once, after it
    s.bt_save_us = hal::micros();
}

void App::btAirpodsMode(int mode)
{
    if (mode >= 1 && mode <= 4 && btReady()) hal::btAirpodsMode(mode);
}

const hal::BtStatus& App::btStatus() const { return S().bt; }
int App::btDeviceCount() const { return S().bt_dev_count; }
const hal::BtDevice& App::btDevice(int i) const
{
    return i >= 0 && i < S().bt_dev_count ? S().bt_dev[i] : kNoBtDevice;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif

// ---------------------------------------------------------------- output volume memory (the VOLUME knob)
namespace {
constexpr uint64_t kVolSaveUs = 1500000ull;   // levels are saved this long after the last change

// The local output (speaker or jack), whatever Bluetooth does: it keeps playing with "Also speaker / jack".
OutKind localKind()
{
    const Settings& t = S().settings;
    if (t.output == AudioOut::Speaker) return OutKind::Speaker;
    if (t.output == AudioOut::Headphones) return OutKind::Headphones;
    return hal::headphonesInserted() ? OutKind::Headphones : OutKind::Speaker;
}

// The codec volume follows the local output's own level (sent only when it changes).
void applyLocalVolume()
{
    State& s = S();
    const int v = localKind() == OutKind::Headphones ? s.settings.vol_headphones : s.settings.vol_speaker;
    if (v == s.local_vol) return;
    s.local_vol = v;
    hal::audioSetVolume(v);
}

void volSaveNow()
{
    State& s = S();
    s.vol_save = false;
    s.bt_save = false;   // one settings file: a waiting Bluetooth volume is written with it
    s.settings_edit.vol_speaker = s.settings.vol_speaker;
    s.settings_edit.vol_headphones = s.settings.vol_headphones;
    s.settings_edit.bt_volume = s.settings.bt_volume;
    if (!hal::settingsSave(s.settings)) say("Nie udało się zapisać ustawień");
}

// tick(): a new output (jack plugged / unplugged, Bluetooth headphones connected / gone, Settings > Audio >
// Output) gets its own level at once; deferred save of the levels.
void volTick()
{
    State& s = S();
    applyLocalVolume();
    const OutKind k = app().outputKind();
    if (k != s.out_kind) {
        // Speaker <-> jack is said here; Bluetooth connect / disconnect has its own toast (btTick).
        const bool say_it = s.out_kind != OutKind::Count && s.out_kind != OutKind::Bluetooth && k != OutKind::Bluetooth;
        s.out_kind = k;
        if (k == OutKind::Bluetooth) hal::btSetVolume(s.settings.bt_volume);
        ++s.revs.params;   // the VOLUME knob shows the new output's level
        if (say_it) sayf("Głośność: %s %d%%", App::outputName(k), app().outputVolume(k));
    }
    if (s.vol_save && hal::micros() - s.vol_save_us >= kVolSaveUs) volSaveNow();
}
}  // namespace

OutKind App::outputKind() const
{
    const State& s = S();
    if (s.bt_on && s.bt.connected) return OutKind::Bluetooth;
    return localKind();
}

int App::outputVolume(OutKind k) const
{
    const Settings& t = S().settings;
    switch (k) {
    case OutKind::Headphones: return t.vol_headphones;
    case OutKind::Bluetooth: return t.bt_volume;
    default: return t.vol_speaker;
    }
}

void App::setOutputVolume(int pct)
{
    State& s = S();
    pct = clampi(pct, 0, 100);
    const OutKind k = outputKind();
    if (outputVolume(k) == pct) return;
    if (k == OutKind::Bluetooth) {
        btSetVolume(pct);   // the Pico's volume, saved by btTick
    } else {
        (k == OutKind::Headphones ? s.settings.vol_headphones : s.settings.vol_speaker) = (uint8_t)pct;
        applyLocalVolume();
        s.vol_save = true;
        s.vol_save_us = hal::micros();
    }
    ++s.revs.params;
}

const char* App::outputName(OutKind k)
{
    switch (k) {
    case OutKind::Headphones: return "słuchawki";
    case OutKind::Bluetooth: return "słuchawki Bluetooth";
    default: return "głośnik";
    }
}

const char* App::outputTag(OutKind k)
{
    switch (k) {
    case OutKind::Headphones: return "HP";
    case OutKind::Bluetooth: return "BT";
    default: return "SPK";
    }
}

// ---------------------------------------------------------------- host-test introspection (not in app.h)
namespace apphooks {
// Registry counters of a heap sample; false when it is not registered (freed, or never adopted).
bool appSampleState(const Sample* p, int* engine, int* preview, int* holds);
bool appSampleState(const Sample* p, int* engine, int* preview, int* holds)
{
    const int i = regFind(p);
    if (i < 0) return false;
    const Owned& o = S().reg[i];
    if (engine) *engine = o.engine;
    if (preview) *preview = o.pv;
    if (holds) *holds = o.holds;
    return true;
}
}  // namespace apphooks

}  // namespace orbit::ui
