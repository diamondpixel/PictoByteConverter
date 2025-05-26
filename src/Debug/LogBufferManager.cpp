#include "headers/LogBufferManager.h"
#include <stdexcept>
#include <format> // Added for C++20 std::format
#include <atomic> // Added for std::atomic<bool>
#include <Threading/headers/ThreadPool.h>

namespace debug {

// Initialize singleton instance
LogBufferManager& LogBufferManager::getInstance() {
    static LogBufferManager instance;
    return instance;
}

LogBufferManager::LogBufferManager() = default;

LogBufferManager::~LogBufferManager() {
    shutdown();
}

LogBuffer& LogBufferManager::getOrCreate(const std::string& name, size_t capacity, LogContext defaultContext) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return getOrCreateInternal(name, capacity, defaultContext);
}

LogBuffer& LogBufferManager::getOrCreateInternal(const std::string& name, size_t capacity, LogContext defaultContext) {
    // This internal method assumes mutex_ is already locked or locking is handled by caller if necessary
    // For safety, let's ensure it's locked here if called from a context that hasn't locked it.
    // However, typical internal calls might be within a locked scope already.
    // For this diagnostic, we'll assume it needs its own lock if not already part of a larger locked operation.
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Check if buffer exists
    auto it = buffers_.find(name);
    if (it != buffers_.end()) {
        return *it->second;
    }
    
    // Determine if initial memory tracking should be skipped for this buffer
    bool skipTracking = (name == "ResourceManager" || name == "LogBufferManagerInternal");

    // Create new buffer, passing the skipTracking flag
    auto buffer = std::make_unique<LogBuffer>(capacity, defaultContext, skipTracking);
    LogBuffer& bufferRef = *buffer;
    buffers_[name] = std::move(buffer);

    return bufferRef;
}

void LogBufferManager::appendTo(const std::string& name, const std::string& message, 
                              std::optional<LogContext> context) {
    if (shutting_down_.load(std::memory_order_relaxed)) {
        return; // Do not operate if shutting down
    }

    // RAII guard for the thread-local flag
    struct RecursionGuard {
        bool& flag_ref;
        bool original_value;
        bool set_new_value{false};

        RecursionGuard(bool& flag, const std::string& buffer_name) : flag_ref(flag), original_value(flag) {
            if (buffer_name == "ResourceManager" && !flag_ref) { // Only set if it's ResourceManager and not already set
                flag_ref = true;
                set_new_value = true;
            }
        }
        ~RecursionGuard() {
            if (set_new_value) { // Only reset if this instance set it
                 flag_ref = original_value; // Should be false if we set it from false
            }
        }
    };

    RecursionGuard guard(tls_inside_resource_manager_log_append, name);

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // Get or create buffer
    LogBuffer& buffer = getOrCreateInternal(name, DEFAULT_CAPACITY, DEFAULT_CONTEXT);
    
    // Append message with specified or default context
    buffer.append(message, context.value_or(buffer.getDefaultContext()));
}

void LogBufferManager::appendToAsync(const std::string& name, const std::string& message, 
                                   std::optional<LogContext> context) {
    if (shutting_down_.load(std::memory_order_relaxed)) {
        return; // Do not operate if shutting down
    }
    // Make copies of parameters for the lambda
    std::string nameCopy = name;
    std::string messageCopy = message;
    std::optional<LogContext> contextCopy = context;
    
    // Get thread pool (initializes if needed)
    ThreadPool& pool = getThreadPool();
    
    // Submit task to thread pool
    pool.submit([this, nameCopy, messageCopy, contextCopy]() {
        this->appendTo(nameCopy, messageCopy, contextCopy);
    });
}

std::vector<LogEntry> LogBufferManager::readFrom(const std::string& name) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // Check if buffer exists
    auto it = buffers_.find(name);
    if (it == buffers_.end()) {
        throw std::out_of_range(std::format("LogBuffer '{}' not found", name));
    }
    
    // Read all entries
    return it->second->readAll();
}

std::vector<LogEntry> LogBufferManager::readRecentFrom(const std::string& name, size_t count,
                                                     std::optional<LogContext> contextFilter) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // Check if buffer exists
    auto it = buffers_.find(name);
    if (it == buffers_.end()) {
        throw std::out_of_range(std::format("LogBuffer '{}' not found", name));
    }
    
    // Read recent entries
    return it->second->readRecent(count, contextFilter);
}

bool LogBufferManager::exists(const std::string& name) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return buffers_.find(name) != buffers_.end();
}

bool LogBufferManager::remove(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // Check if buffer exists
    auto it = buffers_.find(name);
    if (it == buffers_.end()) {
        return false;
    }
    
    // Remove buffer
    buffers_.erase(it);
    LogBufferManager::getInstance().appendTo("LogBufferManagerInternal", std::format("Removed LogBuffer '{}'", name), debug::LogContext::Debug);
    return true;
}

bool LogBufferManager::clear(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // Check if buffer exists
    auto it = buffers_.find(name);
    if (it == buffers_.end()) {
        return false;
    }
    
    // Clear buffer
    it->second->clear();
    LogBufferManager::getInstance().appendTo("LogBufferManagerInternal", std::format("Cleared LogBuffer '{}'", name), debug::LogContext::Debug);
    return true;
}

std::vector<std::string> LogBufferManager::getBufferNames() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    std::vector<std::string> names;
    names.reserve(buffers_.size());
    
    for (const auto& pair : buffers_) {
        names.push_back(pair.first);
    }
    
    return names;
}

void LogBufferManager::initializeThreadPool(size_t numThreads) {
    std::lock_guard<std::mutex> lock(threadPoolMutex_);
    
    // Check if thread pool already exists
    if (threadPool_) {
        LogBufferManager::getInstance().appendTo("LogBufferManagerInternal", "Thread pool already initialized", debug::LogContext::Warning);
        return;
    }
    
    // Create thread pool
    threadPool_ = std::make_unique<ThreadPool>(numThreads, SIZE_MAX, "LogBufferManager_Pool");
    if (threadPool_) { 
        LogBufferManager::getInstance().appendTo("LogBufferManagerInternal", std::format("Initialized LogBufferManager thread pool with {} threads", threadPool_->size()), debug::LogContext::Debug);
    }
}

void LogBufferManager::shutdown() {
    if (bool already_shutting_down = shutting_down_.exchange(true, std::memory_order_acq_rel)) {
        return; // Shutdown already in progress or completed
    }

    // Optional: Log shutdown initiation to a primary/console log if available and safe
    // std::cout << "LogBufferManager shutting down..." << std::endl;

    {
        std::lock_guard<std::recursive_mutex> lock(mutex_); // Changed from std::recursive_mutex
        // Signal all waiting threads that shutdown has started (if any are waiting on cv_shutdown_)
        // cv_shutdown_.notify_all(); // May not be needed if no threads wait on this specifically

        for (auto& pair : buffers_) {
            if (pair.second) {
                pair.second.reset();// Corrected: Assuming flush(bool force) exists for LogBuffer, was pair.second->flush(true);
            }
        }
        buffers_.clear(); // Clear all buffers
    }

    // If there's a thread pool managed or used exclusively by LogBufferManager, shut it down.
    // Ensure thread pool is shut down after logs are flushed.
    if (threadPool_) { // Corrected: was if (threadPool_ && threadPoolOwner_)
        //std::cout << "LogBufferManager initiating ThreadPool shutdown..." << std::endl;
        threadPool_->shutdown(); // Corrected: Assuming ThreadPool has a shutdown() method, was requestStop() and waitForTasks()
        //std::cout << "LogBufferManager ThreadPool shutdown complete." << std::endl;
    } // threadPool_ unique_ptr will be destroyed if owned, or just detach if not owned

    // Optional: Log shutdown completion
    // std::cout << "LogBufferManager shutdown complete." << std::endl;
}

ThreadPool& LogBufferManager::getThreadPool() {
    std::lock_guard<std::mutex> lock(threadPoolMutex_);
    
    // Initialize thread pool if it doesn't exist
    if (!threadPool_) {
        initializeThreadPool(0); // Use default thread count
    }
    
    return *threadPool_;
}

// Define the thread-local storage flag
thread_local bool LogBufferManager::tls_inside_resource_manager_log_append = false;

} // namespace debug
