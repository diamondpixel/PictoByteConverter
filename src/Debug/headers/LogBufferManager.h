#ifndef LOG_BUFFER_MANAGER_H
#define LOG_BUFFER_MANAGER_H

#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <optional>
#include <atomic>
#include "LogBuffer.h"

// Forward declaration
class ThreadPool;

namespace debug {

/**
 * @brief Centralized manager for named LogBuffer instances
 * 
 * Provides a registry for creating, accessing, and operating on multiple LogBuffer
 * instances by name. Thread-safe for concurrent access from multiple threads.
 */
class LogBufferManager {
public:
    /**
     * @brief Get the singleton instance of LogBufferManager
     * 
     * @return Reference to the singleton instance
     */
    static LogBufferManager& getInstance();

    /**
     * @brief Get or create a log buffer with the specified name and capacity
     * 
     * @param name Name of the log buffer
     * @param capacity Maximum number of entries (default: 100)
     * @param defaultContext Default context for new entries (default: Info)
     * @return Reference to the LogBuffer instance
     */
    LogBuffer& getOrCreate(const std::string& name, size_t capacity = 100, 
                           debug::LogContext defaultContext = debug::LogContext::Info);
    
    /**
     * @brief Append a message to a named buffer
     * 
     * If the named buffer doesn't exist, it will be created with default settings.
     * 
     * @param name Name of the buffer to append to
     * @param message Message to append
     * @param context Optional context for the message
     */
    void appendTo(const std::string& name, const std::string& message, 
                 std::optional<debug::LogContext> context = std::nullopt);
    
    /**
     * @brief Asynchronously append a message to a named buffer
     * 
     * This method returns immediately and performs the append operation
     * on a background thread from the thread pool.
     * 
     * @param name Name of the buffer to append to
     * @param message Message to append
     * @param context Optional context for the message
     */
    void appendToAsync(const std::string& name, const std::string& message, 
                      std::optional<debug::LogContext> context = std::nullopt);
    
    /**
     * @brief Read all entries from a named buffer
     * 
     * @param name Name of the buffer to read from
     * @return Vector of all log entries, from oldest to newest
     * @throws std::out_of_range if the named buffer doesn't exist
     */
    std::vector<LogEntry> readFrom(const std::string& name) const;
    
    /**
     * @brief Read recent entries from a named buffer
     * 
     * @param name Name of the buffer to read from
     * @param count Maximum number of entries to read
     * @param contextFilter Optional context to filter entries by
     * @return Vector of recent log entries, from newest to oldest
     * @throws std::out_of_range if the named buffer doesn't exist
     */
    std::vector<LogEntry> readRecentFrom(const std::string& name, size_t count,
                                        std::optional<debug::LogContext> contextFilter = std::nullopt) const;
    
    /**
     * @brief Check if a buffer with the specified name exists
     * 
     * @param name Name to check
     * @return true if the buffer exists, false otherwise
     */
    bool exists(const std::string& name) const;
    
    /**
     * @brief Remove a buffer from the manager
     * 
     * @param name Name of the buffer to remove
     * @return true if the buffer was removed, false if it didn't exist
     */
    bool remove(const std::string& name);
    
    /**
     * @brief Clear all entries from a named buffer
     * 
     * @param name Name of the buffer to clear
     * @return true if the buffer was cleared, false if it didn't exist
     */
    bool clear(const std::string& name);
    
    /**
     * @brief Get the names of all managed buffers
     * 
     * @return Vector of buffer names
     */
    std::vector<std::string> getBufferNames() const;
    
    /**
     * @brief Initialize the thread pool for async operations
     * 
     * @param numThreads Number of threads in the pool (0 = auto)
     */
    void initializeThreadPool(size_t numThreads = 0);
    
    /**
     * @brief Shutdown the thread pool and wait for pending operations
     */
    void shutdown();

public:
    // Flag to prevent recursive logging to ResourceManager from ResourceManager itself
    static thread_local bool tls_inside_resource_manager_log_append;

private:
    // Private constructor for singleton
    LogBufferManager();
    
    // Private destructor
    ~LogBufferManager();
    
    // Prevent copying and moving
    LogBufferManager(const LogBufferManager&) = delete;
    LogBufferManager& operator=(const LogBufferManager&) = delete;
    LogBufferManager(LogBufferManager&&) = delete;
    LogBufferManager& operator=(LogBufferManager&&) = delete;
    
    // Buffer storage
    std::unordered_map<std::string, std::unique_ptr<LogBuffer>> buffers_;
    
    // Mutex for thread-safe operations on buffers_ and other shared resources
    mutable std::recursive_mutex mutex_; // Made mutable for const methods

    // Shutdown flag
    std::atomic<bool> shutting_down_{false};
    
    // Thread pool for async operations
    std::unique_ptr<ThreadPool> threadPool_;
    std::mutex threadPoolMutex_;
    
    // Default settings
    static constexpr size_t DEFAULT_CAPACITY = 100;
    static constexpr debug::LogContext DEFAULT_CONTEXT = debug::LogContext::Info;
    
    // Helper methods
    LogBuffer& getOrCreateInternal(const std::string& name, size_t capacity, debug::LogContext defaultContext);
    ThreadPool& getThreadPool();
};

} // namespace debug

#endif // LOG_BUFFER_MANAGER_H
