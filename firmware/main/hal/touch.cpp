// Multi-touch for the Tab5 (GT911 on ILI9881C boards, ST7123 / ST712x on the later ones).
//
// One reader task (core 0, priority 6) polls the controller every 5 ms (200 Hz) and is the only code
// that talks to it over I2C. It converts up to 5 points to landscape coordinates, keeps finger ids stable
// (controller track id on GT911, nearest-neighbour tracking on ST712x, whose driver drops the slot index),
// calls the TouchHook right away (pads trigger on finger-down) and caches the primary finger for LVGL.
// LVGL's read_cb only returns that cached point: no I2C in the LVGL task. Fingers that come down in the
// exclusive zone (setTouchExclusive, 0.4.3: the BASS KEYS keyboard) are never LVGL's primary.
//
// Coordinates: the controller reports native portrait coordinates (raw x 0..719, raw y 0..1279, same axes
// as the panel). The display uses LV_DISPLAY_ROTATION_90, whose native area of a logical pixel (lx, ly) is
// (ly, 1279 - lx) (display.cpp nativeArea). Inverse: lx = 1279 - raw_y, ly = raw_x, which is also what
// LVGL's indev_pointer_proc applies to the raw point we hand it.
#include <string.h>
#include "bsp/m5stack_tab5.h"
#include "bsp/touch.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "hal/hal.h"
#include "hal/hal_logic.h"
#include "hal/hal_private.h"

namespace hal {

namespace {
const char* TAG = "orbit_touch";
constexpr int kMaxPoints = 5;
constexpr int kPollMs = 5;
constexpr int64_t kReleaseUs = 25000;     // no point for 25 ms: all fingers up (GT911 reports ~100 Hz)
constexpr int kTrackDist2 = 160 * 160;    // nearest-neighbour radius between two samples (px^2)

esp_lcd_touch_handle_t s_tp = nullptr;
lv_indev_t* s_indev = nullptr;
portMUX_TYPE s_hook_mux = portMUX_INITIALIZER_UNLOCKED;
TouchHook s_hook = nullptr;
void* s_hook_user = nullptr;
// primary finger for LVGL: bit 31 pressed, bits 16..30 raw y, bits 0..15 raw x
volatile uint32_t s_lv_point = 0;
volatile int s_rate = 0;

TouchPoint s_prev[kMaxPoints];
int s_prev_n = 0;

// setTouchExclusive(): fingers that come down in it are the hook's alone (LVGL never follows them)
portMUX_TYPE s_excl_mux = portMUX_INITIALIZER_UNLOCKED;
int16_t s_excl[4] = {0, 0, -1, -1};

// touchInject() (test console): fingers that replace the controller's points until released
portMUX_TYPE s_inj_mux = portMUX_INITIALIZER_UNLOCKED;
TouchPoint s_inj[kMaxPoints];
int s_inj_n = 0;
bool s_inj_on = false;

bool inExclusive(int x, int y)
{
    portENTER_CRITICAL(&s_excl_mux);
    const bool in = s_excl[2] >= s_excl[0] && x >= s_excl[0] && x <= s_excl[2] && y >= s_excl[1] && y <= s_excl[3];
    portEXIT_CRITICAL(&s_excl_mux);
    return in;
}

// ---- ST712x reads with a time limit (0.4.1)
// The BSP's touch IO (esp_lcd panel IO over I2C) waits forever (timeout -1), and after a NACK the IDF I2C driver
// busy-waits for the bus to become free. On 2026-10-24 the bus stayed "busy": this task spun on core 0 holding
// the bus lock, every other I2C device (expanders, battery monitor) timed out and touch was dead until the
// Tab5 was switched off. Same registers as esp_lcd_touch_st7123 (16-bit register address, big endian); the
// points go into tp->data, so esp_lcd_touch_get_data still applies the BSP's coordinate flags.
constexpr int kI2cMs = 20;                // per transfer
constexpr int kClearAfter = 3;            // failed reads in a row before a bus clear
constexpr int64_t kClearGapUs = 500000;   // at most two bus clears per second
i2c_master_dev_handle_t s_st = nullptr;   // ST712x at 0x55 (nullptr: GT911, read through esp_lcd)

esp_err_t stRx(uint16_t reg, uint8_t* buf, size_t len)
{
    const uint8_t r[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    return i2c_master_transmit_receive(s_st, r, sizeof r, buf, len, kI2cMs);
}

esp_err_t stReadData(esp_lcd_touch_handle_t tp)
{
    uint8_t adv = 0, max = 0;
    uint8_t rep[10 * 7];   // up to 10 reports: x_h (bits 0..5) | valid (bit 7), x_l, y_h, y_l, area, intensity, -
    esp_err_t err = stRx(0x0010, &adv, 1);   // advanced info: bit 3 = coordinates follow
    if (err != ESP_OK) return err;
    if (adv & 0x08) {
        err = stRx(0x0009, &max, 1);         // reports available
        if (err != ESP_OK) return err;
        if (max > 10) max = 10;
        if (max) {
            err = stRx(0x0014, rep, (size_t)max * 7);
            if (err != ESP_OK) return err;
        }
    }
    portENTER_CRITICAL(&tp->data.lock);
    int j = 0;
    for (int i = 0; i < max && j < CONFIG_ESP_LCD_TOUCH_MAX_POINTS; ++i) {
        const uint8_t* p = rep + i * 7;
        if (!(p[0] & 0x80)) continue;
        tp->data.coords[j].x = (uint16_t)(((p[0] & 0x3F) << 8) | p[1]);
        tp->data.coords[j].y = (uint16_t)((p[2] << 8) | p[3]);
        tp->data.coords[j++].strength = p[4];
    }
    tp->data.points = (uint8_t)j;
    portEXIT_CRITICAL(&tp->data.lock);
    return ESP_OK;
}

void lvRead(lv_indev_t*, lv_indev_data_t* data)
{
    const uint32_t p = s_lv_point;
    data->point.x = (int32_t)(p & 0xFFFF);
    data->point.y = (int32_t)((p >> 16) & 0x7FFF);
    data->state = (p & 0x80000000u) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void publishLv(bool pressed, int raw_x, int raw_y)
{
    const uint32_t v = (pressed ? 0x80000000u : 0u) | ((uint32_t)raw_y << 16) | (uint32_t)raw_x;
    const bool changed = ((s_lv_point ^ v) & 0x80000000u) != 0;
    s_lv_point = v;
    // press / release: let the LVGL task read it now instead of at its next poll
    if (changed) lvgl_port_task_wake(LVGL_PORT_EVENT_TOUCH, nullptr);
}

void callHook(const TouchPoint* pts, int n)
{
    portENTER_CRITICAL(&s_hook_mux);
    TouchHook hook = s_hook;
    void* user = s_hook_user;
    portEXIT_CRITICAL(&s_hook_mux);
    if (hook) hook(pts, n, user);
}

// stable ids for controllers without track ids: nearest point of the previous sample
void assignIds(TouchPoint* pts, int n)
{
    bool taken[kMaxPoints] = {};
    uint32_t used = 0;   // ids given out in this sample
    for (int i = 0; i < n; i++) {
        int best = -1, best_d = kTrackDist2;
        for (int j = 0; j < s_prev_n; j++) {
            if (taken[j]) continue;
            const int dx = pts[i].x - s_prev[j].x, dy = pts[i].y - s_prev[j].y;
            const int d = dx * dx + dy * dy;
            if (d < best_d) {
                best_d = d;
                best = j;
            }
        }
        if (best >= 0) {
            taken[best] = true;
            pts[i].id = s_prev[best].id;
            used |= 1u << pts[i].id;
        } else {
            pts[i].id = 0xFF;   // new finger, numbered below
        }
    }
    for (int j = 0; j < s_prev_n; j++) {
        if (!taken[j]) used |= 1u << s_prev[j].id;   // lifted this sample: do not reuse its id at once
    }
    for (int i = 0; i < n; i++) {
        if (pts[i].id != 0xFF) continue;
        uint8_t id = 0;
        while (id < 15 && (used & (1u << id))) id++;
        pts[i].id = id;
        used |= 1u << id;
    }
}

void touchTask(void*)
{
    const bool hw_ids = s_tp->get_track_id != nullptr;
    esp_lcd_touch_point_data_t raw[kMaxPoints];
    TouchPoint pts[kMaxPoints];
    int raw_x[kMaxPoints], raw_y[kMaxPoints];
    bool down = false;
    logic::PointerFollow follow;   // the finger LVGL follows (not the ones of the exclusive zone)
    int64_t last_touch = 0, window = esp_timer_get_time(), last_clear = 0;
    uint32_t samples = 0, errors = 0, clears = 0;
    int fail = 0;   // failed reads in a row
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(kPollMs) ? pdMS_TO_TICKS(kPollMs) : 1);
        uint8_t cnt = 0;
        const esp_err_t rd = s_st ? stReadData(s_tp) : esp_lcd_touch_read_data(s_tp);
        if (rd == ESP_OK) {
            if (fail >= kClearAfter) ESP_LOGW(TAG, "controller answers again after %d failed reads", fail);
            fail = 0;
            if (esp_lcd_touch_get_data(s_tp, raw, &cnt, kMaxPoints) != ESP_OK) cnt = 0;
        } else {
            if ((++errors & 255) == 1)
                ESP_LOGW(TAG, "controller read failed (%lu, %s)", (unsigned long)errors, esp_err_to_name(rd));
            const int64_t t = esp_timer_get_time();
            if (s_st && ++fail >= kClearAfter && t - last_clear >= kClearGapUs) {
                // a device holding the bus mid-transfer: 9 clocks + STOP (the next read re-arms the controller)
                last_clear = t;
                const esp_err_t r = i2c_master_bus_reset(bsp_i2c_get_handle());
                if (++clears <= 5 || clears % 100 == 0)
                    ESP_LOGW(TAG, "I2C bus clear %lu after %d failed reads: %s", (unsigned long)clears, fail,
                             esp_err_to_name(r));
            }
            vTaskDelay(pdMS_TO_TICKS(10));   // never keep core 0 busy on a dead bus
            wake = xTaskGetTickCount();
        }
        // test console (0.4.4): injected fingers replace the controller's points (ids as given)
        bool inj = false;
        portENTER_CRITICAL(&s_inj_mux);
        if (s_inj_on) {
            inj = true;
            cnt = (uint8_t)s_inj_n;
            for (int i = 0; i < s_inj_n; i++) {
                raw[i].x = (uint16_t)s_inj[i].y;   // landscape (x, y) = (1279 - raw y, raw x)
                raw[i].y = (uint16_t)(1279 - s_inj[i].x);
                raw[i].track_id = s_inj[i].id;
                raw[i].strength = 100;
            }
        }
        portEXIT_CRITICAL(&s_inj_mux);
        const int64_t now = esp_timer_get_time();
        if (cnt > 0) {
            const int n = cnt > kMaxPoints ? kMaxPoints : cnt;
            for (int i = 0; i < n; i++) {
                int x = raw[i].x, y = raw[i].y;
                x = x < 0 ? 0 : x > 719 ? 719 : x;
                y = y < 0 ? 0 : y > 1279 ? 1279 : y;
                raw_x[i] = x;
                raw_y[i] = y;
                pts[i].x = (int16_t)(1279 - y);
                pts[i].y = (int16_t)x;
                pts[i].id = hw_ids || inj ? raw[i].track_id : 0;
                pts[i]._pad = 0;
            }
            if (!hw_ids && !inj) assignIds(pts, n);
            memcpy(s_prev, pts, sizeof(TouchPoint) * (size_t)n);
            s_prev_n = n;
            down = true;
            last_touch = now;
            samples++;
            // LVGL follows one finger (hal_logic.h PointerFollow); the exclusive zone's fingers are the hook's
            uint8_t ids[kMaxPoints];
            bool zone[kMaxPoints];
            for (int i = 0; i < n; i++) {
                ids[i] = pts[i].id;
                zone[i] = inExclusive(pts[i].x, pts[i].y);
            }
            const int k = follow.update(ids, zone, n);
            if (k >= 0) publishLv(true, raw_x[k], raw_y[k]);
            else publishLv(false, (int)(s_lv_point & 0xFFFF), (int)((s_lv_point >> 16) & 0x7FFF));
            callHook(pts, n);
        } else if (down && now - last_touch >= kReleaseUs) {
            down = false;
            s_prev_n = 0;
            follow.reset();
            publishLv(false, (int)(s_lv_point & 0xFFFF), (int)((s_lv_point >> 16) & 0x7FFF));
            callHook(pts, 0);
        }
        if (now - window >= 1000000) {
            s_rate = (int)((int64_t)samples * 1000000 / (now - window));
            samples = 0;
            window = now;
        }
    }
}
}  // namespace

namespace detail {
bool touchStart(void* lv_display)
{
    if (s_tp) return true;
    bsp_touch_config_t cfg = {};
    esp_err_t err = bsp_touch_new(&cfg, &s_tp);
    if (err != ESP_OK || !s_tp) {
        ESP_LOGE(TAG, "touch controller: %s", esp_err_to_name(err));
        s_tp = nullptr;
        return false;
    }
    // ST712x (no track ids): our own timed reads at 0x55 (see stReadData); GT911 stays on esp_lcd
    if (!s_tp->get_track_id && detail::i2cAdd(0x55, &s_st) != ESP_OK) s_st = nullptr;
    // Runs in the background panel task, possibly while app_main holds the lock for the UI build: wait for it
    if (lvLock(0)) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(s_indev, static_cast<lv_display_t*>(lv_display));
        lv_indev_set_read_cb(s_indev, lvRead);
        lv_timer_t* t = lv_indev_get_read_timer(s_indev);
        if (t) lv_timer_set_period(t, 10);
        lvUnlock();
    }
    if (xTaskCreatePinnedToCore(touchTask, "orbit_touch", 4096, nullptr, 6, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "touch task");
        return false;
    }
    ESP_LOGI(TAG, "touch: %s, %d points, %d Hz polling", s_tp->get_track_id ? "GT911 (track ids)" : "ST712x",
             kMaxPoints, 1000 / kPollMs);
    return true;
}
}  // namespace detail

void setTouchHook(TouchHook hook, void* user)
{
    portENTER_CRITICAL(&s_hook_mux);
    s_hook = hook;
    s_hook_user = user;
    portEXIT_CRITICAL(&s_hook_mux);
}

int touchRate() { return s_rate; }

void touchInject(const TouchPoint* points, int count)
{
    if (count < 0) count = 0;
    if (count > kMaxPoints) count = kMaxPoints;
    portENTER_CRITICAL(&s_inj_mux);
    for (int i = 0; i < count; i++) {
        s_inj[i] = points[i];
        s_inj[i].x = s_inj[i].x < 0 ? 0 : s_inj[i].x > 1279 ? 1279 : s_inj[i].x;
        s_inj[i].y = s_inj[i].y < 0 ? 0 : s_inj[i].y > 719 ? 719 : s_inj[i].y;
    }
    s_inj_n = count;
    s_inj_on = count > 0;   // released: the controller's points again (none -> the usual 25 ms release)
    portEXIT_CRITICAL(&s_inj_mux);
}

void setTouchExclusive(int x1, int y1, int x2, int y2)
{
    portENTER_CRITICAL(&s_excl_mux);
    s_excl[0] = (int16_t)x1;
    s_excl[1] = (int16_t)y1;
    s_excl[2] = (int16_t)x2;
    s_excl[3] = (int16_t)y2;
    portEXIT_CRITICAL(&s_excl_mux);
}

}  // namespace hal
