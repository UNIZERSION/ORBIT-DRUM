// M5Stack Tab5 Keyboard (SKU A164) on the Tab5's side port Ext.Port1 (0.4.6), after the Tab5 launcher's driver
// (tab5-launcher main/l5_kbd.c, same author). An STM32 on its own I2C bus (GPIO 0 SDA, GPIO 1 SCL, port 0 — the
// BSP's system bus is port 1 — 100 kHz), address 0x6D, INT on GPIO 50 (low while events wait), switched to HID
// mode: each event is the HID modifier byte and a usage ID (0 = released). Register map: M5Stack's keyboard firmware
// (M5Tab5-Keyboard-Internal-FW, user_i2c_reg.h) and its drivers (M5Unit-KEYBOARD, M5Tab5-Keyboard-UserDemo).
// A worker (core 0, priority 2) looks for the keyboard every 2 s (hot plug), reads its events on INT or every
// 100 ms, repeats held navigation keys and queues KeyEvents; the LVGL task takes them (ui/keys.cpp). At a restart
// (BACK TO LAUNCHER) the bus is deleted and the pins reset, so the next app finds them as after power-on.
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/hal.h"

namespace hal {

namespace {
const char* TAG = "orbit_kbd";
constexpr i2c_port_num_t kPort = I2C_NUM_0;
constexpr gpio_num_t kSda = GPIO_NUM_0, kScl = GPIO_NUM_1, kInt = GPIO_NUM_50;
constexpr uint16_t kAddr = 0x6D;
constexpr uint32_t kHz = 100000;
// registers
constexpr uint8_t kRegIntCfg = 0x00;   // bit 1 HID
constexpr uint8_t kRegIntSta = 0x01;   // write 0: clear, release INT
constexpr uint8_t kRegEvents = 0x02;   // events queued; write 0: flush
constexpr uint8_t kRegMode = 0x10;     // 1 = HID
constexpr uint8_t kRegHid = 0x30;      // 2 bytes per read: modifier, usage (FF FF = empty)
constexpr uint8_t kRegFirmware = 0xFE;
constexpr uint8_t kRegAddr = 0xFF;
constexpr uint8_t kModeHid = 1, kIntHid = 0x02;
constexpr int kFifo = 32;
constexpr int64_t kLookMs = 2000, kPollMs = 100, kCheckMs = 1000;
constexpr int64_t kRepeatDelayMs = 450, kRepeatEveryMs = 60, kRepeatMaxMs = 10000;

i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_dev = nullptr;
SemaphoreHandle_t s_lock = nullptr;
QueueHandle_t s_keys = nullptr;
volatile bool s_present = false, s_stop = false;
struct {
    KeyEvent key;
    bool active;
    int64_t since, next;
} s_rep;

esp_err_t wr(uint8_t reg, uint8_t v)
{
    const uint8_t w[2] = {reg, v};
    return i2c_master_transmit(s_dev, w, sizeof w, 50);
}
esp_err_t rd(uint8_t reg, uint8_t* data, size_t n) { return i2c_master_transmit_receive(s_dev, &reg, 1, data, n, 50); }
int64_t nowMs() { return esp_timer_get_time() / 1000; }

void push(const KeyEvent& k)
{
    if (xQueueSend(s_keys, &k, 0) != pdTRUE) {   // nobody takes them: start over
        xQueueReset(s_keys);
        s_rep.active = false;
    }
}

void gone()
{
    s_present = false;
    s_rep.active = false;
    KeyEvent r = {};
    r.key = Key::Release;
    push(r);   // whatever was held is up now
    ESP_LOGI(TAG, "keyboard gone");
}

esp_err_t setupHid()
{
    esp_err_t e = wr(kRegMode, kModeHid);
    if (e == ESP_OK) e = wr(kRegIntCfg, kIntHid);
    if (e == ESP_OK) e = wr(kRegEvents, 0);
    if (e == ESP_OK) e = wr(kRegIntSta, 0);
    return e;
}

bool probe(uint8_t* fw, uint8_t* mode)
{
    // an empty port fails every 2 s: the I2C driver's errors stay out of the log for the probe
    const esp_log_level_t level = esp_log_level_get("i2c.master");
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    uint8_t addr = 0;
    const bool ok = rd(kRegFirmware, fw, 1) == ESP_OK && rd(kRegAddr, &addr, 1) == ESP_OK && addr == kAddr &&
                    rd(kRegMode, mode, 1) == ESP_OK;
    esp_log_level_set("i2c.master", level);
    return ok;
}

const char kNum[] = "1234567890", kNumS[] = "!@#$%^&*()";
const char kSym[] = "-=[]\\#;'`,./", kSymS[] = "_+{}|~:\"~<>?";   // usage IDs 0x2D..0x38

void event(uint8_t mod, uint8_t usage, int64_t now)
{
    KeyEvent k = {};
    k.ctrl = (mod & 0x11) != 0;
    k.shift = (mod & 0x22) != 0;
    k.alt = (mod & 0x44) != 0;
    k.usage = usage;
    if (!usage) {
        s_rep.active = false;
        k.key = Key::Release;
        push(k);
        return;
    }
    if (usage >= 0x04 && usage <= 0x1D) {
        k.key = Key::Char;
        k.ch = (char)((k.shift ? 'A' : 'a') + usage - 0x04);
    } else if (usage >= 0x1E && usage <= 0x27) {
        k.key = Key::Char;
        k.ch = (k.shift ? kNumS : kNum)[usage - 0x1E];
    } else if (usage == 0x2C) {
        k.key = Key::Char;
        k.ch = ' ';
    } else if (usage >= 0x2D && usage <= 0x38) {
        k.key = Key::Char;
        k.ch = (k.shift ? kSymS : kSym)[usage - 0x2D];
    } else if (usage >= 0x3A && usage <= 0x45) {
        k.key = (Key)((int)Key::F1 + usage - 0x3A);
    } else {
        switch (usage) {
        case 0x28: case 0x58: k.key = Key::Enter; break;
        case 0x29: k.key = Key::Esc; break;
        case 0x2A: k.key = Key::Backspace; break;
        case 0x2B: k.key = Key::Tab; break;
        case 0x4A: k.key = Key::Home; break;
        case 0x4B: k.key = Key::PgUp; break;
        case 0x4C: k.key = Key::Delete; break;
        case 0x4D: k.key = Key::End; break;
        case 0x4E: k.key = Key::PgDn; break;
        case 0x4F: k.key = Key::Right; break;
        case 0x50: k.key = Key::Left; break;
        case 0x51: k.key = Key::Down; break;
        case 0x52: k.key = Key::Up; break;
        default: s_rep.active = false; return;   // Caps Lock, Print Screen...: nothing in ORBIT
        }
    }
    push(k);
    // held navigation keys repeat (arrows, Backspace, Delete, PgUp / PgDn, Home / End); characters do not: in ORBIT
    // they are the note keys and shortcuts (the on-screen text field gets a repeated character once per press)
    const bool repeats = !k.ctrl && !k.alt &&
                         (k.key == Key::Backspace || k.key == Key::Delete || (k.key >= Key::Left && k.key <= Key::PgDn));
    s_rep.key = k;
    s_rep.key.repeat = true;
    s_rep.active = repeats;
    s_rep.since = now;
    s_rep.next = now + kRepeatDelayMs;
}

bool readEvents(int64_t now)
{
    uint8_t n = 0;
    if (rd(kRegEvents, &n, 1) != ESP_OK) return false;
    if (n > kFifo) return wr(kRegEvents, 0) == ESP_OK && wr(kRegIntSta, 0) == ESP_OK;   // not a count: start over
    for (unsigned i = 0; i < n; i++) {
        uint8_t e[2];
        if (rd(kRegHid, e, sizeof e) != ESP_OK) return false;
        if ((e[0] & e[1]) == 0xFF) break;   // empty
        event(e[0], e[1], now);
    }
    return wr(kRegIntSta, 0) == ESP_OK;   // releases INT
}

void worker(void*)
{
    bool first = true;
    int64_t looked = 0, polled = 0, checked = 0;
    for (;;) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_stop) vTaskDelete(nullptr);   // the restart took the bus (the lock stays with it)
            const int64_t now = nowMs();
            if (!s_present) {
                if (first || now - looked >= kLookMs) {
                    first = false;
                    looked = polled = checked = now;
                    uint8_t fw = 0, mode = 0;
                    if (probe(&fw, &mode)) {
                        if (setupHid() != ESP_OK) {
                            ESP_LOGW(TAG, "keyboard at 0x%02X (firmware %u) did not take the HID setup", kAddr, fw);
                        } else {
                            s_rep.active = false;
                            s_present = true;
                            ESP_LOGI(TAG, "M5Stack Tab5 Keyboard at 0x%02X, firmware %u, mode %u -> HID", kAddr, fw, mode);
                        }
                    }
                }
            } else {
                if (gpio_get_level(kInt) == 0 || now - polled >= kPollMs) {
                    if (readEvents(now)) polled = now;
                    else gone();
                }
                if (s_present && now - checked >= kCheckMs) {
                    uint8_t mode = kModeHid;
                    if (rd(kRegMode, &mode, 1) != ESP_OK || (mode != kModeHid && setupHid() != ESP_OK)) gone();
                    checked = now;
                }
                if (s_present && s_rep.active) {
                    if (now - s_rep.since >= kRepeatMaxMs) {
                        s_rep.active = false;
                    } else if (now >= s_rep.next) {
                        s_rep.next = now + kRepeatEveryMs;
                        push(s_rep.key);
                    }
                }
            }
            xSemaphoreGive(s_lock);
        }
        if (s_stop) vTaskDelete(nullptr);
        vTaskDelay(pdMS_TO_TICKS(s_present ? 10 : 100));
    }
}

void onRestart()
{
    s_stop = true;
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) return;
    if (s_dev) i2c_master_bus_rm_device(s_dev);
    if (s_bus) i2c_del_master_bus(s_bus);
    s_dev = nullptr;
    s_bus = nullptr;
    gpio_reset_pin(kSda);
    gpio_reset_pin(kScl);
    gpio_reset_pin(kInt);
}
}  // namespace

void kbdStart()
{
    if (s_keys) return;
    s_keys = xQueueCreate(64, sizeof(KeyEvent));
    s_lock = xSemaphoreCreateMutex();
    if (!s_keys || !s_lock) {
        ESP_LOGW(TAG, "no keyboard support: no memory");
        return;
    }
    gpio_config_t intr = {};
    intr.pin_bit_mask = 1ULL << kInt;
    intr.mode = GPIO_MODE_INPUT;
    intr.pull_up_en = GPIO_PULLUP_ENABLE;
    esp_err_t err = gpio_config(&intr);
    if (err == ESP_OK) {
        i2c_master_bus_config_t bus = {};
        bus.i2c_port = kPort;
        bus.sda_io_num = kSda;
        bus.scl_io_num = kScl;
        bus.clk_source = I2C_CLK_SRC_DEFAULT;
        bus.glitch_ignore_cnt = 7;
        bus.flags.enable_internal_pullup = true;
        err = i2c_new_master_bus(&bus, &s_bus);
    }
    if (err == ESP_OK) {
        i2c_device_config_t dev = {};
        dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev.device_address = kAddr;
        dev.scl_speed_hz = kHz;
        err = i2c_master_bus_add_device(s_bus, &dev, &s_dev);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no keyboard support: %s", esp_err_to_name(err));
        return;   // (the queue stays: the test console can still inject keys)
    }
    if (esp_register_shutdown_handler(onRestart) != ESP_OK)
        ESP_LOGW(TAG, "no shutdown handler: the keyboard bus stays set up across a restart");
    if (xTaskCreatePinnedToCore(worker, "orbit_kbd", 3072, nullptr, 2, nullptr, 0) != pdPASS)
        ESP_LOGW(TAG, "no keyboard support: no memory for the worker");
}

bool kbdPresent() { return s_present; }

bool kbdGet(KeyEvent& out) { return s_keys && xQueueReceive(s_keys, &out, 0) == pdTRUE; }

void kbdInject(const KeyEvent& e)
{
    if (s_keys) xQueueSend(s_keys, &e, 0);
}

}  // namespace hal
