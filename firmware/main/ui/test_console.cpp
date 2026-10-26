// Test console (0.4.4): lets a PC drive and inspect ORBIT over the USB console (the same port as the log), so the
// device tests can run without a person at the screen (tools/device_test.py). One command per line; every answer
// line starts with "T>" (screenshot data: "S:"), so it can be picked out of the log.
//   ping                       -> T>pong <version>
//   state                      -> T>state key=value ...  (view, maximized panel, dialogs, transport, BASS KEYS,
//                                 heap, audio xruns, touch rate, headphones, SD, Bluetooth and the Pico's buffer)
//   audio                      -> T>audio ...            (engine telemetry: peaks, voices, CPU, bass notes, step)
//   prof                       -> T>prof ...             (0.4.9: audio-core cycles per block of each part, the SYNTH
//                                 job on the helper core, the wait for it, the FX bus)
//   cores [1|2]                -> T>cores dual=...       (0.4.9: SYNTH inline (1) or on the second core (2))
//   load                       -> T>load ...             (0.4.9: busy % of both cores and of the busiest tasks since
//                                 the last "load", free stack of the audio and helper tasks)
//   fxalgo [0|1]               -> T>fxalgo block=...     (0.4.9: stereo delay reference code (0) or block-wise (1))
//   fxcheck                    -> T>fxcheck ...          (0.4.9: block-wise delay == reference, bit for bit, here)
//   membench                   -> T>membench ...         (0.4.9: cycles per access of PSRAM / internal RAM / RTC RAM /
//                                 TCM in delay-line streams on the audio core, free memory per kind)
//   quiet [ms]                 -> T>quiet ...            (0.4.9: the UI paused for ms (1200; LVGL lock held): the
//                                 engine profile and membench of that time, to see what the UI costs the audio core)
//   stress raw|zero|copy|sample KB -> T>stress ...       (0.4.9: big PSRAM work as the app does it: raw = an unpaced
//                                 memset (the old bigAlloc), zero = bigAlloc, copy = bulkCopy, sample = a sample made
//                                 and edited (sampleCreate + opGain + analysis))
//   bulkrate [bytes_per_ms]    -> T>bulkrate ...         (0.4.9: the bulk pacing rate, 0 = off)
//   memlayout [0|1]            -> T>memlayout ...        (0.4.9: the memory layout; with a value: restart with it)
//   uistat                     -> T>uistat ...           (0.4.9: since the last uistat: LVGL refreshes per second,
//                                 their render and PPA-wait ms per second, areas and kpx per second, and the ms per
//                                 second of each part of the UI tick)
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

#include "core/fxcheck.h"
#include "core/sample.h"
#include "core/sampleio.h"
#include "core/platform.h"
#include "hal/hal.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/widgets.h"

#ifdef ESP_PLATFORM
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "esp_cpu.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
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
    out("T>audio peak_l=%.4f peak_r=%.4f voices=%d cpu=%.3f xruns=%lu bass_notes=%lu playing=%d step=%d beat=%.3f "
        "synth_notes=%lu synth_voices=%d",
           (double)t.master_peak[0].load(), (double)t.master_peak[1].load(), (int)t.voices.load(), (double)t.cpu.load(),
           (unsigned long)t.xruns.load(), (unsigned long)t.bass_notes.load(), (int)t.playing.load(),
           (int)t.step.load(), (double)t.beat.load(), (unsigned long)t.synth_notes.load(), (int)t.synth_voices.load());
}

// 0.4.9: where the audio block's CPU time goes (Telemetry::prof, cycles per block, last 256 blocks)
void cmdProf()
{
    const Telemetry& t = app().engine().telemetry();
    out("T>prof block=%lu voices_x100=%lu pre=%lu pads=%lu bass=%lu synth=%lu preview=%lu master=%lu post=%lu "
        "job=%lu wait=%lu fx=%lu dly=%lu rev=%lu fxref=%d dual=%d stalls=%lu helper_free=%d",
        (unsigned long)t.prof_block.load(), (unsigned long)t.prof_voices_x100.load(),
        (unsigned long)t.prof[Telemetry::kProfPre].load(), (unsigned long)t.prof[Telemetry::kProfVoices].load(),
        (unsigned long)t.prof[Telemetry::kProfBass].load(), (unsigned long)t.prof[Telemetry::kProfSynth].load(),
        (unsigned long)t.prof[Telemetry::kProfPreview].load(), (unsigned long)t.prof[Telemetry::kProfMaster].load(),
        (unsigned long)t.prof[Telemetry::kProfPost].load(), (unsigned long)t.prof_job.load(),
        (unsigned long)t.prof_wait.load(), (unsigned long)t.prof_fx.load(), (unsigned long)t.prof_dly.load(),
        (unsigned long)t.prof_rev.load(), app().engine().fxReference() ? 1 : 0, (int)t.dual.load(),
        (unsigned long)t.helper_stalls.load(), helperStackFree());
}

void cmdFxAlgo(const char* arg)
{
    int v[1] = {};
    if (ints(arg, v, 1) == 1) app().engine().setFxReference(v[0] == 0);
    out("T>fxalgo block=%d", app().engine().fxReference() ? 0 : 1);
}

void cmdFxCheck()
{
    const int rate = app().engine().sampleRate();
    const uint64_t t0 = nowUs();
    const FxCheck c = fxSelfCheck(rate > 0 ? rate : 48000, 1500);
    out("T>fxcheck ran=%d rate=%d blocks=%lld samples=%lld delay_diff=%lld delay_block=%lld delay_rms=%.4f ms=%lu",
        c.ran ? 1 : 0, rate, (long long)c.blocks, (long long)c.samples, (long long)c.delay_diff,
        (long long)c.delay_block, c.delay_rms, (unsigned long)((nowUs() - t0) / 1000));
}

void cmdCores(const char* arg)
{
    int v[1] = {};
    if (ints(arg, v, 1) == 1) app().engine().setDualCore(v[0] >= 2);
    sleepMs(20);   // a few blocks: Telemetry::dual follows
    out("T>cores dual=%d set=%d", (int)app().engine().telemetry().dual.load(), app().engine().dualCore() ? 1 : 0);
}

#if defined(ESP_PLATFORM) && (configGENERATE_RUN_TIME_STATS == 1)
// Busy % of each core (from its idle task's run time) and of the busiest tasks, since the last call (the first
// call: since boot). Run-time counters are esp_timer microseconds.
void cmdLoad()
{
    constexpr int kMax = 40;
    struct Prev {
        TaskHandle_t h;
        configRUN_TIME_COUNTER_TYPE rt;
    };
    static TaskStatus_t* st = nullptr;
    static Prev* prev = nullptr;
    static int nprev = 0;
    static uint64_t prev_t = 0, prev_idle[2] = {};
    if (!st) st = static_cast<TaskStatus_t*>(heap_caps_malloc(sizeof(TaskStatus_t) * kMax, MALLOC_CAP_SPIRAM));
    if (!prev) prev = static_cast<Prev*>(heap_caps_calloc(kMax, sizeof(Prev), MALLOC_CAP_SPIRAM));
    if (!st || !prev) {
        out("T>err no memory");
        return;
    }
    configRUN_TIME_COUNTER_TYPE total = 0;
    const int n = (int)uxTaskGetSystemState(st, kMax, &total);
    const uint64_t now = (uint64_t)esp_timer_get_time();
    const uint64_t idle0 = ulTaskGetIdleRunTimeCounterForCore(0), idle1 = ulTaskGetIdleRunTimeCounterForCore(1);
    const double dt = (double)(now - prev_t);
    const double busy0 = 100.0 * (1.0 - (double)(idle0 - prev_idle[0]) / dt);
    const double busy1 = 100.0 * (1.0 - (double)(idle1 - prev_idle[1]) / dt);
    // per-task deltas, the 8 largest
    struct Row {
        const char* name;
        double pct;
    } rows[kMax];
    int nr = 0;
    for (int i = 0; i < n; ++i) {
        configRUN_TIME_COUNTER_TYPE before = 0;
        for (int j = 0; j < nprev; ++j)
            if (prev[j].h == st[i].xHandle) before = prev[j].rt;
        const double d = (double)(st[i].ulRunTimeCounter - before);
        rows[nr++] = Row{st[i].pcTaskName, 100.0 * d / dt};
    }
    for (int i = 0; i < nr; ++i)
        for (int j = i + 1; j < nr; ++j)
            if (rows[j].pct > rows[i].pct) {
                const Row t = rows[i];
                rows[i] = rows[j];
                rows[j] = t;
            }
    int audio_free = -1, dsp_free = -1;
    for (int i = 0; i < n; ++i) {
        if (!strcmp(st[i].pcTaskName, "orbit_audio")) audio_free = (int)st[i].usStackHighWaterMark;
        if (!strcmp(st[i].pcTaskName, "orbit_dsp")) dsp_free = (int)st[i].usStackHighWaterMark;
    }
    char list[400];
    int k = 0;
    list[0] = 0;
    for (int i = 0; i < nr && i < 8 && k < (int)sizeof list - 32; ++i)
        k += snprintf(list + k, sizeof list - (size_t)k, "%s%s:%.1f", i ? "," : "", rows[i].name, rows[i].pct);
    for (int i = 0; i < n; ++i) prev[i] = Prev{st[i].xHandle, st[i].ulRunTimeCounter};
    nprev = n;
    prev_t = now;
    prev_idle[0] = idle0;
    prev_idle[1] = idle1;
    out("T>load dt_ms=%.0f core0=%.1f core1=%.1f audio_stack_free=%d dsp_stack_free=%d tasks=%s", dt / 1000.0, busy0,
        busy1, audio_free, dsp_free, list);
}
#else
void cmdLoad() { out("T>err no run-time stats"); }
#endif

#ifdef ESP_PLATFORM
// Memory access cost as the FX bus sees it: 8 delay-line streams (read half a span back, write, advance) over a
// buffer, run in a task on core 1 below the audio task. Cycles per access, the best of 96 runs of 64 samples (the
// audio task's blocks only lengthen some runs). The UI keeps running on core 0 meanwhile, as in normal use.
struct MemBench {
    float* buf = nullptr;
    int32_t len = 0;
    uint32_t best = 0;
    SemaphoreHandle_t done = nullptr;
};

void memBenchTask(void* p)
{
    MemBench& b = *static_cast<MemBench*>(p);
    constexpr int kStreams = 8, kN = 64, kRuns = 96;
    // spans of len / 8 minus 23 cache lines: the streams fall into different cache sets, as the reverb's lines do
    // (power-of-two spacing would make every stream fight for the same set)
    const int32_t span = b.len / kStreams - 23 * 16, d = span / 2;
    int32_t pos[kStreams];
    for (int k = 0; k < kStreams; ++k) pos[k] = k * span + d;
    uint32_t best = 0xFFFFFFFFu;
    for (int run = 0; run < kRuns; ++run) {
        const uint32_t c0 = (uint32_t)esp_cpu_get_cycle_count();
        for (int i = 0; i < kN; ++i) {
            for (int k = 0; k < kStreams; ++k) {
                int32_t rp = pos[k] - d;
                if (rp < 0) rp += b.len;
                b.buf[pos[k]] = b.buf[rp] * 0.5f;
                if (++pos[k] >= b.len) pos[k] = 0;
            }
        }
        const uint32_t c = (uint32_t)esp_cpu_get_cycle_count() - c0;
        if (c < best) best = c;
        if ((run & 15) == 15) vTaskDelay(1);   // let lower tasks breathe
    }
    b.best = best;
    xSemaphoreGive(b.done);
    vTaskDelete(nullptr);
}

// cycles per access x 100 (-1: no memory of that kind)
long memBenchOne(uint32_t caps, size_t bytes)
{
    MemBench b;
    b.buf = static_cast<float*>(heap_caps_calloc(1, bytes, caps));
    if (!b.buf) return -1;
    b.len = (int32_t)(bytes / sizeof(float));
    b.done = xSemaphoreCreateBinary();
    long r = -1;
    if (b.done && xTaskCreatePinnedToCore(memBenchTask, "orbit_bench", 3072, &b, 2, nullptr, 1) == pdPASS &&
        xSemaphoreTake(b.done, pdMS_TO_TICKS(10000)) == pdTRUE)
        r = (long)((uint64_t)b.best * 100u / (64u * 8u * 2u));
    if (b.done) vSemaphoreDelete(b.done);
    heap_caps_free(b.buf);
    return r;
}

void cmdMemBench()
{
    const size_t fi = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t li = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t ft = heap_caps_get_free_size(MALLOC_CAP_TCM), lt = heap_caps_get_largest_free_block(MALLOC_CAP_TCM);
    const size_t fr = heap_caps_get_free_size(MALLOC_CAP_RTCRAM);
    const size_t lr = heap_caps_get_largest_free_block(MALLOC_CAP_RTCRAM);
    const size_t fp = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const long psram = memBenchOne(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, 1024 * 1024);
    const long internal = li > 24 * 1024 ? memBenchOne(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, 16 * 1024) : -1;
    const long rtc = lr >= 16 * 1024 ? memBenchOne(MALLOC_CAP_RTCRAM, 16 * 1024) : -1;
    const long tcm = lt >= 4 * 1024 ? memBenchOne(MALLOC_CAP_TCM, 4 * 1024) : -1;
    out("T>membench psram_x100=%ld internal_x100=%ld rtc_x100=%ld tcm_x100=%ld free_internal=%u largest=%u tcm=%u "
        "tcm_largest=%u rtc=%u rtc_largest=%u psram=%u",
        psram, internal, rtc, tcm, (unsigned)fi, (unsigned)li, (unsigned)ft, (unsigned)lt, (unsigned)fr, (unsigned)lr,
        (unsigned)fp);
}

// The UI paused: the LVGL lock is held for ms, so the UI task neither draws nor touches memory. The engine profile
// is taken near the end (its 256-block windows then fall inside the pause) together with membench, then the lock
// goes back. Compare with "prof" / "membench" while the UI runs.
void cmdQuiet(const char* arg)
{
    int v[1] = {1200};
    ints(arg, v, 1);
    const int ms = v[0] < 800 ? 800 : (v[0] > 3000 ? 3000 : v[0]);
    if (!lock()) {
        out("T>err busy");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(ms - 250));
    const long psram = memBenchOne(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, 1024 * 1024);
    const size_t li = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const long internal = li > 24 * 1024 ? memBenchOne(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, 16 * 1024) : -1;
    const Telemetry& t = app().engine().telemetry();
    char buf[400];
    snprintf(buf, sizeof buf,
             "T>quiet ms=%d block=%lu voices_x100=%lu pre=%lu pads=%lu bass=%lu synth=%lu preview=%lu master=%lu "
             "post=%lu job=%lu wait=%lu fx=%lu dly=%lu rev=%lu psram_x100=%ld internal_x100=%ld",
             ms, (unsigned long)t.prof_block.load(), (unsigned long)t.prof_voices_x100.load(),
             (unsigned long)t.prof[Telemetry::kProfPre].load(), (unsigned long)t.prof[Telemetry::kProfVoices].load(),
             (unsigned long)t.prof[Telemetry::kProfBass].load(), (unsigned long)t.prof[Telemetry::kProfSynth].load(),
             (unsigned long)t.prof[Telemetry::kProfPreview].load(), (unsigned long)t.prof[Telemetry::kProfMaster].load(),
             (unsigned long)t.prof[Telemetry::kProfPost].load(), (unsigned long)t.prof_job.load(),
             (unsigned long)t.prof_wait.load(), (unsigned long)t.prof_fx.load(), (unsigned long)t.prof_dly.load(),
             (unsigned long)t.prof_rev.load(), psram, internal);
    unlock();
    out("%s", buf);
}

void cmdStress(const char* arg)
{
    while (*arg == ' ') ++arg;
    char mode[8] = {};
    int i = 0;
    while (*arg && *arg != ' ' && i < 7) mode[i++] = *arg++;
    int v[1] = {1024};
    ints(arg, v, 1);
    const size_t bytes = (size_t)(v[0] < 16 ? 16 : (v[0] > 8192 ? 8192 : v[0])) * 1024u;
    const uint64_t t0 = nowUs();
    bool ok = true;
    if (!strcmp(mode, "raw")) {   // what bigAlloc did up to 0.4.9: calloc = one memset at full speed
        void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ok = p != nullptr;
        if (p) {
            memset(p, 0, bytes);
            heap_caps_free(p);
        }
    } else if (!strcmp(mode, "zero")) {
        void* p = bigAlloc(bytes);
        ok = p != nullptr;
        bigFree(p);
    } else if (!strcmp(mode, "copy")) {
        void* a = bigAllocRaw(bytes);
        void* b = bigAllocRaw(bytes);
        ok = a && b;
        if (ok) bulkCopy(b, a, bytes);
        bigFree(a);
        bigFree(b);
    } else if (!strcmp(mode, "sample")) {   // a mono sample of `bytes`, written, then GAIN (copy-on-write) + analysis
        Sample* s = sampleCreate((uint32_t)(bytes / 2u), 1, 48000);
        ok = s != nullptr;
        if (s) {
            bulkZero(s->data, bytes);
            Sample* g = opGain(s, -3.0f);
            ok = g != nullptr;
            sampleFree(g);
            sampleFree(s);
        }
    } else {
        out("T>err stress raw|zero|copy|sample KB");
        return;
    }
    out("T>stress mode=%s kb=%u ok=%d ms=%lu rate=%lu", mode, (unsigned)(bytes / 1024u), ok ? 1 : 0,
        (unsigned long)((nowUs() - t0) / 1000u), (unsigned long)bulkRate());
}
#else
void cmdMemBench() { out("T>err no device"); }
void cmdQuiet(const char*) { out("T>err no device"); }
void cmdStress(const char*) { out("T>err no device"); }
#endif

void cmdUiStat()
{
    static hal::DisplayStats pd;
    static TickProf pt;
    static uint64_t p_us = 0;
    hal::DisplayStats d;
    hal::displayStats(d);
    const TickProf t = tickProf();
    const uint64_t now = nowUs();
    const double s = p_us ? (double)(now - p_us) / 1e6 : 1.0;
    const double ms_per_cyc = 1.0 / 360000.0;   // 360 MHz
    auto ms = [&](uint64_t a, uint64_t b) { return (double)(a - b) * ms_per_cyc / s; };
    char buf[600];
    int k = snprintf(buf, sizeof buf,
                     "T>uistat dt_ms=%.0f refresh_hz=%.1f render_ms=%.1f wait_ms=%.1f areas_hz=%.1f kpx_hz=%.1f "
                     "tick_hz=%.1f",
                     s * 1000.0, (d.refreshes - pd.refreshes) / s, ms(d.refr_cycles, pd.refr_cycles),
                     ms(d.wait_cycles, pd.wait_cycles), (d.areas - pd.areas) / s, (double)(d.px - pd.px) / 1000.0 / s,
                     (t.ticks - pt.ticks) / s);
    static const char* const kNames[TickProf::kParts] = {"app", "keys", "top", "pads", "cat",
                                                         "macros", "timeline", "view", "dialogs"};
    for (int i = 0; i < TickProf::kParts && k < (int)sizeof buf - 24; ++i)
        k += snprintf(buf + k, sizeof buf - (size_t)k, " %s=%.2f", kNames[i], ms(t.cyc[i], pt.cyc[i]));
    static DrawProf pdp;
    const DrawProf dp = drawProf();
    static const char* const kDraw[DrawProf::kParts] = {"draw_pads", "draw_grid", "draw_wave"};
    for (int i = 0; i < DrawProf::kParts && k < (int)sizeof buf - 40; ++i)
        k += snprintf(buf + k, sizeof buf - (size_t)k, " %s=%.2f %s_n=%.0f", kDraw[i], ms(dp.cyc[i], pdp.cyc[i]),
                      kDraw[i], (dp.calls[i] - pdp.calls[i]) / s);
    k += snprintf(buf + k, sizeof buf - (size_t)k, " cull=%d layout=%d lines_internal=%d", pads::culling() ? 1 : 0,
                  hal::memLayout(), app().engine().fxLinesInternal() ? 1 : 0);
    pdp = dp;
    pd = d;
    pt = t;
    p_us = now;
    out("%s", buf);
}

void cmdBulkRate(const char* arg)
{
    int v[1] = {};
    if (ints(arg, v, 1) == 1) bulkSetRate(v[0] < 0 ? 0u : (uint32_t)v[0]);
    out("T>bulkrate bytes_per_ms=%lu", (unsigned long)bulkRate());
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
    } else if (!strncmp(line, "prof", 4)) {
        cmdProf();
    } else if (!strncmp(line, "cores", 5)) {
        cmdCores(line + 5);
    } else if (!strncmp(line, "fxalgo", 6)) {
        cmdFxAlgo(line + 6);
    } else if (!strncmp(line, "fxcheck", 7)) {
        cmdFxCheck();
    } else if (!strncmp(line, "membench", 8)) {
        cmdMemBench();
    } else if (!strncmp(line, "quiet", 5)) {
        cmdQuiet(line + 5);
    } else if (!strncmp(line, "stress", 6)) {
        cmdStress(line + 6);
    } else if (!strncmp(line, "bulkrate", 8)) {
        cmdBulkRate(line + 8);
    } else if (!strncmp(line, "uistat", 6)) {
        cmdUiStat();
    } else if (!strncmp(line, "memlayout", 9)) {
        int v[1] = {-1};
        if (ints(line + 9, v, 1) == 1 && (v[0] == 0 || v[0] == 1)) {
            out("T>memlayout restart %d", v[0]);
            sleepMs(100);
            hal::memLayoutRestart(v[0]);
        }
        out("T>memlayout layout=%d lines_internal=%d heap=%lu", hal::memLayout(),
            app().engine().fxLinesInternal() ? 1 : 0, (unsigned long)hal::freeInternal());
    } else if (!strncmp(line, "fxoff", 5)) {   // 0.4.9 diagnostics: fxoff 1 = no delay / reverb (memory tests)
        int v[1] = {0};
        ints(line + 5, v, 1);
        app().engine().setFxBypass(v[0] != 0);
        out("T>fxoff %d", v[0] != 0 ? 1 : 0);
    } else if (!strncmp(line, "padskip", 7)) {   // 0.4.9 diagnostics: padskip mask (1 img 2 rings 4 tints 8 labels)
        int v[1] = {0};
        ints(line + 7, v, 1);
        if (lock()) {
            pads::setDebugSkip((uint8_t)v[0]);
            unlock();
        }
        out("T>padskip %d", v[0]);
    } else if (!strncmp(line, "uicull", 6)) {   // 0.4.9: uicull 0|1 = pads drawn the old way / culled
        int v[1] = {1};
        ints(line + 6, v, 1);
        if (lock()) {
            pads::setCulling(v[0] != 0);
            unlock();
        }
        out("T>uicull %d", pads::culling() ? 1 : 0);
    } else if (!strncmp(line, "load", 4)) {
        cmdLoad();
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
