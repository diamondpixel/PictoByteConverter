#ifndef QUEUE_TYPES_H
#define QUEUE_TYPES_H



/**
 * @brief Enum defining the different types of queues available in the system
 */
enum class QueueType {
    LockFree = 0,   // Standard thread-safe queue with fixed capacity
    Spillable = 1,    // Queue that can spill to disk when memory usage exceeds threshold
};


#endif // QUEUE_TYPES_H
