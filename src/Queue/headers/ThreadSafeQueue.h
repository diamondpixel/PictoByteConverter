#ifndef THREAD_SAFE_QUEUE_H
#define THREAD_SAFE_QUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

#include <string>

// Include base class for queue functionality
#include "QueueBase.h"

/**
 * @brief Thread-safe queue implementation for concurrent processing
 *
 * This class provides a thread-safe queue with the following features:
 * - Generic template implementation for any type
 * - Optional bounded capacity with flow control
 * - Blocking and non-blocking operations
 * - Timed wait operations
 * - Move semantics and emplace support
 * - Explicit shutdown semantics
 * - Integration with ResourceManager for memory tracking
 *
 * Thread Safety Guarantees:
 * - Multiple threads can call push/pop/etc. concurrently
 * - After shutdown() is called, push operations will fail and pop operations
 *   will drain the queue and then return false
 * - All waiting threads will be unblocked when shutdown() is called
 *
 * @tparam T The type of elements stored in the queue
 */
template<typename T>
struct ThreadSafeQueue : public QueueBase<T> {
public:
    /**
     * @brief Construct a new ThreadSafeQueue
     *
     * @param max_size Maximum capacity of the queue (default: unlimited)
     * @param queue_name Name identifier for this queue (for debugging)
     */
    explicit ThreadSafeQueue(size_t max_size = 100, std::string queue_name = "DefaultQueue");

    /**
     * @brief Destructor ensures proper shutdown
     */
    ~ThreadSafeQueue() override;

    // Disable copying
    ThreadSafeQueue(const ThreadSafeQueue &) = delete;
    ThreadSafeQueue &operator=(const ThreadSafeQueue &) = delete;

    // Allow moving
    ThreadSafeQueue(ThreadSafeQueue &&other) noexcept;
    ThreadSafeQueue &operator=(ThreadSafeQueue &&other) noexcept;

    /**
     * @brief Push a task into the queue (rvalue reference version)
     *
     * @param task Task to push (rvalue reference)
     * @return true if successful, false if the queue is shutting down
     */
    bool push(T &&task) override;

    /**
     * @brief Try to push a task into the queue without blocking (rvalue reference version)
     *
     * @param task Task to push (rvalue reference)
     * @return true if successful, false if the queue is full or shutting down
     */
    bool try_push(T &&task) override;

    /**
     * @brief Pop a task from the queue (legacy version)
     *
     * @param task Reference to store the popped task
     * @return true if a task was popped, false if the queue is empty and shutting down
     */
    bool pop(T &task) override;

    /**
     * @brief Try to pop a task from the queue (legacy version)
     *
     * @param task Reference to store the popped task
     * @return true if a task was popped, false if the queue is empty
     */
    bool try_pop(T &task) override;

    /**
     * @brief Signal that no more items will be added to the queue
     *
     * After this is called, push operations will fail and pop operations
     * will drain the queue and then return false.
     */
    void shutdown() override;

    /**
     * @brief Check if the queue is shutting down
     *
     * @return true if shutting down, false otherwise
     */
    [[nodiscard]] bool is_shutting_down() const override;

    /**
     * @brief Get the current size of the queue
     *
     * @return Current queue size
     */
    [[nodiscard]] size_t size() const override;

    /**
     * @brief Notifies all waiting consumer threads.
     *
     * This is useful if external conditions change that might allow consumers
     * to proceed or terminate, even if no new items were added or the queue
     * wasn't marked 'done'.
     */
    void notify_all_consumers();

    /**
     * @brief Check if the queue is empty
     *
     * @return true if queue is empty, false otherwise
     */
    [[nodiscard]] bool empty() const override;

    /**
     * @brief Check if the queue is full
     *
     * @return true if queue is at capacity, false otherwise
     */
    [[nodiscard]] bool full() const;

    /**
     * @brief Clear all items from the queue
     */
    void clear();

    /**
     * @brief Wait until the queue is empty and no more items will be added
     *
     * This is useful for waiting for all tasks to be processed
     */
    void wait_until_empty();

    /**
     * @brief Get the name of this queue
     *
     * @return The queue name
     */
    [[nodiscard]] const std::string& name() const override;

    /**
     * @brief Get the capacity of this queue
     *
     * @return The queue capacity
     */
    [[nodiscard]] size_t capacity() const;

    /**
     * @brief Get the memory usage of the queue
     * 
     * @return The total memory usage of the queue in bytes
     */
    [[nodiscard]] size_t getMemoryUsage() const override;

private:
    std::queue<T> queue_; // Underlying queue
    mutable std::mutex mutex_; // Mutex for thread safety
    std::condition_variable not_empty_; // Signaled when items are added
    std::condition_variable not_full_; // Signaled when items are removed
    std::condition_variable empty_; // Signaled when queue becomes empty
    size_t max_size_; // Maximum capacity
    std::atomic<bool> shutting_; // Shutdown flag
    std::atomic<size_t> size_; // Current size (atomic for thread safety)
    std::string queue_name_; // Name of this queue (for debugging)
};

#endif // THREAD_SAFE_QUEUE_H

