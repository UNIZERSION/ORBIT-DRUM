#pragma once
// Hardware services of the M5Stack Tab5 used by app_main and the App controller.
// Implemented by hal/*.cpp (ESP-IDF 5.5.2 + espressif/m5stack_tab5 BSP) and by
// host/hal_host.cpp (PC: headless LVGL display, offline audio, a folder as the SD card).
#include <stdint.h>
#include "core/settings.h"

typedef struct _lv_display_t lv_display_t;

namespace hal {

// ---------------------------------------------------------------- board
bool boardInit();                       // I2C, IO expanders, power rails, NVS
const char* boardInfo();                // "ESP32-P4 v1.3, ST7123, 32 MB PSRAM"
uint32_t freeInternal();
uint32_t freePsram();
int batteryPercent();                   // -1 unknown
bool charging();

// ---------------------------------------------------------------- display + LVGL
// Starts LVGL (its task on core 0, tick), the landscape 1280x720 tear-free presentation
// (render pieces -> PPA rotation into a hidden 720x1280 frame buffer -> publish on VSYNC)
// and the touch input. Returns the LVGL display.
lv_display_t* displayStart();
// displayStart() returns at once: the panel (probe + BSP init, ~1.9 s of fixed delays) comes up in a background
// task while the App and the UI are built; the first frame waits for it. This waits for that bring-up
// (0 = forever) and tells whether the panel works.
bool displayWaitReady(uint32_t timeout_ms);
// Reserves the two LVGL draw buffers in internal SRAM (2 x 1280x32 RGB565). Call it first thing in
// app_main, before anything else allocates, so the draw buffer size does not depend on heap
// fragmentation; displayStart() uses the reservation (or tries itself, then falls back to PSRAM).
bool displayReserveBuffers();
// The backlight stays off until the UI is ready: call this with the LVGL lock held, right after the UI
// (or an error screen) is built. The first refresh after it lights the backlight once that frame is on
// the glass. Without the call the backlight comes on 3 s after displayStart().
void displayReveal();
bool lvLock(uint32_t timeout_ms);       // for tasks other than the LVGL task
void lvUnlock();
void setBrightness(int percent);        // 0..100 (0 = backlight off)
void setFrameCap(int fps);              // 60 normal, 30 low power
// Test console (0.4.4, ui/test_console.cpp): the frame on the glass, read back in the panel's native portrait
// order (720 x 1280, RGB565; landscape (x, y) is native (y, 1279 - x)): row(px, w, y, user) for every step-th
// native row, every step-th pixel (w = 720 / step). Call with the LVGL lock held (no refresh publishes
// meanwhile). False: no frame yet (or not on this platform).
bool displayReadBack(int step, void (*row)(const uint16_t* px, int w, int y, void* user), void* user);

// ---------------------------------------------------------------- touch (multi-point)
struct TouchPoint {
    int16_t x, y;                       // landscape coordinates 0..1279, 0..719
    uint8_t id;                         // controller track id
    uint8_t _pad;
};
// Called from the touch task (not the LVGL task) for every new controller sample while any
// finger is down and once with count = 0 when all are released. Keep it short and lock-free:
// the pad grid uses it to trigger sounds on finger-down with minimum latency.
using TouchHook = void (*)(const TouchPoint* points, int count, void* user);
void setTouchHook(TouchHook hook, void* user);
int touchRate();                        // measured controller samples per second
// Fingers that come down inside this screen rectangle belong to the TouchHook alone: LVGL never follows
// them, so other fingers keep working on knobs and buttons (0.4.3: the BASS KEYS keyboard, played with
// several fingers). x2 < x1 clears it. A finger keeps its role until it lifts.
void setTouchExclusive(int x1, int y1, int x2, int y2);
// Test console (0.4.4): fingers from the PC (landscape points, ids as given). Until released (count 0) they replace
// the controller's points in the touch task, so they take the real path: TouchHook, LVGL's pointer, the zone.
void touchInject(const TouchPoint* points, int count);

// ---------------------------------------------------------------- audio out
// Renders interleaved stereo int16. Called from the audio task (core 1, top priority).
using RenderFn = void (*)(int16_t* lr, int frames, void* user);
bool audioStart(uint32_t sample_rate, int block_frames, RenderFn render, void* user);
void audioStop();
bool audioRunning();
void audioSetVolume(int percent);       // codec volume 0..100
void audioSetOutput(orbit::AudioOut out);
bool headphonesInserted();
float audioLatencyMs();                 // DMA + block latency of the running stream
// Called from the audio task after every block: render time and the time since the previous block
// started (the real block period). Lock-free, no I/O. nullptr removes it.
using AudioTimingHook = void (*)(uint32_t render_us, uint32_t block_us, void* user);
void audioSetTimingHook(AudioTimingHook fn, void* user);
uint32_t audioXruns();                  // DMA underruns (I2S send queue overflow) since boot
                                        // (counted only while a stream is running, never at its start)

// ---------------------------------------------------------------- diagnostics (Settings / self-test)
// Worst cases since boot or the last diagnosticsReset(), measured on the device (a PC host reports zeros
// and ioe_inputs = -1).
struct Diagnostics {
    uint32_t audio_render_max_us;       // render of one block
    uint32_t audio_write_max_us;        // i2s_channel_write of one block (mostly waiting for a free buffer)
    uint32_t audio_period_max_us;       // block start to block start (> DMA buffers - 1 blocks: underrun)
    uint32_t audio_overload_blocks;     // blocks of the running stream rendered in >= 95 % of the period
    uint32_t rotate_max_us;             // one LVGL piece rotated into the frame buffer (PPA or CPU)
    uint32_t repair_max_us;             // stale areas copied before a refresh (on its critical path)
    uint32_t publish_max_us;            // frame published (esp_lcd_panel_draw_bitmap)
    int16_t ioe_inputs;                 // last IN_STA of IO expander 0x43 (bit 7 = HP_DET), -1 unknown
    uint8_t hp_det_trusted;             // 1: HP_DET verified on the hardware, Auto switches the speaker off
    uint8_t audio_dma_buffers;          // DMA buffers of the running stream (0: stopped)
    uint16_t draw_rows;                 // LVGL draw buffer rows (0: no display)
    uint8_t draw_sram;                  // 1: draw buffers in internal SRAM, 0: PSRAM
    uint8_t _pad;
};
void diagnostics(Diagnostics& out);
void diagnosticsReset();

// ---------------------------------------------------------------- storage
bool sdMount();                         // FAT32 + exFAT, /sdcard
bool sdMounted();
const char* sdRoot();                   // "/sdcard" (device) or a folder (PC)

// ---------------------------------------------------------------- settings
// Stored on the SD card (/sdcard/ORBIT/settings.bin) and mirrored to NVS. A flash (NVS) write
// stalls the cache of both cores and would click the audio, so the NVS mirror is written only
// while the audio stream is silent (transport stopped); otherwise it is deferred to the next stop.
bool settingsLoad(orbit::Settings& s);  // false -> defaults were applied
bool settingsSave(const orbit::Settings& s);
// The App reports whether the audio output is silent (transport stopped, no voices sounding):
// deferred flash (NVS) writes happen only while idle. Initially idle.
void setAudioIdle(bool idle);

// ---------------------------------------------------------------- MIDI (USB host)
struct MidiMsg {
    uint8_t status, d1, d2, cable;
};
using MidiHook = void (*)(const MidiMsg& m, void* user);   // from the USB task
bool midiStart(MidiHook hook, void* user);   // once started it stays on; a later call only replaces the hook
bool midiConnected();
const char* midiDeviceName();           // "" when none
bool midiSend(const MidiMsg& m);        // clock out / notes (false when no device)

// ---------------------------------------------------------------- misc
uint64_t micros();
void restart();

// ---------------------------------------------------------------- Tab5 launcher
// When the Tab5 launcher installed ORBIT, the launcher is the factory app and ORBIT runs from an
// OTA slot. restartToLauncher() then does esp_ota_set_boot_partition(factory) + esp_restart() and
// does not return. False when there is no launcher (standalone flash: ORBIT itself is the factory
// app; PC). launcherAvailable() answers the same question without restarting (reads flash: cache it).
bool launcherAvailable();
bool restartToLauncher();

// ---------------------------------------------------------------- Bluetooth audio (Aurora "Pico bridge")
// The Tab5's ESP32-C6 has no Classic Bluetooth (no A2DP), so Bluetooth headphones / speakers are reached
// through the helper board of Aurora Player: an M5Stamp Pico (ESP32-PICO-D4) running the "Pico Bluetooth
// bridge" firmware (0.9.x), wired as in aurora-player/pico-bridge/README.md:
//   I2S (Tab5 master, I2S_NUM_0, no MCLK): BCK G5 -> Pico G26, WS G18 -> Pico G25, DATA G19 -> Pico G22
//   UART 115200 8N1 (UART_NUM_2): Tab5 TX G6 -> Pico G32 (also the wake pin), Tab5 RX G46 <- Pico G33
// Our own client of the documented text protocol (READY / STATE / LIST / SELECT / SCAN / VOLUME / STREAM /
// STOP / REMOTE ...). Audio to the Pico is 16-bit stereo at 44.1 kHz, so the engine runs at 44.1 kHz while
// Bluetooth is on. The Pico is woken by a 120 ms low pulse on G6 and kept awake with "USE" every 3 s.
struct BtDevice {
    char mac[18];                       // "AA:BB:CC:DD:EE:FF"
    char name[48];
    int16_t rssi;                       // dBm, 0 = unknown (saved device not seen by the last scan)
    uint8_t saved;                      // remembered by the Pico (up to 8)
    uint8_t selected;                   // the Pico's reconnect target
};
struct BtStatus {
    uint32_t revision;                  // increments on every change (UI refresh)
    uint8_t enabled;                    // btEnable(true)
    uint8_t module;                     // the Pico answered since enabling (READY / any reply)
    uint8_t awake;
    uint8_t connected;                  // headphones / speaker connected (STATE connection=2)
    uint8_t streaming;                  // audio stream acknowledged and running (STATE audio=2)
    uint8_t scanning;                   // SCAN until SCAN_DONE
    uint8_t volume;                     // Pico volume 0..100
    uint8_t device_count;
    uint8_t airpods;                    // the Pico reports cap_airpods=1 and AirPods are connected
    int8_t airpods_left, airpods_right, airpods_case;   // battery %, -1 unknown
    int8_t airpods_mode;                // 1 off, 2 noise cancellation, 3 transparency, 4 adaptive, -1 unknown
    uint32_t sbc_rate;                  // negotiated SBC sample rate, 0 unknown
    uint32_t faults;                    // STREAM_FAULT count since enabling
    uint32_t recoveries;                // RECOVERY count since enabling
    uint32_t dropped_blocks;            // audio blocks lost to the Pico I2S DMA: did not fit / it ran dry (should stay 0)
    // the Pico's buffer (its STATUS, every 2 s while streaming; 0.4.4): audio waiting there = latency
    int32_t pico_buffered;              // frames in its ring now (44.1 kHz), -1 unknown
    int32_t pico_floor;                 // the ring between its 30 ms pulls (lowest of the last samples), -1 unknown
    uint32_t pico_underruns;            // its ring ran dry (counted by the Pico since it started)
    uint32_t pico_overflows;            // frames it dropped because its ring was full
    uint32_t resyncs;                   // buffer re-syncs since enabling (a new STREAM in a silent moment)
    char version[16];                   // Pico firmware version from READY, "" unknown
    char connected_name[48];            // name of the connected device, "" none
    char message[96];                   // last notice / ERROR text for the UI (Polish where ours)
};
enum class BtRemote : uint8_t { None = 0, Play, Pause, Next, Previous };

// All of these are thread-safe and non-blocking (commands are queued to the bridge task).
bool btEnable(bool on);                 // on: UART + I2S + wake + keepalive; off: STOP, SLEEP, release
void btStatus(BtStatus& out);
int btDevices(BtDevice* out, int max);  // saved first, then the last scan's results
bool btScan();                          // 20 s discovery on the Pico (SCAN ... SCAN_DONE)
bool btSelect(const char* mac);         // connect + remember (SELECTMAC)
bool btReconnect();
bool btTest();                          // 3 s left/silence/right test tone on the headphones
bool btSetVolume(int percent);
bool btAirpodsMode(int mode);           // 1..4 (only when BtStatus::airpods)
BtRemote btPopRemote();                 // headphone buttons (AVRCP), oldest first; None when empty
// Audio routing: when to_bridge is set and the bridge streams, the audio task copies every rendered block
// to the Pico (non-blocking, same APLL clock as the codec). local = keep the speaker / jack playing too.
void btSetAudioRoute(bool to_bridge, bool local);

// ---------------------------------------------------------------- service timer
// fn(user) every period_us (>= 500) from the esp_timer task (core 0, high priority; not an ISR):
// keep it short and lock-free (the MIDI clock out sender). One timer: a new call replaces the
// callback, fn = nullptr stops it. On the PC it fires only through halHostServiceTimer().
using ServiceFn = void (*)(void* user);
bool serviceTimerStart(ServiceFn fn, void* user, uint32_t period_us);

}  // namespace hal
