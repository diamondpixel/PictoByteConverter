#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <vector>
#include <thread>
#include <functional>
#include <future>
#include <memory>
#include <type_traits>
#include <stdexcept>
#include <string>
#include <sstream>
#include "ThreadSafeQueue.h"
#include "ResourceManager.h"

/**
 * @brief Thread pool for parallel task execution
 * 
 * This class provides a pool of worker threads that can execute tasks in parallel.
 * Features:
 * - Integration with ResourceManager for thread and memory management
 * - Automatic thread management
 * - Task submission with std::future return
 * - Support for any callable type (functions, lambdas, etc.)
 * - Graceful shutdown
 * - Exception propagation from worker threads to calling thread
 * - Performance monitoring and metrics reporting
 */
class ThreadPool {
public:
    /**
     * @brief Construct a new ThreadPool
     * 
     * @param num_threads Number of worker threads (default: use ResourceManager's max threads)
     * @param queue_size Maximum number of tasks in queue (default: unlimited)
     * @param pool_name Name identifier for this thread pool (for debugging)
     */
    explicit ThreadPool(size_t num_threads = 0, size_t queue_size = SIZE_MAX, const std::string& pool_name = "DefaultPool");

    /**
     * @brief Destructor ensures proper shutdown and resource release
     */
    ~ThreadPool();

    // Disable copying
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Allow moving
    ThreadPool(ThreadPool&&) noexcept;
    ThreadPool& operator=(ThreadPool&&) noexcept;

    /**
     * @brief Submit a task for execution
     * 
     * @tparam F Function type
     * @tparam Args Argument types
     * @param f Function to execute
     * @param args Arguments to pass to the function
     * @return std::future<return_type> Future for the task result
     */
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using return_type = std::invoke_result_t<F, Args...>;
        
        // Create a packaged task with the function and its arguments
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        
        // Get the future before we push the task
        std::future<return_type> result = task->get_future();
        
        // Wrap the packaged task in a void function for the queue
        auto wrapper = [task]() { 
            // Execute the task
            (*task)();
        };
        
        // Get a task ID and name from ResourceManager
        auto& resource_manager = ResourceManager::getInstance();
        uint64_t task_id = resource_manager.getNextTaskId();
        std::string task_name = pool_name_ + "_task_" + std::to_string(task_id);
        
        // Push the task to the queue with its ID and name
        if (!tasks_.push(std::move(wrapper), task_id, task_name)) {
            throw std::runtime_error("Cannot submit task to thread pool: pool is shutting down");
        }
        
        return result;
    }

    /**
     * @brief Wait for all tasks to complete and then shut down
     * 
     * @param print_message Whether to print a shutdown message (default: true)
     */
    void shutdown(bool print_message = true);

    /**
     * @brief Asynchronously shut down the thread pool and return a future that resolves when shutdown is complete
     * 
     * @param print_message Whether to print a shutdown message (default: true)
     * @return std::future<void> Future that resolves when the thread pool is fully shut down
     */
    std::future<void> shutdown_async(bool print_message = true);

    /**
     * @brief Check if the pool is shutting down
     * 
     * @return true if shutting down, false otherwise
     */
    bool is_shutting_down() const;

    /**
     * @brief Get the number of worker threads
     * 
     * @return Number of worker threads
     */
    size_t size() const;

    /**
     * @brief Get the number of tasks in the queue
     * 
     * @return Number of queued tasks
     */
    size_t queue_size() const;

    /**
     * @brief Wait for all tasks to complete
     * 
     * This does not shut down the pool, just waits for the queue to empty
     */
    void wait_for_tasks();

    /**
     * @brief Get the pool name
     * 
     * @return The name of this thread pool
     */
    const std::string& name() const;

    /**
     * @brief Print performance metrics for this thread pool
     * 
     * @param detailed Whether to print detailed metrics for each thread
     */
    void print_performance_metrics(bool detailed = false) const;

private:
    /**
     * @brief Worker thread function
     */
    void worker_thread();

    std::vector<std::thread> workers_;             // Worker threads
    ThreadSafeQueue<std::function<void()>> tasks_; // Task queue
    std::atomic<bool> shutdown_{false};            // Shutdown flag
    std::string pool_name_;                        // Name of this thread pool
};

#endif // THREAD_POOL_H
