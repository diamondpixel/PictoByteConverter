#pragma once

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <memory>
#include <chrono>
#include <vector>
#include <tuple>
#include <map>
#include <string>
#include <set>
#include <unordered_map>
#include <optional>
#include <format>
#include <sstream> // Added for ostringstream
#include <FileSystem/headers/MemoryMappedFile.h>

#include "../../Debug/headers/Debug.h" // Re-added: For getDebugMode() if used by inline methods

/**
 * A class to handle thread and memory limitations.
 * This singleton class provides the ability to:
 * 1. Limit the number of threads used by the program
 * 2. Track and limit memory usage across operations
 * 3. Wait for resource availability
 * 4. Monitor thread performance metrics
 */
class ResourceManager {
private:
    // Singleton instance
    static ResourceManager *instance; 
    static std::mutex singleton_mutex; 

    // Helper method to print debug messages
    void debugLog(const std::string &message);

    // Thread limiting
    std::atomic<size_t> active_threads{0};
    size_t max_threads;
    std::mutex thread_mutex;
    std::condition_variable thread_cv;

    // Memory limiting
    std::atomic<size_t> current_memory_usage{0};
    size_t max_memory_usage;
    std::mutex memory_mutex;
    std::condition_variable memory_cv;

    // Memory pools for reusing allocations
    struct MemoryPoolEntry {
        size_t size{0};
        std::string tag;
        std::chrono::system_clock::time_point timestamp;
        bool in_use{false};
    };

    std::vector<MemoryPoolEntry> memory_pool;
    std::mutex pool_mutex;

    // Memory tracking
    struct AllocationInfo {
        size_t size{};
        std::string tag;
        std::chrono::system_clock::time_point timestamp;
    };

    std::map<uintptr_t, AllocationInfo> memory_allocations;
    std::mutex tracking_mutex;
    bool detailed_tracking_enabled{true};
    bool memory_tracking_enabled{true};
    uint64_t allocation_id_counter{0};

    // Memory usage tracking
    size_t memory_usage{0};
    std::map<std::string, size_t> memory_usage_by_tag;
    std::mutex memory_usage_mutex;

    // Thread performance metrics
    struct ThreadMetrics {
        std::thread::id thread_id;
        std::string pool_name;
        std::chrono::system_clock::time_point registration_time;
        std::chrono::system_clock::time_point last_active_time;
        std::atomic<uint64_t> tasks_completed{0};
        std::atomic<uint64_t> total_execution_time_ns{0};
        std::atomic<uint64_t> total_wait_time_ns{0};
        std::atomic<uint64_t> max_execution_time_ns{0};
        std::atomic<uint64_t> min_execution_time_ns{UINT64_MAX};
        std::atomic<uint64_t> max_wait_time_ns{0};
        std::atomic<uint64_t> min_wait_time_ns{UINT64_MAX};
        std::string current_task_name;
        bool is_executing{false};
        std::chrono::system_clock::time_point current_task_start_time;

        // Custom copy constructor to handle atomic variables
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

        // Default constructor
        ThreadMetrics() = default;
    };

    struct TaskMetrics {
        uint64_t task_id{};
        std::chrono::system_clock::time_point enqueue_time;
        std::chrono::system_clock::time_point start_time;
        std::chrono::system_clock::time_point end_time;
        std::string task_name;
        std::string pool_name;
        std::thread::id thread_id;
        uint64_t execution_time_ns{0};
        uint64_t wait_time_ns{0};
        bool completed{false};
    };

    std::unordered_map<std::thread::id, ThreadMetrics> thread_metrics;
    std::mutex thread_metrics_mutex;

    std::vector<TaskMetrics> completed_tasks;
    std::mutex completed_tasks_mutex;
    size_t max_completed_tasks_history{1000};

    // Current active tasks
    std::unordered_map<uint64_t, TaskMetrics> active_tasks;
    std::mutex active_tasks_mutex;

    std::map<std::string, TaskMetrics> task_metrics_map;

    // Helper method to convert thread ID to string
    std::string thread_id_to_string(const std::thread::id &id) {
        std::ostringstream oss;
        oss << id;
        return oss.str();
    }

    // Helper method to format memory size to human-readable format
    std::string formatMemorySize(size_t bytes) const {
        if (bytes < 1024) {
            return std::format("{} B", bytes);
        } else if (bytes < 1024 * 1024) {
            return std::format("{:.2f} KB", bytes / 1024.0);
        } else if (bytes < 1024 * 1024 * 1024) {
            return std::format("{:.2f} MB", bytes / (1024.0 * 1024.0));
        } else {
            return std::format("{:.2f} GB", bytes / (1024.0 * 1024.0 * 1024.0));
        }
    }

public:
    // Helper method to format time duration to human-readable format
    static std::string formatDuration(uint64_t nanoseconds) {
        if (nanoseconds < 1000) {
            return std::format("{} ns", nanoseconds);
        } else if (nanoseconds < 1000 * 1000) {
            return std::format("{:.2f} µs", nanoseconds / 1000.0);
        } else if (nanoseconds < 1000 * 1000 * 1000) {
            return std::format("{:.2f} ms", nanoseconds / (1000.0 * 1000.0));
        } else {
            return std::format("{:.2f} s", nanoseconds / (1000.0 * 1000.0 * 1000.0));
        }
    }

private:
    // Private constructor for singleton
    ResourceManager() : max_threads(std::thread::hardware_concurrency() / 2),
                        max_memory_usage(1024 * 1024 * 1024) // Default 1GB
    {
    }

public:
    /**
     * Get the singleton instance of ResourceManager
     */
    static ResourceManager &getInstance(); // Declaration only

    // Destructor
    ~ResourceManager() = default;

    // Thread management methods
    /**
     * Register a thread with the ResourceManager
     * This should be called when a thread is created
     *
     * @param pool_name Name of the thread pool this thread belongs to
     */
    void registerThread(const std::string &pool_name) {
        std::lock_guard<std::mutex> lock(thread_metrics_mutex);
        std::lock_guard<std::mutex> lock_thread(thread_mutex);

        // Get the current thread ID
        std::thread::id thread_id = std::this_thread::get_id();

        // Increment active threads counter
        active_threads++;

        // Create a new thread metrics entry if it doesn't exist
        auto it = thread_metrics.find(thread_id);
        if (it == thread_metrics.end()) {
            // Create a new ThreadMetrics object
            ThreadMetrics metrics;
            metrics.thread_id = thread_id;
            metrics.pool_name = pool_name;
            metrics.registration_time = std::chrono::system_clock::now();
            metrics.last_active_time = metrics.registration_time;
            metrics.tasks_completed.store(0);
            metrics.total_execution_time_ns.store(0);
            metrics.total_wait_time_ns.store(0);
            metrics.max_execution_time_ns.store(0);
            metrics.min_execution_time_ns.store(UINT64_MAX);
            metrics.max_wait_time_ns.store(0);
            metrics.min_wait_time_ns.store(UINT64_MAX);
            metrics.is_executing = false;
            metrics.current_task_name = "";

            // Insert the new metrics into the map
            thread_metrics.emplace(thread_id, std::move(metrics));

            debugLog(std::format("Thread {} registered with pool '{}'. Active threads: {}", thread_id_to_string(thread_id), pool_name, active_threads.load()));
        } else {
            // Update the pool name if the thread was already registered
            it->second.pool_name = pool_name;
            it->second.last_active_time = std::chrono::system_clock::now();

            debugLog(std::format("Thread {} updated to pool '{}'. Active threads: {}", thread_id_to_string(thread_id), pool_name, active_threads.load()));
        }
    }

    /**
     * Unregister a thread from the ResourceManager
     * This should be called when a thread is about to be destroyed
     */
    void unregisterThread() {
        std::lock_guard<std::mutex> lock_metrics(thread_metrics_mutex);
        std::lock_guard<std::mutex> lock_thread(thread_mutex);

        // Get the current thread ID
        std::thread::id thread_id = std::this_thread::get_id();

        // Find the thread metrics
        auto it = thread_metrics.find(thread_id);
        if (it != thread_metrics.end()) {
            // Get a copy of the metrics for logging
            std::string pool_name = it->second.pool_name;
            uint64_t tasks_completed = it->second.tasks_completed.load();
            uint64_t total_execution_time = it->second.total_execution_time_ns.load();

            // Calculate thread lifetime before erasing
            auto now = std::chrono::system_clock::now();
            auto thread_lifetime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second.registration_time).count();

            // Calculate average execution time if tasks were completed
            std::string avg_execution_time = "N/A";
            if (tasks_completed > 0) {
                avg_execution_time = formatDuration(total_execution_time / tasks_completed);
            }

            // Remove the thread metrics
            thread_metrics.erase(it);

            // Decrement active threads counter
            if (active_threads > 0) {
                active_threads--;
            }

            // Log the unregistration
            debugLog(std::format("Thread {} unregistered from pool '{}'. Lifetime: {} ms, Tasks: {}, Avg execution: {}. Active threads: {}", thread_id_to_string(thread_id), pool_name, thread_lifetime_ms, tasks_completed, avg_execution_time, active_threads.load()));

            // Notify any waiting threads
            thread_cv.notify_all();
        } else {
            debugLog(std::format("Warning: unregisterThread called for unregistered thread {}", thread_id_to_string(thread_id)));
        }
    }

    /**
     * Acquire a thread for task execution
     * This should be called before executing a task in a thread pool
     *
     * @param task_id Unique identifier for the task
     * @param task_name Name of the task
     */
    void acquireThread(uint64_t task_id, const std::string &task_name) {
        std::lock_guard<std::mutex> lock(thread_metrics_mutex);

        // Get the current thread ID
        std::thread::id thread_id = std::this_thread::get_id();

        // Find the thread metrics
        auto it = thread_metrics.find(thread_id);
        if (it != thread_metrics.end()) {
            // Update thread metrics
            it->second.is_executing = true;
            it->second.current_task_name = task_name;
            it->second.current_task_start_time = std::chrono::system_clock::now();
            it->second.last_active_time = it->second.current_task_start_time;

            // Find the task in active tasks
            auto task_it = active_tasks.find(task_id);
            if (task_it != active_tasks.end()) {
                // Update task metrics
                task_it->second.thread_id = thread_id;
                task_it->second.start_time = it->second.current_task_start_time;

                // Calculate wait time
                auto wait_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    task_it->second.start_time - task_it->second.enqueue_time).count();
                task_it->second.wait_time_ns = wait_time;

                if (getDebugMode()) {
                    debugLog(std::format("ResourceManager: Thread {} acquired for task '{}' (ID: {}). Wait time: {}", thread_id_to_string(thread_id), task_name, task_id, formatDuration(wait_time)));
                }
            } else {
                if (getDebugMode()) {
                    debugLog(std::format("ResourceManager: Thread {} acquired for task '{}' (ID: {}) but task not found in active tasks", thread_id_to_string(thread_id), task_name, task_id));
                }
            }
        } else {
            if (getDebugMode()) {
                debugLog(std::format("ResourceManager: Warning - acquireThread called for unregistered thread {}", thread_id_to_string(thread_id)));
            }
        }
    }

    /**
     * Release a thread after task execution
     * This should be called after executing a task in a thread pool
     *
     * @param task_id Unique identifier for the task
     */
    void releaseThread(uint64_t task_id) {
        std::lock_guard<std::mutex> lock_metrics(thread_metrics_mutex);
        std::thread::id thread_id = std::this_thread::get_id();

        // Find the thread metrics
        auto it = thread_metrics.find(thread_id);
        if (it != thread_metrics.end()) {
            // Update thread metrics if the thread was executing a task
            if (it->second.is_executing) {
                auto now = std::chrono::system_clock::now();
                auto execution_time_signed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now - it->second.current_task_start_time).count();
                uint64_t execution_time = static_cast<uint64_t>(execution_time_signed);

                // Update task execution metrics
                it->second.tasks_completed.fetch_add(1, std::memory_order_relaxed);
                it->second.total_execution_time_ns.fetch_add(execution_time, std::memory_order_relaxed);

                // Update max execution time
                uint64_t current_max = it->second.max_execution_time_ns.load(std::memory_order_relaxed);
                while (execution_time > current_max &&
                       !it->second.max_execution_time_ns.compare_exchange_weak(current_max, execution_time,
                                                                               std::memory_order_relaxed,
                                                                               std::memory_order_relaxed)) {
                    // Keep trying if the compare_exchange failed
                }

                // Update min execution time
                uint64_t current_min = it->second.min_execution_time_ns.load(std::memory_order_relaxed);
                while (execution_time < current_min &&
                       !it->second.min_execution_time_ns.compare_exchange_weak(current_min, execution_time,
                                                                               std::memory_order_relaxed,
                                                                               std::memory_order_relaxed)) {
                    // Keep trying if the compare_exchange failed
                }

                // Reset thread state
                it->second.is_executing = false;
                it->second.current_task_name.clear();
                it->second.last_active_time = now;

                // Find the task in active tasks
                auto task_it = active_tasks.find(task_id);
                if (task_it != active_tasks.end()) {
                    // Update task metrics
                    task_it->second.end_time = now;
                    task_it->second.execution_time_ns = execution_time;
                    task_it->second.completed = true;

                    // Move to completed tasks
                    std::lock_guard<std::mutex> lock_completed(completed_tasks_mutex);
                    completed_tasks.push_back(task_it->second);

                    // Limit the history size
                    if (completed_tasks.size() > max_completed_tasks_history) {
                        completed_tasks.erase(completed_tasks.begin());
                    }

                    // Remove from active tasks
                    active_tasks.erase(task_it);

                    if (getDebugMode()) {
                        debugLog(std::format("ResourceManager: Thread {} released after task '{}' (ID: {}). Execution time: {}", thread_id_to_string(thread_id), it->second.current_task_name, task_id, formatDuration(execution_time)));
                    }
                } else {
                    if (getDebugMode()) {
                        debugLog(std::format("ResourceManager: Thread {} released but task ID {} not found in active tasks", thread_id_to_string(thread_id), task_id));
                    }
                }
            } else {
                if (getDebugMode()) {
                    debugLog(std::format("ResourceManager: Warning - releaseThread called for non-executing thread {}", thread_id_to_string(thread_id)));
                }
            }
        } else {
            if (getDebugMode()) {
                debugLog(std::format("ResourceManager: Warning - releaseThread called for unregistered thread {}", thread_id_to_string(thread_id)));
            }
        }
    }

    /**
     * Set task enqueue time for more accurate wait time measurement
     * This should be called when a task is added to a queue
     *
     * @param task_id Unique identifier for the task
     * @param task_name Name of the task
     * @param pool_name Name of the thread pool
     */
    void recordTaskEnqueue(uint64_t task_id, const std::string &task_name, const std::string &pool_name) {
        std::lock_guard<std::mutex> lock(active_tasks_mutex);

        // Create a new task metrics entry
        TaskMetrics metrics;
        metrics.task_id = task_id;
        metrics.task_name = task_name;
        metrics.pool_name = pool_name;
        metrics.enqueue_time = std::chrono::system_clock::now();

        // Store the metrics in the active tasks map
        active_tasks[task_id] = std::move(metrics);

        if (getDebugMode()) {
            debugLog(std::format("ResourceManager: Task '{}' (ID: {}) enqueued in pool '{}'", task_name, task_id, pool_name));
        }
    }

    /**
     * Get performance metrics for all threads
     *
     * @return Vector of thread metrics
     */
    std::vector<std::pair<std::thread::id, ThreadMetrics> > getThreadMetrics() {
        std::lock_guard<std::mutex> lock(thread_metrics_mutex);

        std::vector<std::pair<std::thread::id, ThreadMetrics> > result;
        result.reserve(thread_metrics.size());

        for (const auto &pair: thread_metrics) {
            result.emplace_back(pair.first, pair.second);
        }

        return result;
    }

    /**
     * Get performance metrics for a specific thread
     *
     * @param thread_id ID of the thread to get metrics for
     * @return Optional thread metrics
     */
    std::optional<ThreadMetrics> getThreadMetrics(std::thread::id thread_id) {
        std::lock_guard<std::mutex> lock(thread_metrics_mutex);

        auto it = thread_metrics.find(thread_id);
        if (it != thread_metrics.end()) {
            return it->second;
        }

        return std::nullopt;
    }

    /**
     * Get performance metrics for a specific thread pool
     *
     * @param pool_name Name of the thread pool
     * @return Vector of thread metrics for the pool
     */
    std::vector<ThreadMetrics> getPoolMetrics(const std::string &pool_name) {
        std::lock_guard<std::mutex> lock(thread_metrics_mutex);

        std::vector<ThreadMetrics> result;

        for (const auto &pair: thread_metrics) {
            if (pair.second.pool_name == pool_name) {
                result.push_back(pair.second);
            }
        }

        return result;
    }

    /**
     * Get aggregated performance metrics for a thread pool
     *
     * @param pool_name Name of the thread pool
     * @return Struct with aggregated metrics
     */
    struct PoolAggregateMetrics {
        std::string pool_name;
        size_t thread_count{0};
        uint64_t total_tasks_completed{0};
        uint64_t total_execution_time_ns{0};
        uint64_t avg_execution_time_ns{0};
        uint64_t max_execution_time_ns{0};
        uint64_t min_execution_time_ns{UINT64_MAX};
        uint64_t total_wait_time_ns{0};
        uint64_t avg_wait_time_ns{0};
        uint64_t max_wait_time_ns{0};
        uint64_t min_wait_time_ns{UINT64_MAX};
    };

    PoolAggregateMetrics getPoolAggregateMetrics(const std::string &pool_name) const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(thread_metrics_mutex));

        PoolAggregateMetrics result;
        result.pool_name = pool_name;

        for (const auto &thread_entry: thread_metrics) {
            const ThreadMetrics &metrics = thread_entry.second;

            if (metrics.pool_name == pool_name) {
                result.thread_count++;
                result.total_tasks_completed += metrics.tasks_completed.load();

                // Only include threads that have completed at least one task
                if (metrics.tasks_completed.load() > 0) {
                    // Execution time metrics
                    uint64_t avg_exec_time = metrics.total_execution_time_ns.load() / metrics.tasks_completed.load();
                    result.total_execution_time_ns += metrics.total_execution_time_ns.load();
                    result.max_execution_time_ns = std::max(result.max_execution_time_ns,
                                                            static_cast<uint64_t>(metrics.max_execution_time_ns.
                                                                load()));

                    // Handle min execution time
                    uint64_t min_exec = metrics.min_execution_time_ns.load();
                    if (min_exec < UINT64_MAX) {
                        // Check if it's been set
                        if (result.min_execution_time_ns == 0) {
                            result.min_execution_time_ns = min_exec;
                        } else {
                            result.min_execution_time_ns = std::min(result.min_execution_time_ns, min_exec);
                        }
                    }

                    // Wait time metrics (only if we have wait time data)
                    if (metrics.total_wait_time_ns.load() > 0) {
                        uint64_t avg_wait_time = metrics.total_wait_time_ns.load() / metrics.tasks_completed.load();
                        result.total_wait_time_ns += metrics.total_wait_time_ns.load();
                        result.max_wait_time_ns = std::max(result.max_wait_time_ns,
                                                           static_cast<uint64_t>(metrics.max_wait_time_ns.load()));

                        // Handle min wait time
                        uint64_t min_wait = metrics.min_wait_time_ns.load();
                        if (min_wait < UINT64_MAX) {
                            // Check if it's been set
                            if (result.min_wait_time_ns == 0) {
                                result.min_wait_time_ns = min_wait;
                            } else {
                                result.min_wait_time_ns = std::min(result.min_wait_time_ns, min_wait);
                            }
                        }
                    }
                }
            }
        }

        // Calculate averages
        if (result.total_tasks_completed > 0) {
            result.avg_execution_time_ns = result.total_execution_time_ns / result.total_tasks_completed;
            if (result.total_wait_time_ns > 0) {
                result.avg_wait_time_ns = result.total_wait_time_ns / result.total_tasks_completed;
            }
        }

        return result;
    }

    /**
     * Get recent task history
     *
     * @param max_tasks Maximum number of tasks to return
     * @return Vector of recent task metrics
     */
    std::vector<TaskMetrics> getRecentTasks(size_t max_tasks = 100) {
        std::lock_guard<std::mutex> lock(completed_tasks_mutex);

        std::vector<TaskMetrics> result;

        // Get the most recent tasks up to max_tasks
        size_t start_idx = completed_tasks.size() > max_tasks ? completed_tasks.size() - max_tasks : 0;

        result.reserve(completed_tasks.size() - start_idx);

        for (size_t i = start_idx; i < completed_tasks.size(); ++i) {
            result.push_back(completed_tasks[i]);
        }

        return result;
    }

    /**
     * Get currently active tasks
     *
     * @return Vector of active task metrics
     */
    std::vector<TaskMetrics> getActiveTasks() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(active_tasks_mutex));

        std::vector<TaskMetrics> result;
        result.reserve(active_tasks.size());

        for (const auto &task_entry: active_tasks) {
            result.push_back(task_entry.second);
        }

        return result;
    }

    /**
     * Clear all active tasks
     *
     * This is useful for cleanup before shutdown to prevent stalled task warnings
     */
    void clearActiveTasks() {
        std::lock_guard<std::mutex> lock(active_tasks_mutex);
        active_tasks.clear();
    }

    /**
     * Print thread performance report to debug output
     *
     * @param detailed Whether to print detailed metrics for each thread
     */
    void printThreadPerformanceReport(bool detailed = false) {
        std::stringstream ss;
        ss << "===== Thread Performance Report =====\n";
        ss << "Active Threads: " << active_threads.load() << "\n";

        // Get all thread pools
        std::set<std::string> pool_names; {
            std::lock_guard<std::mutex> lock(thread_metrics_mutex);
            for (const auto &pair: thread_metrics) {
                pool_names.insert(pair.second.pool_name);
            }
        }

        // Print aggregate metrics for each pool
        for (const auto &pool_name: pool_names) {
            auto metrics = getPoolAggregateMetrics(pool_name);

            ss << "\nPool: " << pool_name << "\n";
            ss << "  Threads: " << metrics.thread_count << "\n";
            ss << "  Total Tasks: " << metrics.total_tasks_completed << "\n";

            if (metrics.total_tasks_completed > 0) {
                ss << "  Execution Time: avg=" << formatDuration(metrics.avg_execution_time_ns)
                        << ", min=" << formatDuration(metrics.min_execution_time_ns)
                        << ", max=" << formatDuration(metrics.max_execution_time_ns) << "\n";

                if (metrics.total_wait_time_ns > 0) {
                    ss << "  Wait Time: avg=" << formatDuration(metrics.avg_wait_time_ns)
                            << ", min=" << formatDuration(metrics.min_wait_time_ns)
                            << ", max=" << formatDuration(metrics.max_wait_time_ns) << "\n";
                }
            }

            // Print detailed metrics for each thread in the pool
            if (detailed) {
                auto pool_threads = getPoolMetrics(pool_name);

                ss << "  Thread Details:\n";
                for (const auto &thread: pool_threads) {
                    ss << "    Thread " << thread_id_to_string(thread.thread_id) << ":\n";
                    ss << "      Tasks: " << thread.tasks_completed.load() << "\n";

                    if (thread.tasks_completed.load() > 0) {
                        auto avg_exec_time = thread.total_execution_time_ns.load() / thread.tasks_completed.load();

                        ss << "      Execution Time: avg=" << formatDuration(avg_exec_time)
                                << ", min=" << formatDuration(thread.min_execution_time_ns.load())
                                << ", max=" << formatDuration(thread.max_execution_time_ns.load()) << "\n";

                        if (thread.total_wait_time_ns.load() > 0) {
                            auto avg_wait_time = thread.total_wait_time_ns.load() / thread.tasks_completed.load();

                            ss << "      Wait Time: avg=" << formatDuration(avg_wait_time)
                                    << ", min=" << formatDuration(thread.min_wait_time_ns.load())
                                    << ", max=" << formatDuration(thread.max_wait_time_ns.load()) << "\n";
                        }
                    }
                }
            }
        }

        // Print active tasks
        auto active = getActiveTasks();
        if (!active.empty()) {
            ss << "\nActive Tasks: " << active.size() << "\n";

            for (const auto &task: active) {
                auto now = std::chrono::system_clock::now();
                auto running_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - task.start_time).count();

                // Validate the running time - if it's unrealistically large, it's likely a tracking error
                // Limit to 24 hours (86,400,000 ms) as a reasonable maximum
                if (running_time > 86400000) {
                    running_time = 0; // Reset to 0 to indicate an error
                    ss << "  Thread " << task.thread_id << ": '" << task.task_name
                            << "' (running time tracking error - task may be stalled)\n";
                } else {
                    ss << "  Thread " << task.thread_id << ": '" << task.task_name
                            << "' (running for " << running_time << " ms)\n";
                }
            }
        }

        ss << "====================================";

        debugLog(ss.str());
    }

    /**
     * Set the maximum number of threads
     */
    void setMaxThreads(size_t threads) {
        max_threads = (threads > 0) ? threads : 1;
        debugLog(std::format("ResourceManager: Max threads set to {}", threads));
    }

    /**
     * Set the maximum memory usage in bytes
     */
    void setMaxMemory(size_t memory_bytes) {
        max_memory_usage = (memory_bytes > 1024 * 1024) ? memory_bytes : 1024 * 1024; // Min 1MB
        debugLog(std::format("ResourceManager: Max memory set to {:.2f} MB", memory_bytes / (1024.0 * 1024.0)));
    }

    /**
     * Enable or disable detailed memory tracking
     */
    void setDetailedTracking(bool enabled) {
        detailed_tracking_enabled = enabled;
    }

    /**
     * Enable or disable memory tracking
     */
    void setMemoryTracking(bool enabled) {
        memory_tracking_enabled = enabled;
    }

    /**
     * Get the current memory usage
     */
    size_t getCurrentMemoryUsage() const {
        return current_memory_usage.load();
    }

    /**
     * Get the maximum memory limit
     */
    size_t getMaxMemory() const {
        return max_memory_usage;
    }

    /**
     * Get the current thread count
     */
    size_t getCurrentThreadCount() const {
        return active_threads.load();
    }

    /**
     * Get the maximum thread limit
     */
    size_t getMaxThreads() const {
        return max_threads;
    }

    /**
     * Print current memory allocation status
     */
    void printMemoryStatus() {
        if (!memory_tracking_enabled) {
            return;
        }

        std::lock_guard<std::mutex> lock(tracking_mutex);

        // Calculate memory stats
        size_t total_allocated = 0;
        size_t num_allocations = memory_allocations.size();

        for (const auto &[id, info]: memory_allocations) {
            total_allocated += info.size;
        }

        // Print summary
        std::string status = std::format("Memory Status: Using {} of {} ({})",
                                         formatMemorySize(current_memory_usage.load()),
                                         formatMemorySize(max_memory_usage),
                                         num_allocations);

        debugLog(status);

        // Print detailed report if enabled
        if (detailed_tracking_enabled && !memory_allocations.empty() && getDebugMode()) {
            debugLog("Detailed memory allocations:");

            for (const auto &[id, info]: memory_allocations) {
                auto now = std::chrono::system_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - info.timestamp).count();

                std::string detail = std::format("  ID: {} Size: {} Age: {}s{}",
                                                 id,
                                                 formatMemorySize(info.size),
                                                 duration,
                                                 !info.tag.empty() ? " Tag: " + info.tag : "");

                debugLog(detail);
            }
        }
    }

    /**
     * Get memory from the pool if available, otherwise allocate new memory
     * @param bytes Number of bytes needed
     * @param tag Identifier for this memory allocation
     * @return ID of the memory block that can be used to release it later, or 0 if allocation failed
     */
    uintptr_t getPooledMemory(size_t bytes, const std::string &tag = "") {
        std::unique_lock<std::mutex> pool_lock(pool_mutex);

        // First, try to find a suitable unused block in the pool
        for (size_t i = 0; i < memory_pool.size(); i++) {
            auto &entry = memory_pool[i];
            if (!entry.in_use && entry.size >= bytes) {
                // Found a usable block that's big enough
                entry.in_use = true;
                entry.tag = tag;
                entry.timestamp = std::chrono::system_clock::now();

                // Log reuse
                std::string message = std::format("Reused memory pool block: {} for {} (requested {})",
                                                  formatMemorySize(entry.size),
                                                  tag,
                                                  formatMemorySize(bytes));
                debugLog(message);

                return static_cast<uintptr_t>(i) + 1; // Add 1 to avoid returning 0
            }
        }

        // No suitable block found, need to allocate new memory
        std::unique_lock<std::mutex> lock(memory_mutex);

        // Wait until memory is available
        bool success = memory_cv.wait_for(lock, std::chrono::milliseconds(5000), [this, bytes]() {
            return (current_memory_usage.load() + bytes) <= max_memory_usage;
        });

        if (!success) {
            debugLog(std::format("Memory allocation of {} timed out", formatMemorySize(bytes)));
            return 0;
        }

        // Allocate memory
        current_memory_usage += bytes;

        // Add to pool
        MemoryPoolEntry new_entry;
        new_entry.size = bytes;
        new_entry.tag = tag;
        new_entry.timestamp = std::chrono::system_clock::now();
        new_entry.in_use = true;

        memory_pool.push_back(new_entry);
        uintptr_t pool_id = memory_pool.size(); // 1-indexed

        // Record allocation for tracking
        if (memory_tracking_enabled) {
            std::lock_guard<std::mutex> lock(tracking_mutex);
            uintptr_t alloc_id = ++allocation_id_counter;
            memory_allocations[alloc_id] = {
                bytes,
                tag,
                std::chrono::system_clock::now()
            };

            // Log allocation
            std::string message = std::format("Memory allocated: {}", formatMemorySize(bytes));
            if (!tag.empty()) {
                message += " for " + tag;
            }
            message += std::format(" (Total: {}, {} allocations)", formatMemorySize(current_memory_usage.load()), memory_allocations.size());

            debugLog(message);
        }

        return pool_id;
    }

    /**
     * Release memory back to the pool for reuse
     * @param pool_id ID returned from getPooledMemory
     */
    void releasePooledMemory(uintptr_t pool_id) {
        if (pool_id == 0) return;

        std::lock_guard<std::mutex> pool_lock(pool_mutex);

        // Validate the pool ID
        size_t index = pool_id - 1;
        if (index >= memory_pool.size()) {
            debugLog(std::format("Invalid pool ID: {}", pool_id));
            return;
        }

        // Mark the block as unused
        auto &entry = memory_pool[index];
        if (!entry.in_use) {
            debugLog(std::format("Memory block already released: {}", pool_id));
            return;
        }

        entry.in_use = false;

        if (!entry.tag.empty()) {
            debugLog(std::format("Released memory to pool: {} from {}", formatMemorySize(entry.size), entry.tag));
        } else {
            debugLog(std::format("Released memory to pool: {}", formatMemorySize(entry.size)));
        }
    }

    /**
     * For backward compatibility: Existing code can still use allocateMemory/freeMemory,
     * but behind the scenes we'll use the pooling mechanism
     */
    bool allocateMemory(size_t bytes,
                        std::chrono::milliseconds [[maybe_unused]] timeout = std::chrono::milliseconds(5000),
                        const std::string &tag = "") {
        uintptr_t pool_id = getPooledMemory(bytes, tag);
        return (pool_id != 0);
    }

    /**
     * For backward compatibility: Release memory, but don't actually free it - return to pool
     */
    void freeMemory(size_t bytes, [[maybe_unused]] const std::string &tag = "") {
        // Find the allocation in the pool by tag or size
        std::lock_guard<std::mutex> pool_lock(pool_mutex);

        for (size_t i = 0; i < memory_pool.size(); i++) {
            auto &entry = memory_pool[i];
            if (entry.in_use &&
                ((!tag.empty() && entry.tag == tag) ||
                 (tag.empty() && entry.size == bytes))) {
                // Mark as available in the pool
                entry.in_use = false;

                if (!entry.tag.empty()) {
                    debugLog(std::format("Released memory to pool: {} from {}", formatMemorySize(entry.size), entry.tag));
                } else {
                    debugLog(std::format("Released memory to pool: {}", formatMemorySize(entry.size)));
                }
                return;
            }
        }

        if (!tag.empty()) {
            debugLog(std::format("Warning: Could not find memory allocation to release: {} tag: {}", formatMemorySize(bytes), tag));
        } else {
            debugLog(std::format("Warning: Could not find memory allocation to release: {}", formatMemorySize(bytes)));
        }
    }

    // Simplified for stack overflow diagnosis
    void trackMemory(size_t size, const std::string &tag = "general") {
        if (!memory_tracking_enabled) return;
        std::lock_guard<std::mutex> lock(memory_usage_mutex);
        memory_usage += size;
        memory_usage_by_tag[tag] += size;
    }

    // Simplified for stack overflow diagnosis
    void releaseMemory(size_t size, const std::string &tag = "general") {
        if (!memory_tracking_enabled) return;
        std::lock_guard<std::mutex> lock(memory_usage_mutex);
        if (size > memory_usage) {
            size = memory_usage;
        }
        memory_usage -= size;

        auto it = memory_usage_by_tag.find(tag);
        if (it != memory_usage_by_tag.end()) {
            if (size > it->second) {
                it->second = 0;
            } else {
                it->second -= size;
            }
        }
    }

    /**
     * Execute a function with a thread from the pool, waiting if necessary
     * @param fn Function to execute
     * @param timeout Time to wait for thread availability
     * @return True if the thread was created, false otherwise
     */
    template<typename Func, typename... Args>
    bool runWithThread(Func &&func, Args &&... args) {
        std::unique_lock<std::mutex> lock(thread_mutex);

        // Wait until a thread slot is available
        thread_cv.wait(lock, [this]() {
            return active_threads.load() < max_threads;
        });

        // Acquire a thread slot
        active_threads++;

        // Release the lock before starting the thread
        lock.unlock();

        // Create the thread with a wrapper that will decrement active_threads
        std::thread worker(
            [this, f = std::forward<Func>(func), args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                try {
                    // Call the function with the arguments
                    std::apply(f, args);
                } catch (const std::exception &e) {
                    debugLog(std::format("Thread exception: {}", e.what()));
                }

                // Release the thread slot when done
                {
                    std::lock_guard<std::mutex> lock(thread_mutex);
                    active_threads--;
                }
                thread_cv.notify_one(); // Notify waiting threads
            });

        // Detach the thread and let it run
        worker.detach();
        return true;
    }

    /**
     * Wait for all threads to complete their work
     */
    void waitForAllThreads() {
        std::unique_lock<std::mutex> lock(thread_mutex);
        thread_cv.wait(lock, [this]() {
            return active_threads.load() == 0;
        });
    }

    /**
     * @brief Get a unique task ID for tracking purposes
     *
     * @return A unique task ID
     */
    std::atomic<uint64_t> next_task_id{0};

    uint64_t getNextTaskId() {
        return next_task_id.fetch_add(1, std::memory_order_relaxed);
    }

};
