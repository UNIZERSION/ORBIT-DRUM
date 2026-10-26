#pragma once
// Tiny portability layer for core/ code: memory, time and logging. Implemented by
// hal/platform_idf.cpp on the Tab5 and host/platform_host.cpp on the PC.
#include <stddef.h>
#include <stdint.h>

namespace orbit {

void* bigAlloc(size_t bytes);      // large buffers: PSRAM on the Tab5 (16-byte aligned), zeroed
void* bigAllocRaw(size_t bytes);   // the same, NOT zeroed: for buffers the caller fills completely (0.4.9)
void bigFree(void* p);
void* fastAlloc(size_t bytes);     // small hot buffers: internal RAM on the Tab5, zeroed
bool memIsInternal(const void* p); // 0.4.9: p lies in internal RAM (the PC: always)
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
// Audio helper (0.4.9): runs one job at a time for the audio task on the other core. Tab5: a task pinned to core 0
// at the audio task's priority, woken per job; PC: helperPost runs the job at once. helperPost returns false when
// there is no helper or its last job is still running (the caller then runs the job itself); helperDone: the last
// posted job has finished (the audio task spins on it). helperPost / helperDone: audio task only.
bool helperStart();
bool helperPost(void (*fn)(void* arg), void* arg);
bool helperDone();
int helperStackFree();   // stack bytes the helper task never used (0 on the PC)

// Bulk PSRAM work (0.4.9). Zeroing, copying or one pass over a whole sample at full speed crowds the PSRAM bus the
// audio task reads its sample data and FX lines from, and slows its blocks down (dropouts on the Tab5). While the
// audio runs, such loops call bulkPace() after each chunk (kBulkChunk bytes or so): it waits while the audio task is
// busy (hal::audioLoad) and whenever the work runs ahead of the bulk rate (bulkSetRate, bytes per ms; 0 = no
// pacing). bigAlloc zeroes big blocks this way. The PC never paces. Tasks only (never an ISR); the audio task does no
// bulk work. Never call it (nor bigAlloc of >= 64 KB) inside a critical section or under a spinlock: it sleeps.
struct BulkPacer {
    uint64_t t0 = 0;        // first call (us)
    uint64_t bytes = 0;     // bytes counted so far
    bool rate_only = false; // true: no waiting for the audio load (the UI task: a short wait per call only)
};
constexpr size_t kBulkChunk = 8 * 1024;
void bulkPace(BulkPacer& p, size_t bytes);
void bulkZero(void* dst, size_t n);
void bulkCopy(void* dst, const void* src, size_t n);   // memcpy (no overlap), paced
void bulkSetRate(uint32_t bytes_per_ms);
uint32_t bulkRate();

}  // namespace orbit
