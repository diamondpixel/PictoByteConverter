#include "headers/ThreadSafeQueue.h"
#include <mutex>
#include <condition_variable>
#include <queue>

void ThreadSafeQueue::push(const ImageTask& task) {
    std::lock_guard<std::mutex> lock(mutex);
    queue.push(task);
    cv.notify_one();
}

bool ThreadSafeQueue::pop(ImageTask& task) {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [this] { return !queue.empty() || done; });

    if (queue.empty() && done) {
        return false;
    }

    task = queue.front();
    queue.pop();
    return true;
}

void ThreadSafeQueue::finish() {
    std::lock_guard<std::mutex> lock(mutex);
    done = true;
    cv.notify_all();
}

bool ThreadSafeQueue::isEmpty() {
    std::lock_guard<std::mutex> lock(mutex);
    return queue.empty();
}