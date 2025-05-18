#ifndef SPILLABLE_QUEUE_H
#define SPILLABLE_QUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <functional>
#include <deque>
#include "ResourceManager.h"
#include "../../Debug/headers/LogBufferManager.h"


/**
 * @brief Thread-safe queue with disk spilling capability
 *
 * This class provides a thread-safe queue that can spill items to disk
 * when the in-memory queue reaches a certain size. Features:
 * - Thread-safe operations
 * - Configurable maximum in-memory items
 * - Automatic disk spilling when queue is full
 * - Serialization/deserialization of items to/from disk
 * - Graceful shutdown
 * - Integration with ResourceManager for memory tracking
 *
 * @tparam T Type of items in the queue (must support serialize/deserialize methods)
 */
template<typename T>
class SpillableQueue {
public:
    /**
     * @brief Construct a new SpillableQueue
     *
     * @param max_memory_items Maximum number of items to keep in memory (default: 1000)
     * @param spill_path Directory path for spill files (default: empty, disables spilling)
     * @param queue_name Name identifier for this queue (for debugging)
     */
    explicit SpillableQueue(size_t max_memory_items = 1000,
                            const std::string &spill_path = "",
                            const std::string &queue_name = "DefaultSpillableQueue")
        : max_in_memory_items_(max_memory_items),
          spill_directory_path_(spill_path),
          spill_file_id_counter_(0),
          queue_name_(queue_name),
          shutdown_flag_(false) {
        debug::LogBufferManager::getInstance().appendTo(
            "SpillableQueue",
            "Creating SpillableQueue '" + queue_name_ + "' with max in-memory items: " +
            std::to_string(max_memory_items),
            debug::LogContext::Debug);

        if (!spill_directory_path_.empty()) {
            try {
                if (!std::filesystem::exists(spill_directory_path_)) {
                    if (!std::filesystem::create_directories(spill_directory_path_)) {
                        debug::LogBufferManager::getInstance().appendTo(
                            "SpillableQueue",
                            "SpillableQueue '" + queue_name_ + "': Failed to create spill directory: " +
                            spill_directory_path_,
                            debug::LogContext::Error);
                        spill_directory_path_.clear();
                    }
                } else if (!std::filesystem::is_directory(spill_directory_path_)) {
                    debug::LogBufferManager::getInstance().appendTo(
                        "SpillableQueue",
                        "SpillableQueue '" + queue_name_ + "': Spill path exists but is not a directory: " +
                        spill_directory_path_,
                        debug::LogContext::Error);
                    spill_directory_path_.clear();
                }
            } catch (const std::filesystem::filesystem_error &e) {
                debug::LogBufferManager::getInstance().appendTo(
                    "SpillableQueue",
                    "SpillableQueue '" + queue_name_ + "': Filesystem error with spill directory '" +
                    spill_directory_path_ + "': " + e.what(),
                    debug::LogContext::Error);
                spill_directory_path_.clear();
            }
        }
    }

    /**
     * @brief Destructor ensures proper cleanup of spill files
     */
    ~SpillableQueue() {
        shutdown();
        if (!spill_directory_path_.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            while (!spilled_task_files_.empty()) {
                std::string file_to_delete = spilled_task_files_.front();
                spilled_task_files_.pop();
                try {
                    if (std::filesystem::exists(file_to_delete)) {
                        std::filesystem::remove(file_to_delete);
                    }
                } catch (const std::filesystem::filesystem_error &e) {
                    debug::LogBufferManager::getInstance().appendTo(
                        "SpillableQueue",
                        "SpillableQueue '" + queue_name_ + "': Error deleting spill file: " + e.what(),
                        debug::LogContext::Warning);
                }
            }
        }

        // Log destruction
        debug::LogBufferManager::getInstance().appendTo(
            "SpillableQueue",
            "SpillableQueue '" + queue_name_ + "' destroyed",
            debug::LogContext::Debug);
    }

    // Disable copying
    SpillableQueue(const SpillableQueue &) = delete;

    SpillableQueue &operator=(const SpillableQueue &) = delete;

    // Allow moving
    SpillableQueue(SpillableQueue &&other) noexcept {
        std::lock_guard<std::mutex> lock_other(other.mutex_);
        queue_ = std::move(other.queue_);
        spilled_task_files_ = std::move(other.spilled_task_files_);
        max_in_memory_items_ = other.max_in_memory_items_;
        spill_directory_path_ = std::move(other.spill_directory_path_);
        spill_file_id_counter_ = other.spill_file_id_counter_;
        queue_name_ = std::move(other.queue_name_);
        shutdown_flag_.store(other.shutdown_flag_.load(std::memory_order_acquire));
    }

    SpillableQueue &operator=(SpillableQueue &&other) noexcept {
        if (this != &other) {
            std::scoped_lock lock(mutex_, other.mutex_);
            queue_ = std::move(other.queue_);
            spilled_task_files_ = std::move(other.spilled_task_files_);
            max_in_memory_items_ = other.max_in_memory_items_;
            spill_directory_path_ = std::move(other.spill_directory_path_);
            spill_file_id_counter_ = other.spill_file_id_counter_;
            queue_name_ = std::move(other.queue_name_);
            shutdown_flag_.store(other.shutdown_flag_.load(std::memory_order_acquire));
        }
        return *this;
    }

    /**
     * @brief Add an item to the queue
     *
     * If the in-memory queue is full and spill_path is set, the item will be
     * serialized to disk. Otherwise, it will be added to the in-memory queue.
     *
     * @param item Item to add
     * @return true if item was added, false if queue is shutting down
     */
    bool push(const T &item) {
        std::optional<T> item_to_serialize_optional;
        std::optional<std::string> spill_filename_optional; {
            // Scope for the first lock
            std::lock_guard<std::mutex> lock(mutex_);
            if (shutdown_flag_) {
                debug::LogBufferManager::getInstance().appendTo(
                    "SpillableQueue",
                    "SpillableQueue '" + queue_name_ + "': Attempted to push to a shutting down queue.",
                    debug::LogContext::Warning);
                return false;
            }

            if (queue_.size() >= max_in_memory_items_ && !spill_directory_path_.empty()) {
                std::ostringstream filename_stream;
                // Increment counter under lock
                filename_stream << spill_directory_path_ << "/task_" << spill_file_id_counter_++ << ".dat";
                spill_filename_optional = filename_stream.str();
                item_to_serialize_optional = item; // Make a copy for serialization outside lock

                debug::LogBufferManager::getInstance().appendTo(
                    "SpillableQueue",
                    "SpillableQueue '" + queue_name_ + "': Spilling item to disk: " + filename_stream.str(),
                    debug::LogContext::Debug);
            } else {
                // Track memory usage for this item
                size_t item_size = sizeof(T);
                ResourceManager::getInstance().trackMemory(item_size, "SpillQueue_" + queue_name_);

                queue_.push(item);
                cv_.notify_one();
                return true; // Added to in-memory queue, done.
            }
        } // First lock released

        if (spill_filename_optional && item_to_serialize_optional) {
            const std::string &filename = spill_filename_optional.value();
            bool serialization_succeeded = false;
            try {
                std::ofstream file(filename, std::ios::binary | std::ios::trunc);
                if (!file) {
                    debug::LogBufferManager::getInstance().appendTo(
                        "SpillableQueue",
                        "SpillableQueue '" + queue_name_ + "': Failed to open spill file for writing: " + filename,
                        debug::LogContext::Error);
                    // Item is lost, counter was incremented.
                } else {
                    // Check if the file stream is still good after opening, before serializing
                    if (!item_to_serialize_optional.value().serialize(file)) {
                        debug::LogBufferManager::getInstance().appendTo(
                            "SpillableQueue",
                            "SpillableQueue '" + queue_name_ + "': Failed to serialize item to spill file: " + filename,
                            debug::LogContext::Error);
                        file.close(); // Close before removing
                        try {
                            if (std::filesystem::exists(filename))
                                std::filesystem::remove(filename);
                        } catch (const std::filesystem::filesystem_error &e) {
                            debug::LogBufferManager::getInstance().appendTo(
                                "SpillableQueue",
                                "SpillableQueue '" + queue_name_ +
                                "': push: Filesystem error removing failed spill file '" + filename + "': " + e.what(),
                                debug::LogContext::Warning);
                        } catch (...) {
                        }
                    } else {
                        file.close(); // Ensure file is closed before adding to queue
                        serialization_succeeded = true;
                    }
                }
            } catch (const std::exception &e) {
                debug::LogBufferManager::getInstance().appendTo(
                    "SpillableQueue",
                    "SpillableQueue '" + queue_name_ + "': Exception while spilling item to disk ('" + filename + "'): "
                    + e.what(),
                    debug::LogContext::Error);
                try {
                    if (std::filesystem::exists(filename))
                        std::filesystem::remove(filename);
                } catch (const std::filesystem::filesystem_error &e) {
                    debug::LogBufferManager::getInstance().appendTo(
                        "SpillableQueue",
                        "SpillableQueue '" + queue_name_ +
                        "': push: Filesystem error removing failed spill file (exception) '" + filename + "': " + e.
                        what(),
                        debug::LogContext::Warning);
                } catch (...) {
                }
            }

            if (serialization_succeeded) {
                std::lock_guard<std::mutex> lock(mutex_); // Re-acquire lock
                if (shutdown_flag_) {
                    // Check shutdown again, in case it happened during serialization
                    debug::LogBufferManager::getInstance().appendTo(
                        "SpillableQueue",
                        "SpillableQueue '" + queue_name_ + "': Queue shutdown during spill operation for " + filename,
                        debug::LogContext::Warning);
                    try {
                        if (std::filesystem::exists(filename))
                            std::filesystem::remove(filename);
                    } catch (const std::filesystem::filesystem_error &e) {
                        debug::LogBufferManager::getInstance().appendTo(
                            "SpillableQueue",
                            "SpillableQueue '" + queue_name_ +
                            "': push: Filesystem error removing spill file during shutdown '" + filename + "': " + e.
                            what(),
                            debug::LogContext::Warning);
                    } catch (...) {
                    }
                    return false;
                } else {
                    spilled_task_files_.push(filename);
                    cv_.notify_one();
                    return true;
                }
            }
            // If serialization_succeeded is false, the item is lost. The spill_file_id_counter_ was already incremented.
            return false;
        }

        return false; // Should not reach here
    }

    /**
     * @brief Try to add an item to the queue (non-blocking)
     *
     * @param item Item to add
     * @return true if item was added, false if queue is full or shutting down
     */
    bool try_push(const T &item) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (shutdown_flag_) {
            debug::LogBufferManager::getInstance().appendTo(
                "SpillableQueue",
                "SpillableQueue '" + queue_name_ + "': Attempted to try_push to a shutting down queue.",
                debug::LogContext::Warning);
            return false;
        }

        if (queue_.size() >= max_in_memory_items_ && spill_directory_path_.empty()) {
            // Queue is full and spilling is disabled
            return false;
        }

        // If we can spill, or there's room in memory, use the normal push
        return push(item);
    }

    /**
     * @brief Pop an item from the queue
     *
     * @param item Reference to store the popped item
     * @return true if an item was popped, false if the queue is empty and shutting down
     */
    bool pop(T &item) {
        std::unique_lock<std::mutex> lock(mutex_);

        cv_.wait(lock, [this] {
            return !queue_.empty() || !spilled_task_files_.empty() || shutdown_flag_;
        });

        if (queue_.empty() && spilled_task_files_.empty()) {
            if (shutdown_flag_) {
                return false;
            }
        }

        if (!queue_.empty()) {
            // In-memory item available
            item = std::move(queue_.front());
            queue_.pop();

            // Release memory tracking for this item
            size_t item_size = sizeof(T);
            ResourceManager::getInstance().releaseMemory(item_size, "SpillQueue_" + queue_name_);

            return true;
        }

        if (!spilled_task_files_.empty()) {
            // Need to load an item from disk
            std::string filename = spilled_task_files_.front();
            spilled_task_files_.pop();

            // Release the lock while reading from disk
            lock.unlock();

            bool deserialization_succeeded = false;
            try {
                std::ifstream file(filename, std::ios::binary);
                if (!file) {
                    debug::LogBufferManager::getInstance().appendTo(
                        "SpillableQueue",
                        "SpillableQueue '" + queue_name_ + "': Failed to open spill file for reading: " + filename,
                        debug::LogContext::Error);
                } else {
                    if (!item.deserialize(file)) {
                        debug::LogBufferManager::getInstance().appendTo(
                            "SpillableQueue",
                            "SpillableQueue '" + queue_name_ + "': Failed to deserialize item from spill file: " +
                            filename,
                            debug::LogContext::Error);
                    } else {
                        deserialization_succeeded = true;
                    }
                }
            } catch (const std::exception &e) {
                debug::LogBufferManager::getInstance().appendTo(
                    "SpillableQueue",
                    "SpillableQueue '" + queue_name_ + "': Exception while loading item from disk ('" + filename +
                    "'): " + e.what(),
                    debug::LogContext::Error);
            }

            // Clean up the spill file
            try {
                if (std::filesystem::exists(filename)) {
                    std::filesystem::remove(filename);
                }
            } catch (const std::filesystem::filesystem_error &e) {
                debug::LogBufferManager::getInstance().appendTo(
                    "SpillableQueue",
                    "SpillableQueue '" + queue_name_ + "': Error deleting spill file after read '" + filename + "': " +
                    e.what(),
                    debug::LogContext::Warning);
            }

            return deserialization_succeeded;
        }

        return false; // Should not reach here
    }

    /**
     * @brief Try to pop an item from the queue (non-blocking)
     *
     * @param item Reference to store the popped item
     * @return true if an item was popped, false if the queue is empty
     */
    bool try_pop(T &item) {
        std::unique_lock<std::mutex> lock(mutex_);

        if (queue_.empty() && spilled_task_files_.empty()) {
            return false;
        }

        if (!queue_.empty()) {
            // In-memory item available
            item = std::move(queue_.front());
            queue_.pop();

            // Release memory tracking for this item
            size_t item_size = sizeof(T);
            ResourceManager::getInstance().releaseMemory(item_size, "SpillQueue_" + queue_name_);

            return true;
        }

        if (!spilled_task_files_.empty()) {
            // Need to load an item from disk
            std::string filename = spilled_task_files_.front();
            spilled_task_files_.pop();

            // Release the lock while reading from disk
            lock.unlock();

            bool deserialization_succeeded = false;
            try {
                std::ifstream file(filename, std::ios::binary);
                if (!file) {
                    debug::LogBufferManager::getInstance().appendTo(
                        "SpillableQueue",
                        "SpillableQueue '" + queue_name_ + "': Failed to open spill file for reading: " + filename,
                        debug::LogContext::Error);
                } else {
                    if (!item.deserialize(file)) {
                        debug::LogBufferManager::getInstance().appendTo(
                            "SpillableQueue",
                            "SpillableQueue '" + queue_name_ + "': Failed to deserialize item from spill file: " +
                            filename,
                            debug::LogContext::Error);
                    } else {
                        deserialization_succeeded = true;
                    }
                }
            } catch (const std::exception &e) {
                debug::LogBufferManager::getInstance().appendTo(
                    "SpillableQueue",
                    "SpillableQueue '" + queue_name_ + "': Exception while loading item from disk ('" + filename +
                    "'): " + e.what(),
                    debug::LogContext::Error);
            }

            // Clean up the spill file
            try {
                if (std::filesystem::exists(filename)) {
                    std::filesystem::remove(filename);
                }
            } catch (const std::filesystem::filesystem_error &e) {
                debug::LogBufferManager::getInstance().appendTo(
                    "SpillableQueue",
                    "SpillableQueue '" + queue_name_ + "': Error deleting spill file after read '" + filename + "': " +
                    e.what(),
                    debug::LogContext::Warning);
            }

            return deserialization_succeeded;
        }

        return false; // Should not reach here
    }

    /**
     * @brief Signal that no more items will be added to the queue
     */
    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_flag_ = true;
        cv_.notify_all();

        debug::LogBufferManager::getInstance().appendTo(
            "SpillableQueue",
            "SpillableQueue '" + queue_name_ + "' marked as done",
            debug::LogContext::Debug);
    }

    /**
     * @brief Check if the queue is empty
     *
     * @return true if the queue is empty, false otherwise
     */
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty() && spilled_task_files_.empty();
    }

    /**
     * @brief Get the total number of items in the queue
     *
     * @return Total number of items (in-memory + spilled)
     */
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size() + spilled_task_files_.size();
    }

    /**
     * @brief Get the number of in-memory items
     *
     * @return Number of in-memory items
     */
    size_t in_memory_size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    /**
     * @brief Get the number of spilled items
     *
     * @return Number of spilled items
     */
    size_t spilled_size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return spilled_task_files_.size();
    }

    /**
     * @brief Check if the queue is shutting down
     *
     * @return true if the queue is shutting down, false otherwise
     */
    bool is_shutting_down() const {
        return shutdown_flag_.load(std::memory_order_acquire);
    }

    /**
     * @brief Get the name of this queue
     *
     * @return The queue name
     */
    const std::string &name() const {
        return queue_name_;
    }

private:
    std::queue<T> queue_; // In-memory queue
    std::queue<std::string> spilled_task_files_; // Queue of spill file paths
    size_t max_in_memory_items_; // Maximum items to keep in memory
    std::string spill_directory_path_; // Directory for spill files
    size_t spill_file_id_counter_; // Counter for generating unique filenames
    std::string queue_name_; // Name of this queue (for debugging)
    std::atomic<bool> shutdown_flag_; // Shutdown flag
    mutable std::mutex mutex_; // Mutex for thread safety
    std::condition_variable cv_; // Condition variable for signaling
};

#endif // SPILLABLE_QUEUE_H
