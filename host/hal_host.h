#pragma once
// PC-only helpers of host/hal_host.cpp (the hal.h implementation for host tests and the
// screenshot renderer). Nothing here exists on the Tab5.
//
// Build note: hal_host.cpp starts the headless LVGL display (host/lvgl_host.h) in
// hal::displayStart(). Tests that do not link LVGL compile it with -DHAL_HOST_NO_LVGL; then
// displayStart() returns nullptr.
#include <stdint.h>
#include "hal/hal.h"

// ---- paths
// SD card folder. Default: $ORBIT_SDCARD, else $ORBIT_ROOT/sdcard, else <cwd>/sdcard.
// nullptr or "" = no card (sdMount() fails, sdRoot() returns "").
void halHostSetSdRoot(const char* path);
// Settings file. Default: <root>/host/out/settings.bin (root = $ORBIT_ROOT or <cwd>).
void halHostSetSettingsPath(const char* path);
const char* halHostSettingsPath();

// ---- audio: there is no device; the test pulls blocks through the RenderFn given to
// hal::audioStart() (in block_frames pieces, calling the audioSetTimingHook hook after each
// with the measured render time and the nominal block period). Silence when not running.
void halHostRenderAudio(int frames, int16_t* out);
uint32_t halHostAudioRate();     // 0 when stopped
int halHostAudioBlock();         // block_frames of the running stream
int halHostVolume();             // last hal::audioSetVolume()
orbit::AudioOut halHostOutput(); // last hal::audioSetOutput()
bool halHostAudioIdle();         // last hal::setAudioIdle() (true at start)

// ---- touch / MIDI injection (calls the hooks registered by the firmware code)
void halHostTouch(const hal::TouchPoint* points, int count);
// The last hal::setTouchExclusive() rectangle; false when none is set.
bool halHostExclusive(int* x1, int* y1, int* x2, int* y2);
void halHostSetExclusive(int x1, int y1, int x2, int y2);
void halHostMidi(const hal::MidiMsg& m);

// ---- display state set through hal.h
int halHostBrightness();
int halHostFrameCap();
bool halHostRestartRequested();

// ---- MIDI out: no device by default (midiConnected() false, midiSend() fails). Connected, midiSend()
// records every message; halHostMidiTakeSent() hands them over oldest first (out == nullptr: drops
// them) and returns how many it took.
void halHostMidiSetConnected(bool connected, const char* name = "HOST MIDI");
int halHostMidiTakeSent(hal::MidiMsg* out, int max);

// ---- time: hal::micros() returns us while a fake clock is set (us != 0), the real clock for 0.
// (orbit::nowUs() of platform_host.cpp is not affected.)
void halHostSetMicros(uint64_t us);

// ---- service timer (hal::serviceTimerStart): never fires by itself on the PC.
bool halHostServiceTimer();              // runs the callback once; false when none is set
uint32_t halHostServiceTimerPeriod();    // 0 when stopped

// ---- Tab5 launcher simulation: with present = true, launcherAvailable() is true and
// restartToLauncher() records the request and returns true (the Tab5 would not return).
void halHostSetLauncher(bool present);
bool halHostLauncherRequested();

// ---- Bluetooth (hal::bt*): the real protocol state machine (firmware/main/hal/bt_protocol.h) talking to a
// fake Pico driven by the test, on a virtual clock that only moves through halHostBtAdvance (the bridge task
// of the Tab5 steps every 20 ms; the host steps after every hal::bt* call, every injected line and every
// 20 ms of advance). Typical exchange: hal::btEnable(true); halHostBtAdvance(800) (wake pulse + boot guard:
// "USE", "STATUS" sent); halHostBtFeed("READY version=0.9.5 cap_airpods=1") ("USE", "LIST" ...);
// halHostBtFeed("STATE connection=2 audio=0"); hal::btSetAudioRoute(true, false) -> "STREAM 1" ...
// The bridge only streams while the host audio is stopped or runs at 44.1 kHz (like the Tab5).
void halHostBtFeed(const char* line);          // inject one line as if the Pico sent it (no newline needed)
int halHostBtTakeSent(char* buf, int len);     // pop the next command ORBIT sent (no newline), returns its
                                               // length (copied, truncated to len - 1) or 0 when none
void halHostBtAdvance(uint32_t ms);            // advance the bridge's clock (steps every 20 ms)
// extras for tests
void halHostBtReset();                         // back to the power-on state (disabled, clock 0, queues empty)
uint32_t halHostBtNow();                       // the virtual clock (ms)
bool halHostBtWakeLow();                       // the wake pulse is on (G6 low)
uint64_t halHostBtBridgeFrames();              // frames halHostRenderAudio copied to the bridge since reset
void halHostBtSetQuiet(uint32_t ms);           // how long the bridge audio has been silent (Inputs::quiet_ms, default 0)
