// ORBIT DRUM firmware entry (M5Stack Tab5, ESP-IDF 5.5.2, LVGL 9.3).
//
// Start order:
//   LVGL draw buffers (internal SRAM, reserved before anything else allocates) -> the stage background
//   starts baking on core 1 (idle until the audio stream starts; build() waits for it) -> board (I2C, IO expanders,
//   LCD + touch power, NVS) -> SD card -> base palette (Settings > Display > Base color: the bake follows it)
//   -> App (settings, engine, default project with the 909-style kit,
//   loader task, MIDI, audio timing hook) -> display + LVGL task + touch -> theme + the whole UI (built once,
//   under the LVGL lock) -> backlight with the first frame of that UI -> codec volume / output -> audio
//   stream (its task creates the I2S channel on core 1).
// The App wires everything else itself: hal::audioSetTimingHook (CPU load), hal::midiStart (when
// Settings.midi_usb), the MIDI clock-out service timer, hal::setAudioIdle (deferred NVS writes) and, on its
// first tick, brightness / frame cap / volume / output from the settings. app_main returns afterwards;
// the LVGL task, the audio task, the touch task, the loader task and the USB host keep running.
#include "core/fx.h"
#include "core/settings.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/hal.h"
#include "lvgl.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/ui.h"

namespace {
const char* TAG = "orbit_main";

// Shown instead of the UI when the App could not start (no PSRAM for the project / engine).
void errorScreen(lv_display_t* disp, const char* text)
{
    lv_obj_t* scr = lv_display_get_screen_active(disp);
    if (!scr) return;
    lv_obj_set_style_bg_color(scr, orbit::ui::c(orbit::ui::col::bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_t* l = lv_label_create(scr);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, orbit::ui::c(orbit::ui::col::ink), 0);
    lv_obj_set_style_text_font(l, orbit::ui::f_m20, 0);
    lv_obj_center(l);
}

uint32_t validRate(uint32_t r) { return r == 44100 ? 44100u : 48000u; }

// ~0.6 s of float math off the boot path: core 1 has nothing to do until the audio stream starts.
void bakeTask(void*)
{
    orbit::ui::bgPrebake(1280, 720);   // the LVGL landscape resolution (hal::displayStart)
    vTaskDelete(nullptr);
}

// internal SRAM after each start stage: the audio, DMA and USB paths need internal (DMA-capable) memory
int64_t s_t_stage = 0;

// time of each start stage (boot profile) + internal SRAM after it
void logHeap(const char* stage)
{
    const int64_t now = esp_timer_get_time();
    ESP_LOGI(TAG, "boot %-10s %5lld ms (t=%lld ms)", stage, (long long)((now - s_t_stage) / 1000), (long long)(now / 1000));
    s_t_stage = now;
    ESP_LOGI(TAG, "heap after %s: internal %u B free (largest DMA block %u B), PSRAM %u B", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
}  // namespace

extern "C" void app_main(void)
{
    using orbit::ui::App;
    using orbit::ui::app;

    s_t_stage = esp_timer_get_time();
    // First, before fragmentation makes their size a matter of luck: the reverb's lines in internal RAM (memory
    // layout 1, the default since 0.4.9; the LVGL draw buffers go to PSRAM) or, layout 0, the two 1280x32 SRAM
    // draw buffers as up to 0.4.9
    const int layout = hal::memLayout();
    // (the lines take ~146 KB, less than the 160 KB the SRAM draw buffers took, so the internal RAM left for DMA
    // grows; without room for the lines the draw buffers stay in SRAM as before)
    const bool lines = layout == 1 && orbit::FxBus::reserveLines(48000);
    if (layout == 1 && !lines) ESP_LOGW(TAG, "no internal RAM for the reverb lines: layout 0");
    if (lines) {
        hal::displaySetDrawInPsram(true);
    } else if (!hal::displayReserveBuffers()) {
        ESP_LOGW(TAG, "draw buffers will be in PSRAM");
    }
    ESP_LOGI(TAG, "memory layout %d: reverb lines %s, draw buffers %s", layout,
             orbit::FxBus::linesReserved() ? "internal" : "PSRAM", lines ? "PSRAM" : "internal");
    // The stage background bakes in parallel with the rest of the start (build() waits for it, or bakes
    // itself when the task could not start). Priority 3 on core 1: nothing else runs there yet.
    if (xTaskCreatePinnedToCore(bakeTask, "orbit_bake", 4096, nullptr, 3, nullptr, 1) != pdPASS)
        ESP_LOGW(TAG, "background pre-bake task not started");
    if (!hal::boardInit()) ESP_LOGE(TAG, "board init failed (I2C / IO expanders)");
    logHeap("board");
    // The LVGL display exists at once; the panel itself (~1.9 s of BSP/panel delays) comes up in a background
    // task while the card, the App and the UI are prepared below. The first frame waits for it.
    lv_display_t* disp = hal::displayStart();
    if (!disp) ESP_LOGE(TAG, "display start failed");
    logHeap("display");
    const bool sd = hal::sdMount();
    logHeap("sd");

    // The sample rate comes from the settings (SD card file, NVS mirror); App::init loads them again
    // for itself, this read only picks the engine rate before it exists.
    orbit::Settings boot;
    orbit::settingsDefaults(boot);
    hal::settingsLoad(boot);
    // Settings > Display > Base color, as early as possible: the stage background baking on core 1 follows it
    // (it starts again from the top when this picks another base than LIGHT while it runs).
    orbit::ui::baseSelect(boot.base);
    const uint32_t rate = validRate(boot.sample_rate);
    logHeap("settings");

    App& a = app();
    const bool ok = a.init(sd ? hal::sdRoot() : nullptr, (int)rate);
    if (!ok) ESP_LOGE(TAG, "App init failed (PSRAM?)");
    const orbit::Settings& st = ok ? a.settings() : boot;
    ESP_LOGI(TAG, "SD %s, %u Hz, block %u", sd ? "mounted" : "missing", (unsigned)rate, (unsigned)st.buffer_frames);
    logHeap("App::init");

    hal::setBrightness(st.brightness);   // applied once the first frame of the UI is on the panel

    if (disp && hal::lvLock(0)) {
        // Settings > Display > Base color + Accent color (both apply at start; the base is normally the boot copy's)
        orbit::ui::baseSelect(st.base);
        orbit::ui::themeSelect(st.theme, st.accent_hue, st.accent_sat);
        orbit::ui::themeInit();
        logHeap("theme");
        if (ok) orbit::ui::build(disp);
        else errorScreen(disp, "ORBIT DRUM: no PSRAM memory \xE2\x80\x94 restart");
        hal::displayReveal();   // the next refresh is the finished UI: backlight on with it
        hal::lvUnlock();
    }
    logHeap("UI");

    hal::audioSetVolume(st.volume);
    hal::audioSetOutput(st.output);
    if (ok && !hal::audioStart(rate, st.buffer_frames, &App::render, &a.engine())) {
        ESP_LOGE(TAG, "audio start failed");
        if (hal::lvLock(0)) {
            a.toast("Audio start failed");
            hal::lvUnlock();
        }
    }
    if (ok) a.audioBootDone();   // a saved Bluetooth start (App tick) waits for this audio start to be over
    logHeap("audio");
    ESP_LOGI(TAG, "running: %s", hal::boardInfo());
    if (ok) orbit::ui::testcon::start();   // commands from the PC (tools/device_test.py)
    hal::kbdStart();                        // the M5Stack Tab5 Keyboard, plugged in or not (0.4.6)
}
