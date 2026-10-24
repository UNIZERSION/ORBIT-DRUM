// core/platform.h on the Tab5 (ESP-IDF): memory, time, logging, the loader worker.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "core/platform.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

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
}  // namespace

void* bigAlloc(size_t bytes)
{
    if (bytes == 0) bytes = 1;
    void* p = heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p;
}

void bigFree(void* p)
{
    if (p) heap_caps_free(p);
}

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
