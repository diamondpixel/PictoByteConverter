#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <vector>
#include <queue>
#include <thread>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include <stdexcept>
#include <iostream> // For std::cerr in exceptional cases
#include <chrono> // For time-related metrics
#include <unordered_map> // For thread_metrics_ and active_tasks_
#include <sstream> // For thread ID to string if needed for logging
#include <iomanip> // For formatting output in print_performance_metrics
#include <algorithm> // For std::min/max
#include <limits> // For std::numeric_limits

#include "ThreadSafeQueue.h" // Assumes ThreadSafeQueue.h is in the same directory or accessible

// Forward declaration for ResourceManager to avoid circular dependency if minimally needed
// class ResourceManager;

// Define Metrics Structures (previously in ResourceManager.h)
// These can be nested within ThreadPool or be in the same namespace if ThreadPool is namespaced

struct ThreadMetrics {
    std::thread::id thread_id; // Native thread ID
    std::string pool_name; // Name of the pool this thread belongs to
    std::chrono::system_clock::time_point registration_time; // When the thread was registered/started
    std::chrono::system_clock::time_point last_active_time; // Last time the thread completed a task

    std::atomic<size_t> tasks_completed{0}; // Number of tasks completed by this thread
    std::atomic<uint64_t> total_execution_time_ns{0}; // Total time spent executing tasks (nanoseconds)
    std::atomic<uint64_t> total_wait_time_ns{0}; // Total time spent waiting for tasks (nanoseconds)

    std::atomic<uint64_t> max_execution_time_ns{0}; // Max execution time for a single task
    std::atomic<uint64_t> min_execution_time_ns{std::numeric_limits<uint64_t>::max()};
    // Min execution time for a single task

    std::atomic<uint64_t> max_wait_time_ns{0}; // Max wait time for a single task
    std::atomic<uint64_t> min_wait_time_ns{std::numeric_limits<uint64_t>::max()}; // Min wait time for a single task

    std::string current_task_name; // Name of the task currently being executed (if any)
    bool is_executing{false}; // True if the thread is currently executing a task
    std::chrono::system_clock::time_point current_task_start_time; // Start time of the current task

    // Default constructor
    ThreadMetrics() = default;

    // Constructor for initialization
    ThreadMetrics(std::thread::id id, std::string p_name)
        : thread_id(id), pool_name(std::move(p_name)), registration_time(std::chrono::system_clock::now()),
          last_active_time(registration_time) {
    }

    // Copy constructor to handle atomic variables correctly
    ThreadMetrics(const ThreadMetrics &other)
        : thread_id(other.thread_id),
          pool_name(other.pool_name),
          registration_time(other.registration_time),
          last_active_time(other.last_active_time),
          tasks_completed(other.tasks_completed.load()),
          total_execution_time_ns(other.total_execution_time_ns.load()),
          total_wait_time_ns(other.total_wait_time_ns.load()),
          max_execution_time_ns(other.max_execution_time_ns.load()),
          min_execution_time_ns(other.min_execution_time_ns.load()),
          max_wait_time_ns(other.max_wait_time_ns.load()),
          min_wait_time_ns(other.min_wait_time_ns.load()),
          current_task_name(other.current_task_name),
          is_executing(other.is_executing),
          current_task_start_time(other.current_task_start_time) {
    }

    // Custom assignment operator to handle atomic variables
    ThreadMetrics &operator=(const ThreadMetrics &other) {
        if (this != &other) {
            thread_id = other.thread_id;
            pool_name = other.pool_name;
            registration_time = other.registration_time;
            last_active_time = other.last_active_time;
            tasks_completed.store(other.tasks_completed.load());
            total_execution_time_ns.store(other.total_execution_time_ns.load());
            total_wait_time_ns.store(other.total_wait_time_ns.load());
            max_execution_time_ns.store(other.max_execution_time_ns.load());
            min_execution_time_ns.store(other.min_execution_time_ns.load());
            max_wait_time_ns.store(other.max_wait_time_ns.load());
            min_wait_time_ns.store(other.min_wait_time_ns.load());
            current_task_name = other.current_task_name;
            is_executing = other.is_executing;
            current_task_start_time = other.current_task_start_time;
        }
        return *this;
    }
};

struct TaskMetrics {
    uint64_t task_id{};
    std::chrono::system_clock::time_point enqueue_time;
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point end_time;
    std::string task_name;
    std::thread::id thread_id; // ID of the thread that executed this task
    uint64_t execution_time_ns{0};
    uint64_t wait_time_ns{0};
    bool completed{false};
    // Add a status field if needed (e.g., success, failure_exception)
};

// Package to be stored in the ThreadSafeQueue
struct TaskPackage {
    std::function<void()> func;
    uint64_t task_id;
    std::string task_name;
    std::chrono::system_clock::time_point enqueue_time;
};

class ThreadPool {
public:
    /**
     * @brief Constructs a ThreadPool.
     *
     * @param num_threads Number of threads to create. Defaults to the number of CPU cores if 0.
     * @param queue_size Maximum size of the task queue. Defaults to 100.
     * @param pool_name Name of the thread pool for logging and identification. Defaults to "ThreadPool".
     */
    ThreadPool(size_t num_threads = 0, size_t queue_size = 100, const std::string &pool_name = "ThreadPool");

    /**
     * @brief Destructor. Stops all threads and waits for them to complete.
     */
    ~ThreadPool();

    /**
     * @brief Enqueues a task to be executed by the thread pool.
     *
     * This version attempts to deduce arguments and bind them.
     *
     * @tparam F Type of the function/callable.
     * @tparam Args Types of the arguments to the function.
     * @param task_name Name of the task for metrics and logging.
     * @param task_id Unique ID for the task, for tracking purposes. If 0, one will be generated.
     * @param func The function/callable to execute.
     * @param args Arguments to pass to the function.
     */
    template<typename F, typename... Args>
    void enqueue(std::string task_name, uint64_t task_id, F &&func, Args &&... args);

    /**
     * @brief Enqueues a task (std::function<void()>) to be executed by the thread pool.
     *
     * @param task_name Name of the task for metrics and logging.
     * @param task_id Unique ID for the task. If 0, one might be generated or it might be untracked for detailed metrics.
     * @param task The task to execute.
     */
    void enqueue(std::string task_name, uint64_t task_id, std::function<void()> task);

    /**
    * @brief Enqueues a task with a generated ID and default name.
    */
    template<typename F, typename... Args>
    void enqueue(F &&func, Args &&... args);

    /**
     * @brief Stops the thread pool and waits for all tasks to complete.
     *
     * @param wait_for_completion If true, waits for all tasks in the queue to be processed.
     *                            If false, attempts to stop threads more immediately (tasks in queue may not run).
     */
    void stop(bool wait_for_completion = true);

    /**
     * @brief Dynamically resizes the thread pool.
     *
     * @param new_num_threads The new desired number of threads.
     */
    void resize(size_t new_num_threads);

    /**
     * @brief Gets the current number of threads in the pool.
     *
     * @return The number of active and available threads.
     */
    size_t get_thread_count() const;

    /**
     * @brief Gets the desired number of threads for the pool (after a resize call).
     *
     * @return The desired number of threads.
     */
    size_t get_desired_thread_count() const;

    /**
     * @brief Gets the current number of tasks waiting in the queue.
     *
     * @return The number of tasks in the queue.
     */
    size_t get_queue_size() const;

    /**
     * @brief Prints performance metrics of the thread pool.
     *
     * @param detailed If true, prints detailed metrics for each thread and task.
     */
    void print_performance_metrics(bool detailed = false) const;

    /**
     * @brief Gets the name of the thread pool.
     *
     * @return The name of the pool.
     */
    std::string get_pool_name() const;

private:
    /**
     * @brief The worker function executed by each thread in the pool.
     */
    void worker_thread();

    std::mutex pool_mutex_; // For synchronizing access to workers_ vector during resize and metrics
    std::atomic<size_t> num_threads_{0}; //Thread count
    std::vector<std::thread> workers_; // Worker threads
    ThreadSafeQueue<TaskPackage> tasks_; // Task queue, now stores TaskPackage
    std::string pool_name_; // Name of the thread pool
    std::atomic<bool> shutdown_{false}; // Flag to signal threads to shut down

    // Metrics storage
    std::unordered_map<std::thread::id, ThreadMetrics> thread_metrics_;
    mutable std::mutex thread_metrics_mutex_; // Protects thread_metrics_

    std::vector<TaskMetrics> completed_tasks_history_;
    mutable std::mutex completed_tasks_mutex_; // Protects completed_tasks_history_
    size_t max_completed_tasks_history_ = 1000; // Max number of completed tasks to store

    std::unordered_map<uint64_t, TaskMetrics> active_tasks_;
    mutable std::mutex active_tasks_mutex_; // Protects active_tasks_

    std::atomic<uint64_t> next_task_id_{1}; // For generating unique task IDs, starting from 1

    // Condition variable to signal when all tasks are done (for stop() method)
    std::condition_variable cv_all_tasks_done_;
    std::atomic<size_t> currently_active_tasks_{0}; // Tasks currently being processed by workers
};

#endif // THREAD_POOL_H
