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
#include <sstream>
#include <iomanip>
#include "../../Debug/headers/Debug.h"

/**
 * A class to handle thread and memory limitations.
 * This singleton class provides the ability to:
 * 1. Limit the number of threads used by the program
 * 2. Track and limit memory usage across operations
 * 3. Wait for resource availability
 */
class ResourceManager {
private:
    // Singleton instance
    static inline ResourceManager* instance = nullptr;
    static inline std::mutex singleton_mutex;

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

    // Private constructor for singleton
    ResourceManager() : 
        max_threads(std::thread::hardware_concurrency()),
        max_memory_usage(1024 * 1024 * 1024) // Default 1GB
    {}
    
    // Helper method to format memory size to human-readable format
    std::string formatMemorySize(size_t bytes) const {
        std::ostringstream oss;
        if (bytes < 1024) {
            oss << bytes << " B";
        } else if (bytes < 1024 * 1024) {
            oss << std::fixed << std::setprecision(2) << (bytes / 1024.0) << " KB";
        } else if (bytes < 1024 * 1024 * 1024) {
            oss << std::fixed << std::setprecision(2) << (bytes / (1024.0 * 1024.0)) << " MB";
        } else {
            oss << std::fixed << std::setprecision(2) << (bytes / (1024.0 * 1024.0 * 1024.0)) << " GB";
        }
        return oss.str();
    }
    
    // Helper method to print debug messages
    void debugLog(const std::string& message) {
        if (getDebugMode()) {
            printMessage(message, true);
        }
    }

public:
    /**
     * Get the singleton instance of ResourceManager
     */
    static ResourceManager& getInstance() {
        std::lock_guard<std::mutex> lock(singleton_mutex);
        if (!instance) {
            instance = new ResourceManager();
        }
        return *instance;
    }

    /**
     * Set the maximum number of threads
     */
    void setMaxThreads(size_t threads) {
        max_threads = (threads > 0) ? threads : 1;
        debugLog("ResourceManager: Max threads set to " + std::to_string(threads));
    }

    /**
     * Set the maximum memory usage in bytes
     */
    void setMaxMemory(size_t memory_bytes) {
        max_memory_usage = (memory_bytes > 1024 * 1024) ? memory_bytes : 1024 * 1024; // Min 1MB
        debugLog("ResourceManager: Max memory set to " + std::to_string(max_memory_usage / (1024 * 1024)) + " MB");
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
        return current_memory_usage;
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
        return active_threads;
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
        
        for (const auto& [id, info] : memory_allocations) {
            total_allocated += info.size;
        }
        
        // Print summary
        std::string status = "Memory Status: Using " + formatMemorySize(current_memory_usage) +
                            " of " + formatMemorySize(max_memory_usage) + 
                            " (" + std::to_string(num_allocations) + " active allocations)";
        
        debugLog(status);
        
        // Print detailed report if enabled
        if (detailed_tracking_enabled && !memory_allocations.empty() && getDebugMode()) {
            debugLog("Detailed memory allocations:");
            
            for (const auto& [id, info] : memory_allocations) {
                auto now = std::chrono::system_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - info.timestamp).count();
                
                std::string detail = "  ID: " + std::to_string(id) + 
                                    " Size: " + formatMemorySize(info.size) +
                                    " Age: " + std::to_string(duration) + "s" +
                                    (!info.tag.empty() ? " Tag: " + info.tag : "");
                
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
    uintptr_t getPooledMemory(size_t bytes, const std::string& tag = "") {
        std::unique_lock<std::mutex> pool_lock(pool_mutex);
        
        // First, try to find a suitable unused block in the pool
        for (size_t i = 0; i < memory_pool.size(); i++) {
            auto& entry = memory_pool[i];
            if (!entry.in_use && entry.size >= bytes) {
                // Found a usable block that's big enough
                entry.in_use = true;
                entry.tag = tag;
                entry.timestamp = std::chrono::system_clock::now();
                
                // Log reuse
                std::string message = "Reused memory pool block: " + formatMemorySize(entry.size) + 
                                     " for " + tag + " (requested " + formatMemorySize(bytes) + ")";
                debugLog(message);
                
                return static_cast<uintptr_t>(i) + 1; // Add 1 to avoid returning 0
            }
        }
        
        // No suitable block found, need to allocate new memory
        std::unique_lock<std::mutex> lock(memory_mutex);
        
        // Wait until memory is available
        bool success = memory_cv.wait_for(lock, std::chrono::milliseconds(5000), [this, bytes]() {
            return (current_memory_usage + bytes) <= max_memory_usage;
        });
        
        if (!success) {
            debugLog("Memory allocation of " + formatMemorySize(bytes) + " timed out");
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
            std::lock_guard<std::mutex> tracking_lock(tracking_mutex);
            uintptr_t alloc_id = ++allocation_id_counter;
            memory_allocations[alloc_id] = {
                bytes,
                tag,
                std::chrono::system_clock::now()
            };
            
            // Log allocation
            std::string message = "Memory allocated: " + formatMemorySize(bytes);
            if (!tag.empty()) {
                message += " for " + tag;
            }
            message += " (Total: " + formatMemorySize(current_memory_usage) + 
                      ", " + std::to_string(memory_allocations.size()) + " allocations)";
            
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
            debugLog("Invalid pool ID: " + std::to_string(pool_id));
            return;
        }
        
        // Mark the block as unused
        auto& entry = memory_pool[index];
        if (!entry.in_use) {
            debugLog("Memory block already released: " + std::to_string(pool_id));
            return;
        }
        
        entry.in_use = false;
        
        std::string message = "Released memory to pool: " + formatMemorySize(entry.size);
        if (!entry.tag.empty()) {
            message += " from " + entry.tag;
        }
        debugLog(message);
    }

    /**
     * For backward compatibility: Existing code can still use allocateMemory/freeMemory,
     * but behind the scenes we'll use the pooling mechanism
     */
    bool allocateMemory(size_t bytes, 
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
                       const std::string& tag = "") {
        uintptr_t pool_id = getPooledMemory(bytes, tag);
        return (pool_id != 0);
    }

    /**
     * For backward compatibility: Release memory, but don't actually free it - return to pool
     */
    void freeMemory(size_t bytes, const std::string& tag = "") {
        // Find the allocation in the pool by tag or size
        std::lock_guard<std::mutex> pool_lock(pool_mutex);
        
        for (size_t i = 0; i < memory_pool.size(); i++) {
            auto& entry = memory_pool[i];
            if (entry.in_use && 
                ((!tag.empty() && entry.tag == tag) || 
                 (tag.empty() && entry.size == bytes))) {
                
                // Mark as available in the pool
                entry.in_use = false;
                
                std::string message = "Released memory to pool: " + formatMemorySize(entry.size);
                if (!entry.tag.empty()) {
                    message += " from " + entry.tag;
                }
                debugLog(message);
                
                return;
            }
        }
        
        // If we get here, we didn't find the allocation in the pool
        // This shouldn't happen with properly paired allocate/free calls
        debugLog("Warning: Could not find memory allocation to release: " + 
                  formatMemorySize(bytes) + (tag.empty() ? "" : " tag: " + tag));
    }

    /**
     * Execute a function with a thread from the pool, waiting if necessary
     * @param fn Function to execute
     * @param timeout Time to wait for thread availability
     * @return True if the thread was created, false otherwise
     */
    template<typename Func, typename... Args>
    bool runWithThread(Func&& func, Args&&... args) {
        std::unique_lock<std::mutex> lock(thread_mutex);
        
        // Wait until a thread slot is available
        thread_cv.wait(lock, [this]() {
            return active_threads < max_threads;
        });
        
        // Acquire a thread slot
        active_threads++;
        
        // Release the lock before starting the thread
        lock.unlock();
        
        // Create the thread with a wrapper that will decrement active_threads
        std::thread worker([this, f = std::forward<Func>(func), args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            try {
                // Call the function with the arguments
                std::apply(f, args);
            } catch (const std::exception& e) {
                debugLog("Thread exception: " + std::string(e.what()));
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
            return active_threads == 0;
        });
    }

    // Cleanup
    ~ResourceManager() {
        waitForAllThreads();
    }
};
