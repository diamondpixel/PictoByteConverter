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
        printMessage("ResourceManager: Max threads set to " + std::to_string(max_threads));
    }

    /**
     * Set the maximum memory usage in bytes
     */
    void setMaxMemory(size_t memory_bytes) {
        max_memory_usage = (memory_bytes > 1024 * 1024) ? memory_bytes : 1024 * 1024; // Min 1MB
        printMessage("ResourceManager: Max memory set to " + 
                     std::to_string(max_memory_usage / (1024 * 1024)) + " MB");
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
        printStatus(status);
        
        // Print detailed report if enabled
        if (detailed_tracking_enabled && !memory_allocations.empty()) {
            printMessage("Detailed memory allocations:");
            for (const auto& [id, info] : memory_allocations) {
                auto now = std::chrono::system_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - info.timestamp).count();
                
                std::string detail = "  ID: " + std::to_string(id) + 
                                    " Size: " + formatMemorySize(info.size) +
                                    " Age: " + std::to_string(duration) + "s" +
                                    (!info.tag.empty() ? " Tag: " + info.tag : "");
                printStatus(detail);
            }
        }
    }

    /**
     * Acquire memory resources, waiting if necessary
     * Returns true if memory was allocated, false if not
     * @param bytes Number of bytes to allocate
     * @param timeout Time to wait for allocation
     * @param tag Optional tag to identify this allocation
     */
    bool allocateMemory(size_t bytes, 
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
                       const std::string& tag = "") {
        std::unique_lock<std::mutex> lock(memory_mutex);
        
        // Wait until memory is available or timeout
        bool success = memory_cv.wait_for(lock, timeout, [this, bytes]() {
            return (current_memory_usage + bytes) <= max_memory_usage;
        });
        
        if (success) {
            // Allocate memory
            current_memory_usage += bytes;
            
            // Record allocation if tracking is enabled
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
                printStatus(message);
            }
            return true;
        }
        
        printWarning("Memory allocation of " + formatMemorySize(bytes) + 
                   " timed out. Current usage: " + formatMemorySize(current_memory_usage) + 
                   " of " + formatMemorySize(max_memory_usage));
        return false;
    }

    /**
     * Free memory resources
     * @param bytes Number of bytes to free
     * @param tag Optional tag to identify which allocation this is
     */
    void freeMemory(size_t bytes, const std::string& tag = "") {
        size_t adjusted_bytes = bytes;
        
        {
            std::lock_guard<std::mutex> lock(memory_mutex);
            
            // Check for underflow
            if (bytes > current_memory_usage) {
                adjusted_bytes = current_memory_usage;
                printWarning("Attempted to free more memory than allocated: " + 
                           formatMemorySize(bytes) + " > " + formatMemorySize(current_memory_usage));
            }
            
            // Free memory
            current_memory_usage -= adjusted_bytes;
        }
        
        // Handle tracking
        if (memory_tracking_enabled) {
            std::lock_guard<std::mutex> tracking_lock(tracking_mutex);
            
            // Find and remove allocation by tag
            if (!tag.empty()) {
                auto it = std::find_if(memory_allocations.begin(), memory_allocations.end(),
                                      [&tag](const auto& pair) {
                                          return pair.second.tag == tag;
                                      });
                
                if (it != memory_allocations.end()) {
                    memory_allocations.erase(it);
                }
            } else {
                // Just remove the first matching allocation by size
                auto it = std::find_if(memory_allocations.begin(), memory_allocations.end(),
                                      [bytes](const auto& pair) {
                                          return pair.second.size == bytes;
                                      });
                
                if (it != memory_allocations.end()) {
                    memory_allocations.erase(it);
                }
            }
            
            // Log deallocation
            std::string message = "Memory freed: " + formatMemorySize(adjusted_bytes);
            if (!tag.empty()) {
                message += " from " + tag;
            }
            message += " (Remaining: " + formatMemorySize(current_memory_usage) + 
                      ", " + std::to_string(memory_allocations.size()) + " allocations)";
            printStatus(message);
        }
        
        // Notify waiting threads
        memory_cv.notify_one();
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
                printError("Thread exception: " + std::string(e.what()));
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
