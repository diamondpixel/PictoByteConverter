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
#include <iostream>
#include "../../Debug/headers/LogMacros.h"
#include "../../Debug/headers/LogBufferManager.h"
#include "../../Threading/headers/ResourceManager.h"

using RM = ResourceManager;

// Spill when memory exceeds this fraction of global max (e.g., 0.8 = 80 %)
constexpr double kSpillThresholdRatio = 0.8;

template<typename T>
SpillableQueue<T>::SpillableQueue(std::string spill_path, std::string queue_name)
    : shutdown_flag_(false),
      enable_spilling_(true),
      current_queue_memory_usage_(0),
      spill_directory_path_(std::move(spill_path)),
      spill_file_id_counter_(0),
      queue_name_(std::move(queue_name)) {
    std::string msg_ctor = "Creating SpillableQueue '" + queue_name_ + "'.";
    LOG_DBG("SpillableQueue", msg_ctor);

    if (!spill_directory_path_.empty()) {
        try {
            if (!std::filesystem::exists(spill_directory_path_)) {
                if (!std::filesystem::create_directories(spill_directory_path_)) {
                    std::string msg_create_dir_fail = "Failed to create spill directory: " + spill_directory_path_;
                    LOG_ERR("SpillableQueue", msg_create_dir_fail);
                }
            }
        } catch (const std::exception &e) {
            std::string msg_create_dir_exc = "Exception creating spill directory: " + std::string(e.what());
            LOG_ERR("SpillableQueue", msg_create_dir_exc);
        }
    }
}

template<typename T>
SpillableQueue<T>::~SpillableQueue() {
    std::string msg_dtor = "Destroying SpillableQueue '" + queue_name_ + "'";
    LOG_DBG("SpillableQueue", msg_dtor);

    SpillableQueue<T>::shutdown();

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

    // Move the queue
    queue_ = std::move(other.queue_);

    // Reset other to a valid but empty state
    other.queue_ = std::queue<T>();
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

        // Move resources from other
        queue_ = std::move(other.queue_);
        current_queue_memory_usage_ = other.current_queue_memory_usage_;
        spill_directory_path_ = std::move(other.spill_directory_path_);
        spill_file_id_counter_ = other.spill_file_id_counter_;
        queue_name_ = std::move(other.queue_name_);
        shutdown_flag_.store(other.shutdown_flag_.load());
        enable_spilling_.store(other.enable_spilling_.load());

        // Reset other to a valid but empty state
        other.queue_ = std::queue<T>();
        other.shutdown_flag_.store(false);
        other.enable_spilling_.store(true);
        other.spill_file_id_counter_ = 0;
        other.current_queue_memory_usage_ = 0;
    }
    return *this;
}

template<typename T>
bool SpillableQueue<T>::push(T &&item) {
    // Use unique_lock so we can wait on the condition variable if necessary
    std::unique_lock<std::mutex> lock(mutex_);

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
    std::string msg_push_check = "Queue '" + queue_name_ + "' push check: item_size=" + std::to_string(item_memory_usage) +
                                 " bytes, RM current=" + std::to_string(RM::getInstance().getCurrentMemoryUsage()) +
                                 " bytes, RM max=" + std::to_string(RM::getInstance().getMaxMemory());
    LOG_DBG("SpillableQueue", msg_push_check);
    // Reject only if single item exceeds global max *and* spilling disabled; otherwise we'll spill immediately
    if (item_memory_usage > RM::getInstance().getMaxMemory() &&
        (spill_directory_path_.empty() || !enable_spilling_)) {
        std::string msg_task_rejected = "Task rejected: item " + std::to_string(item_memory_usage) + " bytes exceeds max memory and spilling disabled";
        LOG_WARN("SpillableQueue", msg_task_rejected);
        if constexpr (std::is_same_v<T, std::unique_ptr<Task>> || std::is_same_v<T, std::unique_ptr<typename T::element_type>>) {
            if (item) item.reset();
        }
        return false;
    }

    // Early spilling: if pushing this item would make global usage exceed
    // kSpillThresholdRatio of the max, spill the task immediately (if
    // spilling is enabled).
    if (((RM::getInstance().getCurrentMemoryUsage() >
          static_cast<size_t>(RM::getInstance().getMaxMemory() * kSpillThresholdRatio))) ||
        (item_memory_usage > RM::getInstance().getMaxMemory()) &&
        !spill_directory_path_.empty() && enable_spilling_) {
        std::ostringstream filename_ss;
        filename_ss << spill_directory_path_ << "/task_" << spill_file_id_counter_++ << ".dat";
        std::string spill_filename = filename_ss.str();

        std::ofstream file(spill_filename, std::ios::binary);

        auto wait_for_memory = [&](size_t extra_mem) {
            cv_.wait(lock, [&]() {
                return shutdown_flag_.load() ||
                       (RM::getInstance().getCurrentMemoryUsage() <=
                        RM::getInstance().getMaxMemory());
            });
        };

        if (!file.is_open()) {
            std::string msg_spill_fail = "Failed to open spill file: " + spill_filename + " for queue: " + queue_name_;
            LOG_ERR("SpillableQueue", msg_spill_fail);
            // Decrement counter as file creation failed before use
            spill_file_id_counter_--;
            wait_for_memory(item_memory_usage);
        } else {
            if constexpr (std::is_same_v<T, std::unique_ptr<Task>> || std::is_same_v<T, std::unique_ptr<typename T::element_type>>) {
                // Serialize through unique_ptr
                if (item && item->serialize(file)) {
                    // after serializing, free memory tracked by ResourceManager inside task destructor
                    file.flush();
                    if (file.good()) {
                        // free heavy buffers but keep task & promise alive
                        item->releaseHeavyResources(spill_filename);
                        size_t light_mem = item->getMemoryUsage();
                        queue_.push(std::move(item)); // re-queue as light task
                        current_queue_memory_usage_ += light_mem;
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
                        item->releaseHeavyResources(spill_filename);
                        size_t light_mem2 = item->getMemoryUsage();
                        queue_.push(std::move(item));
                        current_queue_memory_usage_ += light_mem2;
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
            wait_for_memory(item_memory_usage);
        }
    }

    // If adding this item would breach the global memory cap and we cannot
    // (or are not allowed to) spill, block until memory is freed instead of
    // returning false.  This prevents the ThreadPool from seeing a push
    // failure and breaking the associated promise.
    if ((RM::getInstance().getCurrentMemoryUsage() + item_memory_usage > RM::getInstance().getMaxMemory()) &&
        (spill_directory_path_.empty() || !enable_spilling_)) {
        std::string msg_waiting = "Queue '" + queue_name_ + "' waiting – would exceed max memory and spilling disabled.";
        LOG_DBG("SpillableQueue", msg_waiting);
        cv_.wait(lock, [&]() {
            return shutdown_flag_.load() ||
                   (RM::getInstance().getCurrentMemoryUsage() + item_memory_usage <=
                    RM::getInstance().getMaxMemory());
        });

        if (shutdown_flag_) {
            return false;
        }
    }

    // No spill; keep in-memory
    queue_.push(std::move(item));
    current_queue_memory_usage_ += item_memory_usage;
    cv_.notify_one();
    return true;
}

template<typename T>
bool SpillableQueue<T>::try_push(T &&item) {
    std::unique_lock<std::mutex> lock(mutex_);

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
    std::string msg_push_check = "Queue '" + queue_name_ + "' push check: item_size=" + std::to_string(item_memory_usage) +
                                 " bytes, RM current=" + std::to_string(RM::getInstance().getCurrentMemoryUsage()) +
                                 " bytes, RM max=" + std::to_string(RM::getInstance().getMaxMemory());
    LOG_DBG("SpillableQueue", msg_push_check);
    // Reject only if single item exceeds global max *and* spilling disabled
    if (item_memory_usage > RM::getInstance().getMaxMemory() &&
        (spill_directory_path_.empty() || !enable_spilling_)) {
        std::string msg_task_rejected = "Task rejected: item " + std::to_string(item_memory_usage) + " bytes exceeds max memory and spilling disabled";
        LOG_WARN("SpillableQueue", msg_task_rejected);
        if constexpr (std::is_same_v<T, std::unique_ptr<Task>> || std::is_same_v<T, std::unique_ptr<typename T::element_type>>) {
            if (item) item.reset();
        }
        return false;
    }

    // Early spilling: if pushing this item would make global usage exceed
    // kSpillThresholdRatio of the max, spill the task immediately (if
    // spilling is enabled).
    if (((RM::getInstance().getCurrentMemoryUsage() >
          static_cast<size_t>(RM::getInstance().getMaxMemory() * kSpillThresholdRatio))) ||
        (item_memory_usage > RM::getInstance().getMaxMemory()) &&
        !spill_directory_path_.empty() && enable_spilling_) {
        std::ostringstream filename_ss;
        filename_ss << spill_directory_path_ << "/task_" << spill_file_id_counter_++ << ".dat";
        std::string spill_filename = filename_ss.str();

        std::ofstream file(spill_filename, std::ios::binary);

        auto wait_for_memory = [&](size_t extra_mem) {
            cv_.wait(lock, [&]() {
                return shutdown_flag_.load() ||
                       (RM::getInstance().getCurrentMemoryUsage() <=
                        RM::getInstance().getMaxMemory());
            });
        };

        if (!file.is_open()) {
            std::string msg_spill_fail = "Failed to open spill file: " + spill_filename + " for queue: " + queue_name_;
            LOG_ERR("SpillableQueue", msg_spill_fail);
            // Decrement counter as file creation failed before use
            spill_file_id_counter_--;
            wait_for_memory(item_memory_usage);
        } else {
            if constexpr (std::is_same_v<T, std::unique_ptr<Task>> || std::is_same_v<T, std::unique_ptr<typename T::element_type>>) {
                // Serialize through unique_ptr
                if (item && item->serialize(file)) {
                    // after serializing, free memory tracked by ResourceManager inside task destructor
                    file.flush();
                    if (file.good()) {
                        // free heavy buffers but keep task & promise alive
                        item->releaseHeavyResources(spill_filename);
                        size_t light_mem3 = item->getMemoryUsage();
                        queue_.push(std::move(item)); // re-queue as light task
                        current_queue_memory_usage_ += light_mem3;
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
                        item->releaseHeavyResources(spill_filename);
                        size_t light_mem4 = item->getMemoryUsage();
                        queue_.push(std::move(item));
                        current_queue_memory_usage_ += light_mem4;
                        cv_.notify_one();
                        return true;
                    }
                }
            }

            file.close(); // Close stream before removing file
            try {
                if (std::filesystem::exists(spill_filename)) std::filesystem::remove(spill_filename);
            } catch (...) {}
            wait_for_memory(item_memory_usage);
        }
    }

    if ((RM::getInstance().getCurrentMemoryUsage() + item_memory_usage > RM::getInstance().getMaxMemory()) &&
        (spill_directory_path_.empty() || !enable_spilling_)) {
        // Non-blocking: if would exceed and can't spill, just return false
        std::string msg_waiting_would_exceed = "Queue '" + queue_name_ + "' try_push failed – would exceed max memory and spilling disabled.";
        LOG_DBG("SpillableQueue", msg_waiting_would_exceed);
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
    // ----- Phase 1: wait for an element and remove it from the queue -----
    std::unique_lock<std::mutex> lock(mutex_);

    cv_.wait(lock, [this] {
        return !queue_.empty() || shutdown_flag_;
    });

    if (queue_.empty()) {
        return false; // Queue shut down and empty
    }

    // Move the front element out of the queue so we can release the lock early
    item = std::move(queue_.front());
    queue_.pop();

    // Tentatively deduct the memory of the light representation (we’ll correct later if we reload)
    size_t light_mem = 0;
    if constexpr (std::is_same_v<T, std::unique_ptr<Task>> || std::is_same_v<T, std::unique_ptr<typename T::element_type>>) {
        light_mem = item ? item->getMemoryUsage() : 0;
    } else {
        light_mem = sizeof(T);
        if constexpr (requires(T t) { t.getMemoryUsage(); }) {
            light_mem += item.getMemoryUsage();
        }
    }
    current_queue_memory_usage_ -= light_mem;

    // Unlock to allow other push/pop operations while we potentially touch disk
    lock.unlock();

    // ----- Phase 2: If the task was spilled, reload its heavy resources outside the lock -----
    if (item && item->hasSpillFile()) {
        // Preserve the callable so we can restore it after deserialization
        auto saved_func = std::move(item->func);

        std::string spillFileUtf8 = item->getSpillFile(); // local copy
#ifdef _WIN32
        // Convert to wide path once and hold on stack to avoid buffer lifetime issues
        std::wstring widePath = std::filesystem::path(spillFileUtf8).wstring();
#endif

        {
#ifdef _WIN32
            std::ifstream file_stream(widePath, std::ios::binary);
#else
            std::ifstream file_stream(spillFileUtf8, std::ios::binary);
#endif
            if (file_stream) {
                item->deserialize(file_stream);
            }
        }
        // Attempt to delete the spill file – failure is non-fatal
        {
#ifdef _WIN32
            std::error_code ec_rm;
            std::filesystem::remove(widePath, ec_rm);
#else
            std::error_code ec_rm;
            std::filesystem::remove(spillFileUtf8, ec_rm);
#endif
        }

        if (!item->func && saved_func) {
            item->func = std::move(saved_func);
        }
        item->setSpillFile("");
    }

    // ----- Phase 3: notify potential producers that memory was freed -----
    lock.lock();
    cv_.notify_all();
    lock.unlock();

    return true;
}

template<typename T>
bool SpillableQueue<T>::try_pop(T &item) {
    std::unique_lock<std::mutex> lock(mutex_);

    if (queue_.empty()) {
        return false; // Queue is empty
    }

    if (!queue_.empty()) {
        auto task_ptr = std::move(queue_.front());
        queue_.pop();

        // Copy the task to the output parameter
        item = std::move(task_ptr);

        // If task was spilled, reload data
        if (item && item->hasSpillFile()) {
            std::string spillFileUtf8 = item->getSpillFile(); // local copy
#ifdef _WIN32
            // Convert to wide path once and hold on stack to avoid buffer lifetime issues
            std::wstring widePath = std::filesystem::path(spillFileUtf8).wstring();
#endif

            // Preserve the original callable before deserialization and unlock
            auto saved_func2 = std::move(item->func);
            lock.unlock();
            {
#ifdef _WIN32
                std::ifstream file(widePath, std::ios::binary);
#else
                std::ifstream file(spillFileUtf8, std::ios::binary);
#endif
                if (file) {
                    item->deserialize(file);
                    file.close();
#ifdef _WIN32
                    std::error_code ec_rm;
                    std::filesystem::remove(widePath, ec_rm);
#else
                    std::error_code ec_rm;
                    std::filesystem::remove(spillFileUtf8, ec_rm);
#endif
                }
            }
            lock.lock();
            if (!item->func) {
                item->func = std::move(saved_func2);
            }
            item->setSpillFile("");
        }

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
        // Notify waiting producers that memory has been freed
        cv_.notify_all();
        return true;
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
    std::string msg_shutdown = "SpillableQueue '" + queue_name_ + "' shutting down. Items in memory: " +
        std::to_string(queue_.size());
    LOG_DBG("SpillableQueue", msg_shutdown);
}

template<typename T>
bool SpillableQueue<T>::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
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
    return queue_.size();
}

template<typename T>
size_t SpillableQueue<T>::in_memory_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t spilled_count = 0;
    const auto& cont = queue_._Get_container();
    for (const auto& ptr : cont) {
        if constexpr (std::is_same_v<T, std::unique_ptr<Task>> ||
                      std::is_same_v<T, std::unique_ptr<typename T::element_type>>) {
            if (ptr && ptr->hasSpillFile()) {
                ++spilled_count;
            }
        } else {
            if constexpr (requires(const T &v) { v.hasSpillFile(); }) {
                if (ptr.hasSpillFile()) {
                    ++spilled_count;
                }
            }
        }
    }
    return queue_.size() - spilled_count;
}

template<typename T>
size_t SpillableQueue<T>::spilled_size() const {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t spilled_count = 0;
    // Access the underlying container of std::queue (MSVC: _Get_container)
    const auto& cont = queue_._Get_container();
    for (const auto& ptr : cont) {
        if constexpr (std::is_same_v<T, std::unique_ptr<Task>> ||
                      std::is_same_v<T, std::unique_ptr<typename T::element_type>>) {
            if (ptr && ptr->hasSpillFile()) {
                ++spilled_count;
            }
        } else {
            if constexpr (requires(const T &v) { v.hasSpillFile(); }) {
                if (ptr.hasSpillFile()) {
                    ++spilled_count;
                }
            }
        }
    }
    return spilled_count;
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
