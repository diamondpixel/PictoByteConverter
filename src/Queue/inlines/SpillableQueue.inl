#pragma once

#include <optional>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <functional>
#include <deque>
#include <utility>
#include <memory>
#include <queue>
#include "../../Debug/headers/LogBufferManager.h"
#include "../../Threading/headers/ResourceManager.h"

using RM = ResourceManager;

template<typename T>
SpillableQueue<T>::SpillableQueue(std::string spill_path, std::string queue_name)
    : shutdown_flag_(false),
      enable_spilling_(true),
      current_queue_memory_usage_(0),
      spill_directory_path_(std::move(spill_path)),
      spill_file_id_counter_(0),
      queue_name_(std::move(queue_name)) {
    debug::LogBufferManager::getInstance().appendTo(
        "SpillableQueue",
        "Creating SpillableQueue '" + queue_name_ + "'.",
        debug::LogContext::Debug);

    if (!spill_directory_path_.empty()) {
        try {
            if (!std::filesystem::exists(spill_directory_path_)) {
                if (!std::filesystem::create_directories(spill_directory_path_)) {
                    debug::LogBufferManager::getInstance().appendTo(
                        "SpillableQueue",
                        "Failed to create spill directory: " + spill_directory_path_,
                        debug::LogContext::Error);
                }
            }
        } catch (const std::exception &e) {
            debug::LogBufferManager::getInstance().appendTo(
                "SpillableQueue",
                "Exception creating spill directory: " + std::string(e.what()),
                debug::LogContext::Error);
        }
    }
}

template<typename T>
SpillableQueue<T>::~SpillableQueue() {
    debug::LogBufferManager::getInstance().appendTo(
        "SpillableQueue",
        "Destroying SpillableQueue '" + queue_name_ + "'",
        debug::LogContext::Debug);

    SpillableQueue<T>::shutdown();

    // Clean up any spilled files
    std::lock_guard<std::mutex> lock(mutex_);
    while (!spilled_task_files_.empty()) {
        std::string filename = spilled_task_files_.front();
        spilled_task_files_.pop();
        try {
            if (std::filesystem::exists(filename)) {
                std::filesystem::remove(filename);
            }
        } catch (...) {
            // Ignore errors during cleanup
        }
    }
}

template<typename T>
SpillableQueue<T>::SpillableQueue(SpillableQueue<T> &&other) noexcept
    : shutdown_flag_(other.shutdown_flag_.load()),
      enable_spilling_(other.enable_spilling_.load()),
      current_queue_memory_usage_(other.current_queue_memory_usage_),
      spill_directory_path_(std::move(other.spill_directory_path_)),
      spill_file_id_counter_(other.spill_file_id_counter_),
      queue_name_(std::move(other.queue_name_)) {
    std::lock_guard<std::mutex> lock(other.mutex_);

    // Move the queues
    queue_ = std::move(other.queue_);
    spilled_task_files_ = std::move(other.spilled_task_files_);

    // Reset other to a valid but empty state
    other.queue_ = std::queue<T>();
    other.spilled_task_files_ = std::queue<std::string>();
    other.shutdown_flag_.store(false);
    other.enable_spilling_.store(true);
    other.spill_file_id_counter_ = 0;
    other.current_queue_memory_usage_ = 0;
}

template<typename T>
SpillableQueue<T> &SpillableQueue<T>::operator=(SpillableQueue<T> &&other) noexcept {
    if (this != &other) {
        // Acquire locks for both queues to prevent race conditions
        std::lock(mutex_, other.mutex_);
        std::lock_guard<std::mutex> lock_this(mutex_, std::adopt_lock);
        std::lock_guard<std::mutex> lock_other(other.mutex_, std::adopt_lock);

        // Clean up current resources
        while (!queue_.empty()) {
            auto item = std::move(queue_.front());
            queue_.pop();

            // Destroy the task
            item.reset();
        }

        // Clean up spill files
        while (!spilled_task_files_.empty()) {
            std::string file = spilled_task_files_.front();
            spilled_task_files_.pop();
            try {
                if (std::filesystem::exists(file)) std::filesystem::remove(file);
            } catch (...) {
                // Ignore file removal errors
            }
        }

        // Move resources from other
        queue_ = std::move(other.queue_);
        spilled_task_files_ = std::move(other.spilled_task_files_);
        current_queue_memory_usage_ = other.current_queue_memory_usage_;
        spill_directory_path_ = std::move(other.spill_directory_path_);
        spill_file_id_counter_ = other.spill_file_id_counter_;
        queue_name_ = std::move(other.queue_name_);
        shutdown_flag_.store(other.shutdown_flag_.load());
        enable_spilling_.store(other.enable_spilling_.load());

        // Reset other to a valid but empty state
        other.queue_ = std::queue<T>();
        other.spilled_task_files_ = std::queue<std::string>();
        other.shutdown_flag_.store(false);
        other.enable_spilling_.store(true);
        other.spill_file_id_counter_ = 0;
        other.current_queue_memory_usage_ = 0;
    }
    return *this;
}

template<typename T>
bool SpillableQueue<T>::push(T &&item) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (shutdown_flag_) return false;

    // Calculate memory usage (pointer-aware)
    size_t item_memory_usage = 0;
    if constexpr (std::is_same_v<T, std::unique_ptr<Task>> || std::is_same_v<T, std::unique_ptr<typename T::element_type>>) {
        item_memory_usage = item ? item->getMemoryUsage() : 0;
    } else {
        item_memory_usage = sizeof(T);
        if constexpr (requires(T t) { t.getMemoryUsage(); }) {
            item_memory_usage += item.getMemoryUsage();
        }
    }

    // Debug log actual item size and memory state
    debug::LogBufferManager::getInstance().appendTo(
        "SpillableQueue",
        "Queue '" + queue_name_ + "' push check: item_size=" + std::to_string(item_memory_usage) +
            " bytes, RM current=" + std::to_string(RM::getInstance().getCurrentMemoryUsage()) +
            " bytes, RM max=" + std::to_string(RM::getInstance().getMaxMemory()),
        debug::LogContext::Debug);

    // Console debug output as well for immediate visibility
    std::cout << "[DEBUG][SpillableQueue] '" << queue_name_ << "' push: item_size=" << item_memory_usage
              << " bytes, RM current=" << RM::getInstance().getCurrentMemoryUsage()
              << " bytes, RM max=" << RM::getInstance().getMaxMemory() << " bytes" << std::endl;

    // Reject if single item exceeds global max memory (can't ever fit)
    if (item_memory_usage > RM::getInstance().getMaxMemory()) {
        debug::LogBufferManager::getInstance().appendTo(
            "SpillableQueue",
            "Task rejected: memory " + std::to_string(item_memory_usage) + " > max " +
                std::to_string(RM::getInstance().getMaxMemory()),
            debug::LogContext::Warning);
        if constexpr (std::is_same_v<T, std::unique_ptr<Task>> || std::is_same_v<T, std::unique_ptr<typename T::element_type>>) {
            if (item) item.reset();
        }
        return false;
    }

    // Check global memory usage; spill if limit exceeded
    if ((RM::getInstance().getCurrentMemoryUsage() + item_memory_usage > RM::getInstance().getMaxMemory()) &&
        !spill_directory_path_.empty() && enable_spilling_) {
        std::ostringstream filename_ss;
        filename_ss << spill_directory_path_ << "/task_" << spill_file_id_counter_++ << ".dat";
        std::string spill_filename = filename_ss.str();

        std::ofstream file(spill_filename, std::ios::binary);

        if (!file.is_open()) {
            debug::LogBufferManager::getInstance().appendTo(
                "SpillableQueue",
                "Failed to open spill file: " + spill_filename + " for queue: " + queue_name_,
                debug::LogContext::Error);
            // Decrement counter as file creation failed before use
            spill_file_id_counter_--;
            return false;
        }

        if constexpr (std::is_same_v<T, std::unique_ptr<Task>> || std::is_same_v<T, std::unique_ptr<typename T::element_type>>) {
            // Serialize through unique_ptr
            if (item && item->serialize(file)) {
                // after serializing, free memory tracked by ResourceManager inside task destructor
                file.flush();
                if (file.good()) {
                    // free heavy buffers but keep task object & promise alive
                    item->releaseHeavyResources();
                    spilled_task_files_.push(spill_filename);
                    cv_.notify_one();
                    return true;
                }
            }
        } else if constexpr (requires(T t, std::ofstream &f) { t.serialize(f); }) {
            if (item.serialize(file)) {
                file.flush(); // Explicitly flush before checking good() and closing
                if (file.good()) {
                    // Serialization and flush successful
                    // file will be closed by its destructor upon exiting this scope normally
                    spilled_task_files_.push(spill_filename);
                    cv_.notify_one();
                    return true;
                }
            }
        }

        file.close(); // Close stream before removing file
        try {
            if (std::filesystem::exists(spill_filename)) std::filesystem::remove(spill_filename);
        } catch (...) {
            /* ignore cleanup error */
        }
        return false;
    }

    // No spill; keep in-memory
    queue_.push(std::move(item));
    current_queue_memory_usage_ += item_memory_usage;
    cv_.notify_one();
    return true;
}

template<typename T>
bool SpillableQueue<T>::try_push(T &&item) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Don't push if shutting down
    if (shutdown_flag_) return false;

    // Calculate memory usage (pointer-aware)
    size_t item_memory_usage = 0;
    if constexpr (std::is_same_v<T, std::unique_ptr<Task>> || std::is_same_v<T, std::unique_ptr<typename T::element_type>>) {
        item_memory_usage = item ? item->getMemoryUsage() : 0;
    } else {
        item_memory_usage = sizeof(T);
        if constexpr (requires(T t) { t.getMemoryUsage(); }) {
            item_memory_usage += item.getMemoryUsage();
        }
    }

    // Debug log actual item size and memory state
    debug::LogBufferManager::getInstance().appendTo(
        "SpillableQueue",
        "Queue '" + queue_name_ + "' push check: item_size=" + std::to_string(item_memory_usage) +
            " bytes, RM current=" + std::to_string(RM::getInstance().getCurrentMemoryUsage()) +
            " bytes, RM max=" + std::to_string(RM::getInstance().getMaxMemory()),
        debug::LogContext::Debug);

    // Console debug output
    std::cout << "[DEBUG][SpillableQueue] '" << queue_name_ << "' try_push: item_size=" << item_memory_usage
              << " bytes, RM current=" << RM::getInstance().getCurrentMemoryUsage()
              << " bytes, RM max=" << RM::getInstance().getMaxMemory() << " bytes" << std::endl;

    // Reject if single item exceeds global max memory (can't ever fit)
    if (item_memory_usage > RM::getInstance().getMaxMemory()) {
        debug::LogBufferManager::getInstance().appendTo(
            "SpillableQueue",
            "Task rejected: memory " + std::to_string(item_memory_usage) + " > max " +
                std::to_string(RM::getInstance().getMaxMemory()),
            debug::LogContext::Warning);
        if constexpr (std::is_same_v<T, std::unique_ptr<Task>> || std::is_same_v<T, std::unique_ptr<typename T::element_type>>) {
            if (item) item.reset();
        }
        return false;
    }

    // Check global memory usage; spill if limit exceeded
    if ((RM::getInstance().getCurrentMemoryUsage() + item_memory_usage > RM::getInstance().getMaxMemory()) &&
        !spill_directory_path_.empty() && enable_spilling_) {
        std::ostringstream filename_ss;
        filename_ss << spill_directory_path_ << "/task_" << spill_file_id_counter_++ << ".dat";
        std::string spill_filename = filename_ss.str();

        std::ofstream file(spill_filename, std::ios::binary);

        if (!file.is_open()) {
            debug::LogBufferManager::getInstance().appendTo(
                "SpillableQueue",
                "Failed to open spill file: " + spill_filename + " for queue: " + queue_name_,
                debug::LogContext::Error);
            // Decrement counter as file creation failed before use
            spill_file_id_counter_--;
            return false;
        }

        if constexpr (std::is_same_v<T, std::unique_ptr<Task>> || std::is_same_v<T, std::unique_ptr<typename T::element_type>>) {
            // Serialize through unique_ptr
            if (item && item->serialize(file)) {
                // after serializing, free memory tracked by ResourceManager inside task destructor
                file.flush();
                if (file.good()) {
                    // free heavy buffers but keep task object & promise alive
                    item->releaseHeavyResources();
                    spilled_task_files_.push(spill_filename);
                    cv_.notify_one();
                    return true;
                }
            }
        } else if constexpr (requires(T t, std::ofstream &f) { t.serialize(f); }) {
            if (item.serialize(file)) {
                file.flush(); // Explicitly flush before checking good() and closing
                if (file.good()) {
                    // Serialization and flush successful
                    // file will be closed by its destructor upon exiting this scope normally
                    spilled_task_files_.push(spill_filename);
                    cv_.notify_one();
                    return true;
                }
            }
        }

        file.close(); // Close stream before removing file
        try {
            if (std::filesystem::exists(spill_filename)) std::filesystem::remove(spill_filename);
        } catch (...) {
            /* ignore cleanup error */
        }
        return false;
    }

    // No spill; keep in-memory
    queue_.push(std::move(item));
    current_queue_memory_usage_ += item_memory_usage;
    cv_.notify_one();
    return true;
}

template<typename T>
bool SpillableQueue<T>::pop(T &item) {
    std::unique_lock<std::mutex> lock(mutex_);

    // Wait until queue has items or is shutting down
    cv_.wait(lock, [this] {
        return !queue_.empty() || !spilled_task_files_.empty() || shutdown_flag_;
    });

    if (queue_.empty() && spilled_task_files_.empty()) {
        return false; // Queue is empty and shutting down
    }

    if (!queue_.empty()) {
        auto task_ptr = std::move(queue_.front());
        queue_.pop();

        // Copy the task to the output parameter
        item = std::move(task_ptr);

        // Destroy the task
        task_ptr.reset(); // unique_ptr destructor

        // Update memory usage
        size_t item_mem_usage = 0;
        if constexpr (std::is_same_v<T, std::unique_ptr<Task>> || std::is_same_v<T, std::unique_ptr<typename T::element_type>>) {
            item_mem_usage = item ? item->getMemoryUsage() : 0;
        } else {
            item_mem_usage = sizeof(T);
            if constexpr (requires(T t) { t.getMemoryUsage(); }) {
                item_mem_usage += item.getMemoryUsage();
            }
        }
        current_queue_memory_usage_ -= item_mem_usage;
        return true;
    }

    // If in-memory queue is empty, try to load from spilled files
    if (!spilled_task_files_.empty() && !spill_directory_path_.empty()) {
        std::string filename = spilled_task_files_.front();
        spilled_task_files_.pop();

        lock.unlock(); // Unlock while doing file I/O

        std::ifstream file(filename, std::ios::binary);

        bool success = false;
        if constexpr (std::is_same_v<T, std::unique_ptr<Task>> || std::is_same_v<T, std::unique_ptr<typename T::element_type>>) {
            auto new_task = std::make_unique<typename T::element_type>();
            success = file && new_task->deserialize(file);
            if (success) item = std::move(new_task);
        } else if constexpr (requires(T t, std::ifstream &f) { t.deserialize(f); }) {
            success = file && item.deserialize(file);
        }
        file.close();

        try {
            if (std::filesystem::exists(filename)) std::filesystem::remove(filename);
        } catch (...) {
            // Ignore file removal errors
        }

        return success;
    }

    return false; // Should not reach here
}

template<typename T>
bool SpillableQueue<T>::try_pop(T &item) {
    std::unique_lock<std::mutex> lock(mutex_);

    if (queue_.empty() && spilled_task_files_.empty()) {
        return false; // Queue is empty
    }

    if (!queue_.empty()) {
        auto task_ptr = std::move(queue_.front());
        queue_.pop();

        // Copy the task to the output parameter
        item = std::move(task_ptr);

        // Destroy the task
        task_ptr.reset(); // unique_ptr destructor

        // Update memory usage
        size_t item_mem_usage2 = 0;
        if constexpr (std::is_same_v<T, std::unique_ptr<Task>> || std::is_same_v<T, std::unique_ptr<typename T::element_type>>) {
            item_mem_usage2 = item ? item->getMemoryUsage() : 0;
        } else {
            item_mem_usage2 = sizeof(T);
            if constexpr (requires(T t) { t.getMemoryUsage(); }) {
                item_mem_usage2 += item.getMemoryUsage();
            }
        }
        current_queue_memory_usage_ -= item_mem_usage2;
        return true;
    }

    // If in-memory queue is empty, try to load from spilled files
    if (!spilled_task_files_.empty() && !spill_directory_path_.empty()) {
        std::string filename = spilled_task_files_.front();
        spilled_task_files_.pop();

        lock.unlock(); // Unlock while doing file I/O

        std::ifstream file(filename, std::ios::binary);

        bool success = false;
        if constexpr (std::is_same_v<T, std::unique_ptr<Task>> || std::is_same_v<T, std::unique_ptr<typename T::element_type>>) {
            auto new_task = std::make_unique<typename T::element_type>();
            success = file && new_task->deserialize(file);
            if (success) item = std::move(new_task);
        } else if constexpr (requires(T t, std::ifstream &f) { t.deserialize(f); }) {
            success = file && item.deserialize(file);
        }
        file.close();

        try {
            if (std::filesystem::exists(filename)) std::filesystem::remove(filename);
        } catch (...) {
            // Ignore file removal errors
        }

        return success;
    }

    return false; // Should not reach here
}

template<typename T>
void SpillableQueue<T>::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_flag_ = true;

    // Important: notify all waiting threads to check the shutdown flag
    cv_.notify_all();

    // Log shutdown for debugging
    debug::LogBufferManager::getInstance().appendTo(
        "SpillableQueue",
        "SpillableQueue '" + queue_name_ + "' shutting down. Items in memory: " +
        std::to_string(queue_.size()) + ", spilled: " + std::to_string(spilled_task_files_.size()),
        debug::LogContext::Debug);
}

template<typename T>
bool SpillableQueue<T>::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty() && spilled_task_files_.empty();
}

template<typename T>
bool SpillableQueue<T>::is_shutting_down() const {
    return shutdown_flag_;
}

template<typename T>
const std::string &SpillableQueue<T>::name() const {
    return queue_name_;
}

template<typename T>
size_t SpillableQueue<T>::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size() + spilled_task_files_.size();
}

template<typename T>
size_t SpillableQueue<T>::in_memory_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

template<typename T>
size_t SpillableQueue<T>::spilled_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return spilled_task_files_.size();
}

template<typename T>
size_t SpillableQueue<T>::getMemoryUsage() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Base memory usage for the queue structure
    size_t memory_usage = sizeof(SpillableQueue<T>);

    // Add memory for the queue name string
    memory_usage += queue_name_.capacity() * sizeof(char);

    // Add memory for the spill directory path string
    memory_usage += spill_directory_path_.capacity() * sizeof(char);

    // Add current tracked memory of items (maintained by push/pop)
    memory_usage += current_queue_memory_usage_;

    return memory_usage;
}
