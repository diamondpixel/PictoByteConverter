#pragma once
#include <atomic>
#include <cstddef>

namespace debug {


template <typename T, size_t CapacityPow2>
class RingBuffer {
    static_assert((CapacityPow2 & (CapacityPow2 - 1)) == 0, "Capacity must be power-of-two");
public:
    bool try_push(const T &v) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next = (head + 1) & mask_;
        if (next == tail_.load(std::memory_order_acquire)) return false; // full
        buffer_[head] = v;
        head_.store(next, std::memory_order_release);
        return true;
    }
    bool try_pop(T &out) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false; // empty
        out = buffer_[tail];
        tail_.store((tail + 1) & mask_, std::memory_order_release);
        return true;
    }
private:
    static constexpr size_t mask_ = CapacityPow2 - 1;
    T buffer_[CapacityPow2];
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};
}
