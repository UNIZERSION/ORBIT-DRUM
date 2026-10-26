// core/platform.h on the Tab5 (ESP-IDF): memory, time, logging, the loader worker.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/platform.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/hal.h"

namespace orbit {

namespace {
SemaphoreHandle_t s_work = nullptr;

SemaphoreHandle_t workSemaphore()
{
    SemaphoreHandle_t s = __atomic_load_n(&s_work, __ATOMIC_ACQUIRE);
    if (s) return s;
    SemaphoreHandle_t fresh = xSemaphoreCreateCounting(64, 0);
    SemaphoreHandle_t expected = nullptr;
    if (!__atomic_compare_exchange_n(&s_work, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        vSemaphoreDelete(fresh);   // another task created it first
        return expected;
    }
    return fresh;
}

struct WorkerStart {
    void (*fn)(void*);
    void* arg;
};

// FreeRTOS tasks must not return: the worker function may
void workerEntry(void* p)
{
    WorkerStart w = *static_cast<WorkerStart*>(p);
    free(p);
    w.fn(w.arg);
    vTaskDelete(nullptr);
}

// Audio helper (0.4.9): core 0, the audio task's priority (above the UI, display, touch and USB tasks, so a job
// starts within microseconds of helperPost and is not cut into by them). One job at a time: s_job_busy is set by
// helperPost (audio task, core 1) and cleared by the helper when the job is done.
constexpr uint32_t kHelperStack = 4096;
TaskHandle_t s_helper = nullptr;
void (*s_job_fn)(void*) = nullptr;
void* s_job_arg = nullptr;
uint32_t s_job_busy = 0;

IRAM_ATTR void helperTask(void*)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (__atomic_load_n(&s_job_busy, __ATOMIC_ACQUIRE)) {
            s_job_fn(s_job_arg);
            __atomic_store_n(&s_job_busy, 0u, __ATOMIC_RELEASE);
        }
    }
}
}  // namespace

bool helperStart()
{
    if (s_helper) return true;
    TaskHandle_t t = nullptr;
    if (xTaskCreatePinnedToCore(helperTask, "orbit_dsp", kHelperStack, nullptr, configMAX_PRIORITIES - 2, &t, 0) !=
        pdPASS)
        return false;
    s_helper = t;
    return true;
}

IRAM_ATTR bool helperPost(void (*fn)(void*), void* arg)
{
    if (!s_helper || !fn || __atomic_load_n(&s_job_busy, __ATOMIC_ACQUIRE)) return false;
    s_job_fn = fn;
    s_job_arg = arg;
    __atomic_store_n(&s_job_busy, 1u, __ATOMIC_RELEASE);
    xTaskNotifyGive(s_helper);
    return true;
}

IRAM_ATTR bool helperDone() { return __atomic_load_n(&s_job_busy, __ATOMIC_ACQUIRE) == 0; }

int helperStackFree() { return s_helper ? (int)uxTaskGetStackHighWaterMark(s_helper) : 0; }

// ---- bulk PSRAM pacing (platform.h)
// Measured on the Tab5 (device_test t_bulk, demo scene D, audio core ~58 %): bulk work at ~12-15 MB/s for ~300 ms
// slowed the audio renders past the block time (5-6 dropouts); the same work at 2-3 MB/s did not. The audio core has
// little slack while a dense scene plays and the UI draws, so the bulk work yields to it: before each chunk it waits
// while the audio task's blocks take more than kBulkLoadMax of the block time, and it never runs faster than the rate.
namespace {
constexpr uint32_t kBulkDefaultRate = 16 * 1024;   // bytes per ms: the upper bound
constexpr float kBulkLoadMax = 0.70f;              // audioLoad() above this: wait
constexpr int kBulkMaxWaitMs = 10;                 // per chunk: dense scenes slow bulk work (~0.8 MB/s), never stop it
constexpr size_t kBulkPacedAlloc = 64 * 1024;      // bigAlloc paces its zeroing from this size
uint32_t s_bulk_rate = kBulkDefaultRate;           // __atomic
}  // namespace

void bulkSetRate(uint32_t bytes_per_ms) { __atomic_store_n(&s_bulk_rate, bytes_per_ms, __ATOMIC_RELAXED); }

uint32_t bulkRate() { return __atomic_load_n(&s_bulk_rate, __ATOMIC_RELAXED); }

void bulkPace(BulkPacer& p, size_t bytes)
{
    const uint32_t rate = bulkRate();
    if (!rate || !hal::audioRunning() || xPortInIsrContext() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING)
        return;
    // yield to the audio task while its blocks are slow (this work's memory traffic is part of why)
    if (!p.rate_only)
        for (int waited = 0; waited < kBulkMaxWaitMs && hal::audioLoad() > kBulkLoadMax; waited += 2) vTaskDelay(2);
    const uint64_t now = (uint64_t)esp_timer_get_time();
    if (!p.t0) p.t0 = now;
    p.bytes += bytes;
    const uint64_t due = p.bytes * 1000u / rate;   // us these bytes may take at the rate
    const uint64_t el = now - p.t0;
    if (due > el + 1000u) {
        const uint32_t ms = (uint32_t)((due - el) / 1000u);
        const TickType_t t = pdMS_TO_TICKS(ms);
        vTaskDelay(t ? t : 1);
    }
}

void bulkZero(void* dst, size_t n)
{
    BulkPacer p;
    uint8_t* d = static_cast<uint8_t*>(dst);
    while (n) {
        const size_t k = n < kBulkChunk ? n : kBulkChunk;
        memset(d, 0, k);
        d += k;
        n -= k;
        bulkPace(p, k);
    }
}

void bulkCopy(void* dst, const void* src, size_t n)
{
    BulkPacer p;
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    while (n) {
        const size_t k = n < kBulkChunk ? n : kBulkChunk;
        memcpy(d, s, k);
        d += k;
        s += k;
        n -= k;
        bulkPace(p, 2 * k);   // read + write
    }
}

void* bigAllocRaw(size_t bytes)
{
    if (bytes == 0) bytes = 1;
    void* p = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p;
}

void* bigAlloc(size_t bytes)
{
    if (bytes == 0) bytes = 1;
    if (bytes >= kBulkPacedAlloc && hal::audioRunning()) {   // zeroing a big block at full speed: dropouts
        void* p = bigAllocRaw(bytes);
        if (p) bulkZero(p, bytes);
        return p;
    }
    void* p = heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p;
}

void bigFree(void* p)
{
    if (p) heap_caps_free(p);
}

bool memIsInternal(const void* p) { return p && esp_ptr_internal(p); }

void* fastAlloc(size_t bytes)
{
    if (bytes == 0) bytes = 1;
    void* p = heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   // slower, still works
    return p;
}

void fastFree(void* p)
{
    if (p) heap_caps_free(p);
}

uint64_t nowUs() { return (uint64_t)esp_timer_get_time(); }

void logf(const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGI("orbit", "%s", buf);
}

bool startWorker(void (*fn)(void* arg), void* arg, const char* name, int stack_bytes, int priority)
{
    if (!fn) return false;
    WorkerStart* w = static_cast<WorkerStart*>(malloc(sizeof(WorkerStart)));
    if (!w) return false;
    w->fn = fn;
    w->arg = arg;
    if (stack_bytes < 2048) stack_bytes = 2048;
    if (priority < 1) priority = 1;
    if (priority > configMAX_PRIORITIES - 3) priority = configMAX_PRIORITIES - 3;   // below the audio task
    if (xTaskCreatePinnedToCore(workerEntry, name ? name : "orbit_worker", (uint32_t)stack_bytes, w,
                                (UBaseType_t)priority, nullptr, 0) != pdPASS) {
        free(w);
        return false;
    }
    return true;
}

void sleepMs(int ms)
{
    const TickType_t t = pdMS_TO_TICKS(ms > 0 ? ms : 0);
    vTaskDelay(t ? t : (ms > 0 ? 1 : 0));
}

void workerSignal()
{
    SemaphoreHandle_t s = workSemaphore();
    if (s) xSemaphoreGive(s);
}

void workerWait(int timeout_ms)
{
    SemaphoreHandle_t s = workSemaphore();
    if (!s) {
        sleepMs(timeout_ms > 0 ? timeout_ms : 10);
        return;
    }
    xSemaphoreTake(s, timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms));
}

}  // namespace orbit
