#pragma once
// Device settings (SETTINGS dialog). Saved in NVS on the Tab5 (not in projects).
#include <stddef.h>
#include <stdint.h>

namespace orbit {

enum class AudioOut : uint8_t { Auto = 0, Speaker, Headphones, Count };
enum class PadVelocity : uint8_t { Fixed = 0, Position, Count };   // touch has no pressure

struct Settings {
    // General
    uint8_t autosave = 1;            // every 5 min to /sdcard/ORBIT/PROJECTS/<name>.orb when dirty
    uint8_t pad_audition = 1;        // "Demo audio" in the template: selecting a pad plays it
    // Audio
    AudioOut output = AudioOut::Auto;
    uint32_t sample_rate = 48000;    // 44100 or 48000 (restarts the audio task)
    uint16_t buffer_frames = 64;     // 64 / 128 / 256 per DMA block: at 64 the stream uses 4 DMA buffers,
                                     // 4 + 1 blocks = 6.7 ms at 48 kHz (128 / 256: 3 buffers + 1 block)
    uint8_t master_limiter = 1;      // mirrors Project::limiter_on for new projects
    uint8_t volume = 72;             // codec output volume 0..100
    // MIDI
    uint8_t midi_usb = 1;            // USB-MIDI host input
    uint8_t midi_clock_in = 0;       // follow external MIDI clock
    uint8_t midi_channel = 10;       // 1..16, 0 = omni
    uint8_t midi_clock_out = 0;      // send clock (USB)
    // Display
    uint8_t brightness = 80;         // 10..100
    uint8_t animations = 1;
    uint8_t meters = 1;
    uint8_t low_power = 0;           // 30 fps cap, no ripple, dim after 60 s
    // Touch
    uint8_t touch_feedback = 1;      // ripple / press animation
    uint8_t haptic_indicator = 1;    // pad flash ring on touch
    uint8_t knob_sensitivity = 56;   // 1..100 (template default 56)
    uint8_t double_tap_reset = 1;
    // Fixed by default: 62 px pads leave ~5 mm for the position curve, so Position is an opt-in
    PadVelocity pad_velocity = PadVelocity::Fixed;
    uint8_t fixed_velocity = 110;
    uint32_t version = 1;
    // ---- appended fields (an older, shorter settings payload keeps their defaults)
    // Audio: upper polyphony limit (Engine::setVoiceLimit), 8..32; the CPU governor may go lower
    uint8_t voice_limit = 32;
    // Settings files of 0.1.0 are 36 bytes: these 3 were the struct's tail padding (undefined bytes, saved as
    // they were), so nothing new may live here; the fields below start at byte 36 and keep their defaults when
    // such a file is loaded (storage.cpp copies only the payload it has).
    uint8_t reserved_pad[3] = {};
    // Bluetooth audio through the Aurora Pico bridge (Settings > Bluetooth; changed and saved at once by the App's
    // bt* methods, never through the APPLY copy). While it is on the engine runs at 44.1 kHz; sample_rate above
    // stays the user's choice and comes back when Bluetooth is switched off.
    uint8_t bt_enabled = 0;
    uint8_t bt_local = 0;            // speaker / headphone jack keep playing too (~0.2 s ahead of Bluetooth)
    uint8_t bt_volume = 70;          // Pico volume 0..100 (the VOLUME knob while Bluetooth headphones play)
    // Settings files of 0.2.x are 40 bytes: byte 39 was tail padding (undefined), so it stays unused.
    uint8_t reserved_pad2 = 0;
    // Output volume memory (0.3): the main VOLUME knob shows and sets the level of the output that plays now,
    // and every output keeps its own level, applied by itself when the output changes (Bluetooth: bt_volume).
    uint8_t vol_speaker = 72;
    uint8_t vol_headphones = 50;
    uint8_t vol_init = 0;            // 0 in older files: vol_speaker / vol_headphones are seeded from `volume` once
    // Display: accent colour (ui/theme.h kThemes order, kAccentCustom = the hue / saturation below), applied at
    // the next start
    uint8_t theme = 0;
    // App::restartApp (a new colour theme): the project to open again right after that start ("" none).
    // Cleared as soon as it is read, so a project that cannot be opened is never retried.
    char resume_project[32] = {};
    // ---- colours, after the 76-byte payload of 0.3.0 / 0.3.1 settings files: such a file keeps these defaults,
    // and every field reads 0 = default, so a zero-filled tail is the default look too.
    uint8_t base = 0;                // Display > Base color: ui/theme.h kBases order (0 LIGHT ... kBaseCount - 1)
    uint8_t accent_sat = 0;          // CUSTOM accent saturation kAccentSatMin..100 %; 0 = never set (default)
    uint16_t accent_hue = 0;         // CUSTOM accent hue 0..359 degrees
    // ---- 0.4.6, after the 80-byte payload of 0.3.2 .. 0.4.5 settings files (they keep these defaults)
    uint8_t ui_size = 0;             // Display > Interface size: 0 standard, 1 large, 2 extra large (dialogs, targets)
    uint8_t reserved_pad3[3] = {};
};
static_assert(offsetof(Settings, bt_enabled) == 36, "bt_* after the 36-byte payload of older settings files");
static_assert(offsetof(Settings, vol_speaker) == 40, "0.3 fields after the 40-byte payload of 0.2.x settings files");
static_assert(offsetof(Settings, base) == 76, "colour fields after the 76-byte payload of 0.3.0 / 0.3.1 settings files");
static_assert(offsetof(Settings, ui_size) == 80, "0.4.6 fields after the 80-byte payload of older settings files");
static_assert(sizeof(Settings) == 84, "no tail padding: every saved byte is a field");

// Display colours (the UI side lives in ui/theme.h, which checks these against its tables).
constexpr int kBaseCount = 6;            // base palettes: LIGHT WARM SILVER SLATE DARK BLACK
constexpr int kAccentPresets = 8;        // accent presets: ORANGE ... AMBER
constexpr int kAccentCustom = 8;         // Settings::theme of the CUSTOM accent (hue + saturation)
constexpr int kAccentSatMin = 20;        // CUSTOM saturation range kAccentSatMin..100 %
constexpr int kAccentSatDefault = 80;    // accent_sat == 0

void settingsDefaults(Settings& s);

}  // namespace orbit
