#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <utility>
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
#include <future>
#include <limits> // For std::numeric_limits
#include <memory> // For std::shared_ptr, std::make_shared
#include <unordered_map> // Added for task_metrics_ map
#include <Debug/headers/LogBufferManager.h>
#include <Queue/headers/QueueBase.h> // Include QueueBase interface
#include <Queue/headers/ThreadSafeQueue.h> // Include ThreadSafeQueue implementation
#include <Queue/headers/SpillableQueue.h> // Include SpillableQueue implementation
#include <Queue/headers/QueueTypes.h> // Include QueueTypes enum
#include <Queue/headers/WorkStealingDeque.h>
#include <Tasks/headers/_Task.h> // Include Task class header

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


class ThreadPool {
public:
    /**
     * @brief Constructs a ThreadPool.
     *
     * @param num_threads Number of threads to create. Defaults to the number of CPU cores if 0.
     * @param queue_size Maximum size of the task queue. Defaults to 100.
     * @param pool_name Name of the thread pool for logging and identification. Defaults to "ThreadPool".
     * @param queue_type Type of queue to use. See QueueType enum. Defaults to ThreadSafe.
     * @param spill_path Directory path for SpillableQueue spill files (ignored for ThreadSafeQueue). Defaults to empty.
     */
    explicit ThreadPool(size_t num_threads = 0, size_t queue_size = 100,
                        const std::string &pool_name = "ThreadPool",
                        QueueType queue_type = QueueType::ThreadSafe,
                        const std::string &spill_path = "");

    /**
     * @brief Destructor. Stops all threads and waits for them to complete.
     */
    ~ThreadPool();

    ThreadPool(ThreadPool &&other) noexcept;

    ThreadPool &operator=(ThreadPool &&other) noexcept;

    void shutdown(bool wait_for_completion = true);

    bool is_shutting_down() const;

    std::future<void> shutdown_async();

    size_t size() const;

    size_t trueSize() const;

    size_t queue_size() const;

    void wait_for_tasks();

    const std::string &name() const;

    /**
     * @brief Submits a task to be executed by the thread pool and returns a future.
     *
     * @tparam F Type of the function/callable.
     * @tparam Args Types of the arguments to the function.
     * @param task_name Name of the task for metrics and logging.
     * @param task_id Unique ID for the task. If 0, one will be generated.
     * @param f The function/callable to execute.
     * @param args Arguments to pass to the function.
     * @return std::future<typename std::invoke_result<F, Args...>::type> Future representing the result of the task.
     */
    template<typename F, typename... Args>
    auto submit(std::string task_name, uint64_t task_id, F &&f, Args &&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type>;

    /**
     * @brief Submits a task with a generated ID and default name, returns a future.
     */
    template<typename F, typename... Args>
    auto submit(F &&f, Args &&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type>;

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
    void print_performance_metrics(bool detailed = true) const;

    /**
     * @brief Gets the name of the thread pool.
     *
     * @return The name of the pool.
     */
    std::string get_pool_name() const;

    std::vector<Task> get_completed_task_metrics() const;

    std::vector<ThreadMetrics> get_thread_metrics() const;

    size_t get_tasks_submitted_count() const;

    size_t get_tasks_processed_count() const;

    // Note: m_tasks_processed in worker, might be different from completed+failed
    void record_task_completion(Task metrics);

    std::unordered_map<uint64_t, std::shared_ptr<Task> > get_all_task_metrics_map() const;

    // New getter for the main map
    size_t get_tasks_completed_count() const;

    size_t get_tasks_failed_count() const;

    size_t get_peak_active_tasks() const;

    uint64_t get_total_pool_execution_time_ns() const;

    uint64_t get_total_pool_wait_time_ns() const;

    /**
     * @brief Submit a fully-constructed Task (or derived type) so all of its
     *        metadata is preserved.  Ownership of the Task transfers to the
     *        pool’s queue via std::unique_ptr.
     *
     * @tparam TaskT  Type derived from Task.
     * @param task_obj  Unique pointer to the task instance.
     * @return std::future<void>  Completes when the task finishes (or fails).
     */
    template <typename TaskT,
              typename = std::enable_if_t<std::is_base_of_v<Task, TaskT>>>
    std::future<void> submit_task_object(std::unique_ptr<TaskT> task_obj);

    // Convenience overload: accept std::unique_ptr<Task> without template gymnastics
    std::future<void> submit(std::unique_ptr<Task> task_obj) {
        return submit_task_object(std::move(task_obj));
    }

private:
    /**
     * @brief The worker function executed by each thread in the pool.
     */
    void worker_thread(size_t index);

    std::vector<std::thread> workers_;
    std::unique_ptr<QueueBase<std::unique_ptr<Task> > > tasks_; // Pointer-based task queue
    std::string pool_name_;

    std::atomic<size_t> num_threads_; // Desired number of threads for the pool
    std::atomic<bool> shutdown_{false}; // Flag to signal threads to shut down
    std::atomic<size_t> active_thread_count_{0}; // Actual number of threads currently running
    std::atomic<uint64_t> next_task_id_{1}; // For generating unique task IDs

    // For "Fuck Off Slots" (FOS) downscaling mechanism
    std::atomic<size_t> shutdown_slots_to_claim_{0};
    std::mutex claim_slot_mutex_; // Mutex to protect claiming a shutdown slot

    std::mutex pool_mutex_; // Mutex for general pool operations (e.g., resizing workers_ vector)
    std::condition_variable cv_all_tasks_done_; // Condition variable to wait for all tasks to complete

    // Metrics tracking
    std::unordered_map<std::thread::id, ThreadMetrics> thread_metrics_;
    mutable std::mutex thread_metrics_mutex_; // Mutex for thread_metrics_ map

    std::vector<Task> m_completed_task_metrics;
    mutable std::mutex m_completed_metrics_mutex; // Mutex for m_completed_task_metrics
    size_t max_completed_tasks_history_; // Max number of completed tasks to keep in history

    std::atomic<size_t> m_active_threads{0}; // Threads currently executing a task function
    std::atomic<size_t> m_tasks_submitted{0}; // Tasks pushed to the queue
    std::atomic<size_t> m_tasks_processed{0}; // Tasks for which processing has finished (completed or failed)

    std::atomic<size_t> currently_active_tasks_{0}; // Tasks enqueued + processing (submitted - processed)
    std::atomic<size_t> peak_active_tasks_{0}; // Peak value of currently_active_tasks_
    std::atomic<size_t> tasks_submitted_count_{0}; // Total tasks ever submitted
    std::atomic<size_t> tasks_completed_count_{0}; // Total tasks completed successfully
    std::atomic<size_t> tasks_failed_count_{0}; // Total tasks that failed
    std::atomic<uint64_t> total_pool_execution_time_ns_{0}; // Sum of all task execution times
    std::atomic<uint64_t> total_pool_wait_time_ns_{0}; // Sum of all task wait times

    // For storing the final state of all tasks, keyed by task_id
    std::unordered_map<uint64_t, std::shared_ptr<Task> > task_metrics_;
    std::mutex task_metrics_mutex_; // Protects task_metrics_ map

    std::vector<std::shared_ptr<WorkStealingDeque<std::unique_ptr<Task>>>> work_queues_;

    // Helper to generate unique task IDs if not provided
    uint64_t generate_task_id() {
        return next_task_id_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_task_completion_to_history(Task metrics);

    // Explicitly deleted copy constructor and assignment operator
    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    friend class ResourceManager; // If ResourceManager needs to inspect internals
};

// Template method implementations for submit
template<typename F, typename... Args>
auto ThreadPool::submit(std::string task_name, uint64_t task_id, F &&f, Args &&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type> {
    using return_type = typename std::invoke_result<F, Args...>::type;

    auto packaged_task = std::make_shared<std::packaged_task<return_type()> >(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    std::future<return_type> res = packaged_task->get_future();

    uint64_t final_task_id = (task_id == 0) ? generate_task_id() : task_id;

    Task task(std::to_string(final_task_id));
    task.setTaskName(std::move(task_name));
    task.setPoolName(pool_name_);
    task.setFunction([packaged_task]() { (*packaged_task)(); });
    task.setEnqueueTime(std::chrono::system_clock::now());

    if (task.self_destruct == true) {
        task.setStatus(TaskStatus::CANCELLED);

        // Create a shared_ptr copy for metrics tracking
        auto metrics_copy = std::make_shared<Task>(task);
        metrics_copy->setProcessingStarted();
        metrics_copy->setProcessingEnded(TaskStatus::FAILED, "Task self-destructed due to invalid state");

        // Record task metrics even though the task wasn't executed
        {
            std::lock_guard<std::mutex> lock(task_metrics_mutex_);
            task_metrics_[std::stoull(task.getTaskId())] = metrics_copy;
        }

        // Update counters for reporting
        tasks_submitted_count_.fetch_add(1, std::memory_order_relaxed);
        tasks_failed_count_.fetch_add(1, std::memory_order_relaxed);
        m_tasks_processed++;

        // Set exception in future
        std::promise<return_type> promise;
        promise.set_exception(std::make_exception_ptr(
            std::runtime_error("Task forcefully self-destructed..")));
        return promise.get_future();
    }

    task.setStatus(TaskStatus::PENDING);

    // Check if the task's memory usage exceeds the global limit
    size_t task_memory = task.getMemoryUsage();
    size_t max_memory = ResourceManager::getInstance().getMaxMemory();

    // Reject if task itself exceeds the global limit
    if (task_memory > max_memory) {
        // Task is too large, reject it immediately
        debug::LogBufferManager::getInstance().appendTo(
            "Threadpool", "Pool '" + pool_name_ + "' Task '" +
                          task.getTaskName() + "' (ID: " + task.getTaskId() +
                          "): Rejected due to memory limit. Task size: " +
                          std::to_string(task_memory) + " bytes, Limit: " +
                          std::to_string(max_memory) + " bytes.",
            debug::LogContext::Warning);

        // Set a std::exception_ptr in the future
        std::promise<return_type> promise;
        promise.set_exception(std::make_exception_ptr(
            std::runtime_error("Task rejected – individual task exceeds max memory")));
        return promise.get_future();
    }

    if (shutdown_.load(std::memory_order_acquire)) {
        debug::LogBufferManager::getInstance().appendTo(
            "Threadpool", "Pool '" + pool_name_ + "' Task '" +
                          task.getTaskName() + "' (ID: " + task.getTaskId() +
                          "): Attempted to submit to a shutdown pool.",
            debug::LogContext::Error);
        throw std::runtime_error("ThreadPool: submit called on a stopped ThreadPool");
    }

    tasks_submitted_count_.fetch_add(1, std::memory_order_relaxed);

    // Increment active tasks before pushing to the queue
    size_t previous_active_tasks = currently_active_tasks_.fetch_add(1, std::memory_order_release);
    size_t new_current_active = previous_active_tasks + 1;
    size_t current_peak = peak_active_tasks_.load(std::memory_order_relaxed);
    if (new_current_active > current_peak) {
        peak_active_tasks_.store(new_current_active, std::memory_order_release);
    }

    // Check if the task can be successfully pushed to the queue
    bool push_success = tasks_->push(std::make_unique<Task>(std::move(task)));

    if (!push_success) {
        // If push failed, decrement the active tasks counter
        currently_active_tasks_.fetch_sub(1, std::memory_order_release);

        // Create a promise and set an exception
        std::promise<return_type> promise;
        promise.set_exception(std::make_exception_ptr(
            std::runtime_error("Failed to push task to queue (queue full or shutting down)")));
        return promise.get_future();
    }

    return res;
}

template<typename F, typename... Args>
auto ThreadPool::submit(F &&f, Args &&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type> {
    return submit("UnnamedTask", 0, std::forward<F>(f), std::forward<Args>(args)...);
}

// Template method implementation for submit_task_object
template <typename TaskT, typename>
std::future<void> ThreadPool::submit_task_object(std::unique_ptr<TaskT> task_obj) {
    using return_type = void;

    // Check self_destruct first - if true, don't even create packaged_task
    if (task_obj->self_destruct) {
        task_obj->setStatus(TaskStatus::CANCELLED);
        
        // Create a shared_ptr copy for metrics tracking
        auto task_metrics = std::make_shared<Task>(*task_obj);
        task_metrics->setProcessingStarted();
        task_metrics->setProcessingEnded(TaskStatus::CANCELLED);
        task_metrics->setExecutingThreadId(std::this_thread::get_id());
        
        // Track metrics without enqueueing
        tasks_submitted_count_.fetch_add(1, std::memory_order_relaxed);
        
        // Return immediately with completed future
        std::promise<return_type> promise;
        promise.set_value();
        return promise.get_future();
    }

    // Capture the original callable before we overwrite it to avoid recursion
    auto original_callable = task_obj->getFunction();

    // Create a packaged_task that will run the *original* callable
    auto packaged_task = std::make_shared<std::packaged_task<return_type()>>(
        [raw_task = task_obj.get(), orig = std::move(original_callable)]() mutable {
            if (orig) {
                raw_task->setExecutingThreadId(std::this_thread::get_id());
                raw_task->setProcessingStarted();
                try {
                    orig();
                    raw_task->setProcessingEnded(TaskStatus::COMPLETED);
                } catch (const std::exception &ex) {
                    raw_task->setProcessingEnded(TaskStatus::FAILED, ex.what());
                    throw;
                }
            }
        });

    std::future<return_type> res = packaged_task->get_future();

    // Replace the task's function so the worker only needs to invoke it
    task_obj->setFunction([packaged_task]() { (*packaged_task)(); });

    // Metrics bookkeeping similar to normal submit -------------------------
    task_obj->setEnqueueTime(std::chrono::system_clock::now());
    tasks_submitted_count_.fetch_add(1, std::memory_order_relaxed);

    // Memory limit check ----------------------------------------------------
    size_t task_memory = task_obj->getMemoryUsage();
    size_t max_memory  = ResourceManager::getInstance().getMaxMemory();

    // Reject only if single task exceeds absolute limit; otherwise let queue
    // handle spilling when total usage is high.
    if (task_memory > max_memory) {
        std::promise<return_type> promise;
        promise.set_exception(std::make_exception_ptr(
            std::runtime_error("Task rejected – individual task exceeds max memory")));
        return promise.get_future();
    }

    // Push into queue -------------------------------------------------------
    currently_active_tasks_.fetch_add(1, std::memory_order_release);
    if (!tasks_->push(std::move(task_obj))) {
        currently_active_tasks_.fetch_sub(1, std::memory_order_release);
        std::promise<return_type> promise;
        promise.set_exception(std::make_exception_ptr(
            std::runtime_error("Failed to enqueue task")));
        return promise.get_future();
    }

    return res;
}

// Helper to format duration in nanoseconds to a human-readable string
static std::string formatDurationNs(uint64_t ns) {
    if (ns == std::numeric_limits<uint64_t>::max()) return "N/A (inf)";
    if (ns == 0 && std::numeric_limits<uint64_t>::max() != 0) return "0 ns"; // Avoid "N/A" if 0 is a valid min

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    if (ns < 1000) {
        oss << ns << " ns";
    } else if (ns < 1000000) {
        oss << static_cast<double>(ns) / 1000.0 << " µs";
    } else if (ns < 1000000000) {
        oss << static_cast<double>(ns) / 1000000.0 << " ms";
    } else {
        oss << static_cast<double>(ns) / 1000000000.0 << " s";
    }
    return oss.str();
}

// Helper to format a time point to a human-readable string
static std::string formatTimePoint(const std::chrono::system_clock::time_point &tp) {
    auto time_t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};

#ifdef _WIN32
    localtime_s(&tm_buf, &time_t);
#else
    localtime_r(&time_t, &tm_buf);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");

    // Add milliseconds
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        tp.time_since_epoch() % std::chrono::seconds(1));
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();

    return oss.str();
}

#endif // THREAD_POOL_H
