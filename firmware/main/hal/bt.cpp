// Bluetooth audio through the Aurora Player "Pico Bluetooth bridge" (hal.h, "Bluetooth audio"): an M5Stamp
// Pico (ESP32-PICO-D4, bridge firmware 0.9.x) takes 16-bit stereo I2S at 44.1 kHz from the Tab5 and sends it to
// A2DP headphones / speakers; a 115200 8N1 text protocol controls it. The protocol and the link state machine
// are portable (hal/bt_protocol.h, tested on the PC); this file is the ESP-IDF side:
//  - task "orbit_bt" (core 0, priority 4), created by the first btEnable(true) (nothing at boot): every 20 ms
//    UART bytes -> Link::feed, Link::step, commands -> UART, then the outputs below. It sleeps on a task
//    notification while Bluetooth is off and nothing is allocated;
//  - UART2, TX G6 -> Pico G32, RX G46 <- Pico G33 (pull-up), open while the link is not Off. The wake pulse turns
//    G6 into a GPIO driven low for 120 ms, then back to UART TX. On release G6 is left an input with pull-up: the
//    sleeping Pico wakes on a low level of G32, so the line must stay high.
//    The UART is a minimal driver on the ESP-IDF LL layer (IRAM receive interrupt -> 1 KB ring, transmit straight
//    into the 128-byte FIFO): the IDF uart driver would add ~18 KB of flash (events, RS485, wake-up, select...).
//    ORBIT_BT_IDF_UART=1 builds the same with the IDF driver (fallback);
//  - the bridge channel I2S_NUM_0 (TX master, Philips 16-bit stereo, 44.1 kHz, no MCLK pin: BCK G5, WS G18, DOUT
//    G19, auto-clear) from the codec's clock source (APLL, MCLK 256 fs) so both channels share one sample clock.
//    Its DMA buffers are one audio block each (the audio task writes whole buffers: the driver's "queue almost
//    full" rule never abandons a half-filled one) and there are 4 more of them than the codec has, codec + 2
//    preloaded with silence: the audio task writes when the codec lets it, at worst a whole codec queue late, and
//    still finds a free buffer before the bridge DMA runs dry (8.7 / 14.5 / 29 ms more at 64 / 128 / 256 frames,
//    next to 150-300 ms of A2DP). An underrun while the audio task feeds it counts into dropped_blocks.
//    Created while Bluetooth is enabled and the codec runs at 44.1 kHz (or not at all), never at 48 kHz; enabled
//    while a stream is requested. The audio task copies its blocks into it (audio.cpp, detail::g_btTap);
//    audioStart releases it around the codec start (detail::btAudioSuspend / btAudioResume: rate and block size)
//    because the APLL frequency is fixed while two channels hold it.
//  - the hal::bt* calls only take a short mutex (the task holds it for feed + step, never for I/O).
// Nothing here is time-critical (20 ms ticks, 115200 baud): optimised for size, also the inline protocol code.
#pragma GCC optimize("Os")
#ifndef ORBIT_BT_IDF_UART
#define ORBIT_BT_IDF_UART 0
#endif
#include <new>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#if ORBIT_BT_IDF_UART
#include "driver/uart.h"
#else
#include "esp_clk_tree.h"
#include "esp_intr_alloc.h"
#include "esp_private/gpio.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wliteral-suffix"   // periph_ctrl.h: "..."CONFIG_IDF_TARGET in C++
#include "esp_private/uart_share_hw_ctrl.h"
#pragma GCC diagnostic pop
#include "esp_rom_gpio.h"
#include "hal/uart_ll.h"
#include "soc/gpio_sig_map.h"
#include "soc/interrupts.h"
#include "soc/io_mux_reg.h"
#endif
#include "hal/bt_protocol.h"
#include "hal/hal.h"
#include "hal/hal_logic.h"
#include "hal/hal_private.h"

namespace hal {

namespace detail {
BtAudioTap g_btTap = {};
}

namespace {
const char* TAG = "orbit_bt";
constexpr uart_port_t kUart = UART_NUM_2;
constexpr gpio_num_t kTxPin = GPIO_NUM_6;    // -> Pico G32: commands + EXT0 wake
constexpr gpio_num_t kRxPin = GPIO_NUM_46;   // <- Pico G33
constexpr gpio_num_t kBckPin = GPIO_NUM_5, kWsPin = GPIO_NUM_18, kDoutPin = GPIO_NUM_19;
constexpr uint32_t kBaud = 115200;
constexpr uint32_t kBridgeRate = 44100;
constexpr uint32_t kSuspended = 1;           // s_audio_rate while audioStart sets the codec channel up
constexpr uint32_t kTickMs = 20;

struct Ctx {   // allocated (PSRAM) by the first btEnable(true)
    btp::Link link;
    uint8_t rx[1024];
    char tx[btp::kTxMax];
    char shown[96];   // last message logged
    // the Pico's buffer in the log (0.4.4): every ~10 s while streaming, and each re-sync
    int32_t floor_seen;
    uint32_t resyncs_seen, buf_log_ms;
};
Ctx* s_ctx = nullptr;
TaskHandle_t s_task = nullptr;
// under lockMx(): s_ctx->link and these
bool s_enabled = false, s_route = false, s_local = true;
uint32_t s_audio_rate = 0;    // __atomic: the codec rate from audio.cpp (0 stopped, kSuspended)
uint32_t s_audio_frames = 0;  // __atomic: the stream's block size from audio.cpp (0 unknown)
// bridge task only
uint32_t s_link_rate = 0;
bool s_uart = false, s_low = false, s_uart_failed = false;
btp::Phase s_phase = btp::Phase::Off;
// under i2sMx(): bridge task and audioStart
i2s_chan_handle_t s_bridge = nullptr;
bool s_bridge_on = false;
int64_t s_bridge_retry_us = 0;
uint32_t s_bridge_preload = 0;   // bytes of silence loaded before each enable (codec + 2 buffers)

SemaphoreHandle_t lockMx()
{
    static SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return m;
}
SemaphoreHandle_t i2sMx()
{
    static SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return m;
}
bool lock() { return lockMx() && xSemaphoreTake(lockMx(), pdMS_TO_TICKS(50)) == pdTRUE; }
void unlock() { xSemaphoreGive(lockMx()); }

// ---------------------------------------------------------------- UART2 (bridge task)
#if ORBIT_BT_IDF_UART
bool uartOpen()
{
    uart_config_t cfg = {};
    cfg.baud_rate = (int)kBaud;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    esp_err_t err = uart_driver_install(kUart, 1024, 512, 0, nullptr, 0);
    if (err == ESP_OK) err = uart_param_config(kUart, &cfg);
    if (err == ESP_OK) err = uart_set_pin(kUart, kTxPin, kRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err == ESP_OK) gpio_pullup_en(kRxPin);
    else if (uart_is_driver_installed(kUart)) uart_driver_delete(kUart);
    return err == ESP_OK;
}
void uartClose() { uart_driver_delete(kUart); }
int uartRead(uint8_t* out, int max)
{
    const int r = uart_read_bytes(kUart, out, (uint32_t)max, 0);
    return r > 0 ? r : 0;
}
int uartRoom() { return 128; }
void uartWrite(const char* s, int n) { uart_write_bytes(kUart, s, (size_t)n); }
void uartDrain() { uart_wait_tx_done(kUart, pdMS_TO_TICKS(50)); }
void uartFlushInput() { uart_flush_input(kUart); }
void uartTxToUart() { uart_set_pin(kUart, kTxPin, kRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE); }
#else
constexpr uint32_t kRing = 1024;   // power of two
uint8_t* s_ring = nullptr;         // internal RAM: the interrupt also runs while the flash cache is off
uint32_t s_head = 0, s_tail = 0;   // __atomic: the interrupt advances head, the task tail
intr_handle_t s_intr = nullptr;

void IRAM_ATTR uartIsr(void*)
{
    uart_dev_t* hw = &UART2;
    const uint32_t st = uart_ll_get_intsts_mask(hw);
    uint32_t h = s_head;
    const uint32_t t = __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE);
    for (uint32_t n = uart_ll_get_rxfifo_len(hw); n; --n) {
        uint8_t b;
        uart_ll_read_rxfifo(hw, &b, 1);
        if (h - t < kRing) s_ring[h++ & (kRing - 1)] = b;   // ring full: dropped (that line is rejected)
    }
    if (st & UART_INTR_RXFIFO_OVF) uart_ll_rxfifo_rst(hw);
    __atomic_store_n(&s_head, h, __ATOMIC_RELEASE);
    uart_ll_clr_intsts_mask(hw, st);
}

void uartClocks(bool on)
{
    uart_dev_t* hw = &UART2;
    if (!on) {
        HP_UART_SRC_CLK_ATOMIC() { uart_ll_sclk_disable(hw); }
        HP_UART_BUS_CLK_ATOMIC() { uart_ll_enable_bus_clock(kUart, false); }
        return;
    }
    uint32_t xtal = 40000000;
    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_XTAL, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &xtal);
    HP_UART_BUS_CLK_ATOMIC() {
        uart_ll_enable_bus_clock(kUart, true);
        uart_ll_reset_register(kUart);
    }
    HP_UART_SRC_CLK_ATOMIC() {
        uart_ll_set_sclk(hw, (soc_module_clk_t)UART_SCLK_XTAL);
        uart_ll_sclk_enable(hw);
        uart_ll_set_baudrate(hw, kBaud, xtal);
    }
}

bool uartOpen()
{
    if (!s_ring) s_ring = (uint8_t*)heap_caps_malloc(kRing, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_ring) return false;
    uart_dev_t* hw = &UART2;
    gpio_set_level(kTxPin, 1);   // G6 high as a GPIO until the UART drives it
    gpio_set_direction(kTxPin, GPIO_MODE_OUTPUT);
    gpio_func_sel(kRxPin, PIN_FUNC_GPIO);
    gpio_set_direction(kRxPin, GPIO_MODE_INPUT);
    gpio_pullup_en(kRxPin);      // a missing or sleeping Pico leaves the line floating
    esp_rom_gpio_connect_in_signal(kRxPin, UART2_RXD_PAD_IN_IDX, false);
    uartClocks(true);
    uart_ll_set_data_bit_num(hw, UART_DATA_8_BITS);
    uart_ll_set_parity(hw, UART_PARITY_DISABLE);
    uart_ll_set_stop_bits(hw, UART_STOP_BITS_1);
    uart_ll_set_hw_flow_ctrl(hw, UART_HW_FLOWCTRL_DISABLE, 0);
    uart_ll_set_tx_idle_num(hw, 0);
    uart_ll_set_rxfifo_full_thr(hw, 64);
    uart_ll_set_rx_tout(hw, 100);   // 10 characters of silence: the interrupt takes what is in the FIFO
    uart_ll_rxfifo_rst(hw);
    uart_ll_txfifo_rst(hw);
    uart_ll_clr_intsts_mask(hw, UINT32_MAX);
    s_head = s_tail = 0;
    if (esp_intr_alloc(ETS_UART2_INTR_SOURCE, ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LOWMED, uartIsr, nullptr, &s_intr) !=
        ESP_OK) {
        uartClocks(false);
        return false;
    }
    uart_ll_ena_intr_mask(hw, UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT | UART_INTR_RXFIFO_OVF);
    esp_rom_gpio_connect_out_signal(kTxPin, UART2_TXD_PAD_OUT_IDX, false, false);   // TX idles high
    return true;
}

void uartClose()
{
    uart_ll_disable_intr_mask(&UART2, UINT32_MAX);
    esp_intr_free(s_intr);
    s_intr = nullptr;
    uartClocks(false);
}

int uartRead(uint8_t* out, int max)
{
    const uint32_t h = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
    uint32_t t = s_tail;
    int n = 0;
    while (t != h && n < max) out[n++] = s_ring[t++ & (kRing - 1)];
    __atomic_store_n(&s_tail, t, __ATOMIC_RELEASE);
    return n;
}
int uartRoom() { return (int)uart_ll_get_txfifo_len(&UART2); }
void uartWrite(const char* s, int n) { uart_ll_write_txfifo(&UART2, (const uint8_t*)s, (uint32_t)n); }
void uartDrain()
{
    for (int i = 0; i < 50 && !uart_ll_is_tx_idle(&UART2); ++i) vTaskDelay(1);
}
void uartFlushInput()
{
    uart_ll_rxfifo_rst(&UART2);
    __atomic_store_n(&s_tail, __atomic_load_n(&s_head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
}
void uartTxToUart() { esp_rom_gpio_connect_out_signal(kTxPin, UART2_TXD_PAD_OUT_IDX, false, false); }
#endif

void uartApply(bool want, bool low)
{
    if (want && !s_uart && !s_uart_failed) {
        s_uart = uartOpen();
        if (!s_uart) {
            ESP_LOGE(TAG, "UART2 (G6/G46) failed");
            s_uart_failed = true;   // until Bluetooth is switched off and on again
        }
    }
    if (!s_uart) return;
    if (low != s_low) {
        if (low) {   // wake pulse: G6 as a plain GPIO, low
            uartDrain();
            gpio_set_level(kTxPin, 0);
            gpio_set_direction(kTxPin, GPIO_MODE_OUTPUT);
        } else {     // high, then UART TX again (idle high); what arrived meanwhile is dropped
            gpio_set_level(kTxPin, 1);
            uartTxToUart();
            uartFlushInput();
        }
        s_low = low;
    }
    if (!want) {
        uartDrain();
        gpio_set_level(kTxPin, 1);   // G6 high as a GPIO before the UART goes: no low glitch
        gpio_set_direction(kTxPin, GPIO_MODE_OUTPUT);
        uartClose();
        gpio_reset_pin(kTxPin);      // input with pull-up: stays high
        gpio_reset_pin(kRxPin);
        s_uart = false;
        s_low = false;
    }
}

// ---------------------------------------------------------------- bridge I2S (i2sMx held)
void bridgeOff(bool del)
{
    if (s_bridge && s_bridge_on) {
        // out of the audio path first: the audio task never writes to a disabled channel
        __atomic_store_n(&detail::g_btTap.mute_local, (uint8_t)0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&detail::g_btTap.tx, (i2s_chan_handle_t) nullptr, __ATOMIC_SEQ_CST);
        for (int i = 0; i < 50 && __atomic_load_n(&detail::g_btTap.busy, __ATOMIC_SEQ_CST); ++i) vTaskDelay(1);
        __atomic_store_n(&detail::g_btTap.armed, (uint8_t)0, __ATOMIC_SEQ_CST);   // no blocks come: not underruns
        i2s_channel_disable(s_bridge);
        s_bridge_on = false;
    }
    if (s_bridge && del) {
        i2s_del_channel(s_bridge);   // releases its APLL reference
        s_bridge = nullptr;
    }
}

// DMA ran dry (the send queue overflowed: every buffer played, none refilled) while the audio task feeds it
bool IRAM_ATTR bridgeUnderrun(i2s_chan_handle_t, i2s_event_data_t*, void*)
{
    if (__atomic_load_n(&detail::g_btTap.armed, __ATOMIC_RELAXED))
        __atomic_fetch_add(&detail::g_btTap.dropped, 1u, __ATOMIC_RELAXED);
    return false;
}

i2s_chan_handle_t bridgeCreate()
{
    // One buffer per audio block, 4 more than the codec has (64 frames: 8 x 256 B, 128: 7 x 512 B, 256: 7 x 1 KB)
    uint32_t frames = __atomic_load_n(&s_audio_frames, __ATOMIC_ACQUIRE);
    if (frames < 16 || frames > 256) frames = 128;   // no stream yet: the bridge plays silence on its own
    const int codec = logic::dmaDescFor((int)frames);
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = (uint32_t)codec + 4;
    chan.dma_frame_num = frames;
    chan.auto_clear = true;     // an underrun sends silence, not old audio
    s_bridge_preload = (uint32_t)(codec + 2) * frames * 4;
    i2s_chan_handle_t tx = nullptr;
    if (i2s_new_channel(&chan, &tx, nullptr) != ESP_OK) return nullptr;
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kBridgeRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {},
    };
    std.clk_cfg.clk_src = detail::kAudioClkSrc;   // the codec's APLL: one sample clock for both
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std.gpio_cfg.bclk = kBckPin;
    std.gpio_cfg.ws = kWsPin;
    std.gpio_cfg.dout = kDoutPin;
    std.gpio_cfg.din = I2S_GPIO_UNUSED;
    i2s_event_callbacks_t cbs = {};
    cbs.on_send_q_ovf = bridgeUnderrun;
    if (i2s_channel_init_std_mode(tx, &std) != ESP_OK || i2s_channel_register_event_callback(tx, &cbs, nullptr) != ESP_OK) {
        i2s_del_channel(tx);
        return nullptr;
    }
    return tx;
}

// returns whether a channel exists afterwards
bool bridgeApply(bool enabled, bool stream, bool acked, bool local)
{
    xSemaphoreTake(i2sMx(), portMAX_DELAY);
    const uint32_t rate = __atomic_load_n(&s_audio_rate, __ATOMIC_ACQUIRE);   // then s_audio_frames (bridgeCreate)
    const bool chan = enabled && (rate == 0 || rate == kBridgeRate);   // never at 48 kHz / during audioStart
    const bool on = chan && stream;
    if (!on) bridgeOff(!chan);
    if (chan && !s_bridge && esp_timer_get_time() >= s_bridge_retry_us) {
        s_bridge = bridgeCreate();
        if (!s_bridge) {
            ESP_LOGE(TAG, "bridge I2S (I2S_NUM_0) failed");
            s_bridge_retry_us = esp_timer_get_time() + 5000000;
        }
    }
    if (on && s_bridge && !s_bridge_on) {
        // codec + 2 buffers of silence: the first block lands that far ahead of the DMA, 2 buffers stay free
        const uint8_t zeros[128] = {};
        for (uint32_t done = 0; done < s_bridge_preload;) {
            size_t n = 0;
            if (i2s_channel_preload_data(s_bridge, zeros, sizeof zeros, &n) != ESP_OK || !n) break;
            done += (uint32_t)n;
        }
        s_bridge_on = i2s_channel_enable(s_bridge) == ESP_OK;
        if (s_bridge_on) __atomic_store_n(&detail::g_btTap.tx, s_bridge, __ATOMIC_SEQ_CST);
    }
    if (s_bridge_on) __atomic_store_n(&detail::g_btTap.mute_local, (uint8_t)(acked && !local), __ATOMIC_RELAXED);
    const bool exists = s_bridge != nullptr;
    xSemaphoreGive(i2sMx());
    return exists;
}

// ---------------------------------------------------------------- the bridge task
const char* phaseName(btp::Phase p)
{
    static const char* const names[] = {"off", "wake pulse", "boot wait", "awake", "no module", "going to sleep"};
    return names[(int)p];
}

void btTask(void*)
{
    Ctx& c = *s_ctx;
    for (;;) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        const int n = s_uart && !s_low ? uartRead(c.rx, (int)sizeof c.rx) : 0;
        // output room: the free TX FIFO; none during the pulse (kept for later); without a UART it is dropped
        const int room = !s_uart ? (s_uart_failed ? btp::kTxMax : 0) : s_low ? 0 : uartRoom();
        const uint32_t rate = __atomic_load_n(&s_audio_rate, __ATOMIC_RELAXED);
        if (rate != kSuspended) s_link_rate = rate;
        char msg[96] = {};
        xSemaphoreTake(lockMx(), portMAX_DELAY);
        btp::Link& l = c.link;
        if (n) l.feed(c.rx, n, now);
        l.setDropped(__atomic_load_n(&detail::g_btTap.dropped, __ATOMIC_RELAXED));
        const uint32_t quiet = now - __atomic_load_n(&detail::g_btTap.loud_ms, __ATOMIC_RELAXED);
        const btp::Inputs in{s_enabled, s_route, s_link_rate, quiet};
        l.step(now, in);
        const int ntx = l.takeTx(c.tx, room);
        const bool want_uart = l.uartOn(), low = l.wakeLow(), stream = l.streamOn(), acked = l.streamAcked();
        const bool enabled = s_enabled, local = s_local;
        const btp::Phase phase = l.phase();
        if (strcmp(c.shown, l.status().message)) {
            memcpy(msg, l.status().message, sizeof msg);
            memcpy(c.shown, msg, sizeof msg);
        }
        const hal::BtStatus& bs = l.status();
        const int32_t buf = bs.pico_buffered, floor = bs.pico_floor;
        const uint32_t under = bs.pico_underruns, over = bs.pico_overflows, resyncs = bs.resyncs;
        xSemaphoreGive(lockMx());
        if (phase != s_phase) {
            ESP_LOGI(TAG, "link: %s", phaseName(phase));
            s_phase = phase;
        }
        if (msg[0]) ESP_LOGI(TAG, "%s", msg);
        // the Pico's buffer (frames at 44.1 kHz; the floor is the ring between Bluedroid's 30 ms pulls)
        if (resyncs > c.resyncs_seen)
            ESP_LOGI(TAG, "Pico buffer re-sync %lu in silence: floor was %ld frames (%ld ms)", (unsigned long)resyncs,
                     (long)c.floor_seen, (long)(c.floor_seen * 10 / 441));
        c.resyncs_seen = resyncs;   // (back to 0 when Bluetooth is enabled again)
        if (floor >= 0) c.floor_seen = floor;
        if (acked && buf >= 0 && now - c.buf_log_ms >= 10000) {
            c.buf_log_ms = now;
            ESP_LOGI(TAG, "Pico buffer: now %ld, floor %ld frames (~%ld ms), underruns %lu, overflow frames %lu",
                     (long)buf, (long)floor, floor >= 0 ? (long)((floor + 661) * 10 / 441) : -1L,
                     (unsigned long)under, (unsigned long)over);
        }
        if (!enabled && phase == btp::Phase::Off) s_uart_failed = false;
        if (ntx > 0 && s_uart && !s_low) uartWrite(c.tx, ntx);
        uartApply(want_uart, low);
        const bool chan = bridgeApply(enabled, stream, acked, local);
        if (!enabled && phase == btp::Phase::Off && !s_uart && !chan) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        else vTaskDelay(pdMS_TO_TICKS(kTickMs));
    }
}

// starts the task (and its context) once; lock() held
bool ensureTask()
{
    if (s_task) return true;
    if (!s_ctx) {
        void* mem = heap_caps_calloc(1, sizeof(Ctx), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!mem) mem = heap_caps_calloc(1, sizeof(Ctx), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!mem) return false;
        s_ctx = new (mem) Ctx;
        s_ctx->link.reset();
    }
    if (xTaskCreatePinnedToCore(btTask, "orbit_bt", 4096, nullptr, 4, &s_task, 0) != pdPASS) {
        s_task = nullptr;
        ESP_LOGE(TAG, "task");
        return false;
    }
    return true;
}

template <class F>
bool command(F f)
{
    if (!s_ctx || !lock()) return false;
    const bool ok = s_enabled && f(s_ctx->link);
    unlock();
    return ok;
}
}  // namespace

// ---------------------------------------------------------------- audio.cpp coupling
namespace detail {
void btAudioSuspend()
{
    xSemaphoreTake(i2sMx(), portMAX_DELAY);
    __atomic_store_n(&s_audio_rate, kSuspended, __ATOMIC_RELAXED);
    bridgeOff(true);
    xSemaphoreGive(i2sMx());
}

void btAudioResume(uint32_t rate, int frames)
{
    __atomic_store_n(&s_audio_frames, (uint32_t)frames, __ATOMIC_RELEASE);   // read by bridgeCreate after the rate
    __atomic_store_n(&s_audio_rate, rate, __ATOMIC_RELEASE);
    if (s_task) xTaskNotifyGive(s_task);
}
}  // namespace detail

// ---------------------------------------------------------------- hal.h
bool btEnable(bool on)
{
    if (!lock()) return false;
    const bool ok = !on || ensureTask();
    if (ok) s_enabled = on;
    unlock();
    if (s_task) xTaskNotifyGive(s_task);
    return ok;
}

void btStatus(BtStatus& out)
{
    if (s_ctx && lock()) {
        out = s_ctx->link.status();
        unlock();
        return;
    }
    memset(&out, 0, sizeof out);
    out.airpods_left = out.airpods_right = out.airpods_case = out.airpods_mode = -1;
}

int btDevices(BtDevice* out, int max)
{
    if (!s_ctx || !lock()) return 0;
    const int n = s_ctx->link.devices(out, max);
    unlock();
    return n;
}

bool btScan() { return command([](btp::Link& l) { return l.queue("SCAN"); }); }
bool btSelect(const char* mac) { return command([mac](btp::Link& l) { return btp::cmdSelect(l, mac); }); }
bool btReconnect() { return command([](btp::Link& l) { return l.queue("RECONNECT"); }); }
bool btTest() { return command([](btp::Link& l) { return l.queue("TEST"); }); }
bool btSetVolume(int percent) { return command([percent](btp::Link& l) { return l.setVolume(percent); }); }
bool btAirpodsMode(int mode) { return command([mode](btp::Link& l) { return btp::cmdAirpodsMode(l, mode); }); }

BtRemote btPopRemote()
{
    if (!s_ctx || !lock()) return BtRemote::None;
    const BtRemote r = s_ctx->link.popRemote();
    unlock();
    return r;
}

void btSetAudioRoute(bool to_bridge, bool local)
{
    if (local) __atomic_store_n(&detail::g_btTap.mute_local, (uint8_t)0, __ATOMIC_RELAXED);   // at once
    if (!lock()) return;
    s_route = to_bridge;
    s_local = local;
    unlock();
}

}  // namespace hal
