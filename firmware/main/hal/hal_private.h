#pragma once
// Internal glue between the Tab5 hal/*.cpp files (not part of the hal.h contract).
#include <stdint.h>
#include "driver/i2c_master.h"
#include "driver/i2s_types.h"   // i2s_chan_handle_t
#include "hal/i2s_types.h"      // i2s_clock_src_t (ESP-IDF hal component)
#include "esp_err.h"
#include "esp_io_expander.h"

namespace hal {
struct Diagnostics;
}

namespace hal::detail {

// ---- I2C (BSP bus I2C_NUM_1, SDA 31 / SCL 32, shared by touch, codecs, expanders, INA226, RTC)
bool i2cReady();                                     // bsp_i2c_init with retries + bus recovery
esp_err_t i2cAdd(uint16_t addr, i2c_master_dev_handle_t* dev);
esp_err_t i2cRead(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t* data, size_t len);
esp_err_t i2cWrite(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value);

// ---- PI4IOE5V6408 at 0x43 (P1 SPK_EN, P4 LCD_RST, P5 TP_RST, P7 HP_DET), BSP driver.
// The BSP creates (and RESETS) the chip on every bsp_io_expander_init() call until one succeeds, so only
// boardInit / lcdPower (app_main) open it: ioe1Open() tries up to 3 times with a bus reset in between,
// configures HP_DET (P7) as an input without pull, and caches the handle. Everything else uses ioe1(),
// which never creates anything (nullptr: the chip is not available).
esp_io_expander_handle_t ioe1Open();
esp_io_expander_handle_t ioe1();
// esp_timer time at which boardInit powered the touch controller (TP_RST released), 0 = not yet
int64_t touchPowerOnUs();

// ---- PI4IOE5V6408 at 0x44 (WLAN_PWR_EN P0, USB 5 V P3, power-off P4, CHG_STAT P6, CHG_EN P7).
// Driven register by register: the BSP's expander driver resets the chip when it opens it, which
// would drop the charger enable and the USB 5 V rail.
constexpr uint8_t kIoe2Wlan = 0x01, kIoe2Usb5v = 0x08, kIoe2ChgStat = 0x40, kIoe2ChgEn = 0x80;
esp_err_t ioe2Output(uint8_t mask, bool high);       // level first, then push-pull, then output
esp_err_t ioe2Input(uint8_t mask);

// ---- display (display.cpp)
const char* panelName();                             // "ST7123", "ST7121", "ILI9881C" or "?"
void displayDiag(Diagnostics* out, bool reset);      // fills the display fields / clears the maxima
int displayDebug(char* buf, size_t n);               // bring-up: presentation / LVGL flush state

// ---- touch (touch.cpp): started by displayStart once the LVGL display exists (lv_display_t*)
bool touchStart(void* lv_display);

// ---- audio (audio.cpp): headphone / speaker switching task, started by boardInit
void audioAuxStart();
void audioDiag(Diagnostics* out, bool reset);        // fills the audio fields / clears the maxima

// ---- settings (storage.cpp): pending NVS mirror write, called by the aux task while audio is idle
bool audioIdleNow();
bool settingsFlushDeferred();   // true when nothing failed

// ---- Bluetooth bridge (bt.cpp) <-> audio (audio.cpp)
// The codec channel (I2S1) and the bridge channel (I2S_NUM_0 -> Pico) both run from this clock source with
// MCLK = 256 fs, so at 44.1 kHz their sample clocks are the same APLL: no drift, no resampling. The APLL
// frequency is set by the first channel that acquires it and cannot change while two channels hold it, so
// audioStart releases the bridge channel (btAudioSuspend) before it creates the codec channel and reports
// the new rate and block size afterwards (btAudioResume; rate 0 = audio stopped / failed), after which the
// bridge task creates its channel again (only at 44.1 kHz or without audio, never at 48 kHz) with one DMA
// buffer per block (the audio task writes whole buffers) and more buffers than the codec has.
inline constexpr i2s_clock_src_t kAudioClkSrc = I2S_CLK_SRC_APLL;
void btAudioSuspend();                   // audioStart, s_ctl held: waits for the bridge task's I2S work
void btAudioResume(uint32_t rate, int frames);
// Read by the audio task with __atomic loads only (no locks, no logging):
struct BtAudioTap {
    i2s_chan_handle_t tx;   // the enabled bridge channel while it streams, else nullptr (set by bt.cpp)
    uint32_t busy;          // 1 while the audio task writes to tx; bt.cpp clears tx, then waits for 0
    uint32_t dropped;       // blocks lost: did not fit the bridge DMA (timeout 0) or the DMA ran dry (underrun)
    uint8_t mute_local;     // the codec gets silence (route local = false and the stream is acknowledged)
    uint8_t armed;          // the audio task feeds tx: bridge underruns count (set by its writes, cleared when it
                            // stops and by bt.cpp before the channel goes)
    uint32_t loud_ms;       // esp_timer ms of the last block sent to the bridge that was not silent (0.4.4: the
                            // Pico's buffer is re-synced only in silence)
};
extern BtAudioTap g_btTap;

}  // namespace hal::detail
