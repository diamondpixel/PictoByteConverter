#ifndef THREAD_SAFE_QUEUE_H
#define THREAD_SAFE_QUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <optional>
#include "ResourceManager.h"
#include "../../Debug/headers/Debug.h"
#include "../../Tasks/headers/ImageTask.h"
#include "../../Tasks/headers/ImageTaskInternal.h"

/**
 * @brief Task item wrapper for thread performance monitoring
 * 
 * This struct wraps a task with its ID and name for tracking purposes
 * 
 * @tparam T The type of task being wrapped
 */
template <typename T>
struct TaskItem {
    T task;                  // The actual task
    uint64_t id;             // Unique task ID
    std::string name;        // Task name for debugging
    
    // Constructor for task with ID and name
    TaskItem(T&& t, uint64_t task_id, const std::string& task_name)
        : task(std::move(t)), id(task_id), name(task_name) {}
    
    // Default constructor
    TaskItem() : id(0) {}
    
    // Move constructor
    TaskItem(TaskItem&& other) noexcept
        : task(std::move(other.task)), id(other.id), name(std::move(other.name)) {}
    
    // Move assignment
    TaskItem& operator=(TaskItem&& other) noexcept {
        if (this != &other) {
            task = std::move(other.task);
            id = other.id;
            name = std::move(other.name);
        }
        return *this;
    }
    
    // Disable copying
    TaskItem(const TaskItem&) = delete;
    TaskItem& operator=(const TaskItem&) = delete;
};

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
 * - After done() is called, push operations will fail and pop operations
 *   will drain the queue and then return false
 * - All waiting threads will be unblocked when done() is called
 * 
 * @tparam T The type of elements stored in the queue
 */
template <typename T>
class ThreadSafeQueue {
public:
    /**
     * @brief Construct a new ThreadSafeQueue
     * 
     * @param max_size Maximum capacity of the queue (default: unlimited)
     * @param queue_name Name identifier for this queue (for debugging)
     */
    explicit ThreadSafeQueue(size_t max_size = SIZE_MAX, const std::string& queue_name = "DefaultQueue")
        : max_size_(max_size), done_(false), size_(0), queue_name_(queue_name) {
        printDebug("Created ThreadSafeQueue '" + queue_name_ + "' with max size " + 
                  (max_size == SIZE_MAX ? "unlimited" : std::to_string(max_size)));
    }

    /**
     * @brief Destructor ensures proper shutdown
     */
    ~ThreadSafeQueue() {
        done();
        
        // Release any remaining memory tracked by this queue
        if (size_ > 0) {
            printDebug("ThreadSafeQueue '" + queue_name_ + "' destroyed with " + 
                      std::to_string(size_) + " items remaining");
        }
    }

    // Disable copying
    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

    // Allow moving
    ThreadSafeQueue(ThreadSafeQueue&& other) noexcept {
        std::lock_guard<std::mutex> lock(other.mutex_);
        queue_ = std::move(other.queue_);
        max_size_ = other.max_size_;
        done_.store(other.done_.load(std::memory_order_acquire));
        size_.store(other.size_.load(std::memory_order_acquire));
        queue_name_ = std::move(other.queue_name_);
        
        // Reset the other queue
        other.size_.store(0, std::memory_order_release);
    }

    ThreadSafeQueue& operator=(ThreadSafeQueue&& other) noexcept {
        if (this != &other) {
            // First, clear this queue
            done();
            
            std::lock_guard<std::mutex> lock_this(mutex_);
            std::lock_guard<std::mutex> lock_other(other.mutex_);
            
            queue_ = std::move(other.queue_);
            max_size_ = other.max_size_;
            done_.store(other.done_.load(std::memory_order_acquire));
            size_.store(other.size_.load(std::memory_order_acquire));
            queue_name_ = std::move(other.queue_name_);
            
            // Reset the other queue
            other.size_.store(0, std::memory_order_release);
        }
        return *this;
    }

    /**
     * @brief Push a task into the queue
     * 
     * @param task Task to push
     * @param task_id Unique ID for the task
     * @param task_name Name of the task for debugging
     * @return true if successful, false if the queue is shutting down
     */
    bool push(T&& task, uint64_t task_id, const std::string& task_name) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        // Check if we're shutting down
        if (done_.load(std::memory_order_acquire)) {
            return false;
        }
        
        // Wait until there's space in the queue
        not_full_.wait(lock, [this] { 
            return queue_.size() < max_size_ || done_.load(std::memory_order_acquire); 
        });
        
        // Check again if we're shutting down (could have changed while waiting)
        if (done_.load(std::memory_order_acquire)) {
            return false;
        }
        
        // Track memory usage for this item
        size_t item_size = sizeof(TaskItem<T>);
        ResourceManager::getInstance().trackMemory(item_size, "Queue_" + queue_name_);
        
        // Record task enqueue in ResourceManager
        ResourceManager::getInstance().recordTaskEnqueue(task_id, task_name, queue_name_);
        
        // Create and push the task item
        queue_.emplace(std::move(task), task_id, task_name);
        size_.fetch_add(1, std::memory_order_release);
        
        // Notify one waiting consumer
        not_empty_.notify_one();
        
        return true;
    }

    /**
     * @brief Push a task into the queue (legacy version)
     * 
     * @param task Task to push
     * @return true if successful, false if the queue is shutting down
     */
    bool push(T&& task) {
        // Generate a task ID and name
        uint64_t task_id = ResourceManager::getInstance().getNextTaskId();
        std::string task_name = queue_name_ + "_task_" + std::to_string(task_id);
        
        // Use the new push method
        return push(std::move(task), task_id, task_name);
    }

    /**
     * @brief Try to pop a task from the queue
     * 
     * @param task Reference to store the popped task
     * @param task_id Reference to store the task ID
     * @param task_name Reference to store the task name
     * @return true if a task was popped, false if the queue is empty
     */
    bool try_pop(T& task, uint64_t& task_id, std::string& task_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // If the queue is empty, return false
        if (queue_.empty()) {
            return false;
        }
        
        // Get the task item
        TaskItem<T>& item = queue_.front();
        task = std::move(item.task);
        task_id = item.id;
        task_name = item.name;
        
        // Remove the item from the queue
        queue_.pop();
        size_.fetch_sub(1, std::memory_order_release);
        
        // Release memory tracking for this item
        size_t item_size = sizeof(TaskItem<T>);
        ResourceManager::getInstance().releaseMemory(item_size, "Queue_" + queue_name_);
        
        // Notify one waiting producer
        not_full_.notify_one();
        
        return true;
    }

    /**
     * @brief Pop a task from the queue
     * 
     * @param task Reference to store the popped task
     * @param task_id Reference to store the task ID
     * @param task_name Reference to store the task name
     * @return true if a task was popped, false if the queue is empty and shutting down
     */
    bool pop(T& task, uint64_t& task_id, std::string& task_name) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        // Wait until there's an item in the queue or we're shutting down
        not_empty_.wait(lock, [this] { 
            return !queue_.empty() || done_.load(std::memory_order_acquire); 
        });
        
        // If the queue is empty and we're shutting down, return false
        if (queue_.empty()) {
            if (done_.load(std::memory_order_acquire)) {
                return false;
            }
        }
        
        // Get the task item
        TaskItem<T>& item = queue_.front();
        task = std::move(item.task);
        task_id = item.id;
        task_name = item.name;
        
        // Remove the item from the queue
        queue_.pop();
        size_.fetch_sub(1, std::memory_order_release);
        
        // Release memory tracking for this item
        size_t item_size = sizeof(TaskItem<T>);
        ResourceManager::getInstance().releaseMemory(item_size, "Queue_" + queue_name_);
        
        // Notify one waiting producer
        not_full_.notify_one();
        
        return true;
    }

    /**
     * @brief Pop a task from the queue (legacy version)
     * 
     * @param task Reference to store the popped task
     * @return true if a task was popped, false if the queue is empty and shutting down
     */
    bool pop(T& task) {
        uint64_t task_id;
        std::string task_name;
        return pop(task, task_id, task_name);
    }

    /**
     * @brief Try to pop a task from the queue (legacy version)
     * 
     * @param task Reference to store the popped task
     * @return true if a task was popped, false if the queue is empty
     */
    bool try_pop(T& task) {
        uint64_t task_id;
        std::string task_name;
        return try_pop(task, task_id, task_name);
    }

    /**
     * @brief Signal that no more items will be added to the queue
     * 
     * After this is called, push operations will fail and pop operations
     * will drain the queue and then return false.
     */
    void done() {
        std::lock_guard<std::mutex> lock(mutex_);
        done_.store(true, std::memory_order_release);
        
        // Notify all waiting threads
        not_empty_.notify_all();
        not_full_.notify_all();
        empty_.notify_all();
        
        printDebug("ThreadSafeQueue '" + queue_name_ + "' marked as done");
    }

    /**
     * @brief Check if the queue is marked as done
     * 
     * @return true if done, false otherwise
     */
    bool is_done() const {
        return done_.load(std::memory_order_acquire);
    }

    /**
     * @brief Get the current size of the queue
     * 
     * @return Current queue size
     */
    size_t size() const {
        return size_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Check if the queue is empty
     * 
     * @return true if queue is empty, false otherwise
     */
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    /**
     * @brief Check if the queue is full
     * 
     * @return true if queue is at capacity, false otherwise
     */
    bool full() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size() >= max_size_;
    }

    /**
     * @brief Clear all items from the queue
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Release memory tracking for all items
        size_t item_size = sizeof(TaskItem<T>);
        ResourceManager::getInstance().releaseMemory(item_size * queue_.size(), "Queue_" + queue_name_);
        
        std::queue<TaskItem<T>> empty;
        std::swap(queue_, empty);
        size_.store(0, std::memory_order_relaxed);
        
        not_full_.notify_all();
    }

    /**
     * @brief Wait until the queue is empty and no more items will be added
     * 
     * This is useful for waiting for all tasks to be processed
     */
    void wait_until_empty() {
        std::unique_lock<std::mutex> lock(mutex_);
        
        empty_.wait(lock, [this] { 
            return queue_.empty() && done_.load(std::memory_order_acquire); 
        });
    }

    /**
     * @brief Get the name of this queue
     * 
     * @return The queue name
     */
    const std::string& name() const {
        return queue_name_;
    }

private:
    std::queue<TaskItem<T>> queue_;              // Underlying queue
    mutable std::mutex mutex_;                   // Mutex for thread safety
    std::condition_variable not_empty_;          // Signaled when items are added
    std::condition_variable not_full_;           // Signaled when items are removed
    std::condition_variable empty_;              // Signaled when queue becomes empty
    size_t max_size_;                            // Maximum capacity
    std::atomic<bool> done_;                     // Shutdown flag
    std::atomic<size_t> size_;                   // Current size (atomic for thread safety)
    std::string queue_name_;                     // Name of this queue (for debugging)
};

// Explicit instantiation declaration for ImageTask
// The actual instantiation will be in the .cpp file
extern template class ThreadSafeQueue<struct ImageTask>;
extern template class ThreadSafeQueue<struct ImageTaskInternal>;

#endif // THREAD_SAFE_QUEUE_H