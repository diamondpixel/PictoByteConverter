#ifndef LOG_BUFFER_H
#define LOG_BUFFER_H

#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <memory>
#include <atomic>
#include <optional>

// Forward declaration for ResourceManager
class ResourceManager;

namespace debug {

/**
 * @brief Represents the context of a log entry
 * 
 * Used to categorize log entries for filtering and organization
 */
enum class LogContext {
    Debug,
    Info,
    Warning,
    Error,
    Success,
    UserAction,
    Processing,
    Memory,
    Performance,
    System
};

/**
 * @brief Converts LogContext enum to string representation
 * 
 * @param context The context to convert
 * @return String representation of the context
 */
inline std::string logContextToString(LogContext context) {
    switch (context) {
        case LogContext::Debug:       return "Debug";
        case LogContext::Info:        return "Info";
        case LogContext::Warning:     return "Warning";
        case LogContext::Error:       return "Error";
        case LogContext::Success:     return "Success";
        case LogContext::UserAction:  return "UserAction";
        case LogContext::Processing:  return "Processing";
        case LogContext::Memory:      return "Memory";
        case LogContext::Performance: return "Performance";
        case LogContext::System:      return "System";
        default:                      return "Unknown";
    }
}

/**
 * @brief Represents a single log entry with timestamp and context
 */
struct LogEntry {
    std::chrono::system_clock::time_point timestamp;
    std::string message;
    LogContext context;
    
    LogEntry(const std::string& msg, LogContext ctx)
        : timestamp(std::chrono::system_clock::now()),
          message(msg),
          context(ctx) {}
    
    LogEntry(const std::string& msg, LogContext ctx, std::chrono::system_clock::time_point ts)
        : timestamp(ts),
          message(msg),
          context(ctx) {}
};

/**
 * @brief Fixed-size, thread-safe log buffer with context support
 * 
 * Provides a circular buffer for storing log entries with timestamps and contexts.
 * Thread-safe for concurrent append and read operations.
 */
class LogBuffer {
public:
    /**
     * @brief Construct a new LogBuffer with the specified capacity
     * 
     * @param capacity Maximum number of log entries to store
     * @param defaultContext Default context for entries without specified context
     * @param skipInitialTracking Skip initial memory tracking (default: false)
     */
    explicit LogBuffer(size_t capacity, LogContext defaultContext, bool skipInitialTracking = false);
    
    /**
     * @brief Destructor
     */
    ~LogBuffer();
    
    /**
     * @brief Append a log entry to the buffer
     * 
     * If the buffer is full, the oldest entry will be overwritten.
     * 
     * @param message The log message to append
     * @param context The context of the log entry (optional)
     */
    void append(const std::string& message, LogContext context = LogContext::Info);
    
    /**
     * @brief Read all log entries in the buffer
     * 
     * @return Vector of all log entries, from oldest to newest
     */
    std::vector<LogEntry> readAll() const;
    
    /**
     * @brief Read the most recent log entries
     * 
     * @param count Maximum number of entries to read
     * @param contextFilter Optional context to filter entries by
     * @return Vector of recent log entries, from newest to oldest
     */
    std::vector<LogEntry> readRecent(size_t count, std::optional<LogContext> contextFilter = std::nullopt) const;
    
    /**
     * @brief Clear all log entries from the buffer
     */
    void clear();
    
    /**
     * @brief Get the current number of entries in the buffer
     * 
     * @return Current entry count
     */
    size_t size() const;
    
    /**
     * @brief Get the maximum capacity of the buffer
     * 
     * @return Buffer capacity
     */
    size_t capacity() const;
    
    /**
     * @brief Set the default context for new entries
     * 
     * @param context The new default context
     */
    void setDefaultContext(LogContext context);
    
    /**
     * @brief Get the default context for new entries
     * 
     * @return The current default context
     */
    LogContext getDefaultContext() const;

private:
    // Buffer storage and metadata
    std::vector<LogEntry> buffer_;
    size_t capacity_;
    size_t head_; // Position to write next entry
    size_t count_; // Number of valid entries
    LogContext defaultContext_;
    
    // Thread synchronization
    mutable std::mutex mutex_;
    
    // Memory tracking
    std::string resourceTag_;
    std::atomic<size_t> totalTrackedMemory_{0}; // Total memory reported to ResourceManager
    std::atomic<std::ptrdiff_t> pendingMessageMemoryDelta_{0}; // Accumulates message memory changes

    static constexpr size_t MESSAGE_MEMORY_FLUSH_THRESHOLD = 4096; // 4KB threshold
    
    // Helper methods
    void flushMessageMemoryDelta(bool forceFlush = false);
    void updateAndFlushMessageMemoryDelta(std::ptrdiff_t messageDelta);
    size_t calculateEntrySize(const LogEntry& entry) const;
};

} // namespace debug

#endif // LOG_BUFFER_H
