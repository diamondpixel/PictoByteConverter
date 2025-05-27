#include "headers/ResourceManager.h"
#include <iostream>
#include "../Debug/headers/LogBufferManager.h"
#include "Debug/headers/LogBuffer.h"
#include "Debug/headers/Debug.h"
#include <algorithm>
#include <cmath>
#include <memory>

// Static member definitions
ResourceManager *ResourceManager::instance = nullptr;
std::mutex ResourceManager::singleton_mutex;

ResourceManager &ResourceManager::getInstance() {
    std::lock_guard<std::mutex> lock(singleton_mutex);
    if (!instance) {
        instance = new ResourceManager();
    }
    return *instance;
}

// Constructor implementation
ResourceManager::ResourceManager() : max_threads(1),
                                     max_memory_usage(1024 * 1024 * 1024) {
    // Initialize the pools vector with the right dimensions
    pools.resize(MAX_CATEGORIES);
    for (auto& categoryBuckets : pools) {
        categoryBuckets.resize(NUM_SIZE_BUCKETS);
        for (size_t i = 0; i < NUM_SIZE_BUCKETS; ++i) {
            categoryBuckets[i] = std::make_unique<PoolBucket>();
        }
    }
    initializeBuckets();
}

ResourceManager::ResourceManager(size_t max_threads, size_t max_memory_usage) : max_threads(max_threads),
    max_memory_usage(max_memory_usage) {
    // Initialize the pools vector with the right dimensions
    pools.resize(MAX_CATEGORIES);
    for (auto& categoryBuckets : pools) {
        categoryBuckets.resize(NUM_SIZE_BUCKETS);
        for (size_t i = 0; i < NUM_SIZE_BUCKETS; ++i) {
            categoryBuckets[i] = std::make_unique<PoolBucket>();
        }
    }
    initializeBuckets();
}

// Private method to initialize buckets with appropriate sizes
void ResourceManager::initializeBuckets() {
    // Initialize the pool buckets with appropriate sizes
    for (size_t category = 0; category < pools.size(); ++category) {
        for (size_t bucket = 0; bucket < pools[category].size(); ++bucket) {
            size_t size = getSizeForBucket(bucket);
            if (size > max_memory_usage) {
                size = max_memory_usage;
            }
            pools[category][bucket]->block_size = size;
        }
    }
}

// Destructor implementation
ResourceManager::~ResourceManager() {
    // Free all memory in pools
    for (size_t cat = 0; cat < pools.size(); ++cat) {
        for (size_t bucket = 0; bucket < pools[cat].size(); ++bucket) {
            auto& pool = pools[cat][bucket];
            if (!pool) continue;
            
            std::lock_guard<std::mutex> lock(pool->bucket_mutex);
            for (auto &block: pool->blocks) {
                if (block.memory_ptr) {
                    free(block.memory_ptr);
                    block.memory_ptr = nullptr;

                    // Update memory usage
                    if (current_memory_usage.load() >= block.actual_size) {
                        current_memory_usage -= block.actual_size;
                    }

                    if (cat < static_cast<size_t>(MAX_CATEGORIES)) {
                        auto &cat_usage = memory_usage_by_category[cat];
                        if (cat_usage.load() >= block.actual_size) {
                            cat_usage -= block.actual_size;
                        }
                    }
                }
            }
            pool->blocks.clear();
            pool->free_indices.clear();
        }
    }
}

// Helper method implementations
std::string ResourceManager::thread_id_to_string(const std::thread::id &id) {
    std::ostringstream oss;
    oss << id;
    return oss.str();
}

std::string ResourceManager::formatMemorySize(size_t bytes) {
    if (bytes < 1024) return std::format("{} B", bytes);
    if (bytes < 1024 * 1024) return std::format("{:.2f} KB", bytes / 1024.0);
    if (bytes < 1024 * 1024 * 1024) return std::format("{:.2f} MB", bytes / (1024.0 * 1024.0));
    return std::format("{:.2f} GB", bytes / (1024.0 * 1024.0 * 1024.0));
}

std::string ResourceManager::formatDuration(uint64_t nanoseconds) {
    if (nanoseconds < 1000) return std::format("{} ns", nanoseconds);
    if (nanoseconds < 1000 * 1000) return std::format("{:.2f} µs", nanoseconds / 1000.0);
    if (nanoseconds < 1000 * 1000 * 1000) return std::format("{:.2f} ms", nanoseconds / (1000.0 * 1000.0));
    return std::format("{:.2f} s", nanoseconds / (1000.0 * 1000.0 * 1000.0));
}

std::string ResourceManager::categoryToString(MemoryBlockCategory category) {
    switch (category) {
        case MemoryBlockCategory::GENERIC: return "Generic";
        case MemoryBlockCategory::BUFFER: return "Buffer";
        case MemoryBlockCategory::COMPUTE: return "Compute";
        default: return "Unknown";
    }
}

uint8_t ResourceManager::getSizeBucketForSize(size_t bytes) {
    if (bytes < MIN_POOL_BLOCK_SIZE) bytes = MIN_POOL_BLOCK_SIZE;

    // Calculate which power-of-2 bucket this belongs in
    // For example: 8, 16, 32, 64, 128, 256, 512, 1K, 2K, 4K, 8K, etc.
    const auto bucket = static_cast<uint8_t>(std::log2(bytes - 1) + 1);

    return std::min(bucket, static_cast<uint8_t>(NUM_SIZE_BUCKETS - 1));
}

size_t ResourceManager::getSizeForBucket(uint8_t bucket) const {
    // Size equals 2^bucket
    bucket = std::min(bucket, static_cast<uint8_t>(NUM_SIZE_BUCKETS - 1));
    size_t size = static_cast<size_t>(1) << bucket;

    // Ensure size doesn't exceed max_memory_usage
    return std::min(size, max_memory_usage);
}

std::pair<size_t, size_t> ResourceManager::getRecommendedThreadDistribution() const {
    if (max_threads == 1 || max_threads == 2) {
        return {1, 1};
    }
    size_t writer_threads = std::max<size_t>(1, static_cast<size_t>(std::ceil(max_threads * 0.25)));
    size_t worker_threads = std::max<size_t>(1, max_threads - writer_threads);
    return {writer_threads, worker_threads};
}

// Thread management methods
void ResourceManager::registerThread(const std::string &pool_name) {
    std::lock_guard<std::mutex> lock_thread(thread_mutex);

    std::thread::id thread_id = std::this_thread::get_id();

    active_threads++;

    size_t current_active_threads = active_threads.load(std::memory_order_relaxed);
    size_t peak_active_threads_value = peak_active_threads.load(std::memory_order_relaxed);
    if (current_active_threads > peak_active_threads_value) {
        peak_active_threads.store(current_active_threads, std::memory_order_relaxed);
    }

    debugLog(std::format("Thread {} from pool '{}' registered. Active threads: {}",
                         thread_id_to_string(thread_id), pool_name, active_threads.load()));
}

void ResourceManager::unregisterThread() {
    std::lock_guard<std::mutex> lock_thread(thread_mutex);

    std::thread::id thread_id = std::this_thread::get_id();

    if (active_threads > 0) {
        active_threads--;
    }

    debugLog(std::format("Thread {} unregistered. Active threads: {}",
                         thread_id_to_string(thread_id), active_threads.load()));

    if (active_threads == 0) {
        thread_cv.notify_all();
    }
}

// Getter methods for thread counts
size_t ResourceManager::getMaxThreads() const {
    return max_threads;
}

size_t ResourceManager::getActiveThreadCount() const {
    return active_threads.load(std::memory_order_relaxed);
}

size_t ResourceManager::getPeakActiveThreadCount() const {
    return peak_active_threads.load(std::memory_order_relaxed);
}

void ResourceManager::setMaxThreads(size_t threads) {
    max_threads = (threads > 0) ? threads : 1;
    debugLog(std::format("ResourceManager: Max threads set to {}", threads));
}

void ResourceManager::setMaxMemory(size_t memory_bytes) {
    max_memory_usage = (memory_bytes > 1024 * 1024 * 64) ? memory_bytes : 1024 * 1024 * 64; // Min 1MB
    debugLog(std::format("ResourceManager: Max memory set to {:.2f} MB", memory_bytes / (1024.0 * 1024.0)));
}

void ResourceManager::setDetailedTracking(bool enabled) {
    detailed_tracking_enabled = enabled;
}

void ResourceManager::setMemoryTracking(bool enabled) {
    memory_tracking_enabled = enabled;
}

size_t ResourceManager::getCurrentMemoryUsage() const {
    return current_memory_usage.load();
}

size_t ResourceManager::getMaxMemory() const {
    return max_memory_usage;
}

size_t ResourceManager::getPeakMemoryUsage() const {
    return peak_memory_usage.load();
}

size_t ResourceManager::getCurrentThreadCount() const {
    return active_threads.load();
}

void ResourceManager::printMemoryStatus() {
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

    // Print category breakdowns
    for (size_t cat = 0; cat < static_cast<size_t>(MAX_CATEGORIES); ++cat) {
        auto cat_usage = memory_usage_by_category[cat].load();
        if (cat_usage > 0) {
            debugLog(std::format("  {}: {}",
                                 categoryToString(static_cast<MemoryBlockCategory>(cat)),
                                 formatMemorySize(cat_usage)));
        }
    }

    // Print detailed report if enabled
    if (detailed_tracking_enabled && !memory_allocations.empty() && getDebugMode()) {
        debugLog("Detailed memory allocations:");

        for (const auto &[id, info]: memory_allocations) {
            auto now = std::chrono::system_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - info.timestamp).count();

            std::string detail = std::format("  ID: {} Size: {} Age: {}s Category: {}",
                                             id,
                                             formatMemorySize(info.size),
                                             duration,
                                             categoryToString(info.category));

            debugLog(detail);
        }
    }
}

void ResourceManager::printPoolStatistics() {
    debugLog("=== Memory Pool Statistics ===");

    for (size_t cat = 0; cat < pools.size(); ++cat) {
        auto category = static_cast<MemoryBlockCategory>(cat);
        size_t total_hits = 0;
        size_t total_misses = 0;
        size_t total_blocks = 0;
        size_t total_bytes = 0;

        for (size_t bucket = 0; bucket < pools[cat].size(); ++bucket) {
            std::lock_guard<std::mutex> lock(pools[cat][bucket]->bucket_mutex);
            auto &pool = pools[cat][bucket];

            if (pool->current_blocks > 0 || pool->hits > 0 || pool->misses > 0) {
                auto block_size = getSizeForBucket(bucket);
                total_hits += pool->hits;
                total_misses += pool->misses;
                total_blocks += pool->current_blocks;
                total_bytes += pool->current_blocks * block_size;

                // Only show details for buckets with activity
                debugLog(std::format("  {}, {}: {} blocks, {} hits, {} misses, hit rate: {:.1f}%",
                                     categoryToString(category),
                                     formatMemorySize(block_size),
                                     pool->current_blocks,
                                     pool->hits,
                                     pool->misses,
                                     pool->hits + pool->misses > 0
                                         ? (100.0 * pool->hits / (pool->hits + pool->misses))
                                         : 0.0));
            }
        }

        if (total_blocks > 0 || total_hits > 0 || total_misses > 0) {
            debugLog(std::format("{} Total: {} blocks, {} bytes, {} hits, {} misses, hit rate: {:.1f}%",
                                 categoryToString(category),
                                 total_blocks,
                                 formatMemorySize(total_bytes),
                                 total_hits,
                                 total_misses,
                                 total_hits + total_misses > 0
                                     ? (100.0 * total_hits / (total_hits + total_misses))
                                     : 0.0));
        }
    }

    debugLog("=============================");
}

ResourceManager::MemoryBlockId ResourceManager::getPooledMemory(size_t bytes, MemoryBlockCategory category) {
    // Find the appropriate size bucket for the requested size
    const uint8_t size_bucket = getSizeBucketForSize(bytes);
    size_t actual_size = getSizeForBucket(size_bucket);

    // Get reference to the appropriate pool bucket
    auto cat_index = static_cast<size_t>(category);
    if (cat_index >= static_cast<size_t>(MAX_CATEGORIES)) {
        cat_index = 0; // Default to generic
    }

    auto &pool = pools[cat_index][size_bucket];
    MemoryBlockId block_id;

    // Try to find an existing block in the specified pool
    {
        std::lock_guard<std::mutex> lock(pool->bucket_mutex);

        if (!pool->free_indices.empty()) {
            // Reuse an existing block
            uint32_t index = pool->free_indices.back();
            pool->free_indices.pop_back();

            auto &block = pool->blocks[index];
            block.in_use = true;
            block.timestamp = std::chrono::system_clock::now();

            // Create the block ID
            block_id = MemoryBlockId(index + 1, size_bucket, category); // +1 because 0 is invalid

            // Update pool statistics
            pool->hits++;

            // Log reuse
            debugLog(std::format("Reused memory block: {} from {} pool",
                                 formatMemorySize(actual_size),
                                 categoryToString(category)));

            return block_id;
        }
    }

    // No suitable block found, need to allocate new memory
    std::unique_lock<std::mutex> lock(memory_mutex);

    // Wait until memory is available
    bool success = memory_cv.wait_for(lock, std::chrono::milliseconds(5000), [this, actual_size]() {
        return (current_memory_usage.load() + actual_size) <= max_memory_usage;
    });

    if (!success) {
        debugLog(std::format("Memory allocation of {} timed out", formatMemorySize(actual_size)));
        return {}; // Return invalid ID
    }

    // Allocate memory
    void *memory = malloc(actual_size);
    if (!memory) {
        debugLog(std::format("Failed to allocate {} of memory", formatMemorySize(actual_size)));
        return {}; // Return invalid ID
    }

    // Update tracking
    current_memory_usage += actual_size;
    memory_usage_by_category[cat_index] += actual_size;
    peak_memory_usage = std::max(peak_memory_usage.load(), current_memory_usage.load());

    // Add to pool
    {
        std::lock_guard<std::mutex> pool_lock(pool->bucket_mutex);

        MemoryPoolBlock new_block;
        new_block.memory_ptr = memory;
        new_block.actual_size = actual_size;
        new_block.timestamp = std::chrono::system_clock::now();
        new_block.in_use = true;

        uint32_t index;

        // Check if we can reuse a spot in blocks vector from a previously deleted block
        if (!pool->free_indices.empty()) {
            index = pool->free_indices.back();
            pool->free_indices.pop_back();
            pool->blocks[index] = new_block;
        } else {
            // Add as new block
            index = static_cast<uint32_t>(pool->blocks.size());
            pool->blocks.push_back(new_block);
        }

        // Update pool statistics
        pool->misses++;
        pool->current_blocks++;
        pool->peak_blocks = std::max(pool->peak_blocks, pool->current_blocks);

        // Create the block ID
        block_id = MemoryBlockId(index + 1, size_bucket, category); // +1 because 0 is invalid
    }

    // Record allocation for tracking
    if (memory_tracking_enabled) {
        std::lock_guard<std::mutex> tracking_lock(tracking_mutex);
        uint32_t alloc_id = ++allocation_id_counter;
        memory_allocations[alloc_id] = {
            actual_size,
            category,
            std::chrono::system_clock::now(),
            memory
        };

        // Log allocation
        std::string message = std::format("Memory allocated: {} for {} usage",
                                          formatMemorySize(actual_size),
                                          categoryToString(category));

        message += std::format(" (Total: {}, allocations: {})",
                               formatMemorySize(current_memory_usage.load()),
                               memory_allocations.size());

        debugLog(message);
    }

    return block_id;
}

bool ResourceManager::releasePooledMemory(MemoryBlockId block_id) {
    if (!block_id.isValid()) return false;

    auto cat_index = static_cast<size_t>(block_id.category);
    if (cat_index >= pools.size() || block_id.size_bucket >= pools[cat_index].size()) return false;

    auto &pool = pools[cat_index][block_id.size_bucket];
    uint32_t index = block_id.pool_index - 1; // adjust for +1 offset

    std::lock_guard<std::mutex> lock(pool->bucket_mutex);
    if (index >= pool->blocks.size()) return false;

    auto &block = pool->blocks[index];
    if (!block.in_use || !block.memory_ptr) return false;

    block.in_use = false;
    pool->free_indices.push_back(index);

    return true;
}

void* ResourceManager::getMemoryPtr(MemoryBlockId block_id) {
    if (!block_id.isValid()) return nullptr;

    auto cat_index = static_cast<size_t>(block_id.category);
    if (cat_index >= pools.size() || block_id.size_bucket >= pools[cat_index].size()) return nullptr;

    auto &pool = pools[cat_index][block_id.size_bucket];
    uint32_t index = block_id.pool_index - 1;

    std::lock_guard<std::mutex> lock(pool->bucket_mutex);
    if (index >= pool->blocks.size()) return nullptr;

    return pool->blocks[index].memory_ptr;
}

size_t ResourceManager::getMemorySize(MemoryBlockId block_id) {
    if (!block_id.isValid()) return 0;

    auto cat_index = static_cast<size_t>(block_id.category);
    if (cat_index >= pools.size() || block_id.size_bucket >= pools[cat_index].size()) return 0;

    auto &pool = pools[cat_index][block_id.size_bucket];
    uint32_t index = block_id.pool_index - 1;

    std::lock_guard<std::mutex> lock(pool->bucket_mutex);
    if (index >= pool->blocks.size()) return 0;

    return pool->blocks[index].actual_size;
}

void ResourceManager::waitForAllThreads() {
    std::unique_lock<std::mutex> lock(thread_mutex);
    thread_cv.wait(lock, [this]() {
        return active_threads.load() == 0;
    });
}

uint64_t ResourceManager::getNextTaskId() {
    return next_task_id.fetch_add(1, std::memory_order_relaxed);
}

void ResourceManager::printThreadPerformanceReport(bool detailed) const {
    std::lock_guard<std::mutex> lock(pool_metrics_mutex);

    debugLog("=== Thread Performance Report ===");

    for (const auto& [name, metrics] : pool_metrics) {
        debugLog(std::format("Pool '{}':", name));
        debugLog(std::format("  Threads used: {}", metrics.thread_count));
        debugLog(std::format("  Tasks completed: {}", metrics.total_tasks_completed));
        debugLog(std::format("  Total exec time: {}", formatDuration(metrics.total_execution_time_ns)));
        debugLog(std::format("  Avg exec time: {}", formatDuration(metrics.avg_execution_time_ns)));
        debugLog(std::format("  Max exec time: {}", formatDuration(metrics.max_execution_time_ns)));
        debugLog(std::format("  Min exec time: {}", formatDuration(metrics.min_execution_time_ns)));

        debugLog(std::format("  Total wait time: {}", formatDuration(metrics.total_wait_time_ns)));
        debugLog(std::format("  Avg wait time: {}", formatDuration(metrics.avg_wait_time_ns)));
        debugLog(std::format("  Max wait time: {}", formatDuration(metrics.max_wait_time_ns)));
        debugLog(std::format("  Min wait time: {}", formatDuration(metrics.min_wait_time_ns)));

        if (detailed) {
            debugLog("  [Detailed reporting available]");
        }
    }

    debugLog("==================================");
}

void ResourceManager::updateThreadMetrics(const std::string& pool_name,
                                          uint64_t exec_time_ns,
                                          uint64_t wait_time_ns) {
    std::lock_guard<std::mutex> lock(pool_metrics_mutex);
    auto& metrics = pool_metrics[pool_name];
    metrics.pool_name = pool_name;
    metrics.thread_count++;
    metrics.total_tasks_completed++;
    metrics.total_execution_time_ns += exec_time_ns;
    metrics.total_wait_time_ns += wait_time_ns;

    metrics.avg_execution_time_ns = metrics.total_execution_time_ns / metrics.total_tasks_completed;
    metrics.avg_wait_time_ns = metrics.total_wait_time_ns / metrics.total_tasks_completed;

    metrics.max_execution_time_ns = std::max(metrics.max_execution_time_ns, exec_time_ns);
    metrics.min_execution_time_ns = std::min(metrics.min_execution_time_ns, exec_time_ns);

    metrics.max_wait_time_ns = std::max(metrics.max_wait_time_ns, wait_time_ns);
    metrics.min_wait_time_ns = std::min(metrics.min_wait_time_ns, wait_time_ns);
}

// Manual memory accounting helpers
void ResourceManager::increaseMemory(size_t bytes) {
    if (bytes == 0) return;
    size_t new_val = current_memory_usage.fetch_add(bytes) + bytes;
    // update peak
    size_t prev_peak = peak_memory_usage.load();
    while (new_val > prev_peak && !peak_memory_usage.compare_exchange_weak(prev_peak, new_val)) {}
}

void ResourceManager::decreaseMemory(size_t bytes) {
    if (bytes == 0) return;
    current_memory_usage.fetch_sub(bytes);
}

// Definition of the debugLog method
void ResourceManager::debugLog(const std::string &message) {
    // Check the thread-local flag from LogBufferManager
    if (debug::LogBufferManager::tls_inside_resource_manager_log_append) {
        return; // Avoid recursive logging if we're already in a ResourceManager log append operation
    }

    if (getDebugMode()) {
        debug::LogBufferManager::getInstance().appendTo(
            "ResourceManager",
            message,
            debug::LogContext::Debug);
    }
}
