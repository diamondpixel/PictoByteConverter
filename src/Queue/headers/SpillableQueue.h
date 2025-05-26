#ifndef SPILLABLE_QUEUE_H
#define SPILLABLE_QUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <string>
#include <atomic>
#include <utility> // For std::pair
#include <memory> // For std::unique_ptr
 #include <type_traits>
#include "QueueBase.h"
#include "../../Threading/headers/ResourceManager.h"


/**
 * @brief Thread-safe queue with disk spilling capability
 *
 * This class provides a thread-safe queue that can spill items to disk
 * when the in-memory queue reaches a certain size. Features:
 * - Thread-safe operations
 * - Configurable maximum in-memory items
 * - Automatic disk spilling when queue is full
 * - Serialization/deserialization of items to/from disk
 * - Graceful shutdown
 * - Integration with ResourceManager for memory pooling
 *
 * @tparam T Type of items in the queue (must support serialize/deserialize methods
 *           and getMemoryUsage() method for accurate memory tracking)
 */
template<typename T>
class SpillableQueue : public QueueBase<T>{
    static_assert(std::is_same_v<T, std::unique_ptr<Task>>, "SpillableQueue must be instantiated with std::unique_ptr<Task>");
public:
    /**
     * @brief Construct a new SpillableQueue
     *
     * @param spill_path Directory path for spill files (default: empty, disables spilling)
     * @param queue_name Name identifier for this queue (for debugging)
     */
    explicit SpillableQueue(std::string spill_path = "",
                            std::string queue_name = "DefaultSpillableQueue");

    /**
     * @brief Destructor ensures proper cleanup of spill files
     */
    ~SpillableQueue() override;

    // Disable copying
    SpillableQueue(const SpillableQueue &) = delete;
    SpillableQueue &operator=(const SpillableQueue &) = delete;

    // Allow moving
    SpillableQueue(SpillableQueue &&other) noexcept;
    SpillableQueue &operator=(SpillableQueue &&other) noexcept;

    /**
     * @brief Push an rvalue item to the queue
     *
     * @param item Item to push (rvalue reference)
     * @return true if successful, false if queue is shutting down
     */
    bool push(T &&item) override;

    /**
     * @brief Try to push an rvalue item to the queue (non-blocking)
     *
     * @param item Item to push (rvalue reference)
     * @return true if successful, false if queue is shutting down or full
     */
    bool try_push(T &&item) override;

    /**
     * @brief Pop an item from the queue
     *
     * This implementation uses a "peek-first" approach for spilled files:
     * 1. Peek at the spill file name without removing it from the queue
     * 2. Try to deserialize the file
     * 3. Only remove the file name from the queue if deserialization succeeds
     * This prevents data loss if deserialization fails.
     *
     * For in-memory items, it properly releases the pooled memory using
     * ResourceManager.
     *
     * @param item Reference to store the popped item
     * @return true if an item was popped, false if the queue is empty and shutting down
     */
    bool pop(T &item) override;

    /**
     * @brief Try to pop an item from the queue (non-blocking)
     *
     * This implementation uses a "peek-first" approach for spilled files:
     * 1. Peek at the spill file name without removing it from the queue
     * 2. Try to deserialize the file
     * 3. Only remove the file name from the queue if deserialization succeeds
     * This prevents data loss if deserialization fails.
     *
     * For in-memory items, it properly releases the pooled memory using
     * ResourceManager.
     *
     * @param item Reference to store the popped item
     * @return true if an item was popped, false if the queue is empty
     */
    bool try_pop(T &item) override;

    /**
     * @brief Signal that no more items will be added to the queue
     */
    void shutdown() override;

    /**
     * @brief Check if the queue is empty
     *
     * @return true if the queue is empty, false otherwise
     */
    bool empty() const override;

    /**
     * @brief Get the total number of items in the queue
     *
     * @return Total number of items (in-memory + spilled)
     */
    size_t size() const override;

    /**
     * @brief Get the number of in-memory items
     *
     * @return Number of in-memory items
     */
    size_t in_memory_size() const;

    /**
     * @brief Get the number of spilled items
     *
     * @return Number of spilled items
     */
    [[nodiscard]] size_t spilled_size() const;

    /**
     * @brief Get the memory usage of the queue
     *
     * @return The total memory usage of the queue in bytes
     */
    [[nodiscard]] size_t getMemoryUsage() const override;

    /**
     * @brief Check if the queue is shutting down
     *
     * @return true if the queue is shutting down, false otherwise
     */
    bool is_shutting_down() const override;

    /**
     * @brief Get the name of this queue
     *
     * @return The queue name
     */
    const std::string &name() const override;

private:
    // Pointer-based queue stores the template type directly (T can be std::unique_ptr<Task>)
    std::queue<T> queue_;
    std::queue<std::string> spilled_task_files_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> shutdown_flag_;
    std::atomic<bool> enable_spilling_;
    
    // Memory management
    size_t current_queue_memory_usage_;
    
    // Spill file management
    std::string spill_directory_path_;
    uint64_t spill_file_id_counter_;
    std::string queue_name_;
};

#endif // SPILLABLE_QUEUE_H
