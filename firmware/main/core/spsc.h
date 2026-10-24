#pragma once
// Lock-free single-producer / single-consumer ring buffer. Wait-free on both sides, no
// allocation after construction: safe to use from the audio task. N must be a power of two.
#include <atomic>
#include <stdint.h>

namespace orbit {

template <typename T, uint32_t N>
class Spsc {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");

public:
    bool push(const T& v)
    {
        const uint32_t w = write_.load(std::memory_order_relaxed);
        const uint32_t r = read_.load(std::memory_order_acquire);
        if (w - r >= N) return false;  // full
        buf_[w & (N - 1)] = v;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& v)
    {
        const uint32_t r = read_.load(std::memory_order_relaxed);
        const uint32_t w = write_.load(std::memory_order_acquire);
        if (r == w) return false;  // empty
        v = buf_[r & (N - 1)];
        read_.store(r + 1, std::memory_order_release);
        return true;
    }
    uint32_t size() const { return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire); }
    bool empty() const { return size() == 0; }

private:
    T buf_[N];
    alignas(64) std::atomic<uint32_t> write_{0};
    alignas(64) std::atomic<uint32_t> read_{0};
};

}  // namespace orbit
