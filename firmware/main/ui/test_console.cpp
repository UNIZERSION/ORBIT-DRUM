// Test console (0.4.4): lets a PC drive and inspect ORBIT over the USB console (the same port as the log), so the
// device tests can run without a person at the screen (tools/device_test.py). One command per line; every answer
// line starts with "T>" (screenshot data: "S:"), so it can be picked out of the log.
//   ping                       -> T>pong <version>
//   state                      -> T>state key=value ...  (view, maximized panel, dialogs, transport, BASS KEYS,
//                                 heap, audio xruns, touch rate, headphones, SD, Bluetooth and the Pico's buffer)
//   audio                      -> T>audio ...            (engine telemetry: peaks, voices, CPU, bass notes, step)
//   touch x y [x y ...]        -> fingers down (landscape px, up to 5), held until "release"
//   release                    -> all fingers up
//   tap x y [ms]               -> one finger for ms (80), then up
//   drag x1 y1 x2 y2 [ms]      -> one finger moved in 10 ms steps over ms (300), then up
//   shot [step]                -> T>shot w h bytes crc32, S:<n>:<base64> lines, T>shot end
//                                 (the frame on the glass in the panel's native portrait order, every step-th
//                                 pixel; RLE: count-1, RGB565 low, high; the PC rotates it to landscape)
// Fingers go through hal::touchInject: the real input path (touch task, TouchHook, LVGL's pointer, the KEYS zone).
// Reading the commands needs no driver: the task polls the USB-Serial/JTAG receive FIFO every 20 ms.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal/hal.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/widgets.h"

#ifdef ESP_PLATFORM
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#endif

namespace orbit::ui::testcon {

namespace {

const char* version()
{
#ifdef ESP_PLATFORM
    return esp_app_get_description()->version;
#else
    return "host";
#endif
}

void sleepMs(int ms)
{
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(ms > 0 ? ms : 1));
#else
    (void)ms;
#endif
}

bool lock()
{
#ifdef ESP_PLATFORM
    return hal::lvLock(1000);
#else
    return true;
#endif
}

void unlock()
{
#ifdef ESP_PLATFORM
    hal::lvUnlock();
#endif
}

// up to n integers after the command word; returns how many were read
int ints(const char* s, int* out, int n)
{
    int k = 0;
    while (k < n) {
        while (*s == ' ') ++s;
        if (!*s) break;
        char* e = nullptr;
        const long v = strtol(s, &e, 10);
        if (e == s) break;
        out[k++] = (int)v;
        s = e;
    }
    return k;
}

// One answer line. USB-Serial/JTAG sends 64-byte packets; a line of exactly n x 64 bytes (with the CR LF the
// console adds) ends without a short packet and waits in the PC's driver until the next one arrives: such a line
// gets a trailing space. Flushed at once.
void out(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void out(const char* fmt, ...)
{
    static char buf[720];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 3) n = (int)sizeof buf - 3;
    if ((n + 2) % 64 == 0) buf[n++] = ' ';
    buf[n] = 0;
    fputs(buf, stdout);
    fputs("\n", stdout);
    fflush(stdout);
}

void finger(int x, int y)
{
    hal::TouchPoint p = {(int16_t)x, (int16_t)y, 0, 0};
    hal::touchInject(&p, 1);
}

void cmdState()
{
    if (!lock()) {
        out("T>err busy");
        return;
    }
    App& a = app();
    const hal::BtStatus& bt = a.btStatus();
    char buf[640];
    snprintf(buf, sizeof buf,
             "T>state ver=%s view=%d max=%d full=%d modal=%d overlay=%d playing=%d bpm=%.1f scene=%d pad=%d keys=%d "
             "bass_notes=%lu cutoff=%.3f heap=%lu psram=%lu xruns=%lu touch_hz=%d hp=%d sd=%d bt=%d bt_conn=%d "
             "bt_stream=%d pico_buf=%ld pico_floor=%ld pico_under=%lu resyncs=%lu",
             version(), (int)a.ui().view, (int)panelMaxed() == (int)Panel::Count ? -1 : (int)panelMaxed(),
             panelMaxedFull() ? 1 : 0, modalIsOpen() ? 1 : 0, overlayOpen() ? 1 : 0, a.playing() ? 1 : 0,
             (double)a.project().bpm, (int)a.project().scene, (int)a.ui().pad, view_bass::keysOn() ? 1 : 0,
             (unsigned long)a.bassNotes(), (double)a.paramNorm(ParamRef::global(GP_BASS_CUTOFF)),
             (unsigned long)hal::freeInternal(), (unsigned long)hal::freePsram(), (unsigned long)hal::audioXruns(),
             hal::touchRate(), hal::headphonesInserted() ? 1 : 0, a.sdReady() ? 1 : 0, a.btEnabled() ? 1 : 0,
             bt.connected, bt.streaming, (long)bt.pico_buffered, (long)bt.pico_floor, (unsigned long)bt.pico_underruns,
             (unsigned long)bt.resyncs);
    unlock();
    out("%s", buf);
}

void cmdAudio()
{
    const Telemetry& t = app().engine().telemetry();
    out("T>audio peak_l=%.4f peak_r=%.4f voices=%d cpu=%.3f xruns=%lu bass_notes=%lu playing=%d step=%d beat=%.3f",
           (double)t.master_peak[0].load(), (double)t.master_peak[1].load(), (int)t.voices.load(), (double)t.cpu.load(),
           (unsigned long)t.xruns.load(), (unsigned long)t.bass_notes.load(), (int)t.playing.load(),
           (int)t.step.load(), (double)t.beat.load());
}

#ifdef ESP_PLATFORM
// ---- shot: RLE of the native rows into a PSRAM buffer (under the LVGL lock), then printed as base64
struct Shot {
    uint8_t* buf;
    size_t cap, len;
    int w, h;
};

void shotRow(const uint16_t* px, int w, int y, void* user)
{
    Shot& s = *(Shot*)user;
    s.w = w;
    s.h = y + 1;
    for (int x = 0; x < w;) {
        const uint16_t v = px[x];
        int n = 1;
        while (x + n < w && n < 256 && px[x + n] == v) ++n;
        if (s.len + 3 > s.cap) return;   // (cannot happen: cap is the worst case)
        s.buf[s.len++] = (uint8_t)(n - 1);
        s.buf[s.len++] = (uint8_t)(v & 0xFF);
        s.buf[s.len++] = (uint8_t)(v >> 8);
        x += n;
    }
}

uint32_t crc32(const uint8_t* p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
#endif

void cmdShot(int step)
{
#ifdef ESP_PLATFORM
    step = step == 2 || step == 4 ? step : 1;
    Shot s = {};
    s.cap = (size_t)(720 / step) * (size_t)(1280 / step) * 3;
    s.buf = (uint8_t*)heap_caps_malloc(s.cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.buf) {
        out("T>err no memory");
        return;
    }
    bool ok = false;
    if (lock()) {
        ok = hal::displayReadBack(step, shotRow, &s);
        unlock();
    }
    if (!ok) {
        heap_caps_free(s.buf);
        out("T>err no frame");
        return;
    }
    out("T>shot %d %d %u %08lx", s.w, s.h, (unsigned)s.len, (unsigned long)crc32(s.buf, s.len));
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char line[96];
    int n = 0;
    for (size_t i = 0; i < s.len; i += 57, ++n) {
        const size_t m = s.len - i < 57 ? s.len - i : 57;
        int o = snprintf(line, sizeof line, "S:%d:", n);
        for (size_t j = 0; j < m; j += 3) {
            const uint32_t b0 = s.buf[i + j], b1 = j + 1 < m ? s.buf[i + j + 1] : 0, b2 = j + 2 < m ? s.buf[i + j + 2] : 0;
            const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
            line[o++] = b64[(v >> 18) & 63];
            line[o++] = b64[(v >> 12) & 63];
            line[o++] = j + 1 < m ? b64[(v >> 6) & 63] : '=';
            line[o++] = j + 2 < m ? b64[v & 63] : '=';
        }
        line[o] = 0;
        out("%s", line);
    }
    heap_caps_free(s.buf);
    out("T>shot end %d", n);
#else
    (void)step;
    out("T>err no frame");
#endif
}

}  // namespace

void handleLine(const char* line)
{
    while (*line == ' ') ++line;
    int v[10] = {};
    if (!strncmp(line, "ping", 4)) {
        out("T>pong %s", version());
    } else if (!strncmp(line, "state", 5)) {
        cmdState();
    } else if (!strncmp(line, "audio", 5)) {
        cmdAudio();
    } else if (!strncmp(line, "touch", 5)) {
        const int n = ints(line + 5, v, 10) / 2;
        hal::TouchPoint p[5];
        for (int i = 0; i < n && i < 5; ++i) p[i] = {(int16_t)v[2 * i], (int16_t)v[2 * i + 1], (uint8_t)i, 0};
        hal::touchInject(p, n < 5 ? n : 5);
        out("T>ok touch %d", n);
    } else if (!strncmp(line, "release", 7)) {
        hal::touchInject(nullptr, 0);
        out("T>ok release");
    } else if (!strncmp(line, "tap", 3)) {
        const int n = ints(line + 3, v, 3);
        if (n < 2) {
            out("T>err tap x y [ms]");
            return;
        }
        finger(v[0], v[1]);
        sleepMs(n > 2 ? v[2] : 80);
        hal::touchInject(nullptr, 0);
        sleepMs(60);
        out("T>ok tap");
    } else if (!strncmp(line, "drag", 4)) {
        const int n = ints(line + 4, v, 5);
        if (n < 4) {
            out("T>err drag x1 y1 x2 y2 [ms]");
            return;
        }
        const int ms = n > 4 && v[4] > 0 ? v[4] : 300;
        const int steps = ms / 10 > 1 ? ms / 10 : 1;
        for (int i = 0; i <= steps; ++i) {
            finger(v[0] + (v[2] - v[0]) * i / steps, v[1] + (v[3] - v[1]) * i / steps);
            sleepMs(10);
        }
        hal::touchInject(nullptr, 0);
        sleepMs(60);
        out("T>ok drag");
    } else if (!strncmp(line, "key", 3)) {
        // key [ctrl+][shift+][alt+]<name>: a, space, enter, esc, tab, backspace, delete, up, down, left, right,
        // f1..f12, release (as the Tab5 Keyboard would send it; the UI tick takes it)
        const char* s = line + 3;
        while (*s == ' ') ++s;
        hal::KeyEvent e = {};
        for (;;) {
            if (!strncmp(s, "ctrl+", 5)) e.ctrl = true, s += 5;
            else if (!strncmp(s, "shift+", 6)) e.shift = true, s += 6;
            else if (!strncmp(s, "alt+", 4)) e.alt = true, s += 4;
            else break;
        }
        static const char* const kNames[] = {"enter", "esc", "backspace", "delete", "tab", "left", "right", "up",
                                             "down", "home", "end", "pgup", "pgdn"};
        static const hal::Key kKeys[] = {hal::Key::Enter, hal::Key::Esc, hal::Key::Backspace, hal::Key::Delete,
                                         hal::Key::Tab, hal::Key::Left, hal::Key::Right, hal::Key::Up,
                                         hal::Key::Down, hal::Key::Home, hal::Key::End, hal::Key::PgUp,
                                         hal::Key::PgDn};
        bool ok = true;
        if (!strcmp(s, "release")) e.key = hal::Key::Release;
        else if (!strcmp(s, "space")) e.key = hal::Key::Char, e.ch = ' ';
        else if ((s[0] == 'f' || s[0] == 'F') && s[1] >= '1' && s[1] <= '9') {
            const int n = atoi(s + 1);
            ok = n >= 1 && n <= 12;
            e.key = (hal::Key)((int)hal::Key::F1 + n - 1);
        } else if (s[0] && !s[1]) {
            e.key = hal::Key::Char;
            e.ch = e.shift && s[0] >= 'a' && s[0] <= 'z' ? (char)(s[0] - 32) : s[0];
        } else {
            ok = false;
            for (int i = 0; i < (int)(sizeof kNames / sizeof kNames[0]); ++i)
                if (!strcmp(s, kNames[i])) e.key = kKeys[i], ok = true;
        }
        if (!ok) {
            out("T>err key name: %s", s);
            return;
        }
        hal::kbdInject(e);
        sleepMs(60);   // (the UI tick takes it)
        out("T>ok key");
    } else if (!strncmp(line, "shot", 4)) {
        const int n = ints(line + 4, v, 1);
        cmdShot(n ? v[0] : 1);
    } else if (*line) {
        out("T>err unknown: %s", line);
    }
}

#ifdef ESP_PLATFORM
namespace {
esp_timer_handle_t s_poll = nullptr;

void consoleTask(void*)
{
    char line[160];
    int n = 0;
    for (;;) {
        uint8_t b[64];
        const int got = usb_serial_jtag_ll_rxfifo_data_available() ? usb_serial_jtag_ll_read_rxfifo(b, sizeof b) : 0;
        for (int i = 0; i < got; ++i) {
            const char c = (char)b[i];
            if (c == '\r' || c == '\n') {
                line[n] = 0;
                if (n) handleLine(line);
                n = 0;
            } else if (n < (int)sizeof line - 1 && c >= ' ') {
                line[n++] = c;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// The task (6 KB of internal RAM) exists only once the PC has sent something: until then a 100 ms timer looks
// at the receive FIFO (the first command waits there for the task).
void pollCb(void*)
{
    if (!usb_serial_jtag_ll_rxfifo_data_available()) return;
    esp_timer_stop(s_poll);
    xTaskCreatePinnedToCore(consoleTask, "orbit_test", 6144, nullptr, 1, nullptr, 0);
}
}  // namespace

void start()
{
    out("T>ready ORBIT %s", version());
    esp_timer_create_args_t a = {};
    a.callback = pollCb;
    a.name = "orbit_test";
    if (esp_timer_create(&a, &s_poll) == ESP_OK) esp_timer_start_periodic(s_poll, 100000);
}
#else
void start() {}
#endif

}  // namespace orbit::ui::testcon
