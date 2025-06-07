#ifndef WORK_STEALING_DEQUE_H
#define WORK_STEALING_DEQUE_H

#include <deque>
#include <mutex>
#include <optional>
#include <atomic>

/**
 * @brief WorkStealingDeque is a thread-safe double-ended queue for work-stealing schedulers.
 *
 * Owner thread should use push_back and pop_front (FIFO). Thieves use steal(), which pops from the back.
 * Not lock-free: uses a single mutex, as contention is expected to be low.
 *
 * @tparam T Type of elements stored in the deque.
 */
template<typename T>
class WorkStealingDeque {
public:
    /**
     * @brief Construct a new WorkStealingDeque.
     */
    WorkStealingDeque() = default;

    /**
     * @brief Copy constructor is deleted.
     */
    WorkStealingDeque(const WorkStealingDeque &) = delete;

    /**
     * @brief Assignment operator is deleted.
     */
    WorkStealingDeque &operator=(const WorkStealingDeque &) = delete;

    /**
     * @brief Push an item to the back of the deque (owner thread only).
     * @param item The item to push.
     */
    void push_back(T &&item) {
        std::lock_guard<std::mutex> lock(mutex_);
        deq_.emplace_back(std::move(item));
    }

    /**
     * @brief Pop an item from the front of the deque (owner thread only).
     * @param item Reference to store the popped item.
     * @return True if an item was popped, false if the deque was empty.
     */
    bool pop_front(T &item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (deq_.empty()) return false;
        item = std::move(deq_.front());
        deq_.pop_front();
        return true;
    }

    /**
     * @brief Overload returning optional (used in ThreadPool edits).
     * @param opt Reference to store the popped item.
     * @return True if an item was popped, false if the deque was empty.
     */
    bool pop_front(std::optional<T> &opt) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (deq_.empty()) return false;
        opt.emplace(std::move(deq_.front()));
        deq_.pop_front();
        return true;
    }

    /**
     * @brief Steal an item from the back of the deque (other threads).
     * @param item Reference to store the stolen item.
     * @return True if an item was stolen, false if the deque was empty.
     */
    bool steal(T &item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (deq_.empty()) return false;
        item = std::move(deq_.back());
        deq_.pop_back();
        return true;
    }

    /**
     * @brief Check if the deque is empty.
     * @return True if the deque is empty, false otherwise.
     */
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return deq_.empty();
    }

    /**
     * @brief Get the current size of the deque.
     * @return Number of elements in the deque.
     */
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return deq_.size();
    }

private:
    // Internal storage and synchronization
    mutable std::mutex mutex_;
    std::deque<T> deq_;
};

#endif // WORK_STEALING_DEQUE_H
