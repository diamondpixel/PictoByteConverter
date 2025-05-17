#include "headers/ThreadPool.h"
#include <algorithm>
#include "../Debug/headers/Debug.h"

ThreadPool::ThreadPool(size_t num_threads, size_t queue_size, const std::string& pool_name) 
    : tasks_(queue_size, pool_name + "_queue"), pool_name_(pool_name) {
    
    auto& resource_manager = ResourceManager::getInstance();
    
    // Use ResourceManager's max threads if num_threads is 0
    if (num_threads == 0) {
        num_threads = resource_manager.getMaxThreads();
        // Fallback to 1 thread if getMaxThreads returns 0
        if (num_threads == 0) {
            num_threads = 1;
        }
    }
    
    // Limit the number of threads based on ResourceManager's available threads
    num_threads = std::min(num_threads, resource_manager.getMaxThreads());
    
    // Log thread pool creation
    printDebug("Creating ThreadPool '" + pool_name_ + "' with " + 
              std::to_string(num_threads) + " threads", true);
    
    // Create the worker threads
    workers_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back(&ThreadPool::worker_thread, this);
    }
}

ThreadPool::~ThreadPool() {
    shutdown(false);
}

ThreadPool::ThreadPool(ThreadPool&& other) noexcept 
    : workers_(std::move(other.workers_)), 
      tasks_(std::move(other.tasks_)),
      pool_name_(std::move(other.pool_name_)) {
    
    shutdown_.store(other.shutdown_.load(std::memory_order_acquire));
    
    // Clear the other's workers to prevent them from accessing moved resources
    other.workers_.clear();
    other.shutdown_.store(true, std::memory_order_release);
}

ThreadPool& ThreadPool::operator=(ThreadPool&& other) noexcept {
    if (this != &other) {
        // Shut down the current pool
        shutdown();
        
        // Move resources
        workers_ = std::move(other.workers_);
        tasks_ = std::move(other.tasks_);
        pool_name_ = std::move(other.pool_name_);
        shutdown_.store(other.shutdown_.load(std::memory_order_acquire));
        
        // Clear the other's workers to prevent them from accessing moved resources
        other.workers_.clear();
        other.shutdown_.store(true, std::memory_order_release);
    }
    return *this;
}

void ThreadPool::shutdown(bool print_message) {
    // Set the shutdown flag
    shutdown_.store(true, std::memory_order_release);
    
    // Signal all tasks to finish
    tasks_.done();
    
    // Join all worker threads
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    
    // Log thread pool shutdown
    if (print_message) {
        std::string debugText = "ThreadPool '" + pool_name_ + "' shut down";
        printDebug(debugText, true);
    }
    
    // Clear the workers
    workers_.clear();
}

bool ThreadPool::is_shutting_down() const {
    return shutdown_.load(std::memory_order_acquire);
}

std::future<void> ThreadPool::shutdown_async(bool print_message) {
    // Create a new promise for this shutdown request
    std::promise<void> shutdown_promise;
    std::future<void> shutdown_future = shutdown_promise.get_future();
    
    // Create a thread to handle the shutdown process
    std::thread shutdown_thread([this, print_message, promise = std::move(shutdown_promise)]() mutable {
        // Call the synchronous shutdown method
        this->shutdown(print_message);
        
        // Signal that shutdown is complete
        promise.set_value();
    });
    
    // Detach the thread since we'll wait on the future
    shutdown_thread.detach();
    
    return shutdown_future;
}

size_t ThreadPool::size() const {
    return workers_.size();
}

size_t ThreadPool::queue_size() const {
    return tasks_.size();
}

void ThreadPool::wait_for_tasks() {
    tasks_.wait_until_empty();
}

const std::string& ThreadPool::name() const {
    return pool_name_;
}

void ThreadPool::print_performance_metrics(bool detailed) const {
    // Get the ResourceManager instance
    auto& rm = ResourceManager::getInstance();
    
    // Get aggregate metrics for this pool
    auto metrics = rm.getPoolAggregateMetrics(pool_name_);
    
    // Print the metrics
    std::stringstream ss;
    ss << "Thread Pool '" << pool_name_ << "' Performance Metrics:\n";
    ss << "  Threads: " << metrics.thread_count << "\n";
    ss << "  Tasks Completed: " << metrics.total_tasks_completed << "\n";
    
    if (metrics.total_tasks_completed > 0) {
        ss << "  Execution Time: avg=" << rm.formatDuration(metrics.avg_execution_time_ns)
           << ", min=" << rm.formatDuration(metrics.min_execution_time_ns)
           << ", max=" << rm.formatDuration(metrics.max_execution_time_ns) << "\n";
        
        if (metrics.total_wait_time_ns > 0) {
            ss << "  Wait Time: avg=" << rm.formatDuration(metrics.avg_wait_time_ns)
               << ", min=" << rm.formatDuration(metrics.min_wait_time_ns)
               << ", max=" << rm.formatDuration(metrics.max_wait_time_ns) << "\n";
        }
    }
    
    // Print active tasks
    auto active_tasks = rm.getActiveTasks();
    int pool_active_tasks = 0;
    
    for (const auto& task : active_tasks) {
        if (task.pool_name == pool_name_) {
            pool_active_tasks++;
        }
    }
    
    ss << "  Active Tasks: " << pool_active_tasks << "\n";
    ss << "  Queue Size: " << tasks_.size() << "\n";
    
    // If detailed is true, print metrics for each thread
    if (detailed) {
        auto pool_threads = rm.getPoolMetrics(pool_name_);
        
        ss << "\n  Thread Details:\n";
        for (const auto& thread : pool_threads) {
            ss << "    Thread " << thread.thread_id << ":\n";
            ss << "      Tasks: " << thread.tasks_completed.load() << "\n";
            
            if (thread.tasks_completed.load() > 0) {
                auto avg_exec_time = thread.total_execution_time_ns.load() / thread.tasks_completed.load();
                
                ss << "      Execution Time: avg=" << rm.formatDuration(avg_exec_time)
                   << ", min=" << rm.formatDuration(thread.min_execution_time_ns.load())
                   << ", max=" << rm.formatDuration(thread.max_execution_time_ns.load()) << "\n";
                   
                if (thread.total_wait_time_ns.load() > 0) {
                    auto avg_wait_time = thread.total_wait_time_ns.load() / thread.tasks_completed.load();
                    
                    ss << "      Wait Time: avg=" << rm.formatDuration(avg_wait_time)
                       << ", min=" << rm.formatDuration(thread.min_wait_time_ns.load())
                       << ", max=" << rm.formatDuration(thread.max_wait_time_ns.load()) << "\n";
                }
            }
        }
    }
    
    // Print the report
    printDebug(ss.str());
}

void ThreadPool::worker_thread() {
    // Register this thread with the ResourceManager
    ResourceManager::getInstance().registerThread(pool_name_);
    
    while (!shutdown_.load(std::memory_order_acquire)) {
        std::function<void()> task;
        uint64_t task_id = 0;
        std::string task_name;
        
        // Try to get a task from the queue with its ID and name
        if (tasks_.pop(task, task_id, task_name)) {
            try {
                // Mark this thread as active with the task ID and name
                ResourceManager::getInstance().acquireThread(task_id, task_name);
                
                // Execute the task
                task();
                
                // Mark this thread as inactive
                ResourceManager::getInstance().releaseThread(task_id);
            }
            catch (const std::exception& e) {
                // Log any exceptions that occur during task execution
                printError("Exception in ThreadPool '" + pool_name_ + "' worker: " + e.what());
                
                // Make sure we release the thread even if an exception occurs
                ResourceManager::getInstance().releaseThread(task_id);
            }
        }
        else {
            // No task available, sleep for a short time to prevent busy waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    
    // Unregister this thread from the ResourceManager
    ResourceManager::getInstance().unregisterThread();
}
