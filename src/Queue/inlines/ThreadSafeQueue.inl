#pragma once

#include <utility>
#include "../Debug/headers/LogBufferManager.h"
#include "../Debug/headers/LogBuffer.h"
#include "../Threading/headers/ResourceManager.h"

using RM = ResourceManager;

template<typename T>
ThreadSafeQueue<T>::ThreadSafeQueue(size_t max_size, std::string queue_name)
    : max_size_(max_size), shutting_(false), size_(0), queue_name_(std::move(queue_name)) {
    debug::LogBufferManager::getInstance().appendTo(
        "ThreadSafeQueue",
        "Created ThreadSafeQueue '" + queue_name_ + "' with max size " +
        (max_size == SIZE_MAX ? "unlimited" : std::to_string(max_size)),
        debug::LogContext::Debug);
}

template<typename T>
ThreadSafeQueue<T>::~ThreadSafeQueue() {
    clear();
    ThreadSafeQueue<T>::shutdown();
    if (size_ > 0) {
        debug::LogBufferManager::getInstance().appendTo(
            "ThreadSafeQueue",
            "ThreadSafeQueue '" + queue_name_ + "' destroyed with " + std::to_string(size_) + " items remaining",
            debug::LogContext::Debug);
    }
}

template<typename T>
ThreadSafeQueue<T>::ThreadSafeQueue(ThreadSafeQueue &&other) noexcept
    : max_size_(other.max_size_), 
      shutting_(other.shutting_.load()), 
      size_(other.size_.load()),
      queue_name_(std::move(other.queue_name_)) {
    std::lock_guard<std::mutex> lock(other.mutex_);
    queue_ = std::move(other.queue_);
}

template<typename T>
ThreadSafeQueue<T> &ThreadSafeQueue<T>::operator=(ThreadSafeQueue &&other) noexcept {
    if (this != &other) {
        // First, clean up our own resources
        clear();
        
        // Now acquire the lock on the other queue and move its contents
        std::lock_guard<std::mutex> lock_other(other.mutex_);
        std::lock_guard<std::mutex> lock_this(mutex_);
        
        max_size_ = other.max_size_;
        shutting_.store(other.shutting_.load());
        size_.store(other.size_.load());
        queue_name_ = std::move(other.queue_name_);
        queue_ = std::move(other.queue_);
        
        // Reset the other queue
        other.size_.store(0);
        other.shutting_.store(true);
    }
    return *this;
}

template<typename T>
bool ThreadSafeQueue<T>::push(T &&item) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    // Check if we're shutting down
    if (shutting_.load(std::memory_order_acquire)) {
        debug::LogBufferManager::getInstance().appendTo(
            "ThreadSafeQueue",
            "ThreadSafeQueue '" + queue_name_ + "': Push rejected - queue is shutting down",
            debug::LogContext::Debug);
        return false;
    }
    
    // Check if we're at capacity
    if (size_.load(std::memory_order_relaxed) >= max_size_) {
        // Wait for space to become available
        not_full_.wait(lock, [this] {
            return size_.load(std::memory_order_relaxed) < max_size_ || 
                   shutting_.load(std::memory_order_acquire);
        });
        
        // Check again if we're shutting down after waiting
        if (shutting_.load(std::memory_order_acquire)) {
            return false;
        }
    }
    
    // Add the item to the queue
    queue_.push(std::move(item));
    size_.fetch_add(1, std::memory_order_release);
    
    // Notify one waiting consumer
    lock.unlock();
    not_empty_.notify_one();
    
    return true;
}

template<typename T>
bool ThreadSafeQueue<T>::try_push(T &&item) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Check if we're shutting down or at capacity
    if (shutting_.load(std::memory_order_acquire) || 
        size_.load(std::memory_order_relaxed) >= max_size_) {
        return false;
    }
    
    // Add the item to the queue
    queue_.push(std::move(item));
    size_.fetch_add(1, std::memory_order_release);
    
    // Notify one waiting consumer
    not_empty_.notify_one();
    
    return true;
}

template<typename T>
bool ThreadSafeQueue<T>::pop(T &item) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    // Wait until there's an item or we're shutting down with an empty queue
    not_empty_.wait(lock, [this] {
        return !queue_.empty() || (shutting_.load(std::memory_order_acquire) && queue_.empty());
    });
    
    // If we're shutting down and the queue is empty, return false
    if (queue_.empty()) {
        return false;
    }
    
    // Get the item from the queue
    item = std::move(queue_.front());
    queue_.pop();
    size_.fetch_sub(1, std::memory_order_release);
    
    // Notify one waiting producer
    lock.unlock();
    not_full_.notify_one();
    
    return true;
}

template<typename T>
bool ThreadSafeQueue<T>::try_pop(T &item) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Check if the queue is empty
    if (queue_.empty()) {
        return false;
    }
    
    // Get the item from the queue
    item = std::move(queue_.front());
    queue_.pop();
    size_.fetch_sub(1, std::memory_order_release);
    
    // Notify one waiting producer
    not_full_.notify_one();
    
    return true;
}

template<typename T>
void ThreadSafeQueue<T>::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutting_.store(true, std::memory_order_release);
    }
    
    // Notify all waiting threads
    not_empty_.notify_all();
    not_full_.notify_all();
    
    debug::LogBufferManager::getInstance().appendTo(
        "ThreadSafeQueue",
        "ThreadSafeQueue '" + queue_name_ + "': Shutdown initiated",
        debug::LogContext::Debug);
}

template<typename T>
bool ThreadSafeQueue<T>::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
}

template<typename T>
size_t ThreadSafeQueue<T>::size() const {
    return size_.load(std::memory_order_acquire);
}

template<typename T>
bool ThreadSafeQueue<T>::is_shutting_down() const {
    return shutting_.load(std::memory_order_acquire);
}

template<typename T>
const std::string &ThreadSafeQueue<T>::name() const {
    return queue_name_;
}

template<typename T>
void ThreadSafeQueue<T>::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Clear the queue
    std::queue<T> empty;
    std::swap(queue_, empty);
    size_.store(0, std::memory_order_release);
    
    // Notify all waiting producers
    not_full_.notify_all();
}

template<typename T>
size_t ThreadSafeQueue<T>::getMemoryUsage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Base memory usage for the queue structure
    size_t usage = sizeof(*this);
    
    // Add memory usage for each item in the queue
    std::queue<T> queue_copy = queue_;
    while (!queue_copy.empty()) {
        if constexpr (std::is_same_v<T, Task> || std::is_same_v<T, ImageTaskInternal>) {
            usage += queue_copy.front().getMemoryUsage();
        } else {
            usage += sizeof(T);
        }
        queue_copy.pop();
    }
    
    return usage;
}

template<typename T>
void ThreadSafeQueue<T>::notify_all_consumers() {
    std::lock_guard<std::mutex> lock(mutex_);
    not_empty_.notify_all();
}

template<typename T>
bool ThreadSafeQueue<T>::full() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size() >= max_size_;
}

template<typename T>
void ThreadSafeQueue<T>::wait_until_empty() {
    std::unique_lock<std::mutex> lock(mutex_);
    empty_.wait(lock, [this] { return queue_.empty() || shutting_.load(); });
}

template<typename T>
size_t ThreadSafeQueue<T>::capacity() const {
    return max_size_;
}
