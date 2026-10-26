// platform.h for the PC (host tests, screenshot renderer).
#include "core/platform.h"

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace orbit {

namespace {
std::mutex g_mu;
std::condition_variable g_cv;
int g_signals = 0;

void* aligned_zalloc(size_t bytes)
{
    if (bytes == 0) bytes = 1;
    const size_t rounded = (bytes + 15) & ~size_t(15);
#if defined(_WIN32)
    void* p = _aligned_malloc(rounded, 16);
#else
    void* p = aligned_alloc(16, rounded);
#endif
    if (p) memset(p, 0, rounded);
    return p;
}

void aligned_free(void* p)
{
#if defined(_WIN32)
    _aligned_free(p);
#else
    free(p);
#endif
}
}  // namespace

void* bigAlloc(size_t bytes) { return aligned_zalloc(bytes); }
// Not zeroed: filled with a pattern so a reader of bytes the caller forgot to write sees garbage in tests
void* bigAllocRaw(size_t bytes)
{
    if (bytes == 0) bytes = 1;
    const size_t rounded = (bytes + 15) & ~size_t(15);
#if defined(_WIN32)
    void* p = _aligned_malloc(rounded, 16);
#else
    void* p = aligned_alloc(16, rounded);
#endif
    if (p) memset(p, 0xA5, rounded);
    return p;
}
void bigFree(void* p) { if (p) aligned_free(p); }
void* fastAlloc(size_t bytes) { return aligned_zalloc(bytes); }
bool memIsInternal(const void* p) { return p != nullptr; }
void fastFree(void* p) { if (p) aligned_free(p); }

uint64_t nowUs()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

void logf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

bool startWorker(void (*fn)(void*), void* arg, const char*, int, int)
{
    try {
        std::thread(fn, arg).detach();
    } catch (...) {
        return false;
    }
    return true;
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// The audio helper on the PC runs each job at once (tests stay deterministic; the engine's dual path still runs).
bool helperStart() { return true; }

bool helperPost(void (*fn)(void*), void* arg)
{
    if (!fn) return false;
    fn(arg);
    return true;
}

bool helperDone() { return true; }

int helperStackFree() { return 0; }

// Bulk pacing is a Tab5 matter (PSRAM bus): plain work on the PC
namespace {
uint32_t g_bulk_rate = 16 * 1024;
}
void bulkPace(BulkPacer& p, size_t bytes) { p.bytes += bytes; }
void bulkZero(void* dst, size_t n) { memset(dst, 0, n); }
void bulkCopy(void* dst, const void* src, size_t n) { memcpy(dst, src, n); }
void bulkSetRate(uint32_t bytes_per_ms) { g_bulk_rate = bytes_per_ms; }
uint32_t bulkRate() { return g_bulk_rate; }

void workerSignal()
{
    {
        std::lock_guard<std::mutex> lk(g_mu);
        ++g_signals;
    }
    g_cv.notify_one();
}

void workerWait(int timeout_ms)
{
    std::unique_lock<std::mutex> lk(g_mu);
    g_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [] { return g_signals > 0; });
    if (g_signals > 0) --g_signals;
}

}  // namespace orbit
