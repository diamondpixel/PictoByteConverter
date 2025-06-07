#ifndef LOCKFREE_QUEUE_H
#define LOCKFREE_QUEUE_H

#include <vector>
#include <atomic>
#include <condition_variable>
#include <cassert>
#include <cstddef>
#include <limits>
#include <Debug/headers/LogMacros.h>
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

    inline size_t calculate_actual_capacity(size_t requested_capacity) {
        size_t new_capacity;

        if (requested_capacity == std::numeric_limits<size_t>::max()) {
            new_capacity = static_cast<size_t>(1) << ((sizeof(size_t) * 8) - 1);
        } else {
            new_capacity = next_pow2(requested_capacity);
        }
        if (new_capacity < 2) {
            new_capacity = 2;
        }

        return new_capacity;
    }
}

/**
 * @brief LockFreeQueue is a thread-safe queue for concurrent producers and consumers.
 * @tparam T Type of elements stored in the queue.
 */
template<typename T>
class LockFreeQueue final : public QueueBase<T> {
public:
    /**
     * @brief Construct a new LockFreeQueue.
     * @param capacity The maximum number of elements the queue can hold.
     * @param name The name of the queue (optional, defaults to "LFQueue").
     */
    explicit LockFreeQueue(size_t capacity, std::string name = "LFQueue")
        : capacity_(lf::calculate_actual_capacity(capacity)),
          mask_(capacity_ - 1),
          name_(std::move(name)),
          buffer_(capacity_) {
        std::string log_message = "Created " + name_ + " capacity " + std::to_string(capacity_);
        LOG_DBG("LockFreeQueue", log_message);
    }

    /**
     * @brief Destroy the LockFreeQueue and free all resources.
     */
    ~LockFreeQueue() override { shutdown(); }

    /**
     * @brief Push an item to the back of the queue.
     * @param item The item to enqueue.
     * @return True if the item was successfully enqueued, false if the queue is shutting down.
     */
    bool push(T &&item) override {
        while (!try_push(std::move(item))) {
            if (is_shutting_down()) return false;
            std::unique_lock<std::mutex> lk(cv_mutex_);
            not_full_cv_.wait(lk, [&] { return shutting_.load() || size() < capacity_; });
        }
        return true;
    }

    /**
     * @brief Attempt to push an item to the back of the queue without blocking.
     * @param item The item to enqueue.
     * @return True if the item was successfully enqueued, false if the queue is full.
     */
    bool try_push(T &&item) override {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= capacity_) return false; // full
        if (!head_.compare_exchange_strong(head, head + 1, std::memory_order_acq_rel)) return false;
        buffer_[head & mask_] = std::move(item);
        not_empty_cv_.notify_one();
        return true;
    }

    /**
     * @brief Pop an item from the front of the queue.
     * @param out Reference to store the popped item.
     * @return True if an item was successfully popped, false if the queue is shutting down.
     */
    bool pop(T &out) override {
        while (!try_pop(out)) {
            if (is_shutting_down()) return false;
            std::unique_lock<std::mutex> lk(cv_mutex_);
            not_empty_cv_.wait(lk, [&] { return shutting_.load() || !empty(); });
        }
        return true;
    }

    /**
     * @brief Attempt to pop an item from the front of the queue without blocking.
     * @param out Reference to store the popped item.
     * @return True if an item was successfully popped, false if the queue is empty.
     */
    bool try_pop(T &out) override {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t head = head_.load(std::memory_order_acquire);
        if (tail >= head) return false; // empty
        if (!tail_.compare_exchange_strong(tail, tail + 1, std::memory_order_acq_rel)) return false;
        out = std::move(buffer_[tail & mask_]);
        not_full_cv_.notify_one();
        return true;
    }

    /**
     * @brief Shut down the queue and prevent further operations.
     */
    void shutdown() override {
        shutting_.store(true, std::memory_order_release);
        not_empty_cv_.notify_all();
        not_full_cv_.notify_all();
    }

    /**
     * @brief Check if the queue is empty.
     * @return True if the queue is empty, false otherwise.
     */
    bool empty() const override { return size() == 0; }

    /**
     * @brief Get the current number of elements in the queue.
     * @return Number of elements in the queue.
     */
    size_t size() const override { return head_.load() - tail_.load(); }

    /**
     * @brief Check if the queue is shutting down.
     * @return True if the queue is shutting down, false otherwise.
     */
    bool is_shutting_down() const override { return shutting_.load(); }

    /**
     * @brief Get the name of the queue.
     * @return The name of the queue.
     */
    const std::string &name() const override { return name_; }

    /**
     * @brief Get the capacity of the queue.
     * @return The maximum number of elements the queue can hold.
     */
    size_t capacity() const { return capacity_; }

    /**
     * @brief Get the memory usage of the queue.
     * @return The total memory used by the queue.
     */
    size_t getMemoryUsage() const override { return sizeof(*this) + sizeof(T) * capacity_; }

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
