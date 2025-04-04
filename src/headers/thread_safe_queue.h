#ifndef THREAD_SAFE_QUEUE_H
#define THREAD_SAFE_QUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include "image_task.h"

/**
 * Thread-safe queue implementation for image processing tasks
 */
class ThreadSafeQueue {
private:
 std::queue<ImageTask> queue;
 std::mutex mutex;
 std::condition_variable cv;
 bool done = false;

public:
 /**
  * Add a task to the queue
  * @param task The task to add
  */
 void push(const ImageTask& task);

 /**
  * Get and remove a task from the queue
  * @param task Reference to store the retrieved task
  * @return True if a task was retrieved, false if queue is empty and finished
  */
 bool pop(ImageTask& task);

 /**
  * Signal that no more tasks will be added
  */
 void finish();

 /**
  * Check if the queue is empty
  * @return True if the queue is empty
  */
 bool isEmpty();
};

#endif // THREAD_SAFE_QUEUE_H