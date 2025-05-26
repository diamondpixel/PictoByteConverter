#include "headers/LogBuffer.h"
#include "Threading/headers/ResourceManager.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cmath> // For std::abs
#include <format> // Added for C++20 std::format
#include <mutex> // For std::mutex

namespace debug {
    LogBuffer::LogBuffer(size_t capacity, LogContext defaultContext, bool skipInitialTracking)
        : capacity_(capacity),
          head_(0),
          count_(0),
          defaultContext_(defaultContext),
          resourceTag_(std::format("LogBuffer_{}", reinterpret_cast<uintptr_t>(this))) {
        buffer_.reserve(capacity_); // Pre-allocate buffer

        size_t structuralMemory = sizeof(LogBuffer) + (capacity_ * sizeof(LogEntry));
        if (!skipInitialTracking) {
            // Track initial structural memory allocation using pooled memory
            structuralMemoryBlock_ = ResourceManager::getInstance().getPooledMemory(
                structuralMemory, ResourceManager::MemoryBlockCategory::GENERIC);
        }
        totalTrackedMemory_.store(structuralMemory, std::memory_order_relaxed);
        // pendingMessageMemoryDelta_ is already initialized to 0 by default member initializer
    }

    LogBuffer::~LogBuffer() {
        flushMessageMemoryDelta(true); // Force flush any pending message memory changes

        // Release the structural memory block if it was allocated
        if (structuralMemoryBlock_.isValid()) {
            ResourceManager::getInstance().releasePooledMemory(structuralMemoryBlock_);
        }
        
        // Release any message memory blocks that are still tracked
        for (auto& block_id : messageMemoryBlocks_) {
            if (block_id.isValid()) {
                ResourceManager::getInstance().releasePooledMemory(block_id);
            }
        }
    }

    void LogBuffer::append(const std::string &message, LogContext context) {
        std::lock_guard<std::mutex> lock(mutex_);

        std::ptrdiff_t messageDelta = 0;
        LogEntry newEntry(message, context);

        if (count_ == capacity_ && count_ > 0) { // Buffer full, overwriting oldest
            // Account for memory of the message being overwritten
            messageDelta -= static_cast<std::ptrdiff_t>(buffer_[head_].message.capacity());
            buffer_[head_] = std::move(newEntry);
        } else if (count_ < capacity_) { // Buffer not full, adding new entry
            buffer_.push_back(std::move(newEntry));
            count_++;
        } else { // count_ == 0, capacity_ > 0, first element (or capacity is 0, which is an edge case)
             if (capacity_ > 0) {
                buffer_.push_back(std::move(newEntry));
                count_++;
             }
        }
        
        // Account for memory of the new/replacing message
        // Note: For push_back, newEntry is already moved. If it was an overwrite, buffer_[head_] is the new one.
        if (capacity_ > 0) {
             if (count_ == capacity_ || (count_ < capacity_ && !buffer_.empty())) { // If overwritten or added
                // If overwritten, head_ points to the new entry. If added, last element is the new entry.
                size_t newEntryIndex = (count_ == capacity_) ? head_ : (buffer_.size() -1) ;
                messageDelta += static_cast<std::ptrdiff_t>(buffer_[newEntryIndex].message.capacity());
            }
        }

        if (capacity_ > 0) {
            head_ = (head_ + 1) % capacity_;
        }
        
        // Unlock not needed here as updateAndFlush is outside the critical section of buffer modification
        // Dropping the lock before potentially calling ResourceManager
        // However, messageDelta calculation depends on buffer state, so it must be within lock.
        // The actual updateAndFlush can be outside if messageDelta is passed by value.
        // For simplicity and correctness of delta calculation, keep it like this for now, then call outside.

        // Call this *after* releasing the lock if append itself becomes a bottleneck due to flush.
        // For now, this is fine as flush itself is mostly atomic operations or short locks.
        if (messageDelta != 0) {
             updateAndFlushMessageMemoryDelta(messageDelta);
        }
    }

    std::vector<LogEntry> LogBuffer::readAll() const {
        std::lock_guard<std::mutex> lock(mutex_);

        if (count_ == 0) {
            return {};
        }

        std::vector<LogEntry> result;
        result.reserve(count_);

        // Start from oldest entry (head - count) and read all entries in order
        size_t start = (head_ + capacity_ - count_) % capacity_;

        for (size_t i = 0; i < count_; ++i) {
            size_t index = (start + i) % capacity_;
            result.push_back(buffer_[index]);
        }

        return result;
    }

    std::vector<LogEntry> LogBuffer::readRecent(size_t count, std::optional<LogContext> contextFilter) const {
        std::lock_guard<std::mutex> lock(mutex_);

        if (count_ == 0) {
            return {};
        }

        std::vector<LogEntry> result;
        result.reserve(std::min(count, count_));

        // Start from newest entry (head - 1) and go backwards
        size_t current = (head_ + capacity_ - 1) % capacity_;
        size_t remaining = std::min(count, count_);

        while (remaining > 0 && result.size() < count) {
            // If we have a context filter, only include matching entries
            if (!contextFilter.has_value() || buffer_[current].context == contextFilter.value()) {
                result.push_back(buffer_[current]);
                remaining--;
            }

            // Move to previous entry
            current = (current + capacity_ - 1) % capacity_;

            // Stop if we've checked all entries
            if ((head_ + capacity_ - current - 1) % capacity_ >= count_) {
                break;
            }
        }

        return result;
    }

    void LogBuffer::clear() {
        std::lock_guard<std::mutex> lock(mutex_);

        flushMessageMemoryDelta(true); // Flush any pending changes first

        std::ptrdiff_t messagesMemoryToRelease = 0;
        if (count_ > 0) {
            // Calculate memory of all messages currently in the buffer
            // This iterates through the valid entries in their logical order
            size_t currentIdx = (head_ + capacity_ - count_) % capacity_; // Oldest entry
            for (size_t i = 0; i < count_; ++i) {
                messagesMemoryToRelease -= static_cast<std::ptrdiff_t>(buffer_[(currentIdx + i) % capacity_].message.capacity());
            }
        }

        buffer_.clear();
        buffer_.reserve(capacity_); // Keep the vector's capacity for future appends
        head_ = 0;
        count_ = 0;

        if (messagesMemoryToRelease != 0) {
            // This will update pendingMessageMemoryDelta_ and potentially call ResourceManager
            updateAndFlushMessageMemoryDelta(messagesMemoryToRelease);
        }
        // Structural memory (sizeof(LogBuffer) + capacity_ * sizeof(LogEntry)) remains tracked.
        // It was tracked in constructor and will be released in destructor.
    }

    size_t LogBuffer::size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

    size_t LogBuffer::capacity() const {
        // Capacity doesn't change, no need for lock
        return capacity_;
    }

    void LogBuffer::setDefaultContext(LogContext context) {
        std::lock_guard<std::mutex> lock(mutex_);
        defaultContext_ = context;
    }

    LogContext LogBuffer::getDefaultContext() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return defaultContext_;
    }

    void LogBuffer::updateAndFlushMessageMemoryDelta(std::ptrdiff_t messageDelta) {
        pendingMessageMemoryDelta_.fetch_add(messageDelta, std::memory_order_relaxed);
        flushMessageMemoryDelta(false); // Attempt non-forced flush
    }

    void LogBuffer::flushMessageMemoryDelta(bool forceFlush) {
        std::ptrdiff_t currentPending = pendingMessageMemoryDelta_.load(std::memory_order_acquire);

        if (forceFlush || std::abs(currentPending) >= MESSAGE_MEMORY_FLUSH_THRESHOLD) {
            std::ptrdiff_t deltaToProcess = pendingMessageMemoryDelta_.exchange(0, std::memory_order_acq_rel);

            if (deltaToProcess == 0) {
                return; // Nothing to process or already handled by a concurrent flush
            }

            if (deltaToProcess > 0) {
                // Allocate new memory block for the additional message memory
                size_t trackAmount = static_cast<size_t>(deltaToProcess);
                auto block_id = ResourceManager::getInstance().getPooledMemory(
                    trackAmount, ResourceManager::MemoryBlockCategory::GENERIC);
                
                if (block_id.isValid()) {
                    // Store the block ID for later release
                    std::lock_guard<std::mutex> lock(memoryBlocksMutex_);
                    messageMemoryBlocks_.push_back(block_id);
                    totalTrackedMemory_.fetch_add(trackAmount, std::memory_order_relaxed);
                }
            } else { // deltaToProcess < 0
                size_t releaseAmount = static_cast<size_t>(-deltaToProcess);
                
                // Release memory blocks until we've released enough memory
                std::lock_guard<std::mutex> lock(memoryBlocksMutex_);
                size_t releasedSoFar = 0;
                
                // We'll release blocks until we've released approximately the right amount
                while (!messageMemoryBlocks_.empty() && releasedSoFar < releaseAmount) {
                    auto block_id = messageMemoryBlocks_.back();
                    if (block_id.isValid()) {
                        size_t blockSize = releaseAmount / messageMemoryBlocks_.size();
                        if (blockSize == 0) blockSize = 1; // Ensure we're releasing something
                        ResourceManager::getInstance().releasePooledMemory(block_id);
                        releasedSoFar += blockSize;
                        messageMemoryBlocks_.pop_back();
                    } else {
                        // Invalid block ID, just remove it
                        messageMemoryBlocks_.pop_back();
                    }
                }
                
                // Update the total tracked memory
                totalTrackedMemory_.fetch_sub(std::min(releaseAmount, releasedSoFar), std::memory_order_relaxed);
            }
        }
    }

    size_t LogBuffer::calculateEntrySize(const LogEntry &entry) const {
        // Calculate approximate memory usage of a log entry
        return sizeof(LogEntry) + entry.message.capacity() * sizeof(char);
    }
}
