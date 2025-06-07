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
#include <unordered_map>
#include <string>
#include <format>
#include "../../Debug/headers/MemoryTypes.h" // shared types
#include "../../Debug/headers/LogMacros.h" // logging macros

/**
 * A class to handle thread and memory limitations.
 * This singleton class provides the ability to:
 * 1. Limit the number of threads used by the program
 * 2. Track and limit memory usage across operations
 * 3. Wait for resource availability
 * 4. Monitor thread performance metrics
 */
class ResourceManager {
public:
    /**
     * @brief Constants for memory pool sizes.
     */
    static constexpr size_t MAX_BUCKETS = 32; // Still our ceiling
    static constexpr size_t NUM_SIZE_BUCKETS = 32;
    static constexpr size_t MIN_POOL_BLOCK_SIZE = 8; // 8 bytes minimum
    static constexpr size_t MAX_CATEGORIES = 3; // Max categories

    /**
     * @brief Shared memory block types.
     */
    using MemoryBlockCategory = memory::MemoryBlockCategory;
    using MemoryBlockId = memory::MemoryBlockId;

    /**
     * @brief Memory pools for reusing allocations - restructured.
     */
    struct MemoryPoolBlock {
        void *memory_ptr{nullptr}; // Pointer to actual memory block
        size_t actual_size{0}; // Actual size in bytes of allocated memory
        std::chrono::system_clock::time_point timestamp; // When was this block created/last used
        bool in_use{false}; // Is block currently in use
    };

    /**
     * @brief Memory pools organized by size and category.
     */
    struct PoolBucket {
        std::vector<MemoryPoolBlock> blocks;
        std::vector<uint32_t> free_indices; // Indices of available blocks
        std::mutex bucket_mutex;
        size_t block_size{0}; // Standard size for this bucket

        // Statistics
        size_t hits{0}; // Number of successful reuses
        size_t misses{0}; // Number of times a new allocation was needed
        size_t current_blocks{0}; // Number of blocks currently allocated
        size_t peak_blocks{0}; // Peak number of blocks
    };

    /**
     * @brief Pools organized by [category][size_bucket].
     */
    std::vector<std::vector<std::unique_ptr<PoolBucket> > > pools;
    std::mutex pool_mutex; // For operations affecting pool as a whole

    /**
     * @brief Memory tracking.
     */
    struct AllocationInfo {
        size_t size{};
        MemoryBlockCategory category{MemoryBlockCategory::GENERIC};
        std::chrono::system_clock::time_point timestamp;
        void *memory_ptr{nullptr};
    };

    std::unordered_map<uint32_t, AllocationInfo> memory_allocations;
    std::mutex tracking_mutex;
    bool detailed_tracking_enabled{true};
    bool memory_tracking_enabled{true};
    uint32_t allocation_id_counter{0};

    /**
     * @brief Memory usage tracking by category.
     */
    std::atomic<size_t> memory_usage_by_category[MAX_CATEGORIES]{};
    std::mutex memory_usage_mutex;

    /**
     * @brief Helper methods.
     */
    static std::string thread_id_to_string(const std::thread::id &id);

    static std::string formatMemorySize(size_t bytes);

    static std::string categoryToString(MemoryBlockCategory category);

    /**
     * @brief Find appropriate size bucket for requested size.
     * @param bytes Number of bytes needed.
     * @return Size bucket index.
     */
    static uint8_t getSizeBucketForSize(size_t bytes);

    /**
     * @brief Get actual size for a given size bucket.
     * @param bucket Size bucket index.
     * @return Actual size in bytes.
     */
    size_t getSizeForBucket(uint8_t bucket) const;

    /**
     * @brief Initialize memory pool buckets.
     */
    void initializeBuckets();

    /**
     * @brief Aggregated performance metrics per pool.
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

    /**
     * @brief Singleton instance.
     */
    static ResourceManager *instance;
    static std::mutex singleton_mutex;

    /**
     * @brief Thread limiting.
     */
    std::atomic<size_t> active_threads{0};
    std::atomic<size_t> peak_active_threads{0};
    size_t max_threads{std::thread::hardware_concurrency()};
    std::mutex thread_mutex;
    std::condition_variable thread_cv;

    /**
     * @brief Memory limiting.
     */
    std::atomic<size_t> current_memory_usage{0};
    std::atomic<size_t> peak_memory_usage{0};
    size_t max_memory_usage{static_cast<size_t>(1ULL * 1024 * 1024 * 1024)}; // default 1GB
    std::mutex memory_mutex;
    std::condition_variable memory_cv;

    /**
     * @brief Pool metrics.
     */
    mutable std::mutex pool_metrics_mutex;
    std::unordered_map<std::string, PoolAggregateMetrics> pool_metrics;

    /**
     * @brief Helper method to format time duration to human-readable format.
     * @param nanoseconds Time duration in nanoseconds.
     * @return Human-readable time duration string.
     */
    static std::string formatDuration(uint64_t nanoseconds);

public:
    /**
     * @brief Get the singleton instance of the ResourceManager.
     * @return Reference to the ResourceManager instance.
     */
    static ResourceManager &getInstance();

    /**
     * @brief Destructor for ResourceManager. Cleans up resources and memory pools.
     */
    ~ResourceManager();

    /**
     * @brief Register a thread with the ResourceManager.
     * @param pool_name Name of the thread pool this thread belongs to.
     */
    void registerThread(const std::string &pool_name);

    /**
     * @brief Unregister a thread from the ResourceManager.
     */
    void unregisterThread();

    /**
     * @brief Get the maximum number of threads allowed.
     * @return Maximum thread count.
     */
    size_t getMaxThreads() const;

    /**
     * @brief Get the current number of active threads.
     * @return Number of active threads.
     */
    size_t getActiveThreadCount() const;

    /**
     * @brief Get the peak number of active threads observed.
     * @return Peak active thread count.
     */
    size_t getPeakActiveThreadCount() const;

    /**
     * @brief Set the maximum number of threads.
     * @param threads New thread limit.
     */
    void setMaxThreads(size_t threads);

    /**
     * @brief Set the maximum memory usage in bytes.
     * @param memory_bytes Maximum allowed memory usage.
     */
    void setMaxMemory(size_t memory_bytes);

    /**
     * @brief Enable or disable detailed memory tracking.
     * @param enabled True to enable, false to disable.
     */
    void setDetailedTracking(bool enabled);

    /**
     * @brief Enable or disable memory tracking.
     * @param enabled True to enable, false to disable.
     */
    void setMemoryTracking(bool enabled);

    /**
     * @brief Get the current memory usage in bytes.
     * @return Current memory usage.
     */
    size_t getCurrentMemoryUsage() const;

    /**
     * @brief Get the maximum memory limit in bytes.
     * @return Maximum memory usage allowed.
     */
    size_t getMaxMemory() const;

    /**
     * @brief Get the peak memory usage observed.
     * @return Peak memory usage in bytes.
     */
    size_t getPeakMemoryUsage() const;

    /**
     * @brief Get the current thread count (total threads).
     * @return Current thread count.
     */
    size_t getCurrentThreadCount() const;

    /**
     * @brief Print current memory allocation status to log.
     */
    void printMemoryStatus();

    /**
     * @brief Print detailed pool statistics to log.
     */
    void printPoolStatistics();

    /**
     * @brief Get memory from the pool if available, otherwise allocate new memory.
     * @param bytes Number of bytes needed.
     * @param category Category for this memory allocation.
     * @return ID of the memory block, or a zeroed ID if allocation failed.
     */
    MemoryBlockId getPooledMemory(size_t bytes, MemoryBlockCategory category = MemoryBlockCategory::GENERIC);

    /**
     * @brief Release memory back to the pool for reuse.
     * @param block_id ID returned from getPooledMemory.
     * @return True if successful, false if invalid block ID.
     */
    bool releasePooledMemory(MemoryBlockId block_id);

    /**
     * @brief Get the actual pointer to memory from a block ID.
     * @param block_id ID returned from getPooledMemory.
     * @return Pointer to memory block, or nullptr if invalid.
     */
    void *getMemoryPtr(MemoryBlockId block_id);

    /**
     * @brief Get the size of memory from a block ID.
     * @param block_id ID returned from getPooledMemory.
     * @return Size of memory block in bytes, or 0 if invalid.
     */
    size_t getMemorySize(MemoryBlockId block_id);

    /**
     * @brief Wait for all threads to complete their work.
     */
    void waitForAllThreads();


    std::atomic<uint64_t> next_task_id{0};
    /**
     * @brief Get a unique task ID for tracking purposes.
     * @return A unique task ID.
     */
    uint64_t getNextTaskId();

    /**
     * @brief Prints a summary report of thread performance.
     * @param detailed Whether to print detailed metrics.
     */
    void printThreadPerformanceReport(bool detailed = false) const;

    /**
     * @brief Update thread metrics for a pool.
     * @param pool_name Name of the thread pool.
     * @param exec_time_ns Execution time in nanoseconds.
     * @param wait_time_ns Wait time in nanoseconds.
     */
    void updateThreadMetrics(const std::string &pool_name, uint64_t exec_time_ns, uint64_t wait_time_ns);

    /**
     * @brief Execute a function with a thread from the pool, waiting if necessary.
     * @tparam Func Function type.
     * @tparam Args Argument types.
     * @param func Function to execute.
     * @param args Arguments to pass to the function.
     * @param timeout_ms Maximum time to wait for thread availability in milliseconds (0 = wait indefinitely).
     * @return True if the thread was created, false if timeout occurred.
     */
    template<typename Func, typename... Args>
    bool runWithThread(Func &&func, Args &&... args,
                       std::chrono::milliseconds timeout_ms = std::chrono::milliseconds(0));

    /**
     * @brief High-level memory copy that ensures destination buffer is large enough; may allocate a new pooled BUFFER block and release the old one.
     * @param dest_id Destination memory block ID (BUFFER category).
     * @param src_id Source memory block ID.
     * @param size Number of bytes to copy.
     * @return The (possibly new) destination MemoryBlockId.
     */
    MemoryBlockId doMemcpy(MemoryBlockId dest_id, MemoryBlockId src_id, size_t size);

private:
    /**
     * @brief Private constructor for singleton pattern.
     */
    ResourceManager();

    /**
     * @brief Private constructor for singleton pattern with resource limits.
     * @param max_threads Maximum number of threads.
     * @param max_memory_usage Maximum memory usage in bytes.
     */
    ResourceManager(size_t max_threads, size_t max_memory_usage);

    // Memory copy statistics
    std::atomic<size_t> total_memcpy_bytes{0};
};
