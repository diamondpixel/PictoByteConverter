#ifndef QUEUE_BASE_H
#define QUEUE_BASE_H

#include <string>
#include <mutex>
#include <type_traits>
#include "../../Tasks/headers/_Task.h"

/**
 * @brief Base template for queue implementations
 *
 * This template defines the common interface that all queue implementations
 * should support. It provides a consistent API for different queue types.
 *
 * @tparam T Type of items in the queue
 */
template<typename T>
struct QueueBase {
    /**
     * @brief Push an item to the queue
     *
     * @param item Item to push (rvalue reference)
     * @return true if successful, false if queue is shutting down or full
     */
    virtual bool push(T &&item) = 0;

    /**
     * @brief Try to push an item to the queue (non-blocking)
     *
     * @param item Item to push (rvalue reference)
     * @return true if successful, false if queue is shutting down or full
     */
    virtual bool try_push(T &&item) = 0;

    /**
     * @brief Pop an item from the queue
     *
     * @param item Reference to store the popped item
     * @return true if successful, false if queue is empty and shutting down
     */
    virtual bool pop(T &item) = 0;

    /**
     * @brief Try to pop an item from the queue (non-blocking)
     *
     * @param item Reference to store the popped item
     * @return true if successful, false if queue is empty
     */
    virtual bool try_pop(T &item) = 0;

    /**
     * @brief Signal that no more items will be added to the queue
     */
    virtual void shutdown() = 0;

    /**
     * @brief Check if the queue is empty
     *
     * @return true if empty, false otherwise
     */
    [[nodiscard]] virtual bool empty() const = 0;

    /**
     * @brief Get the total number of items in the queue
     *
     * @return Size of the queue
     */
    [[nodiscard]] virtual size_t size() const = 0;

    /**
     * @brief Check if the queue is shutting down
     *
     * @return true if shutting down, false otherwise
     */
    [[nodiscard]] virtual bool is_shutting_down() const = 0;

    /**
     * @brief Get the name of this queue
     *
     * @return The queue name
     */
    [[nodiscard]] virtual const std::string &name() const = 0;

    /**
     * @brief Get the accurate current size of the queue including the Queue and
     * itself.
     *
     * @return The accurate size of the queue
     */
    [[nodiscard]] virtual size_t getMemoryUsage() const = 0;

    /**
     * @brief Virtual destructor
     */
    virtual ~QueueBase() = default;
};

#endif // QUEUE_BASE_H
