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
    // Constants for memory pool sizes
    static constexpr size_t MAX_BUCKETS = 32; // Still our ceiling
    static constexpr size_t NUM_SIZE_BUCKETS = 16; // Default value, will be adjusted in constructor
    static constexpr size_t MIN_POOL_BLOCK_SIZE = 8; // 8 bytes minimum
    static constexpr size_t MAX_CATEGORIES = 3; // Max categories

    // Define memory block categories
    enum class MemoryBlockCategory : uint8_t {
        GENERIC = 0,
        BUFFER = 1,
        COMPUTE = 2
    };

    // Memory block identifier
    struct MemoryBlockId {
        uint32_t pool_index;
        uint8_t size_bucket;
        MemoryBlockCategory category;

        MemoryBlockId() : pool_index(0), size_bucket(0), category(MemoryBlockCategory::GENERIC) {
        }

        MemoryBlockId(uint32_t idx, uint8_t bucket, MemoryBlockCategory cat)
            : pool_index(idx), size_bucket(bucket), category(cat) {
        }

        // Allow converting to and from a single 32-bit value for storage/retrieval
        [[nodiscard]] uint32_t toUint32() const {
            return (static_cast<uint32_t>(pool_index) |
                    (static_cast<uint32_t>(size_bucket) << 24) |
                    (static_cast<uint32_t>(category) << 29));
        }

        static MemoryBlockId fromUint32(uint32_t value) {
            MemoryBlockId id;
            id.pool_index = value & 0x00FFFFFF;
            id.size_bucket = (value >> 24) & 0x1F;
            id.category = static_cast<MemoryBlockCategory>((value >> 29) & 0x07);
            return id;
        }

        bool operator==(const MemoryBlockId &other) const {
            return pool_index == other.pool_index &&
                   size_bucket == other.size_bucket &&
                   category == other.category;
        }

        bool isValid() const {
            return pool_index > 0; // 0 is reserved for invalid
        }
    };

private:
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

    // Singleton instance
    static ResourceManager *instance;
    static std::mutex singleton_mutex;

    // Helper method to print debug messages
    static void debugLog(const std::string &message);

    // Thread limiting
    std::atomic<size_t> active_threads{0};
    std::atomic<size_t> peak_active_threads{0};
    size_t max_threads;
    std::mutex thread_mutex;
    std::condition_variable thread_cv;

    // Memory limiting
    std::atomic<size_t> current_memory_usage{0};
    std::atomic<size_t> peak_memory_usage{0};
    size_t max_memory_usage;
    std::mutex memory_mutex;
    std::condition_variable memory_cv;

    //Pool metrics
    mutable std::mutex pool_metrics_mutex;
    std::unordered_map<std::string, PoolAggregateMetrics> pool_metrics;


    // Memory pools for reusing allocations - restructured
    struct MemoryPoolBlock {
        void *memory_ptr{nullptr}; // Pointer to actual memory block
        size_t actual_size{0}; // Actual size in bytes of allocated memory
        std::chrono::system_clock::time_point timestamp; // When was this block created/last used
        bool in_use{false}; // Is block currently in use
    };

    // Memory pools organized by size and category
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

    // Pools organized by [category][size_bucket]
    std::vector<std::vector<std::unique_ptr<PoolBucket>>> pools;
    std::mutex pool_mutex; // For operations affecting pool as a whole

    // Memory tracking
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

    // Memory usage tracking by category
    std::atomic<size_t> memory_usage_by_category[MAX_CATEGORIES]{};
    std::mutex memory_usage_mutex;

    // Helper methods
    static std::string thread_id_to_string(const std::thread::id &id);

    static std::string formatMemorySize(size_t bytes);

    static std::string categoryToString(MemoryBlockCategory category);

    // Find appropriate size bucket for requested size
    static uint8_t getSizeBucketForSize(size_t bytes);

    // Get actual size for a given size bucket
    size_t getSizeForBucket(uint8_t bucket) const;

    // Initialize memory pool buckets
    void initializeBuckets();

public:
    //Helper method that returns a suggestion of <type> Threads based on system resources.
    std::pair<size_t, size_t> getRecommendedThreadDistribution() const;

    // Helper method to format time duration to human-readable format
    static std::string formatDuration(uint64_t nanoseconds);

private:
    // Private constructor for singleton
    ResourceManager();

    ResourceManager(size_t max_threads, size_t max_memory_usage);

public:
    /**
     * Get the singleton instance of ResourceManager
     */
    static ResourceManager &getInstance();

    // Destructor
    ~ResourceManager();

    // Thread management methods
    /**
     * Register a thread with the ResourceManager
     * This should be called when a thread is created
     *
     * @param pool_name Name of the thread pool this thread belongs to
     */
    void registerThread(const std::string &pool_name);

    /**
     * Unregister a thread from the ResourceManager
     * This should be called when a thread is about to be destroyed
     */
    void unregisterThread();

    // Getter methods for thread counts
    size_t getMaxThreads() const;

    size_t getActiveThreadCount() const;

    size_t getPeakActiveThreadCount() const;

    /**
     * Set the maximum number of threads
     */
    void setMaxThreads(size_t threads);

    /**
     * Set the maximum memory usage in bytes
     */
    void setMaxMemory(size_t memory_bytes);

    /**
     * Enable or disable detailed memory tracking
     */
    void setDetailedTracking(bool enabled);

    /**
     * Enable or disable memory tracking
     */
    void setMemoryTracking(bool enabled);

    /**
     * Get the current memory usage
     */
    size_t getCurrentMemoryUsage() const;

    /**
     * Get the maximum memory limit
     */
    size_t getMaxMemory() const;

    /**
     * Get the peak memory usage
     */
    size_t getPeakMemoryUsage() const;

    /**
     * Get the current thread count
     */
    size_t getCurrentThreadCount() const;

    /**
     * Print current memory allocation status
     */
    void printMemoryStatus();

    /**
     * Print detailed pool statistics
     */
    void printPoolStatistics();

    /**
     * Get memory from the pool if available, otherwise allocate new memory
     * @param bytes Number of bytes needed
     * @param category Category for this memory allocation
     * @return ID of the memory block, or a zeroed ID if allocation failed
     */
    MemoryBlockId getPooledMemory(size_t bytes, MemoryBlockCategory category = MemoryBlockCategory::GENERIC);

    /**
     * Release memory back to the pool for reuse
     * @param block_id ID returned from getPooledMemory
     * @return True if successful, false if invalid block ID
     */
    bool releasePooledMemory(MemoryBlockId block_id);

    /**
     * Get the actual pointer to memory from a block ID
     * @param block_id ID returned from getPooledMemory
     * @return Pointer to memory block, or nullptr if invalid
     */
    void *getMemoryPtr(MemoryBlockId block_id);

    /**
     * Get the size of memory from a block ID
     * @param block_id ID returned from getPooledMemory
     * @return Size of memory block in bytes, or 0 if invalid
     */
    size_t getMemorySize(MemoryBlockId block_id);

    /**
     * Wait for all threads to complete their work
     */
    void waitForAllThreads();

    /**
     * @brief Get a unique task ID for tracking purposes
     *
     * @return A unique task ID
     */
    std::atomic<uint64_t> next_task_id{0};

    uint64_t getNextTaskId();

    /**
     * Prints a summary report of thread performance.
     * @param detailed Whether to print detailed metrics
     */
    void printThreadPerformanceReport(bool detailed = false) const;

    void updateThreadMetrics(const std::string &pool_name, uint64_t exec_time_ns, uint64_t wait_time_ns);

    /**
     * Execute a function with a thread from the pool, waiting if necessary
     * @param func Function to execute
     * @param args Arguments to pass to the function
     * @param timeout_ms Maximum time to wait for thread availability in milliseconds (0 = wait indefinitely)
     * @return True if the thread was created, false if timeout occurred
     */
    template<typename Func, typename... Args>
    bool runWithThread(Func &&func, Args &&... args,
                       std::chrono::milliseconds timeout_ms = std::chrono::milliseconds(0)) {
        std::unique_lock<std::mutex> lock(thread_mutex);

        // Wait until a thread slot is available or timeout occurs
        bool thread_available;
        if (timeout_ms.count() > 0) {
            // Wait with timeout
            thread_available = thread_cv.wait_for(lock, timeout_ms, [this]() {
                return active_threads.load() < max_threads;
            });
        } else {
            // Wait indefinitely
            thread_cv.wait(lock, [this]() {
                return active_threads.load() < max_threads;
            });
            thread_available = true;
        }

        // If timeout occurred and no thread is available, return false
        if (!thread_available) {
            return false;
        }

        // Acquire a thread slot
        active_threads++;

        // Update peak active threads counter
        size_t current_active_threads = active_threads.load(std::memory_order_relaxed);
        size_t peak_active_threads_value = peak_active_threads.load(std::memory_order_relaxed);
        if (current_active_threads > peak_active_threads_value) {
            peak_active_threads.store(current_active_threads, std::memory_order_relaxed);
        }

        // Release the lock before starting the thread
        lock.unlock();

        std::string pool_name = "default";

        std::thread worker(
            [this, pool_name, f = std::forward<Func>(func), args = std::make_tuple(std::forward<Args>(args)...)
            ]() mutable {
                auto start_time = std::chrono::steady_clock::now();
                auto wait_duration = std::chrono::nanoseconds(0);

                try {
                    auto exec_start = std::chrono::steady_clock::now();
                    std::apply(f, args);
                    auto exec_end = std::chrono::steady_clock::now();

                    wait_duration = exec_start - start_time;
                    auto exec_time = exec_end - exec_start;

                    updateThreadMetrics(pool_name,
                                        std::chrono::duration_cast<std::chrono::nanoseconds>(exec_time).count(),
                                        std::chrono::duration_cast<std::chrono::nanoseconds>(wait_duration).count());
                } catch (const std::exception &e) {
                    debugLog(std::format("Thread exception: {}", e.what()));
                } {
                    std::lock_guard<std::mutex> lock(thread_mutex);
                    active_threads--;
                }
                thread_cv.notify_one();
            });
        // Detach the thread and let it run
        worker.detach();
        return true;
    }

    // ---- Manual memory accounting helpers ----
    /**
     * @brief Increase tracked memory usage.
     * Call this after allocating raw memory not automatically tracked.
     */
    void increaseMemory(size_t bytes);

    /**
     * @brief Decrease tracked memory usage.
     * Call this after freeing memory previously registered with increaseMemory().
     */
    void decreaseMemory(size_t bytes);
};
