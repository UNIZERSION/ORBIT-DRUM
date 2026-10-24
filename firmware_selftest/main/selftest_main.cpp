// ORBIT DRUM hardware self-test for the M5Stack Tab5.
// Screen: colour bars, "ORBIT SELFTEST", live stats (FPS, touch rate, audio timing / xruns / latency,
// headphones, battery, memory, SD, MIDI), touch points drawn with their ids, a PAD square that clicks
// straight from the touch task (touch -> sound latency by ear), buttons for tone / metronome / frame cap /
// output / volume / buffer / sample rate / SD rescan / animation. The same stats go to the console every 5 s.
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "core/settings.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/hal.h"
#include "hal/hal_private.h"
#include "lvgl.h"

extern "C" bool orbit_fs_info(const char* path, uint64_t* total, uint64_t* free_bytes);

namespace {
const char* TAG = "orbit_selftest";

constexpr uint32_t kBg = 0xE9EAEC, kPanel = 0xFFFFFF, kInk = 0x1E2023, kMuted = 0x7A7F87, kAccent = 0xFF6531;
constexpr float kTwoPi = 6.28318531f;

// ---------------------------------------------------------------- audio (render runs in the audio task)
struct Synth {
    float rate = 48000.0f;
    float phase = 0.0f;          // 440 Hz tone
    float click_phase = 0.0f;
    float click_env = 0.0f;
    float click_inc = 0.0f;
    uint32_t counter = 0;        // metronome sample counter
    uint32_t beat = 0;
};
Synth g_synth;
bool g_tone = false;             // __atomic, set by the UI
bool g_metronome = false;        // __atomic
int g_trigger = 0;               // __atomic: 1 = click, 2 = accent (touch PAD, MIDI note)
uint32_t g_rate = 48000;
int g_frames = 128;

void startClick(Synth& s, bool accent)
{
    s.click_env = accent ? 0.7f : 0.45f;
    s.click_phase = 0.0f;
    s.click_inc = kTwoPi * (accent ? 1600.0f : 1000.0f) / s.rate;
}

ORBIT_HOT void render(int16_t* lr, int frames, void*)
{
    Synth& s = g_synth;
    const bool tone = __atomic_load_n(&g_tone, __ATOMIC_RELAXED);
    const bool metro = __atomic_load_n(&g_metronome, __ATOMIC_RELAXED);
    const int trig = __atomic_exchange_n(&g_trigger, 0, __ATOMIC_ACQ_REL);
    if (trig) startClick(s, trig == 2);
    const float inc = kTwoPi * 440.0f / s.rate;
    const uint32_t period = (uint32_t)(s.rate * 0.5f);   // 120 BPM
    const float decay = expf(-1.0f / (0.012f * s.rate));
    for (int i = 0; i < frames; i++) {
        float v = 0.0f;
        if (tone) {
            v += 0.25f * sinf(s.phase);
            s.phase += inc;
            if (s.phase >= kTwoPi) s.phase -= kTwoPi;
        }
        if (metro) {
            if (s.counter == 0) startClick(s, (s.beat++ & 3) == 0);
            if (++s.counter >= period) s.counter = 0;
        }
        if (s.click_env > 0.0005f) {
            v += s.click_env * sinf(s.click_phase);
            s.click_phase += s.click_inc;
            if (s.click_phase >= kTwoPi) s.click_phase -= kTwoPi;
            s.click_env *= decay;
        }
        v = v > 0.99f ? 0.99f : v < -0.99f ? -0.99f : v;
        const int16_t o = (int16_t)(v * 32767.0f);
        lr[2 * i] = o;
        lr[2 * i + 1] = o;
    }
}

uint32_t g_render_us = 0, g_render_max = 0, g_block_us = 0;   // __atomic

void onTiming(uint32_t render_us, uint32_t block_us, void*)
{
    __atomic_store_n(&g_render_us, render_us, __ATOMIC_RELAXED);
    __atomic_store_n(&g_block_us, block_us, __ATOMIC_RELAXED);
    if (render_us > __atomic_load_n(&g_render_max, __ATOMIC_RELAXED)) {
        __atomic_store_n(&g_render_max, render_us, __ATOMIC_RELAXED);
    }
}

bool startAudio()
{
    g_synth.rate = (float)g_rate;
    g_synth.counter = 0;
    return hal::audioStart(g_rate, g_frames, render, nullptr);
}

// ---------------------------------------------------------------- touch (hook runs in the touch task)
portMUX_TYPE g_touch_mux = portMUX_INITIALIZER_UNLOCKED;
hal::TouchPoint g_points[5];
int g_count = 0;
uint32_t g_touch_samples = 0;
lv_area_t g_pad_area = {1040, 176, 1248, 384};   // PAD square: finger-down clicks from the touch task
uint32_t g_down_ids = 0;                         // touch task only

void onTouch(const hal::TouchPoint* pts, int n, void*)
{
    uint32_t ids = 0;
    for (int i = 0; i < n; i++) {
        const uint32_t bit = 1u << (pts[i].id & 31);
        ids |= bit;
        const bool fresh = !(g_down_ids & bit);
        if (fresh && pts[i].x >= g_pad_area.x1 && pts[i].x <= g_pad_area.x2 && pts[i].y >= g_pad_area.y1 &&
            pts[i].y <= g_pad_area.y2) {
            __atomic_store_n(&g_trigger, 2, __ATOMIC_RELEASE);
        }
    }
    g_down_ids = ids;
    portENTER_CRITICAL(&g_touch_mux);
    g_count = n > 5 ? 5 : n;
    memcpy(g_points, pts, sizeof(hal::TouchPoint) * (size_t)g_count);
    g_touch_samples++;
    portEXIT_CRITICAL(&g_touch_mux);
}

// ---------------------------------------------------------------- MIDI (hook runs in the USB task)
uint32_t g_midi_count = 0;
uint32_t g_midi_last = 0;   // status | d1 << 8 | d2 << 16

void onMidi(const hal::MidiMsg& m, void*)
{
    __atomic_add_fetch(&g_midi_count, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_midi_last, (uint32_t)m.status | ((uint32_t)m.d1 << 8) | ((uint32_t)m.d2 << 16),
                     __ATOMIC_RELAXED);
    if ((m.status & 0xF0) == 0x90 && m.d2) __atomic_store_n(&g_trigger, 1, __ATOMIC_RELEASE);
}

// ---------------------------------------------------------------- UI
lv_obj_t* g_stats = nullptr;
lv_obj_t* g_sd_label = nullptr;
lv_obj_t* g_dots[5] = {};
lv_obj_t* g_dot_labels[5] = {};
lv_obj_t* g_anim_box = nullptr;
lv_obj_t* g_btn_labels[10] = {};
uint32_t g_renders = 0;
int g_fps = 0;
int g_fps_cap = 60;
int g_volume = 60;
int g_out = 0;   // 0 auto, 1 speaker, 2 headphones
bool g_anim = false;
char g_sd_text[640];

enum Btn { kTone = 0, kClick, kFps, kOut, kVolDown, kVolUp, kBuf, kRate, kSd, kAnim };

void countRender(lv_event_t*) { g_renders++; }

void refreshSd()
{
    size_t o = 0;
    o += (size_t)snprintf(g_sd_text + o, sizeof(g_sd_text) - o, "SD: ");
    if (!hal::sdMounted() && !hal::sdMount()) {
        snprintf(g_sd_text + o, sizeof(g_sd_text) - o, "brak karty lub brak FAT32/exFAT");
    } else {
        uint64_t total = 0, free_b = 0;
        if (orbit_fs_info(hal::sdRoot(), &total, &free_b)) {
            o += (size_t)snprintf(g_sd_text + o, sizeof(g_sd_text) - o, "%.1f GB, free %.1f GB\n",
                                  (double)total / 1e9, (double)free_b / 1e9);
        }
        DIR* d = opendir(hal::sdRoot());
        int n = 0;
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != nullptr && n < 14 && o + 48 < sizeof(g_sd_text)) {
                o += (size_t)snprintf(g_sd_text + o, sizeof(g_sd_text) - o, "%s%s\n", e->d_name,
                                      e->d_type == DT_DIR ? "/" : "");
                n++;
            }
            closedir(d);
        }
        if (!n) snprintf(g_sd_text + o, sizeof(g_sd_text) - o, "(pusta)");
    }
    if (g_sd_label) lv_label_set_text(g_sd_label, g_sd_text);
}

void setBtnText(int i, const char* t)
{
    if (g_btn_labels[i]) lv_label_set_text(g_btn_labels[i], t);
}

void updateButtons()
{
    setBtnText(kTone, __atomic_load_n(&g_tone, __ATOMIC_RELAXED) ? "TONE ON" : "TONE 440");
    setBtnText(kClick, __atomic_load_n(&g_metronome, __ATOMIC_RELAXED) ? "CLICK ON" : "CLICK");
    setBtnText(kFps, g_fps_cap == 60 ? "60 FPS" : "30 FPS");
    setBtnText(kOut, g_out == 0 ? "OUT AUTO" : g_out == 1 ? "SPEAKER" : "PHONES");
    char b[24];
    snprintf(b, sizeof(b), "BUF %d", g_frames);
    setBtnText(kBuf, b);
    setBtnText(kRate, g_rate == 48000 ? "48 kHz" : "44.1 kHz");
    setBtnText(kAnim, g_anim ? "ANIM ON" : "ANIM");
}

void animX(void* obj, int32_t v) { lv_obj_set_x(static_cast<lv_obj_t*>(obj), v); }

void onButton(lv_event_t* e)
{
    const int id = (int)(intptr_t)lv_event_get_user_data(e);
    switch (id) {
    case kTone: __atomic_store_n(&g_tone, !g_tone, __ATOMIC_RELAXED); break;
    case kClick: __atomic_store_n(&g_metronome, !g_metronome, __ATOMIC_RELAXED); break;
    case kFps:
        g_fps_cap = g_fps_cap == 60 ? 30 : 60;
        hal::setFrameCap(g_fps_cap);
        break;
    case kOut:
        g_out = (g_out + 1) % 3;
        hal::audioSetOutput(g_out == 0 ? orbit::AudioOut::Auto
                            : g_out == 1 ? orbit::AudioOut::Speaker
                                         : orbit::AudioOut::Headphones);
        break;
    case kVolDown:
    case kVolUp:
        g_volume += id == kVolUp ? 10 : -10;
        g_volume = g_volume < 0 ? 0 : g_volume > 100 ? 100 : g_volume;
        hal::audioSetVolume(g_volume);
        break;
    case kBuf:
        g_frames = g_frames == 64 ? 128 : g_frames == 128 ? 256 : 64;
        startAudio();
        break;
    case kRate:
        g_rate = g_rate == 48000 ? 44100 : 48000;
        startAudio();
        break;
    case kSd: refreshSd(); break;
    case kAnim:
        g_anim = !g_anim;
        if (g_anim) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, g_anim_box);
            lv_anim_set_exec_cb(&a, animX);
            lv_anim_set_values(&a, 0, 520);
            lv_anim_set_duration(&a, 1000);
            lv_anim_set_reverse_duration(&a, 1000);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);
        } else {
            lv_anim_delete(g_anim_box, animX);
        }
        break;
    default: break;
    }
    updateButtons();
}

lv_obj_t* panel(lv_obj_t* parent, int x, int y, int w, int h)
{
    lv_obj_t* p = lv_obj_create(parent);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, lv_color_hex(kPanel), 0);
    lv_obj_set_style_border_color(p, lv_color_hex(0xD3D5D9), 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_radius(p, 14, 0);
    lv_obj_set_style_pad_all(p, 12, 0);
    lv_obj_set_style_shadow_width(p, 0, 0);
    return p;
}

lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* text)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

void buildUi()
{
    lv_obj_t* scr = lv_screen_active();
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kBg), 0);

    lv_obj_t* title = label(scr, &lv_font_montserrat_28, kInk, "ORBIT SELFTEST");
    lv_obj_set_pos(title, 24, 16);
    lv_obj_t* dot = lv_obj_create(scr);
    lv_obj_set_size(dot, 14, 14);
    lv_obj_set_pos(dot, 290, 27);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(kAccent), 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_t* info = label(scr, &lv_font_montserrat_20, kMuted, hal::boardInfo());
    lv_obj_align(info, LV_ALIGN_TOP_RIGHT, -24, 22);

    // colour bars: rotation, colour order and the RGB565 path at a glance
    static const uint32_t bars[8] = {0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000};
    for (int i = 0; i < 8; i++) {
        lv_obj_t* b = lv_obj_create(scr);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(b, 24 + i * 154, 64);
        lv_obj_set_size(b, 154, 90);
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(bars[i]), 0);
    }
    lv_obj_t* grad = lv_obj_create(scr);
    lv_obj_remove_flag(grad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(grad, 24, 154);
    lv_obj_set_size(grad, 1232, 12);
    lv_obj_set_style_radius(grad, 0, 0);
    lv_obj_set_style_border_width(grad, 0, 0);
    lv_obj_set_style_bg_color(grad, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_color(grad, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_grad_dir(grad, LV_GRAD_DIR_HOR, 0);

    lv_obj_t* stats = panel(scr, 24, 176, 640, 392);
    g_stats = label(stats, &lv_font_montserrat_20, kInk, "...");
    lv_obj_set_width(g_stats, 612);
    lv_label_set_long_mode(g_stats, LV_LABEL_LONG_MODE_WRAP);

    lv_obj_t* sd = panel(scr, 680, 176, 344, 392);
    g_sd_label = label(sd, &lv_font_montserrat_14, kInk, "SD: ...");
    lv_obj_set_width(g_sd_label, 316);

    lv_obj_t* pad = panel(scr, g_pad_area.x1, g_pad_area.y1, lv_area_get_width(&g_pad_area),
                          lv_area_get_height(&g_pad_area));
    lv_obj_set_style_bg_color(pad, lv_color_hex(kAccent), 0);
    lv_obj_set_style_border_width(pad, 0, 0);
    lv_obj_t* pl = label(pad, &lv_font_montserrat_28, 0xFFFFFF, "PAD");
    lv_obj_center(pl);

    // animation strip (FPS under load)
    lv_obj_t* strip = panel(scr, 1040, 400, 216, 168);
    lv_obj_set_style_pad_all(strip, 0, 0);
    lv_obj_t* strip_box = lv_obj_create(strip);
    lv_obj_remove_flag(strip_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(strip_box, 600, 160);
    lv_obj_set_pos(strip_box, 0, 4);
    lv_obj_set_style_bg_opa(strip_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(strip_box, 0, 0);
    g_anim_box = lv_obj_create(strip_box);
    lv_obj_set_size(g_anim_box, 60, 60);
    lv_obj_set_pos(g_anim_box, 0, 46);
    lv_obj_set_style_bg_color(g_anim_box, lv_color_hex(kInk), 0);
    lv_obj_set_style_radius(g_anim_box, 10, 0);

    static const char* names[10] = {"TONE 440", "CLICK", "60 FPS", "OUT AUTO", "VOL -", "VOL +", "BUF 128", "48 kHz", "SD", "ANIM"};
    for (int i = 0; i < 10; i++) {
        lv_obj_t* b = lv_button_create(scr);
        lv_obj_set_pos(b, 24 + i * 124, 588);
        lv_obj_set_size(b, 112, 108);
        lv_obj_set_style_radius(b, 16, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(i < 2 ? kAccent : 0x2A2C30), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, onButton, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        g_btn_labels[i] = label(b, &lv_font_montserrat_20, 0xFFFFFF, names[i]);
        lv_obj_set_width(g_btn_labels[i], 104);
        lv_obj_set_style_text_align(g_btn_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(g_btn_labels[i], LV_LABEL_LONG_MODE_WRAP);
        lv_obj_center(g_btn_labels[i]);
    }

    // touch points on the top layer, with their ids
    for (int i = 0; i < 5; i++) {
        g_dots[i] = lv_obj_create(lv_layer_top());
        lv_obj_remove_flag(g_dots[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(g_dots[i], 64, 64);
        lv_obj_set_style_radius(g_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(g_dots[i], lv_color_hex(kAccent), 0);
        lv_obj_set_style_bg_opa(g_dots[i], LV_OPA_70, 0);
        lv_obj_set_style_border_color(g_dots[i], lv_color_hex(kInk), 0);
        lv_obj_set_style_border_width(g_dots[i], 2, 0);
        g_dot_labels[i] = label(g_dots[i], &lv_font_montserrat_20, 0xFFFFFF, "0");
        lv_obj_center(g_dot_labels[i]);
        lv_obj_add_flag(g_dots[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_display_add_event_cb(lv_display_get_default(), countRender, LV_EVENT_RENDER_READY, nullptr);
    updateButtons();
    refreshSd();
}

void touchTimer(lv_timer_t*)
{
    hal::TouchPoint pts[5];
    int n;
    portENTER_CRITICAL(&g_touch_mux);
    n = g_count;
    memcpy(pts, g_points, sizeof(pts));
    portEXIT_CRITICAL(&g_touch_mux);
    for (int i = 0; i < 5; i++) {
        if (i < n) {
            lv_obj_set_pos(g_dots[i], pts[i].x - 32, pts[i].y - 32);
            lv_label_set_text_fmt(g_dot_labels[i], "%u", (unsigned)pts[i].id);
            lv_obj_remove_flag(g_dots[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_dots[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// free stack (bytes, high-water mark) of the HAL tasks: the presentation task runs with 4 KB
void logStacks()
{
    static const char* const kTasks[] = {"orbit_present", "orbit_touch", "orbit_aux", "orbit_audio",
                                         "orbit_usb", "orbit_midi", "taskLVGL"};
    char line[200];
    int pos = 0;
    for (const char* name : kTasks) {
        TaskHandle_t t = xTaskGetHandle(name);
        if (!t) continue;
        const int w = snprintf(line + pos, sizeof(line) - (size_t)pos, "%s %u ", name,
                               (unsigned)uxTaskGetStackHighWaterMark(t));
        if (w < 0 || pos + w >= (int)sizeof(line)) break;
        pos += w;
    }
    line[pos] = 0;
    ESP_LOGI(TAG, "stack free (B): %s", pos ? line : "-");
}

void statsTimer(lv_timer_t*)
{
    static int64_t last = 0;
    static int ticks = 0;
    const int64_t now = (int64_t)hal::micros();
    if (last) g_fps = (int)((int64_t)g_renders * 1000000 / (now - last));
    g_renders = 0;
    last = now;
    const uint32_t render_max = __atomic_exchange_n(&g_render_max, 0u, __ATOMIC_RELAXED);
    const uint32_t midi = __atomic_load_n(&g_midi_count, __ATOMIC_RELAXED);
    const uint32_t ml = __atomic_load_n(&g_midi_last, __ATOMIC_RELAXED);
    int n;
    portENTER_CRITICAL(&g_touch_mux);
    n = g_count;
    portEXIT_CRITICAL(&g_touch_mux);
    const int bat = hal::batteryPercent();
    char battery[40];
    if (bat < 0) snprintf(battery, sizeof(battery), "unknown (no pack?)");
    else snprintf(battery, sizeof(battery), "%d%%%s", bat, hal::charging() ? ", charging" : "");
    hal::Diagnostics dg;
    hal::diagnostics(dg);
    char b[900];
    snprintf(b, sizeof(b),
             "DISPLAY  %d fps (cap %d), draw 1280x%u %s\n"
             "    rotate max %lu us, repair max %lu us, publish max %lu us\n"
             "TOUCH  %d Hz, %d points\n"
             "AUDIO  %s %lu Hz, %d frames x %u DMA, latency %.1f ms\n"
             "    render %lu us (max %lu), block %lu us, xruns %lu\n"
             "    write max %lu us, period max %lu us, overload %lu blocks\n"
             "OUTPUT  %s, headphones %s (HP_DET raw 0x%02x, bit 7 = %d%s), volume %d\n"
             "BATTERY  %s\n"
             "MEMORY  internal %lu KB, PSRAM %lu KB\n"
             "MIDI  %s (%lu msgs, last %02lx %02lx %02lx)\n"
             "PAD square: click from the touch task",
             g_fps, g_fps_cap, (unsigned)dg.draw_rows, dg.draw_sram ? "SRAM" : "PSRAM",
             (unsigned long)dg.rotate_max_us, (unsigned long)dg.repair_max_us, (unsigned long)dg.publish_max_us,
             hal::touchRate(), n, hal::audioRunning() ? "on" : "OFF", (unsigned long)g_rate, g_frames,
             (unsigned)dg.audio_dma_buffers, (double)hal::audioLatencyMs(),
             (unsigned long)__atomic_load_n(&g_render_us, __ATOMIC_RELAXED),
             (unsigned long)render_max, (unsigned long)__atomic_load_n(&g_block_us, __ATOMIC_RELAXED),
             (unsigned long)hal::audioXruns(), (unsigned long)dg.audio_write_max_us,
             (unsigned long)dg.audio_period_max_us, (unsigned long)dg.audio_overload_blocks,
             g_out == 0 ? "auto" : g_out == 1 ? "speaker" : "headphones", hal::headphonesInserted() ? "IN" : "out",
             (unsigned)(dg.ioe_inputs < 0 ? 0 : dg.ioe_inputs), dg.ioe_inputs < 0 ? -1 : (dg.ioe_inputs >> 7) & 1,
             dg.hp_det_trusted ? "" : ", not trusted", g_volume, battery,
             (unsigned long)(hal::freeInternal() / 1024), (unsigned long)(hal::freePsram() / 1024),
             hal::midiConnected() ? hal::midiDeviceName() : "no device", (unsigned long)midi,
             (unsigned long)(ml & 0xFF), (unsigned long)((ml >> 8) & 0xFF), (unsigned long)((ml >> 16) & 0xFF));
    lv_label_set_text(g_stats, b);
    if (++ticks % 10 == 0) {   // every 5 s on the console
        ESP_LOGI(TAG, "fps %d, touch %d Hz, audio %s render %lu/%lu us, xruns %lu, hp %d, bat %d, int %lu KB, psram %lu KB",
                 g_fps, hal::touchRate(), hal::audioRunning() ? "on" : "off",
                 (unsigned long)__atomic_load_n(&g_render_us, __ATOMIC_RELAXED), (unsigned long)render_max,
                 (unsigned long)hal::audioXruns(), hal::headphonesInserted() ? 1 : 0, bat,
                 (unsigned long)(hal::freeInternal() / 1024), (unsigned long)(hal::freePsram() / 1024));
        ESP_LOGI(TAG, "max: audio render %lu / write %lu / period %lu us, overload %lu; rotate %lu / repair %lu / "
                      "publish %lu us; HP_DET IN_STA 0x%02x",
                 (unsigned long)dg.audio_render_max_us, (unsigned long)dg.audio_write_max_us,
                 (unsigned long)dg.audio_period_max_us, (unsigned long)dg.audio_overload_blocks,
                 (unsigned long)dg.rotate_max_us, (unsigned long)dg.repair_max_us, (unsigned long)dg.publish_max_us,
                 (unsigned)(dg.ioe_inputs < 0 ? 0 : dg.ioe_inputs));
        logStacks();
        hal::diagnosticsReset();   // the next 5 s get their own maxima
    }
}
}  // namespace

// Bring-up heartbeat on core 1: shows where the display pipeline is when the UI stops updating.
static void heartbeat(void*)
{
    char b[300];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        hal::detail::displayDebug(b, sizeof(b));
        ESP_LOGI("beat", "%s | touch %d Hz | int %lu KB", b, hal::touchRate(), (unsigned long)(hal::freeInternal() / 1024));
    }
}

extern "C" void app_main(void)
{
    xTaskCreatePinnedToCore(heartbeat, "beat", 4096, nullptr, 2, nullptr, 1);
    if (!hal::displayReserveBuffers()) ESP_LOGW(TAG, "draw buffers will be in PSRAM");
    if (!hal::boardInit()) ESP_LOGE(TAG, "board init failed");
    const bool sd = hal::sdMount();
    orbit::Settings settings;
    const bool loaded = hal::settingsLoad(settings);
    ESP_LOGI(TAG, "SD %s, settings %s", sd ? "mounted" : "missing", loaded ? "loaded" : "defaults");

    lv_display_t* disp = hal::displayStart();
    if (!disp) {
        ESP_LOGE(TAG, "display start failed");
    } else {
        hal::setBrightness(80);
        if (hal::lvLock(0)) {
            buildUi();
            lv_timer_create(touchTimer, 20, nullptr);
            lv_timer_create(statsTimer, 500, nullptr);
            hal::displayReveal();   // backlight on with the first frame of the test screen
            hal::lvUnlock();
        }
    }
    hal::setTouchHook(onTouch, nullptr);
    hal::audioSetVolume(g_volume);
    hal::audioSetTimingHook(onTiming, nullptr);
    if (!startAudio()) ESP_LOGE(TAG, "audio start failed");
    hal::midiStart(onMidi, nullptr);
    // settings round trip: SD file + NVS mirror (audio idle: nothing is playing yet)
    hal::setAudioIdle(true);
    const bool saved = hal::settingsSave(settings);
    orbit::Settings check;
    ESP_LOGI(TAG, "settings save %s, reload %s", saved ? "ok" : "FAILED",
             hal::settingsLoad(check) && check.volume == settings.volume ? "ok" : "FAILED");
}
