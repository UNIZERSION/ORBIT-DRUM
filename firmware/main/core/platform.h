#pragma once
// Tiny portability layer for core/ code: memory, time and logging. Implemented by
// hal/platform_idf.cpp on the Tab5 and host/platform_host.cpp on the PC.
#include <stddef.h>
#include <stdint.h>

namespace orbit {

void* bigAlloc(size_t bytes);      // large buffers: PSRAM on the Tab5 (16-byte aligned), zeroed
void bigFree(void* p);
void* fastAlloc(size_t bytes);     // small hot buffers: internal RAM on the Tab5, zeroed
void fastFree(void* p);
uint64_t nowUs();                  // monotonic microseconds
void logf(const char* fmt, ...);   // never call from the audio task
// Background worker (loader task): FreeRTOS task on the Tab5 (core 0, low priority), a thread
// on the PC. Returns false on failure.
bool startWorker(void (*fn)(void* arg), void* arg, const char* name, int stack_bytes, int priority);
void sleepMs(int ms);
// Wakes a worker blocked in workerWait() (counting semaphore semantics).
void workerSignal();
void workerWait(int timeout_ms);

}  // namespace orbit
