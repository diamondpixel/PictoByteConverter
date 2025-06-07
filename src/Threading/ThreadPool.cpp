#include <algorithm>
#include <iostream> // Required for std::cout, std::cerr
#include <iomanip>  // Required for std::fixed, std::setprecision
#include <sstream>  // Required for std::ostringstream
#include <numeric>  // Required for std::accumulate (potentially)
#include <limits>   // Required for std::numeric_limits

#include "headers/ThreadPool.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <numeric>
#include <utility>
#include <ranges>

#include "Debug/headers/LogMacros.h"
#include "Queue/headers/WorkStealingDeque.h"

ThreadPool::ThreadPool(size_t num_threads, size_t queue_size, const std::string &pool_name, 
                       QueueType queue_type, const std::string &spill_path)
    : pool_name_(pool_name),
      num_threads_(num_threads == 0 ? 1 : num_threads),
      shutdown_(false),
      active_thread_count_(0),
      next_task_id_(1),
      max_completed_tasks_history_(1000), // Default, can be configurable
      currently_active_tasks_(0)
      {
    // Create the appropriate queue type based on queue_type parameter
    switch (queue_type) {
        case QueueType::Spillable: {
            // Create SpillableQueue
            tasks_ = std::make_unique<SpillableQueue<std::unique_ptr<Task>>>(
                spill_path.empty() ? "" : spill_path, 
                pool_name
            );
            
            std::string msg_init_spillable = "Initializing ThreadPool '" + pool_name_ + "' with desired " + std::to_string(num_threads_.load())
                + " threads. Using SpillableQueue with spill path: " + (spill_path.empty() ? "disabled" : spill_path);
            LOG_DBG("ThreadPool", msg_init_spillable);
            break;
        }
        
        case QueueType::LockFree:
        default: {
            // Default: Create LockFreeQueue (bounded)
            tasks_ = std::make_unique<LockFreeQueue<std::unique_ptr<Task>>>(queue_size, pool_name);
            
            std::string msg_init_lockfree = "Initializing ThreadPool '" + pool_name_ + "' with desired " + std::to_string(num_threads_.load())
                + " threads. Using LockFreeQueue with max size: " + std::to_string(queue_size);
            LOG_DBG("ThreadPool", msg_init_lockfree);
            break; }
    }
    // Create per-thread work-stealing deques
    work_queues_.reserve(num_threads_);
    for (size_t i = 0; i < num_threads_; ++i) {
        work_queues_.push_back(std::make_shared<WorkStealingDeque<std::unique_ptr<Task>>>());
    }
    // LAUNCH WORKER THREADS
    for (size_t i = 0; i < num_threads_; ++i) {
        workers_.emplace_back(&ThreadPool::worker_thread, this, i);
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::shutdown(bool wait_for_completion) {
    if (shutdown_.exchange(true)) {
        std::unique_lock<std::mutex> lock(pool_mutex_);
        for (size_t i = 0; i < workers_.size(); ++i) {
            if (workers_[i].joinable()) {
                workers_[i].join();
            }
        }
        return;
    }
    std::string msg_shutdown_start = "Stopping ThreadPool '" + pool_name_ + "'. Wait for completion: " + (wait_for_completion ? "true" : "false");
    LOG_INF("ThreadPool", msg_shutdown_start);

    if (wait_for_completion) {
        std::unique_lock<std::mutex> lock(pool_mutex_);
        cv_all_tasks_done_.wait(lock, [this] {
            bool empty = tasks_->empty();
            size_t active_tasks = currently_active_tasks_.load();
            return empty && (active_tasks == 0);
        });
    }
    tasks_->shutdown(); // Signal queue that no more tasks will be added & wake up waiting threads

    std::vector<std::thread> threads_to_join; {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        threads_to_join.swap(workers_); // Move workers to a local variable to join outside lock
    }

    for (auto & i : threads_to_join) {
        if (i.joinable()) {
            i.join();
        }
    }
    // active_thread_count_ should be 0 if all threads exited cleanly.
    std::string msg_shutdown_end = "ThreadPool '" + pool_name_ + "' fully stopped. Active threads final: " + std::to_string(
            active_thread_count_.load());
    LOG_INF("ThreadPool", msg_shutdown_end);

    // Clear metrics on full shutdown
    std::lock_guard<std::mutex> lock_tm(thread_metrics_mutex_);
    thread_metrics_.clear();
    std::lock_guard<std::mutex> lock_ctm(m_completed_metrics_mutex);
    m_completed_task_metrics.clear();
}

void ThreadPool::resize(size_t new_desired_count) {
    if (shutdown_.load()) {
        std::string msg_resize_shutting_down = "Pool '" + pool_name_ + "': Cannot resize, pool is shutting down.";
        LOG_WARN("ThreadPool", msg_resize_shutting_down);
        return;
    }

    if (new_desired_count == 0) {
        std::string msg_resize_zero = "Pool '" + pool_name_ + "': Cannot resize to 0 threads. Minimum is 1. Use stop() to shut down.";
        LOG_WARN("ThreadPool", msg_resize_zero);
        return; // Or set to 1, depending on desired strictness
    }

    std::lock_guard<std::mutex> lock(pool_mutex_); // Protects workers_ vector and related logic

    size_t old_desired_count = num_threads_.load();
    num_threads_.store(new_desired_count); // Set the new target immediately

    size_t current_actual_threads = active_thread_count_.load();
    // More reliable than workers_.size() if threads can exit
    // workers_.size() is what we *think* we have based on vector
    // active_thread_count_ is what *is* running.
    // For resize, we compare against what the vector holds.
    current_actual_threads = workers_.size(); // Let's use workers_.size() as the basis for adding/removing from vector


    std::string msg_resizing = "Resizing pool '" + pool_name_ + "' from " + std::to_string(old_desired_count) +
        " (current actual in vector: " + std::to_string(current_actual_threads) + ") to " +
        std::to_string(new_desired_count) + " desired threads.";
    LOG_INF("ThreadPool", msg_resizing);

    if (new_desired_count < current_actual_threads) {
        // Downscaling: Request threads to shut down using FOS
        size_t num_to_shed = current_actual_threads - new_desired_count;
        shutdown_slots_to_claim_.fetch_add(num_to_shed, std::memory_order_release); // Add to existing, if any

        std::string msg_downscaling = "Pool '" + pool_name_ + "': Downscaling. Requesting " + std::to_string(num_to_shed) +
            " threads to claim shutdown slots. Total slots to claim now: " + std::to_string(
                shutdown_slots_to_claim_.load());
        LOG_INF("ThreadPool", msg_downscaling);
        tasks_->shutdown();
    } else if (new_desired_count > current_actual_threads) {
        // Upscaling: Add new threads
        size_t num_to_add = new_desired_count - current_actual_threads;
        workers_.reserve(new_desired_count); // Reserve space if needed
        for (size_t i = 0; i < num_to_add; ++i) {
            // TODO: Consider re-adding ResourceManager check here if RM is active
            // if (ResourceManager::getInstance().getActiveThreadCount() >= max_system_threads) { break; }
            workers_.emplace_back(&ThreadPool::worker_thread, this, i);
            // active_thread_count_ is incremented by the thread itself upon starting
        }
        std::string msg_upscaling = "Pool '" + pool_name_ + "': Upscaling. Added " + std::to_string(num_to_add) +
            " threads. New worker vector size: " + std::to_string(workers_.size());
        LOG_INF("ThreadPool", msg_upscaling);
    }
    // If new_desired_count == current_actual_threads, no change to worker vector needed.
    // num_threads_ (desired) is already updated.
}

void ThreadPool::worker_thread(size_t index) {
    active_thread_count_++; // This thread is now active
    std::thread::id native_id = std::this_thread::get_id(); {
        ThreadMetrics current_thread_metrics_local(native_id, pool_name_);
        std::lock_guard<std::mutex> lock(thread_metrics_mutex_);
        thread_metrics_[native_id] = current_thread_metrics_local; // Add/update in pool's map
    }

    std::hash<std::thread::id> hasher;
    std::string short_id_str = std::to_string(hasher(native_id) % 100000);

    std::string msg_thread_start = "Worker thread [" + pool_name_ + "-" + short_id_str + "] started. Native ID: " +
        std::to_string(hasher(native_id)) + ". Total active: " + std::to_string(active_thread_count_.load());
    LOG_DBG("ThreadPool", msg_thread_start);

    while (true) {
        // Check for forced shutdown slot before trying to get a task
        if (shutdown_slots_to_claim_.load(std::memory_order_acquire) > 0) {
            size_t current_fos = shutdown_slots_to_claim_.load();
            bool claimed_slot = false;
            while (current_fos > 0) {
                if (shutdown_slots_to_claim_.compare_exchange_weak(current_fos, current_fos - 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    claimed_slot = true;
                    break;
                }
                // If CAS failed, current_fos is updated with the current value, loop and retry
            }

            if (claimed_slot) {
                std::string msg_thread_shutdown = "Worker thread [" + pool_name_ + "-" + short_id_str + "] claiming a shutdown slot. Exiting.";
                LOG_INF("ThreadPool", msg_thread_shutdown);
                break; // Exit worker loop
            }
        }

        std::unique_ptr<Task> taskPtr;

        // 1. Try to pop from own queue fast-path
        if (work_queues_[index]->pop_front(taskPtr)) {
            // got task
        } else {
            // 2. Try to steal from others
            size_t n = work_queues_.size();
            bool stolen = false;
            for (size_t offset = 1; offset < n && !stolen; ++offset) {
                size_t victim = (index + offset) % n;
                if (work_queues_[victim]->steal(taskPtr)) {
                    stolen = true;
                    break;
                }
            }

            // 3. Fallback to global queue (may block)
            if (!stolen) {
                bool task_dequeued = tasks_->pop(taskPtr);
                if (!task_dequeued) {
                    // queue empty
                }
            }
        }

        if (!taskPtr) {
            // If pool shutting down and no task, exit
            if (shutdown_.load()) {
                std::string msg_thread_no_task = "Worker thread [" + pool_name_ + "-" + short_id_str + "] no task and shutting down.";
                LOG_INF("ThreadPool", msg_thread_no_task);
                break;
            }
            continue; // loop again
        }

        Task& task = *taskPtr;
        m_active_threads++; // Thread is now processing a task
        
        // Update wait duration before processing starts
        task.updateWaitDuration();
        
        // Set the executing thread ID and start processing
        task.setExecutingThreadId(native_id);
        task.setProcessingStarted();

        std::chrono::steady_clock::time_point task_start_steady = std::chrono::steady_clock::now();
        std::string task_exception_msg;

        try {
            // Update ThreadMetrics: current task name and is_executing
            {
                std::lock_guard<std::mutex> lock(thread_metrics_mutex_);
                auto it = thread_metrics_.find(native_id);
                if (it != thread_metrics_.end()) {
                    it->second.is_executing = true;
                    it->second.current_task_name = task.getTaskName();
                    it->second.current_task_start_time = task.getStartProcessingTime();
                }
            }

            // Execute the task function
            if (task.func) {
                task.func();
            }

            task.setProcessingEnded(TaskStatus::COMPLETED);

        } catch (const std::exception &e) {
            task_exception_msg = e.what();
            task.setProcessingEnded(TaskStatus::FAILED, task_exception_msg);
            std::string msg_task_exception = "Worker thread [" + pool_name_ + "-" + short_id_str + "] task '" + task.getTaskName() +
                "' (ID: " + task.getTaskId() + ") threw exception: " + task_exception_msg;
            LOG_ERR("ThreadPool", msg_task_exception);
        } catch (...) {
            task_exception_msg = "Unknown exception";
            task.setProcessingEnded(TaskStatus::FAILED, task_exception_msg);
            std::string msg_task_unknown_exception = "Worker thread [" + pool_name_ + "-" + short_id_str + "] task '" + task.getTaskName() +
                "' (ID: " + task.getTaskId() + ") threw an unknown exception.";
            LOG_ERR("ThreadPool", msg_task_unknown_exception);
        }
        record_task_completion(task);
        m_tasks_processed++;
        currently_active_tasks_.fetch_sub(1, std::memory_order_release); // Decrement after processing

        // Update ThreadMetrics after task completion
        {
            std::lock_guard<std::mutex> lock(thread_metrics_mutex_);
            auto it = thread_metrics_.find(native_id);
            if (it != thread_metrics_.end()) {
                it->second.tasks_completed++;
                it->second.total_execution_time_ns += task.getExecutionDurationNs();
                it->second.total_wait_time_ns += task.getWaitDurationNs();
                it->second.is_executing = false;
                it->second.current_task_name = "";
                it->second.last_active_time = std::chrono::system_clock::now();

                if (task.getExecutionDurationNs() > it->second.max_execution_time_ns.load(std::memory_order_relaxed)) {
                    it->second.max_execution_time_ns.store(task.getExecutionDurationNs(), std::memory_order_release);
                }
                if (task.getExecutionDurationNs() < it->second.min_execution_time_ns.load(std::memory_order_relaxed)) {
                    it->second.min_execution_time_ns.store(task.getExecutionDurationNs(), std::memory_order_release);
                }
                
                // Update min/max wait times for the thread
                uint64_t task_wait_ns = task.getWaitDurationNs();
                if (task_wait_ns > it->second.max_wait_time_ns.load(std::memory_order_relaxed)) {
                    it->second.max_wait_time_ns.store(task_wait_ns, std::memory_order_release);
                }
                if (task_wait_ns < it->second.min_wait_time_ns.load(std::memory_order_relaxed)) {
                    it->second.min_wait_time_ns.store(task_wait_ns, std::memory_order_release);
                }
            }
        }
        m_active_threads--; // Thread is no longer processing this task
        cv_all_tasks_done_.notify_all(); // Notify if anyone is waiting for all tasks to complete
    } // End while(true)

    // Thread is exiting
    active_thread_count_--;
    {
        std::lock_guard<std::mutex> lock(thread_metrics_mutex_);
        // Optionally, update thread_metrics_ for this thread to mark it as non-operational or remove it
        // For now, we keep its final state for potential later inspection if not cleared on stop()
         auto it = thread_metrics_.find(native_id);
         if (it != thread_metrics_.end()) {
            // Mark as no longer executing, if it was somehow stuck in that state due to abrupt exit
            it->second.is_executing = false; 
         }
    }

    std::string msg_thread_finish = "Worker thread [" + pool_name_ + "-" + short_id_str + "] finished. Native ID: " +
        std::to_string(hasher(native_id)) + ". Total active: " + std::to_string(active_thread_count_.load());
    LOG_DBG("ThreadPool", msg_thread_finish);
}

size_t ThreadPool::get_thread_count() const {
    return active_thread_count_.load(std::memory_order_relaxed);
}

size_t ThreadPool::get_desired_thread_count() const {
    return num_threads_.load(std::memory_order_relaxed);
}

size_t ThreadPool::get_queue_size() const {
    return tasks_->size();
}

std::string ThreadPool::get_pool_name() const {
    return pool_name_;
}

size_t ThreadPool::get_tasks_submitted_count() const {
    return m_tasks_submitted.load(std::memory_order_relaxed);
}

size_t ThreadPool::get_tasks_processed_count() const {
    return m_tasks_processed.load(std::memory_order_relaxed);
}

// Method to record task completion metrics
void ThreadPool::record_task_completion(Task metrics) {
    // Update the main task_metrics_ map with the final state of the task
    {
        std::lock_guard<std::mutex> lock(task_metrics_mutex_); 
        uint64_t task_id = std::stoull(metrics.getTaskId());
        task_metrics_[task_id] = std::make_shared<Task>(metrics);
    }

    // Update overall pool counters and accumulators
    if (metrics.getStatus() == TaskStatus::COMPLETED) {
        tasks_completed_count_.fetch_add(1, std::memory_order_relaxed);
    } else if (metrics.getStatus() == TaskStatus::FAILED) {
        tasks_failed_count_.fetch_add(1, std::memory_order_relaxed);
    }

    total_pool_execution_time_ns_.fetch_add(metrics.getExecutionDurationNs(), std::memory_order_relaxed);
    total_pool_wait_time_ns_.fetch_add(metrics.getWaitDurationNs(), std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(m_completed_metrics_mutex);

    // Let's find the existing PENDING record and update it, or add if not found (e.g. task not from this pool's submit)
    auto hist_it = std::find_if(m_completed_task_metrics.rbegin(), m_completed_task_metrics.rend(), 
                                [&](const Task& tm){ return tm.getTaskId() == metrics.getTaskId(); });
    if (hist_it != m_completed_task_metrics.rend()) {
        // Preserve memory usage if it wasn't set in the new metrics
        if (metrics.getMemoryUsage() == 0 && hist_it->getMemoryUsage() > 0) {
            // No direct setter for memory usage in Task, but it's tracked by the virtual getMemoryUsage method
        }
        *hist_it = metrics;
    } else {
        if (m_completed_task_metrics.size() >= max_completed_tasks_history_) {
            if (!m_completed_task_metrics.empty()) {
                m_completed_task_metrics.erase(m_completed_task_metrics.begin());
            }
        }
        m_completed_task_metrics.push_back(std::move(metrics)); 
    }
}

// Getter for the main task_metrics_ map
std::unordered_map<uint64_t, std::shared_ptr<Task>> ThreadPool::get_all_task_metrics_map() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(task_metrics_mutex_));
    return task_metrics_;
}

std::vector<Task> ThreadPool::get_completed_task_metrics() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(m_completed_metrics_mutex));
    return m_completed_task_metrics; // Returns a copy
}

// Overall pool aggregated metrics getters
size_t ThreadPool::get_tasks_completed_count() const {
    return tasks_completed_count_.load(std::memory_order_relaxed);
}

size_t ThreadPool::get_tasks_failed_count() const {
    return tasks_failed_count_.load(std::memory_order_relaxed);
}

size_t ThreadPool::get_peak_active_tasks() const {
    return peak_active_tasks_.load(std::memory_order_relaxed);
}

uint64_t ThreadPool::get_total_pool_execution_time_ns() const {
    return total_pool_execution_time_ns_.load(std::memory_order_relaxed);
}

uint64_t ThreadPool::get_total_pool_wait_time_ns() const {
    return total_pool_wait_time_ns_.load(std::memory_order_relaxed);
}

std::vector<ThreadMetrics> ThreadPool::get_thread_metrics() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(thread_metrics_mutex_));
    std::vector<ThreadMetrics> result;
    result.reserve(thread_metrics_.size());
    for (const auto& pair : thread_metrics_) {
        result.push_back(pair.second);
    }
    return result;
}

void ThreadPool::print_performance_metrics(bool detailed) const {
    std::ostringstream oss;
    oss << "\n--- ThreadPool Performance Report for '" << pool_name_ << "' ---"
        << "\n";
    oss << "Desired Threads: " << num_threads_.load() 
        << ", Active Threads: " << active_thread_count_.load()
        << ", Currently Processing Tasks: " << currently_active_tasks_.load() 
        << " (Peak: " << get_peak_active_tasks() << ")\n";
    oss << "Task Queue Size: " << tasks_->size() << "\n";
    oss << "Tasks Submitted: " << tasks_submitted_count_.load()
        << ", Tasks Processed (worker): " << m_tasks_processed.load() // m_tasks_processed from worker loop
        << ", Tasks Completed Successfully: " << get_tasks_completed_count() 
        << ", Tasks Failed: " << get_tasks_failed_count() << "\n";
    oss << "Total Pool Execution Time: " << formatDurationNs(get_total_pool_execution_time_ns()) << "\n";
    oss << "Total Pool Wait Time: " << formatDurationNs(get_total_pool_wait_time_ns()) << "\n";

    if (detailed) {
        oss << "\nIndividual Thread Metrics:\n";
        std::vector<ThreadMetrics> current_thread_metrics = get_thread_metrics(); // Uses its own mutex
        if (current_thread_metrics.empty()) {
            oss << "  No thread metrics available.\n";
        } else {
            for (const auto &tm : current_thread_metrics) {
                std::hash<std::thread::id> hasher;
                std::string short_id_str = std::to_string(hasher(tm.thread_id) % 100000);
                oss << "  Thread [" << tm.pool_name << "-" << short_id_str << "] (Native: " << tm.thread_id << ")\n";
                oss << "    Status: " << (tm.is_executing ? "Executing '" + tm.current_task_name + "'" : "Idle") << "\n";
                oss << "    Tasks Completed: " << tm.tasks_completed.load() << "\n";
                oss << "    Total Exec Time: " << formatDurationNs(tm.total_execution_time_ns.load()) << "\n";
                oss << "    Avg Exec Time:   " << formatDurationNs(tm.tasks_completed.load() == 0 ? 0 : tm.total_execution_time_ns.load() / tm.tasks_completed.load()) << "\n";
                oss << "    Min Exec Time:   " << formatDurationNs(tm.min_execution_time_ns.load()) << "\n";
                oss << "    Max Exec Time:   " << formatDurationNs(tm.max_execution_time_ns.load()) << "\n";
                oss << "    Total Wait Time: " << formatDurationNs(tm.total_wait_time_ns.load()) << "\n";
                oss << "    Avg Wait Time:   " << formatDurationNs(tm.tasks_completed.load() == 0 ? 0 : tm.total_wait_time_ns.load() / tm.tasks_completed.load()) << "\n";
                oss << "    Min Wait Time:   " << formatDurationNs(tm.min_wait_time_ns.load()) << "\n"; // New
                oss << "    Max Wait Time:   " << formatDurationNs(tm.max_wait_time_ns.load()) << "\n"; // New
                oss << "    Registered: " << formatTimePoint(tm.registration_time) << ", Last Active: " << formatTimePoint(tm.last_active_time) << "\n";
            }
        }

        oss << "\nTask History (most recent " << m_completed_task_metrics.size() << " tasks):\n";
        std::vector<Task> task_history = get_completed_task_metrics(); // Uses its own mutex
        
        // Sort by completion time, most recent first
        std::ranges::sort(task_history, [](const Task& a, const Task& b) {
            return a.getEndProcessingTime() > b.getEndProcessingTime();
        });

        for (const auto& task_m : task_history) {
            oss << "  Task ID: " << task_m.getTaskId() << ", Name: " << task_m.getTaskName() << "\n";
            oss << "    Thread: " << task_m.getExecutingThreadId() << "\n";
            oss << "    Enqueued: " << formatTimePoint(task_m.getEnqueueTime()) << "\n";
            oss << "    Started: " << formatTimePoint(task_m.getStartProcessingTime()) << "\n";
            oss << "    Finished: " << formatTimePoint(task_m.getEndProcessingTime()) << "\n";
            oss << "    Wait Time: " << formatDurationNs(task_m.getWaitDurationNs()) << "\n";
            oss << "    Execution Time: " << formatDurationNs(task_m.getExecutionDurationNs()) << "\n";
            oss << "    Memory Usage: " << task_m.getMemoryUsage() << " bytes\n";
            oss << "    Status: " << Task::taskStatusToString(task_m.getStatus()) << (task_m.getStatus() == TaskStatus::FAILED ? " Error: " + task_m.getErrorMessage() : "") << "\n";
        }
    } else {
        // Just show summary stats for recent tasks
        std::vector<Task> task_history = get_completed_task_metrics(); // Uses its own mutex
        
        size_t completed_count = 0, failed_count = 0, pending_count = 0, processing_count = 0;
        uint64_t total_exec_ns = 0, total_wait_ns = 0;
        size_t total_memory = 0;
        
        for (const auto& task_m : task_history) {
            switch (task_m.getStatus()) {
                case TaskStatus::COMPLETED: completed_count++; break;
                case TaskStatus::FAILED: failed_count++; break;
                case TaskStatus::PENDING: pending_count++; break;
                case TaskStatus::PROCESSING: processing_count++; break;
                default: break;
            }
            total_exec_ns += task_m.getExecutionDurationNs();
            total_wait_ns += task_m.getWaitDurationNs();
            total_memory += task_m.getMemoryUsage();
        }
        
        oss << "\nRecent Task Summary (" << task_history.size() << " tasks):\n";
        oss << "  Completed: " << completed_count << ", Failed: " << failed_count 
            << ", Pending: " << pending_count << ", Processing: " << processing_count << "\n";
        if (!task_history.empty()) {
            oss << "  Avg Execution Time: " << formatDurationNs(total_exec_ns / task_history.size()) << "\n";
            oss << "  Avg Wait Time: " << formatDurationNs(total_wait_ns / task_history.size()) << "\n";
            oss << "  Total Memory Usage: " << total_memory << " bytes\n";
        }
        
        // Show a few most recent tasks
        size_t to_show = std::min(task_history.size(), size_t(3));
        if (to_show > 0) {
            // Sort by completion time, most recent first
            std::ranges::sort(task_history, [](const Task& a, const Task& b) {
                return a.getEndProcessingTime() > b.getEndProcessingTime();
            });

            oss << "\n  Most Recent Tasks:\n";
            for (size_t i = 0; i < to_show; i++) {
                 // Print from most recent
                const auto& task_m = task_history[i];
                oss << "    Task ID: " << task_m.getTaskId() << ", Name: " << task_m.getTaskName() << "\n";
                oss << "      Execution Time: " << formatDurationNs(task_m.getExecutionDurationNs()) << "\n";
                oss << "      Status: " << Task::taskStatusToString(task_m.getStatus()) << (task_m.getStatus() == TaskStatus::FAILED ? " Error: " + task_m.getErrorMessage() : "") << "\n";
            }
        }
    }
    
    // Optionally print from the task_metrics_ map if different/more complete
    oss << "\nAll Task Metrics (from task_metrics_ map):\n";
    auto all_tasks_map = get_all_task_metrics_map(); // Uses its own mutex
    if (all_tasks_map.empty()) {
        oss << "  No task metrics map available.\n";
    } else {
        for (const auto &val: all_tasks_map | std::views::values) {
            const auto& task_m = *val;
             oss << "  Task ID: " << task_m.getTaskId() << ", Name: " << task_m.getTaskName() << "\n";
            oss << "    Thread: " << task_m.getExecutingThreadId() << "\n";
            oss << "    Enqueued: " << formatTimePoint(task_m.getEnqueueTime())
                << ", Started: " << formatTimePoint(task_m.getStartProcessingTime())
                << ", Finished: " << formatTimePoint(task_m.getEndProcessingTime()) << "\n";
            oss << "    Wait Time: " << formatDurationNs(task_m.getWaitDurationNs()) 
                << ", Exec Time: " << formatDurationNs(task_m.getExecutionDurationNs()) << "\n";
        }
    }

    oss << "--- End of Report ---";
    // Output to console (or log buffer)
    LOG_INF("ResourceManager", oss.str());
}
