// hal.h for the PC (host tests, screenshot renderer): headless LVGL display, no audio device
// (tests pull audio through halHostRenderAudio), a folder as the SD card, settings in a file, a
// simulated MIDI port, launcher and service timer driven by the test, Bluetooth through the real
// protocol state machine (hal/bt_protocol.h) and a fake Pico. See host/hal_host.h for the helpers.
#include "hal_host.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>
#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

#ifndef HAL_HOST_NO_LVGL
#include "lvgl_host.h"
#endif
#include "hal/bt_protocol.h"

namespace {

// ------------------------------------------------------------------ paths
std::string g_root;          // repository root
std::string g_sd;            // SD folder ("" = no card)
std::string g_settings;      // settings file
bool g_paths = false;        // defaults resolved
bool g_sd_mounted = false;

bool isDir(const char* p)
{
    struct stat st;
    return p && p[0] && stat(p, &st) == 0 && (st.st_mode & S_IFDIR) != 0;
}

void mkdir1(const std::string& p)
{
#if defined(_WIN32)
    _mkdir(p.c_str());
#else
    mkdir(p.c_str(), 0775);
#endif
}

// mkdir -p (ignores errors; the caller checks the result with isDir / fopen).
void mkdirs(const std::string& path)
{
    for (size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/' || path[i] == '\\') {
            const std::string part = path.substr(0, i);
            if (part.size() == 2 && part[1] == ':') continue;   // "C:"
            if (!isDir(part.c_str())) mkdir1(part);
        }
}

std::string parentDir(const std::string& path)
{
    const size_t k = path.find_last_of("/\\");
    return k == std::string::npos ? std::string() : path.substr(0, k);
}

void resolvePaths()
{
    if (g_paths) return;
    g_paths = true;
    if (const char* r = getenv("ORBIT_ROOT"); r && r[0]) {
        g_root = r;
    } else {
        char buf[1024] = {};
#if defined(_WIN32)
        if (_getcwd(buf, sizeof buf)) g_root = buf;
#else
        if (getcwd(buf, sizeof buf)) g_root = buf;
#endif
        if (g_root.empty()) g_root = ".";
    }
    for (char& c : g_root)
        if (c == '\\') c = '/';
    if (const char* sd = getenv("ORBIT_SDCARD"); sd && sd[0]) g_sd = sd;
    else g_sd = g_root + "/sdcard";
    g_settings = g_root + "/host/out/settings.bin";
}

// ------------------------------------------------------------------ display / touch / MIDI
std::recursive_timed_mutex g_lv;
int g_brightness = 80;
int g_fps = 60;
bool g_restart = false;

hal::TouchHook g_touch = nullptr;
void* g_touch_user = nullptr;
hal::MidiHook g_midi = nullptr;
void* g_midi_user = nullptr;
std::mutex g_midi_out_mx;
std::vector<hal::MidiMsg> g_midi_out;   // sent while "connected"
bool g_midi_connected = false;
std::string g_midi_name;
std::atomic<uint64_t> g_fake_us{0};
hal::ServiceFn g_svc = nullptr;
void* g_svc_user = nullptr;
uint32_t g_svc_period = 0;
bool g_launcher = false;
bool g_launcher_req = false;

// ------------------------------------------------------------------ audio
std::mutex g_audio;
hal::RenderFn g_render = nullptr;
void* g_render_user = nullptr;
uint32_t g_rate = 0;
int g_block = 128;
bool g_running = false;
int g_volume = 72;
orbit::AudioOut g_out = orbit::AudioOut::Auto;
hal::AudioTimingHook g_timing = nullptr;
void* g_timing_user = nullptr;
bool g_idle = true;

// ------------------------------------------------------------------ Bluetooth (fake Pico, virtual clock)
std::mutex g_bt_mx;                      // everything below except the atomics
hal::btp::Link g_bt;                     // static storage: zeroed; reset() on first use
bool g_bt_init = false;
uint32_t g_bt_now = 0;                   // ms
bool g_bt_enabled = false, g_bt_route = false, g_bt_local = true;
std::deque<std::string> g_bt_sent;       // complete command lines ORBIT sent
std::string g_bt_partial;
std::atomic<uint32_t> g_bt_rate{0};      // host audio rate (0 stopped), mirrors the Tab5 audio hook
std::atomic<bool> g_bt_tap{false};       // halHostRenderAudio copies blocks to the bridge
std::atomic<bool> g_bt_mute{false};      // ... and silences the local output (route local = false)
std::atomic<uint64_t> g_bt_frames{0};
std::atomic<uint32_t> g_bt_quiet{0};    // Inputs::quiet_ms (the bridge audio's silence; tests set it)

void btInitLocked()
{
    if (g_bt_init) return;
    g_bt.reset();
    g_bt_init = true;
}

// one step of the state machine at g_bt_now, then its output into g_bt_sent
void btPumpLocked()
{
    btInitLocked();
    hal::btp::Inputs in{g_bt_enabled, g_bt_route, g_bt_rate.load(), g_bt_quiet.load()};
    g_bt.step(g_bt_now, in);
    char buf[hal::btp::kTxMax];
    for (int n; (n = g_bt.takeTx(buf, (int)sizeof buf)) > 0;)
        for (int i = 0; i < n; ++i) {
            if (buf[i] != '\n') {
                g_bt_partial += buf[i];
                continue;
            }
            g_bt_sent.push_back(g_bt_partial);
            g_bt_partial.clear();
        }
    g_bt_tap = g_bt.streamOn();
    g_bt_mute = g_bt.streamAcked() && !g_bt_local;
}

uint64_t nowMicros()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// ------------------------------------------------------------------ settings file
constexpr uint32_t kSettingsMagic = 0x5342524Fu;   // "ORBS"
struct SettingsFile {
    uint32_t magic;
    uint32_t size;
    orbit::Settings s;
};

}  // namespace

// ==================================================================== test helpers
void halHostSetSdRoot(const char* path)
{
    resolvePaths();
    g_sd = path ? path : "";
    g_sd_mounted = false;
}

void halHostSetSettingsPath(const char* path)
{
    resolvePaths();
    g_settings = path ? path : "";
}

const char* halHostSettingsPath()
{
    resolvePaths();
    return g_settings.c_str();
}

void halHostRenderAudio(int frames, int16_t* out)
{
    if (!out || frames <= 0) return;
    std::lock_guard<std::mutex> lk(g_audio);
    if (!g_running || !g_render) {
        memset(out, 0, sizeof(int16_t) * 2 * (size_t)frames);
        return;
    }
    const int block = g_block > 0 ? g_block : 128;
    for (int done = 0; done < frames;) {
        const int n = frames - done < block ? frames - done : block;
        const uint64_t t0 = nowMicros();
        g_render(out + done * 2, n, g_render_user);
        // Bluetooth route (Tab5: audio task -> bridge I2S): counted; the local output gets silence when muted
        if (g_bt_tap) {
            g_bt_frames += (uint64_t)n;
            if (g_bt_mute) memset(out + done * 2, 0, sizeof(int16_t) * 2 * (size_t)n);
        }
        // Offline: the "block period" is the nominal duration of the block.
        if (g_timing) g_timing((uint32_t)(nowMicros() - t0), (uint32_t)((uint64_t)n * 1000000u / g_rate), g_timing_user);
        done += n;
    }
}

uint32_t halHostAudioRate() { return g_running ? g_rate : 0; }
int halHostAudioBlock() { return g_block; }
int halHostVolume() { return g_volume; }
orbit::AudioOut halHostOutput() { return g_out; }

void halHostTouch(const hal::TouchPoint* points, int count)
{
    if (g_touch) g_touch(points, count, g_touch_user);
}

static int g_excl[4] = {0, 0, -1, -1};
void halHostSetExclusive(int x1, int y1, int x2, int y2)
{
    g_excl[0] = x1;
    g_excl[1] = y1;
    g_excl[2] = x2;
    g_excl[3] = y2;
}
bool halHostExclusive(int* x1, int* y1, int* x2, int* y2)
{
    if (x1) *x1 = g_excl[0];
    if (y1) *y1 = g_excl[1];
    if (x2) *x2 = g_excl[2];
    if (y2) *y2 = g_excl[3];
    return g_excl[2] >= g_excl[0] && g_excl[3] >= g_excl[1];
}

void halHostMidi(const hal::MidiMsg& m)
{
    if (g_midi) g_midi(m, g_midi_user);
}

bool halHostAudioIdle() { return g_idle; }
int halHostBrightness() { return g_brightness; }
int halHostFrameCap() { return g_fps; }
bool halHostRestartRequested() { return g_restart; }

void halHostMidiSetConnected(bool connected, const char* name)
{
    std::lock_guard<std::mutex> lk(g_midi_out_mx);
    g_midi_connected = connected;
    g_midi_name = connected ? (name ? name : "") : "";
    if (!connected) g_midi_out.clear();
}

int halHostMidiTakeSent(hal::MidiMsg* out, int max)
{
    std::lock_guard<std::mutex> lk(g_midi_out_mx);
    int n = (int)g_midi_out.size();
    if (out) {
        if (max < 0) max = 0;
        if (n > max) n = max;
        for (int i = 0; i < n; ++i) out[i] = g_midi_out[(size_t)i];
    }
    g_midi_out.erase(g_midi_out.begin(), g_midi_out.begin() + n);
    return n;
}

void halHostSetMicros(uint64_t us) { g_fake_us.store(us); }
void halHostBtSetQuiet(uint32_t ms) { g_bt_quiet.store(ms); }

bool halHostServiceTimer()
{
    if (!g_svc) return false;
    g_svc(g_svc_user);
    return true;
}

uint32_t halHostServiceTimerPeriod() { return g_svc ? g_svc_period : 0; }

void halHostSetLauncher(bool present)
{
    g_launcher = present;
    g_launcher_req = false;
}

bool halHostLauncherRequested() { return g_launcher_req; }

void halHostBtFeed(const char* line)
{
    std::lock_guard<std::mutex> lk(g_bt_mx);
    btInitLocked();
    std::string s = line ? line : "";
    s += '\n';
    g_bt.feed((const uint8_t*)s.data(), (int)s.size(), g_bt_now);
    btPumpLocked();
}

int halHostBtTakeSent(char* buf, int len)
{
    std::lock_guard<std::mutex> lk(g_bt_mx);
    if (g_bt_sent.empty()) {
        if (buf && len > 0) buf[0] = 0;
        return 0;
    }
    const std::string s = g_bt_sent.front();
    g_bt_sent.pop_front();
    if (!buf || len <= 0) return (int)s.size();
    const int n = (int)s.size() < len - 1 ? (int)s.size() : len - 1;
    memcpy(buf, s.data(), (size_t)n);
    buf[n] = 0;
    return n;
}

void halHostBtAdvance(uint32_t ms)
{
    std::lock_guard<std::mutex> lk(g_bt_mx);
    while (ms) {
        const uint32_t d = ms < 20 ? ms : 20;
        g_bt_now += d;
        ms -= d;
        btPumpLocked();
    }
}

void halHostBtReset()
{
    std::lock_guard<std::mutex> lk(g_bt_mx);
    g_bt.reset();
    g_bt_init = true;
    g_bt_now = 0;
    g_bt_enabled = g_bt_route = false;
    g_bt_local = true;
    g_bt_sent.clear();
    g_bt_partial.clear();
    g_bt_tap = g_bt_mute = false;
    g_bt_frames = 0;
}

uint32_t halHostBtNow()
{
    std::lock_guard<std::mutex> lk(g_bt_mx);
    return g_bt_now;
}

bool halHostBtWakeLow()
{
    std::lock_guard<std::mutex> lk(g_bt_mx);
    return g_bt_init && g_bt.wakeLow();
}

uint64_t halHostBtBridgeFrames() { return g_bt_frames; }

// ==================================================================== hal.h
namespace hal {

// ---------------------------------------------------------------- board
bool boardInit()
{
    resolvePaths();
    return true;
}

const char* boardInfo() { return "PC host (no Tab5 hardware)"; }
uint32_t freeInternal() { return 512u * 1024u; }
uint32_t freePsram() { return 24u * 1024u * 1024u; }
int batteryPercent() { return -1; }
bool charging() { return false; }

// ---------------------------------------------------------------- display + LVGL
bool displayWaitReady(uint32_t) { return true; }

lv_display_t* displayStart()
{
#ifndef HAL_HOST_NO_LVGL
    return lvhost_init(1280, 720);
#else
    return nullptr;
#endif
}

bool displayReserveBuffers() { return true; }   // nothing to reserve: the host display renders into its own buffer
void displayReveal() {}                          // no backlight

bool lvLock(uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        g_lv.lock();
        return true;
    }
    return g_lv.try_lock_for(std::chrono::milliseconds(timeout_ms));
}

void lvUnlock() { g_lv.unlock(); }

void setBrightness(int percent) { g_brightness = percent < 0 ? 0 : (percent > 100 ? 100 : percent); }
void setFrameCap(int fps) { g_fps = fps > 0 ? fps : 60; }

// ---------------------------------------------------------------- touch
void setTouchHook(TouchHook hook, void* user)
{
    g_touch = hook;
    g_touch_user = user;
}

int touchRate() { return 120; }
void setTouchExclusive(int x1, int y1, int x2, int y2) { halHostSetExclusive(x1, y1, x2, y2); }
void touchInject(const TouchPoint* points, int count) { halHostTouch(points, count); }   // (no touch task here)
bool displayReadBack(int, void (*)(const uint16_t*, int, int, void*), void*) { return false; }
// keyboard: no device on the PC; tests inject keys (kbdInject) and the UI tick takes them
static std::deque<KeyEvent> g_keys;
void kbdStart() {}
bool kbdPresent() { return false; }
bool kbdGet(KeyEvent& out)
{
    if (g_keys.empty()) return false;
    out = g_keys.front();
    g_keys.pop_front();
    return true;
}
void kbdInject(const KeyEvent& e) { g_keys.push_back(e); }

// ---------------------------------------------------------------- audio out
bool audioStart(uint32_t sample_rate, int block_frames, RenderFn render, void* user)
{
    if (!render || sample_rate == 0 || block_frames <= 0) return false;
    std::lock_guard<std::mutex> lk(g_audio);
    g_render = render;
    g_render_user = user;
    g_rate = sample_rate;
    g_block = block_frames;
    g_running = true;
    g_bt_rate = sample_rate;
    return true;
}

void audioStop()
{
    std::lock_guard<std::mutex> lk(g_audio);
    g_running = false;
    g_bt_rate = 0;
}

bool audioRunning() { return g_running; }
void audioSetVolume(int percent) { g_volume = percent < 0 ? 0 : (percent > 100 ? 100 : percent); }
void audioSetOutput(orbit::AudioOut out) { g_out = out; }
bool headphonesInserted() { return false; }

float audioLatencyMs()
{
    if (!g_running || !g_rate) return 0.0f;
    return 2.0f * (float)g_block * 1000.0f / (float)g_rate;   // two DMA blocks in flight
}

void audioSetTimingHook(AudioTimingHook fn, void* user)
{
    std::lock_guard<std::mutex> lk(g_audio);
    g_timing = fn;
    g_timing_user = user;
}

uint32_t audioXruns() { return 0; }   // offline rendering never underruns

// ---------------------------------------------------------------- diagnostics: nothing is measured on the PC
void diagnostics(Diagnostics& d)
{
    d = Diagnostics{};
    d.ioe_inputs = -1;
}
void diagnosticsReset() {}

// ---------------------------------------------------------------- storage
bool sdMount()
{
    resolvePaths();
    g_sd_mounted = false;
    if (g_sd.empty()) return false;
    if (!isDir(g_sd.c_str())) mkdirs(g_sd);
    g_sd_mounted = isDir(g_sd.c_str());
    return g_sd_mounted;
}

bool sdMounted() { return g_sd_mounted; }

const char* sdRoot()
{
    resolvePaths();
    return g_sd.c_str();
}

// ---------------------------------------------------------------- settings
bool settingsLoad(orbit::Settings& s)
{
    resolvePaths();
    orbit::settingsDefaults(s);
    if (g_settings.empty()) return false;
    FILE* f = fopen(g_settings.c_str(), "rb");
    if (!f) return false;
    SettingsFile file;
    const size_t got = fread(&file, 1, sizeof file, f);
    fclose(f);
    if (got != sizeof file || file.magic != kSettingsMagic || file.size != sizeof(orbit::Settings)) return false;
    orbit::Settings def;
    orbit::settingsDefaults(def);
    if (file.s.version != def.version) return false;
    s = file.s;
    return true;
}

bool settingsSave(const orbit::Settings& s)
{
    resolvePaths();
    if (g_settings.empty()) return false;
    const std::string dir = parentDir(g_settings);
    if (!dir.empty() && !isDir(dir.c_str())) mkdirs(dir);
    SettingsFile file;
    memset(&file, 0, sizeof file);
    file.magic = kSettingsMagic;
    file.size = sizeof(orbit::Settings);
    file.s = s;
    FILE* f = fopen(g_settings.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(&file, 1, sizeof file, f) == sizeof file;
    return fclose(f) == 0 && ok;
}

void setAudioIdle(bool idle) { g_idle = idle; }   // no flash on the PC: only remembered

// ---------------------------------------------------------------- MIDI (none on the PC)
bool midiStart(MidiHook hook, void* user)
{
    g_midi = hook;
    g_midi_user = user;
    return true;   // the hook is stored; halHostMidi() injects messages
}

bool midiConnected()
{
    std::lock_guard<std::mutex> lk(g_midi_out_mx);
    return g_midi_connected;
}

const char* midiDeviceName()
{
    std::lock_guard<std::mutex> lk(g_midi_out_mx);
    return g_midi_connected ? g_midi_name.c_str() : "";
}

bool midiSend(const MidiMsg& m)
{
    std::lock_guard<std::mutex> lk(g_midi_out_mx);
    if (!g_midi_connected) return false;
    if (g_midi_out.size() >= 65536) return false;   // like the device ring: full = dropped
    g_midi_out.push_back(m);
    return true;
}

// ---------------------------------------------------------------- misc
uint64_t micros()
{
    const uint64_t fake = g_fake_us.load();
    return fake ? fake : nowMicros();
}

void restart()
{
    g_restart = true;
    fprintf(stderr, "[hal] restart requested\n");
}

// ---------------------------------------------------------------- Tab5 launcher (simulated)
bool launcherAvailable() { return g_launcher; }

bool restartToLauncher()
{
    if (!g_launcher) return false;
    g_launcher_req = true;
    fprintf(stderr, "[hal] restart into the launcher requested\n");
    return true;
}

// ---------------------------------------------------------------- Bluetooth (fake Pico: halHostBtFeed / TakeSent / Advance)
bool btEnable(bool on)
{
    std::lock_guard<std::mutex> lk(g_bt_mx);
    g_bt_enabled = on;
    btPumpLocked();
    return true;
}

void btStatus(BtStatus& out)
{
    std::lock_guard<std::mutex> lk(g_bt_mx);
    btInitLocked();
    out = g_bt.status();
}

int btDevices(BtDevice* out, int max)
{
    std::lock_guard<std::mutex> lk(g_bt_mx);
    btInitLocked();
    return g_bt.devices(out, max);
}

namespace {
// an API command: only while enabled, then one step (like the next tick of the bridge task)
bool btCommand(bool (*fn)(btp::Link&, const void*), const void* arg)
{
    std::lock_guard<std::mutex> lk(g_bt_mx);
    btInitLocked();
    if (!g_bt_enabled || !fn(g_bt, arg)) return false;
    btPumpLocked();
    return true;
}
}  // namespace

bool btScan() { return btCommand([](btp::Link& l, const void*) { return l.queue("SCAN"); }, nullptr); }
bool btSelect(const char* mac)
{
    return btCommand([](btp::Link& l, const void* a) { return btp::cmdSelect(l, (const char*)a); }, mac);
}
bool btReconnect() { return btCommand([](btp::Link& l, const void*) { return l.queue("RECONNECT"); }, nullptr); }
bool btTest() { return btCommand([](btp::Link& l, const void*) { return l.queue("TEST"); }, nullptr); }
bool btSetVolume(int percent)
{
    return btCommand([](btp::Link& l, const void* a) { return l.setVolume(*(const int*)a); }, &percent);
}
bool btAirpodsMode(int mode)
{
    return btCommand([](btp::Link& l, const void* a) { return btp::cmdAirpodsMode(l, *(const int*)a); }, &mode);
}

BtRemote btPopRemote()
{
    std::lock_guard<std::mutex> lk(g_bt_mx);
    btInitLocked();
    return g_bt.popRemote();
}

void btSetAudioRoute(bool to_bridge, bool local)
{
    std::lock_guard<std::mutex> lk(g_bt_mx);
    g_bt_route = to_bridge;
    g_bt_local = local;
    btPumpLocked();
}

// ---------------------------------------------------------------- service timer
bool serviceTimerStart(ServiceFn fn, void* user, uint32_t period_us)
{
    g_svc = fn;
    g_svc_user = user;
    g_svc_period = fn ? (period_us < 500 ? 500 : period_us) : 0;
    return true;
}

}  // namespace hal
