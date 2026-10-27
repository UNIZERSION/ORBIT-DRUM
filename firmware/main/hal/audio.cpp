// Low-latency audio out: I2S1 (MCLK 30, BCLK 27, WS 29, DOUT 26) -> ES8388 DAC (I2C 0x10) -> speaker
// amplifier (NS4150B, fed from LOUT1, enabled by SPK_EN = IO expander 0x43 P1) and the headphone jack
// (HP_DET = 0x43 P7).
//
// The BSP's bsp_audio_codec_speaker_init() is not used: it creates a mono duplex channel with 6 x 240
// frames of DMA (~30 ms). Here: TX-only master channel, 16-bit stereo, MCLK = 256 fs from the APLL (an
// integer divider: the default source below silicon v3 is the 40 MHz XTAL, whose fractional divider
// jitters MCLK by ~25 %), DMA descriptors of one block each (4 for 64-frame blocks, else 3; 64/128/256
// frames), auto-clear (an underrun plays silence, not old data). The codec is driven by esp_codec_dev over
// the BSP I2C bus with our TX handle as its data interface (and the same APLL clock source, because
// esp_codec_dev_open reconfigures the channel clock); samples go straight to i2s_channel_write.
// The audio task (core 1, configMAX_PRIORITIES-2) creates and enables the I2S channel itself, so the
// GDMA interrupt is allocated on core 1 too, away from the display / USB / SD work of core 0. It renders a
// block and blocks in the write, which paces the loop. Speaker: L = R = (L+R)/2 after render (the amplifier
// plays LOUT1 only).
// Bluetooth (bt.cpp, Pico bridge): at 44.1 kHz, while the bridge streams, every rendered block (stereo, before
// the mono mix) is also copied into the bridge's I2S_NUM_0 channel with timeout 0 (one DMA buffer per block; a
// block that does not fit counts as dropped); with route local = false the codec then gets silence and keeps
// pacing the task. Both channels share the APLL (detail::kAudioClkSrc): audioStart releases the bridge channel
// before it creates the codec channel and passes the new rate + block size on (detail::btAudioSuspend /
// btAudioResume, hal_private.h).
#include <string.h>
#include "bsp/m5stack_tab5.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_io_expander.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/hal.h"
#include "hal/hal_logic.h"
#include "hal/hal_private.h"

namespace hal {

namespace {
const char* TAG = "orbit_audio";
constexpr int kMaxFrames = 256;
constexpr uint32_t kSpkEnPin = IO_EXPANDER_PIN_NUM_1;
constexpr uint32_t kHpDetPin = IO_EXPANDER_PIN_NUM_7;
constexpr bool kHpDetActiveHigh = true;    // IN bit 7 = 1 -> headphones
// Confirmed on the Tab5 on 2026-10-24: IN_STA 0x90 with a plug in the jack, 0x10 without (bit 4 = LCD_RST),
// the same on every boot. AudioOut::Auto: headphones -> amplifier off, stereo; none -> amplifier on, mono.
constexpr bool kHpDetTrusted = true;
constexpr int kHpDebounce = 2;             // equal readings in a row (2 x 200 ms) before a change counts
constexpr int kAuxPeriodMs = 200;          // HP_DET poll: 5 Hz
// 0x43 can stop answering (seen on 2026-10-24 a few seconds after the Pico bridge came up; every read failed
// until the next reset; cause unknown). Then: reads once a second, a bus probe line + bus clear after 2 s and
// every 10 s (at most kHpMaxClears per episode, probe lines every 60 s after that).
constexpr int64_t kHpRetryUs = 1000000;
constexpr int64_t kHpFirstProbeUs = 2000000;
constexpr int64_t kHpProbeUs = 10000000;
constexpr int kHpMaxClears = 6;
constexpr uint32_t kOverloadYieldBlocks = 32;   // overloaded blocks in a row before the task sleeps a tick
constexpr i2s_clock_src_t kClkSrc = detail::kAudioClkSrc;   // APLL, shared with the Bluetooth bridge

i2s_chan_handle_t s_tx = nullptr;
const audio_codec_data_if_t* s_data_if = nullptr;
const audio_codec_ctrl_if_t* s_ctrl_if = nullptr;
const audio_codec_gpio_if_t* s_gpio_if = nullptr;
const audio_codec_if_t* s_codec_if = nullptr;
esp_codec_dev_handle_t s_dev = nullptr;
bool s_dev_open = false;                   // esp_codec_dev_open succeeded (close then disables I2S)
bool s_i2s_on = false;                     // TX channel enabled

TaskHandle_t s_task = nullptr;
SemaphoreHandle_t s_task_ready = nullptr;  // the audio task finished its I2S start (s_start_ok)
SemaphoreHandle_t s_task_done = nullptr;
SemaphoreHandle_t s_ctl = nullptr;         // start / stop / volume / output switching
bool s_run = false;                        // written by the controller, read by the audio task
bool s_start_ok = false;                   // __atomic: i2sStart result of the audio task
bool s_running = false;
RenderFn s_render = nullptr;
void* s_render_user = nullptr;
uint32_t s_rate = 48000;
int s_frames = 128;
int s_desc = 3;                            // DMA descriptors of the stream
uint32_t s_xruns = 0;                      // __atomic, incremented in the I2S ISR
bool s_xrun_armed = false;                 // __atomic: count overflows (after the stream's first write)

portMUX_TYPE s_hook_mux = portMUX_INITIALIZER_UNLOCKED;
AudioTimingHook s_timing = nullptr;
void* s_timing_user = nullptr;

int s_volume = 72;
bool s_mono = true;                        // __atomic: speaker output, sum to mono
orbit::AudioOut s_out_mode = orbit::AudioOut::Auto;
bool s_hp = false;                         // __atomic: debounced HP_DET
int s_ioe_in = -1;                         // __atomic: last IN_STA of 0x43, -1 unknown
logic::Debounce s_hp_filter;               // aux task (and audioAuxStart before it runs)
int s_spk_state = -1;                      // SPK_EN as last written (-1 unknown)
bool s_aux_started = false;
// failed 0x43 reads (readHeadphones: aux task, or audioAuxStart before the task exists)
int s_hp_fail = 0;                         // in a row
int s_hp_probes = 0;                       // probe lines in this episode
int64_t s_hp_next_us = 0;                  // next read attempt while failing
int64_t s_hp_probe_us = 0;                 // next probe line / bus clear while failing
esp_log_level_t s_ioe_log[2] = {ESP_LOG_INFO, ESP_LOG_INFO};   // driver tag levels before muting
bool s_hp_reapply = false;                 // __atomic: 0x43 answers again, the aux task rewrites SPK_EN

// diagnostics: worst cases, written by the audio task (__atomic)
uint32_t s_render_max_us = 0;
uint32_t s_write_max_us = 0;
uint32_t s_period_max_us = 0;
uint32_t s_overload_blocks = 0;
uint32_t s_load_pm = 0;   // __atomic: audioLoad() x 1000

DRAM_ATTR int16_t s_buf[kMaxFrames * 2] __attribute__((aligned(64)));

void noteMax(uint32_t* slot, int64_t us)
{
    const uint32_t v = us < 0 ? 0u : us > 0x7FFFFFFF ? 0x7FFFFFFFu : (uint32_t)us;
    if (v > __atomic_load_n(slot, __ATOMIC_RELAXED)) __atomic_store_n(slot, v, __ATOMIC_RELAXED);
}

bool IRAM_ATTR onSendQueueOverflow(i2s_chan_handle_t, i2s_event_data_t*, void*)
{
    // Before the stream's first write the queue is full of the start-up backlog (the DMA runs while the
    // codec is configured): not an underrun.
    if (__atomic_load_n(&s_xrun_armed, __ATOMIC_RELAXED)) __atomic_fetch_add(&s_xruns, 1u, __ATOMIC_RELAXED);
    return false;
}

void ensureControl()
{
    if (!s_ctl) s_ctl = xSemaphoreCreateMutex();
    if (!s_task_done) s_task_done = xSemaphoreCreateBinary();
    if (!s_task_ready) s_task_ready = xSemaphoreCreateBinary();
}

// speaker amp and mono mix from the output mode and HP_DET. Call with s_ctl held.
void applyOutputLocked()
{
    const bool hp = __atomic_load_n(&s_hp, __ATOMIC_RELAXED);
    const logic::OutputState o = logic::outputFor(s_out_mode, hp, kHpDetTrusted);
    __atomic_store_n(&s_mono, o.mono, __ATOMIC_RELAXED);
    if (!s_running) return;   // the amp follows the stream; the 0x43 driver is shared with the display start
    const int want = o.speaker ? 1 : 0;
    if (want == s_spk_state) return;
    esp_io_expander_handle_t io = detail::ioe1();
    if (!io) return;
    detail::I2cCeiling ceiling;
    esp_err_t err = esp_io_expander_set_dir(io, kSpkEnPin, IO_EXPANDER_OUTPUT);
    if (err == ESP_OK) err = esp_io_expander_set_level(io, kSpkEnPin, (uint8_t)want);
    if (err == ESP_OK) err = esp_io_expander_set_output_mode(io, kSpkEnPin, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    if (err == ESP_OK) {
        s_spk_state = want;
        ESP_LOGI(TAG, "output: speaker %s, %s (HP_DET %s%s)", o.speaker ? "on" : "off", o.mono ? "mono" : "stereo",
                 hp ? "in" : "out", kHpDetTrusted ? "" : ", not trusted");
    }
}

const char* probeText(esp_err_t e)
{
    return e == ESP_OK ? "ok" : e == ESP_ERR_NOT_FOUND ? "NACK" : e == ESP_ERR_TIMEOUT ? "timeout" : esp_err_to_name(e);
}

// one line: who answers on the board bus now (each probe takes the bus lock like a normal transfer)
void logBusProbe(const char* when)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return;
    detail::I2cCeiling ceiling;
    const esp_err_t a = i2c_master_probe(bus, 0x43, 20), b = i2c_master_probe(bus, 0x44, 20);
    const esp_err_t t = i2c_master_probe(bus, 0x55, 20), p = i2c_master_probe(bus, 0x41, 20);
    ESP_LOGW(TAG, "I2C %s: 0x43 %s, 0x44 %s, 0x55 touch %s, 0x41 INA226 %s", when, probeText(a), probeText(b),
             probeText(t), probeText(p));
}

// A failed 0x43 read: one line with the error (the driver's line per failed read is muted meanwhile), reads
// once a second, then probe lines with a bus clear. The last debounced HP_DET state stays.
void headphonesReadFailed(esp_err_t err, int64_t now)
{
    s_hp_next_us = now + kHpRetryUs;
    if (++s_hp_fail == 1) {
        ESP_LOGW(TAG, "HP_DET: 0x43 read failed (%s), headphones stay %s; retrying every second", esp_err_to_name(err),
                 __atomic_load_n(&s_hp, __ATOMIC_RELAXED) ? "in" : "out");
        s_ioe_log[0] = esp_log_level_get("pi4ioe5v6408");
        s_ioe_log[1] = esp_log_level_get("io_expander");
        esp_log_level_set("pi4ioe5v6408", ESP_LOG_NONE);
        esp_log_level_set("io_expander", ESP_LOG_NONE);
        s_hp_probes = 0;
        s_hp_probe_us = now + kHpFirstProbeUs;   // a single failure is not an episode yet
        return;
    }
    if (now < s_hp_probe_us) return;
    ++s_hp_probes;
    s_hp_probe_us = now + (s_hp_probes < kHpMaxClears ? kHpProbeUs : 6 * kHpProbeUs);
    logBusProbe(s_hp_probes == 1 ? "while 0x43 fails" : "still failing");
    if (s_hp_probes <= kHpMaxClears) {
        // 9 clocks + STOP for a chip stuck mid-transfer; not under the bus lock (a touch read at that moment
        // fails once)
        esp_err_t r;
        {
            detail::I2cCeiling ceiling;
            r = i2c_master_bus_reset(bsp_i2c_get_handle());
        }
        ESP_LOGW(TAG, "HP_DET: %d failed reads, I2C bus clear %d/%d: %s", s_hp_fail, s_hp_probes, kHpMaxClears,
                 esp_err_to_name(r));
    }
}

// 0x43 answers again: the driver's lines back, P7 a plain input again (the chip may have been reset), SPK_EN
// rewritten by the aux task
void headphonesReadBack(esp_io_expander_handle_t io)
{
    ESP_LOGW(TAG, "HP_DET: 0x43 answers again after %d failed reads", s_hp_fail);
    s_hp_fail = 0;
    esp_log_level_set("pi4ioe5v6408", s_ioe_log[0]);
    esp_log_level_set("io_expander", s_ioe_log[1]);
    detail::I2cCeiling ceiling;
    esp_io_expander_set_dir(io, kHpDetPin, IO_EXPANDER_INPUT);
    esp_io_expander_set_pullupdown(io, kHpDetPin, IO_EXPANDER_PULL_NONE);
    __atomic_store_n(&s_hp_reapply, true, __ATOMIC_RELAXED);
}

// one HP_DET reading (aux task, or audioAuxStart before the task exists); only the cached 0x43 handle is
// used: bsp_io_expander_init() would re-create and reset the chip when it failed at boot
void readHeadphones()
{
    esp_io_expander_handle_t io = detail::ioe1();
    if (!io) return;
    const int64_t now = esp_timer_get_time();
    if (s_hp_fail && now < s_hp_next_us) return;
    uint32_t levels = 0;
    esp_err_t err;
    {
        detail::I2cCeiling ceiling;   // orbit_aux (2): LVGL (4) must not stop it while it holds the bus
        err = esp_io_expander_get_level(io, 0xFF, &levels);
    }
    if (err != ESP_OK) {
        headphonesReadFailed(err, now);
        return;
    }
    if (s_hp_fail) headphonesReadBack(io);
    const int prev = __atomic_exchange_n(&s_ioe_in, (int)(levels & 0xFF), __ATOMIC_RELAXED);
    const bool high = (levels & kHpDetPin) != 0;
    const bool hp = s_hp_filter.update(high == kHpDetActiveHigh, kHpDebounce);
    const bool was = __atomic_exchange_n(&s_hp, hp, __ATOMIC_RELAXED);
    if (prev < 0 || hp != was) {
        ESP_LOGI(TAG, "HP_DET: IN_STA 0x%02x -> headphones %s", (unsigned)(levels & 0xFF), hp ? "in" : "out");
    }
}

void auxTask(void*)
{
    bool last_hp = __atomic_load_n(&s_hp, __ATOMIC_RELAXED);
    for (;;) {
        readHeadphones();
        const bool hp = __atomic_load_n(&s_hp, __ATOMIC_RELAXED);
        const bool reapply = __atomic_exchange_n(&s_hp_reapply, false, __ATOMIC_RELAXED);
        if (hp != last_hp || s_spk_state < 0 || reapply) {
            last_hp = hp;
            if (xSemaphoreTake(s_ctl, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (reapply) s_spk_state = -1;   // the expander may have lost SPK_EN
                applyOutputLocked();
                xSemaphoreGive(s_ctl);
            } else if (reapply) {
                __atomic_store_n(&s_hp_reapply, true, __ATOMIC_RELAXED);
            }
        }
        if (detail::audioIdleNow()) detail::settingsFlushDeferred();
        vTaskDelay(pdMS_TO_TICKS(kAuxPeriodMs));
    }
}

void codecTeardown()
{
    detail::I2cCeiling ceiling;
    if (s_dev) {
        if (s_dev_open) {
            esp_codec_dev_set_out_mute(s_dev, true);
            esp_codec_dev_close(s_dev);   // codec off, I2S channel disabled
            s_i2s_on = false;
        }
        esp_codec_dev_delete(s_dev);
        s_dev = nullptr;
        s_dev_open = false;
    }
    if (s_codec_if) audio_codec_delete_codec_if(s_codec_if);
    if (s_ctrl_if) audio_codec_delete_ctrl_if(s_ctrl_if);
    if (s_gpio_if) audio_codec_delete_gpio_if(s_gpio_if);
    if (s_data_if) audio_codec_delete_data_if(s_data_if);
    s_codec_if = nullptr;
    s_ctrl_if = nullptr;
    s_gpio_if = nullptr;
    s_data_if = nullptr;
    if (s_tx) {
        if (s_i2s_on) i2s_channel_disable(s_tx);
        s_i2s_on = false;
        i2s_del_channel(s_tx);   // frees the core-1 interrupt through IPC when called from core 0
        s_tx = nullptr;
    }
}

// Runs in the audio task (core 1): the channel's DMA interrupt is allocated on the calling core.
bool i2sStart(uint32_t rate, int frames, int desc)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG((i2s_port_t)CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan.dma_desc_num = (uint32_t)desc;
    chan.dma_frame_num = (uint32_t)frames;
    chan.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan, &s_tx, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel: %s", esp_err_to_name(err));
        s_tx = nullptr;
        return false;
    }
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {},
    };
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    // APLL = 2 x MCLK (24.576 / 22.5792 MHz): integer MCLK divider
    std.clk_cfg.clk_src = kClkSrc;
    std.gpio_cfg.mclk = BSP_I2S_MCLK;
    std.gpio_cfg.bclk = BSP_I2S_SCLK;
    std.gpio_cfg.ws = BSP_I2S_LCLK;
    std.gpio_cfg.dout = BSP_I2S_DOUT;
    std.gpio_cfg.din = I2S_GPIO_UNUSED;
    err = i2s_channel_init_std_mode(s_tx, &std);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S std mode: %s", esp_err_to_name(err));
        return false;
    }
    i2s_event_callbacks_t cbs = {};
    cbs.on_send_q_ovf = onSendQueueOverflow;
    i2s_channel_register_event_callback(s_tx, &cbs, nullptr);
    // silence in every DMA buffer before the clocks start
    memset(s_buf, 0, sizeof(s_buf));
    for (int i = 0; i < desc + 1; i++) {
        size_t loaded = 0;
        if (i2s_channel_preload_data(s_tx, s_buf, (size_t)frames * 4, &loaded) != ESP_OK || loaded == 0) break;
    }
    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S enable: %s", esp_err_to_name(err));
        return false;
    }
    s_i2s_on = true;
    return true;
}

void audioTask(void*)
{
    // 1. the I2S channel, on this core
    const bool ok = i2sStart(s_rate, s_frames, s_desc);
    __atomic_store_n(&s_start_ok, ok, __ATOMIC_RELEASE);
    xSemaphoreGive(s_task_ready);
    // 2. wait for the controller: codec open (go, s_run = true) or abort (s_run = false)
    if (ok) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // 3. the stream
    const size_t bytes = (size_t)s_frames * 2 * sizeof(int16_t);
    const uint32_t period_us = (uint32_t)((uint64_t)s_frames * 1000000u / s_rate);
    const bool bt_rate = s_rate == 44100;   // the Pico takes 44.1 kHz only (bt.cpp never streams at 48 kHz)
    logic::OverloadGuard guard;
    bool first = true;
    // the task watchdog watches this loop itself (the idle check of CPU1 is off: sdkconfig.defaults)
    const bool wdt = ok && __atomic_load_n(&s_run, __ATOMIC_ACQUIRE) && esp_task_wdt_add(nullptr) == ESP_OK;
    int64_t prev = esp_timer_get_time();
    while (ok && __atomic_load_n(&s_run, __ATOMIC_ACQUIRE)) {
        if (wdt) esp_task_wdt_reset();
        const int64_t t0 = esp_timer_get_time();
        s_render(s_buf, s_frames, s_render_user);
        // Bluetooth bridge: a stereo copy of the block, never waiting (atomics only; bt.cpp clears tx and waits
        // for busy == 0 before it disables the channel, so the handle stays valid during the write)
        bool mute = false;
        if (bt_rate && __atomic_load_n(&detail::g_btTap.tx, __ATOMIC_RELAXED)) {
            __atomic_store_n(&detail::g_btTap.busy, 1u, __ATOMIC_SEQ_CST);
            i2s_chan_handle_t bridge = __atomic_load_n(&detail::g_btTap.tx, __ATOMIC_SEQ_CST);
            if (bridge) {   // one block = one bridge DMA buffer (bt.cpp sizes the channel from btAudioResume)
                size_t sent = 0;
                i2s_channel_write(bridge, s_buf, bytes, &sent, 0);
                // not silent (above ~-54 dBFS): the Pico's buffer is re-synced only in silence (bt.cpp)
                for (int i = 0; i < 2 * s_frames; i++) {
                    if (s_buf[i] > 64 || s_buf[i] < -64) {
                        __atomic_store_n(&detail::g_btTap.loud_ms, (uint32_t)(t0 / 1000), __ATOMIC_RELAXED);
                        break;
                    }
                }
                if (sent != bytes) __atomic_fetch_add(&detail::g_btTap.dropped, 1u, __ATOMIC_RELAXED);
                __atomic_store_n(&detail::g_btTap.armed, (uint8_t)1, __ATOMIC_RELAXED);   // underruns count now
                mute = __atomic_load_n(&detail::g_btTap.mute_local, __ATOMIC_RELAXED) != 0;
            }
            __atomic_store_n(&detail::g_btTap.busy, 0u, __ATOMIC_RELEASE);
        }
        if (mute) memset(s_buf, 0, bytes);
        else if (__atomic_load_n(&s_mono, __ATOMIC_RELAXED)) {
            for (int i = 0; i < s_frames; i++) {
                const int16_t m = (int16_t)(((int32_t)s_buf[2 * i] + (int32_t)s_buf[2 * i + 1]) >> 1);
                s_buf[2 * i] = m;
                s_buf[2 * i + 1] = m;
            }
        }
        const int64_t t1 = esp_timer_get_time();
        size_t written = 0;
        i2s_channel_write(s_tx, s_buf, bytes, &written, pdMS_TO_TICKS(20));
        const int64_t t2 = esp_timer_get_time();
        if (first) {
            // the start-up backlog of the send queue is being consumed: overflows count from now on
            first = false;
            __atomic_store_n(&s_xrun_armed, true, __ATOMIC_RELAXED);
        } else {
            noteMax(&s_period_max_us, t0 - prev);
        }
        const uint32_t render_us = (uint32_t)(t1 - t0);
        noteMax(&s_render_max_us, render_us);
        {   // audioLoad(): up at once, down over ~8 blocks
            const uint32_t pm = period_us ? (uint32_t)((uint64_t)render_us * 1000u / period_us) : 0u;
            const uint32_t cur = __atomic_load_n(&s_load_pm, __ATOMIC_RELAXED);
            __atomic_store_n(&s_load_pm, pm >= cur ? pm : cur - (cur - pm) / 8u, __ATOMIC_RELAXED);
        }
        noteMax(&s_write_max_us, t2 - t1);
        portENTER_CRITICAL(&s_hook_mux);
        AudioTimingHook hook = s_timing;
        void* user = s_timing_user;
        portEXIT_CRITICAL(&s_hook_mux);
        if (hook) hook(render_us, (uint32_t)(t0 - prev), user);
        prev = t0;
        // Overloaded (render ~ block period): the write never blocks, so nothing below this priority would
        // run on core 1 (IDLE1, the task watchdog's idle check). Sleep one tick now and then: that block
        // underruns, which the overload already does anyway.
        if (guard.block(render_us, period_us, kOverloadYieldBlocks)) vTaskDelay(1);
        __atomic_store_n(&s_overload_blocks, guard.total, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&detail::g_btTap.armed, (uint8_t)0, __ATOMIC_RELAXED);   // no more blocks: not underruns
    if (wdt) esp_task_wdt_delete(nullptr);
    xSemaphoreGive(s_task_done);
    vTaskDelete(nullptr);
}

bool codecStart(uint32_t rate)
{
    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port = (uint8_t)CONFIG_BSP_I2S_NUM;
    i2s_cfg.tx_handle = s_tx;
    // esp_codec_dev_open reconfigures the channel clock: keep the APLL (0 would mean the XTAL default)
    i2s_cfg.clk_src = (int)kClkSrc;
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    s_gpio_if = audio_codec_new_gpio();
    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.port = (uint8_t)BSP_I2C_NUM;
    i2c_cfg.addr = ES8388_CODEC_DEFAULT_ADDR;
    i2c_cfg.bus_handle = bsp_i2c_get_handle();
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!s_data_if || !s_gpio_if || !s_ctrl_if) return false;
    es8388_codec_cfg_t es = {};
    es.ctrl_if = s_ctrl_if;
    es.gpio_if = s_gpio_if;
    es.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es.master_mode = false;
    es.pa_pin = -1;
    es.pa_reverted = false;
    es.hw_gain.pa_voltage = 5.0f;
    es.hw_gain.codec_dac_voltage = 3.3f;
    s_codec_if = es8388_codec_new(&es);
    if (!s_codec_if) {
        ESP_LOGE(TAG, "ES8388 not answering");
        return false;
    }
    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    dev_cfg.codec_if = s_codec_if;
    dev_cfg.data_if = s_data_if;
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_dev) return false;
    detail::I2cCeiling ceiling;   // the codec's register writes: one transfer each, other devices in between
    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel = 2;
    fs.sample_rate = rate;
    fs.mclk_multiple = 256;
    if (esp_codec_dev_open(s_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open");
        return false;
    }
    s_dev_open = true;
    // The driver leaves LOUT2/ROUT2 at -30 dB (DACCONTROL26/27 = 0): same 0 dB as LOUT1/ROUT1
    esp_codec_dev_write_reg(s_dev, 0x30, 0x1E);
    esp_codec_dev_write_reg(s_dev, 0x31, 0x1E);
    esp_codec_dev_set_out_vol(s_dev, s_volume);
    esp_codec_dev_set_out_mute(s_dev, false);
    return true;
}
}  // namespace

namespace detail {
void audioAuxStart()
{
    ensureControl();
    if (s_aux_started) return;
    readHeadphones();
    s_aux_started = xTaskCreatePinnedToCore(auxTask, "orbit_aux", 4096, nullptr, 2, nullptr, 0) == pdPASS;
}

void audioDiag(Diagnostics* out, bool reset)
{
    if (out) {
        out->audio_render_max_us = __atomic_load_n(&s_render_max_us, __ATOMIC_RELAXED);
        out->audio_write_max_us = __atomic_load_n(&s_write_max_us, __ATOMIC_RELAXED);
        out->audio_period_max_us = __atomic_load_n(&s_period_max_us, __ATOMIC_RELAXED);
        out->audio_overload_blocks = __atomic_load_n(&s_overload_blocks, __ATOMIC_RELAXED);
        out->ioe_inputs = (int16_t)__atomic_load_n(&s_ioe_in, __ATOMIC_RELAXED);
        out->hp_det_trusted = kHpDetTrusted ? 1 : 0;
        out->audio_dma_buffers = (uint8_t)(s_running ? s_desc : 0);
    }
    if (reset) {
        __atomic_store_n(&s_render_max_us, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&s_write_max_us, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&s_period_max_us, 0u, __ATOMIC_RELAXED);
    }
}
}  // namespace detail

bool audioStart(uint32_t sample_rate, int block_frames, RenderFn render, void* user)
{
    if (!render) return false;
    ensureControl();
    audioStop();
    if (!detail::i2cReady()) return false;
    if (!s_ctl || !s_task_done || !s_task_ready) return false;
    xSemaphoreTake(s_ctl, portMAX_DELAY);
    s_rate = sample_rate == 44100 ? 44100 : 48000;
    int frames = block_frames < 16 ? 16 : block_frames > kMaxFrames ? kMaxFrames : block_frames;
    frames &= ~15;   // DMA buffer of whole 64-byte cache lines
    s_frames = frames;
    s_desc = logic::dmaDescFor(frames);
    s_render = render;
    s_render_user = user;
    __atomic_store_n(&s_xrun_armed, false, __ATOMIC_RELAXED);
    __atomic_store_n(&s_run, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_start_ok, false, __ATOMIC_RELEASE);
    (void)xSemaphoreTake(s_task_ready, 0);   // no stale signals of an earlier stream
    (void)xSemaphoreTake(s_task_done, 0);
    // The Bluetooth bridge channel lets go of the APLL first, so the codec channel sets it for s_rate.
    detail::btAudioSuspend();
    // The audio task creates and enables the I2S channel on core 1, then waits for the go below.
    bool task = xTaskCreatePinnedToCore(audioTask, "orbit_audio", 8192, nullptr, configMAX_PRIORITIES - 2, &s_task,
                                        1) == pdPASS;
    bool ok = task;
    if (task) {
        xSemaphoreTake(s_task_ready, portMAX_DELAY);   // i2sStart: no blocking waits, returns in milliseconds
        ok = __atomic_load_n(&s_start_ok, __ATOMIC_ACQUIRE);
    }
    // the codec (I2C) from here; esp_codec_dev_open disables / reconfigures / enables the channel, which
    // keeps its interrupt where it was allocated
    if (ok) ok = codecStart(s_rate);
    if (task) {
        __atomic_store_n(&s_run, ok, __ATOMIC_RELEASE);
        if (__atomic_load_n(&s_start_ok, __ATOMIC_ACQUIRE)) xTaskNotifyGive(s_task);   // go, or leave
        if (!ok) xSemaphoreTake(s_task_done, portMAX_DELAY);   // the task has left: safe to tear down
    }
    if (!ok) {
        s_task = nullptr;
        codecTeardown();
        detail::btAudioResume(0, frames);
        xSemaphoreGive(s_ctl);
        ESP_LOGE(TAG, "audio start failed");
        return false;
    }
    s_running = true;
    __atomic_store_n(&s_load_pm, 0u, __ATOMIC_RELAXED);   // audioLoad() of the new stream
    s_spk_state = -1;
    applyOutputLocked();
    detail::btAudioResume(s_rate, frames);
    xSemaphoreGive(s_ctl);
    detail::audioAuxStart();   // HP_DET polling (normally already started by boardInit)
    ESP_LOGI(TAG, "audio %lu Hz, %d frames x %d DMA buffers, latency %.1f ms, MCLK from APLL", (unsigned long)s_rate,
             frames, s_desc, (double)audioLatencyMs());
    return true;
}

void audioStop()
{
    ensureControl();
    xSemaphoreTake(s_ctl, portMAX_DELAY);
    if (s_running) {
        // stop counting first: the DMA keeps cycling (silence) until the channel is disabled
        __atomic_store_n(&s_xrun_armed, false, __ATOMIC_RELAXED);
        __atomic_store_n(&s_run, false, __ATOMIC_RELEASE);
        // the task leaves after its current write (at most ~20 ms)
        if (xSemaphoreTake(s_task_done, pdMS_TO_TICKS(500)) != pdTRUE) ESP_LOGW(TAG, "audio task did not stop");
        s_task = nullptr;
        s_running = false;
        // amplifier off before the codec powers down: no pop from the speaker
        esp_io_expander_handle_t io = s_spk_state == 1 ? detail::ioe1() : nullptr;
        detail::I2cCeiling ceiling;
        if (io && esp_io_expander_set_level(io, kSpkEnPin, 0) == ESP_OK) s_spk_state = 0;
    }
    codecTeardown();
    xSemaphoreGive(s_ctl);
}

bool audioRunning() { return s_running; }

float audioLoad() { return s_running ? (float)__atomic_load_n(&s_load_pm, __ATOMIC_RELAXED) * 0.001f : 0.0f; }

void audioSetVolume(int percent)
{
    ensureControl();
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    xSemaphoreTake(s_ctl, portMAX_DELAY);
    s_volume = percent;
    if (s_dev) {
        detail::I2cCeiling ceiling;   // the volume knob: the LVGL task (4)
        esp_codec_dev_set_out_vol(s_dev, percent);
    }
    xSemaphoreGive(s_ctl);
}

void audioSetOutput(orbit::AudioOut out)
{
    ensureControl();
    xSemaphoreTake(s_ctl, portMAX_DELAY);
    s_out_mode = out;
    applyOutputLocked();
    xSemaphoreGive(s_ctl);
}

bool headphonesInserted() { return __atomic_load_n(&s_hp, __ATOMIC_RELAXED); }

float audioLatencyMs()
{
    // DMA queue (3 or 4 blocks) + the block being rendered
    return (float)((s_desc + 1) * s_frames) * 1000.0f / (float)s_rate;
}

void audioSetTimingHook(AudioTimingHook fn, void* user)
{
    portENTER_CRITICAL(&s_hook_mux);
    s_timing = fn;
    s_timing_user = user;
    portEXIT_CRITICAL(&s_hook_mux);
}

uint32_t audioXruns() { return __atomic_load_n(&s_xruns, __ATOMIC_RELAXED); }

}  // namespace hal
