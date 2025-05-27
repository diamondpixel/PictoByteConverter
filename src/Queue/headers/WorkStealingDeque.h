#ifndef WORK_STEALING_DEQUE_H
#define WORK_STEALING_DEQUE_H

#include <deque>
#include <mutex>
#include <optional>
#include <atomic>

// Simple work-stealing deque suitable for N-thread pool. Owner thread uses
// push_back / pop_front (FIFO). Thieves use steal() which pops from the back.
// This is **not** lock-free; it relies on a single lightweight mutex because
// contention is expected to be low (each deque mostly accessed by one owner).
// It can be replaced with a chase-lev deque later.

template<typename T>
class WorkStealingDeque {
public:
    WorkStealingDeque() = default;
    WorkStealingDeque(const WorkStealingDeque &) = delete;
    WorkStealingDeque &operator=(const WorkStealingDeque &) = delete;

    // Push by owner thread
    void push_back(T &&item) {
        std::lock_guard<std::mutex> lock(mutex_);
        deq_.emplace_back(std::move(item));
    }

    // Pop by owner thread (front) – returns true if an item was popped
    bool pop_front(T &item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (deq_.empty()) return false;
        item = std::move(deq_.front());
        deq_.pop_front();
        return true;
    }

    // Overload returning optional (used in ThreadPool edits)
    bool pop_front(std::optional<T> &opt) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (deq_.empty()) return false;
        opt.emplace(std::move(deq_.front()));
        deq_.pop_front();
        return true;
    }

    // Steal by other threads – pops from back (LIFO) to minimise collisions
    bool steal(T &item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (deq_.empty()) return false;
        item = std::move(deq_.back());
        deq_.pop_back();
        return true;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return deq_.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return deq_.size();
    }

private:
    mutable std::mutex mutex_;
    std::deque<T> deq_;
};

#endif // WORK_STEALING_DEQUE_H
