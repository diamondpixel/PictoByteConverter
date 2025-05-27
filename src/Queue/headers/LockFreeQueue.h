#ifndef LOCKFREE_QUEUE_H
#define LOCKFREE_QUEUE_H

#include <vector>
#include <atomic>
#include <condition_variable>
#include <cassert>
#include <cstddef>
#include <Debug/headers/LogBufferManager.h>
#include "QueueBase.h"

// Simple bounded MPMC lock-free-ish queue using a ring buffer.
// Push/pop are wait-free for uncontended case; we still use condition variables
// solely to provide blocking pop/push APIs to match QueueBase.
//
// Capacity MUST be a power of two for mask optimisation.
// This queue is designed for the ThreadPool hot path where contention is low.

namespace lf {
    inline size_t next_pow2(size_t n) {
        if (n == 0) return 1;
        --n;
        for (size_t i = 1; i < sizeof(size_t) * 8; i <<= 1) n |= n >> i;
        return ++n;
    }
}

template <typename T>
class LockFreeQueue final : public QueueBase<T> {
public:
    explicit LockFreeQueue(size_t capacity, std::string name = "LFQueue")
        : capacity_(lf::next_pow2(capacity)),
          mask_(capacity_ - 1),
          name_(std::move(name)),
          buffer_(capacity_) {
        debug::LogBufferManager::getInstance().appendTo(
            "LockFreeQueue", "Created " + name_ + " capacity " + std::to_string(capacity_),
            debug::LogContext::Debug);
    }

    ~LockFreeQueue() override { shutdown(); }

    bool push(T &&item) override {
        while (!try_push(std::move(item))) {
            if (is_shutting_down()) return false;
            std::unique_lock<std::mutex> lk(cv_mutex_);
            not_full_cv_.wait(lk, [&]{ return shutting_.load() || size() < capacity_; });
        }
        return true;
    }

    bool try_push(T &&item) override {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= capacity_) return false; // full
        if (!head_.compare_exchange_strong(head, head + 1, std::memory_order_acq_rel)) return false;
        buffer_[head & mask_] = std::move(item);
        not_empty_cv_.notify_one();
        return true;
    }

    bool pop(T &out) override {
        while (!try_pop(out)) {
            if (is_shutting_down()) return false;
            std::unique_lock<std::mutex> lk(cv_mutex_);
            not_empty_cv_.wait(lk, [&]{ return shutting_.load() || !empty(); });
        }
        return true;
    }

    bool try_pop(T &out) override {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t head = head_.load(std::memory_order_acquire);
        if (tail >= head) return false; // empty
        if (!tail_.compare_exchange_strong(tail, tail + 1, std::memory_order_acq_rel)) return false;
        out = std::move(buffer_[tail & mask_]);
        not_full_cv_.notify_one();
        return true;
    }

    void shutdown() override {
        shutting_.store(true, std::memory_order_release);
        not_empty_cv_.notify_all();
        not_full_cv_.notify_all();
    }

    bool empty() const override { return size() == 0; }
    size_t size() const override { return head_.load() - tail_.load(); }
    bool is_shutting_down() const override { return shutting_.load(); }
    const std::string &name() const override { return name_; }
    size_t capacity() const { return capacity_; }
    size_t getMemoryUsage() const override { return sizeof(*this) + sizeof(T)*capacity_; }

private:
    const size_t capacity_;
    const size_t mask_;
    std::string name_;

    std::vector<T> buffer_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};

    std::atomic<bool> shutting_{false};

    // condition vars for blocking ops
    mutable std::mutex cv_mutex_;
    std::condition_variable not_empty_cv_;
    std::condition_variable not_full_cv_;
};

#endif // LOCKFREE_QUEUE_H
