#ifndef TASK_H
#define TASK_H

#include <string>
#include <chrono>
#include <functional>
#include <thread>
#include <utility>
#include <iostream>

/**
 * @brief Represents the current status of a task.
 */
enum class TaskStatus {
    PENDING, // Task is waiting to be processed
    PROCESSING, // Task is currently being processed
    COMPLETED, // Task finished successfully
    FAILED, // Task finished with an error
    CANCELLED // Task was cancelled before or during processing
};

/**
 * @brief Template base class for tasks with common properties and behaviors.
 *
 */
struct Task {
    // Basic task properties
    std::string task_id;
    int64_t creation_timestamp;
    TaskStatus status;
    std::function<void()> func;
    bool self_destruct{false}; // Flag to indicate if the task should be destroyed without processing

    // Metrics properties
    std::string task_name;
    std::string pool_name;
    std::thread::id executing_thread_id;
    std::string error_message;

    std::chrono::system_clock::time_point enqueue_time;
    std::chrono::system_clock::time_point start_processing_time;
    std::chrono::system_clock::time_point end_processing_time;

    uint64_t wait_duration_ns{0};
    uint64_t execution_duration_ns{0};

    /**
     * @brief Default constructor for deserialization or default instantiation.
     * Initializes members to default values. task_id is empty, timestamp to 0,
     * status to PENDING, and func to nullptr.
     */
    Task()
        : task_id(""),
          creation_timestamp(0),
          status(TaskStatus::PENDING),
          func(nullptr){
    }

    /**
     * @brief Constructor with task_id
     * 
     * @param id The task identifier
     */
    explicit Task(std::string id)
        : task_id(std::move(id)),
          creation_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch()).count()),
          status(TaskStatus::PENDING),
          func(nullptr),
          enqueue_time(std::chrono::system_clock::now()) {
    }

    /**
     * @brief Get the memory usage of this task
     * 
     * @return Size in bytes
     */
    [[nodiscard]] virtual size_t getMemoryUsage() const {
        return sizeof(*this) +
               (task_id.capacity() - task_id.size()) +
               (task_name.capacity() - task_name.size()) +
               (pool_name.capacity() - pool_name.size()) +
               (error_message.capacity() - error_message.size());
    }

    /**
     * @brief Allow derived tasks to drop large buffers when spilling to disk
     */
    virtual void releaseHeavyResources() {}

    /**
     * @brief Copy constructor (explicitly defined for clarity)
     * 
     * @param other The task to copy from
     */
    Task(const Task &other)
        : task_id(other.task_id),
          creation_timestamp(other.creation_timestamp),
          status(other.status),
          self_destruct(other.self_destruct),
          func(other.func),
          task_name(other.task_name),
          pool_name(other.pool_name),
          executing_thread_id(other.executing_thread_id),
          error_message(other.error_message),
          enqueue_time(other.enqueue_time),
          start_processing_time(other.start_processing_time),
          end_processing_time(other.end_processing_time),
          wait_duration_ns(other.wait_duration_ns),
          execution_duration_ns(other.execution_duration_ns) {
    }

    /**
     * @brief Move constructor
     * 
     * @param other The task to move from
     */
    Task(Task &&other) noexcept
        : task_id(std::move(other.task_id)),
          creation_timestamp(other.creation_timestamp),
          status(other.status),
          self_destruct(other.self_destruct),
          func(std::move(other.func)),
          task_name(std::move(other.task_name)),
          pool_name(std::move(other.pool_name)),
          executing_thread_id(other.executing_thread_id),
          error_message(std::move(other.error_message)),
          enqueue_time(other.enqueue_time),
          start_processing_time(other.start_processing_time),
          end_processing_time(other.end_processing_time),
          wait_duration_ns(other.wait_duration_ns),
          execution_duration_ns(other.execution_duration_ns) {
    }

    /**
     * @brief Copy assignment operator
     */
    Task &operator=(const Task &other) {
        if (this != &other) {
            task_id = other.task_id;
            creation_timestamp = other.creation_timestamp;
            status = other.status;
            self_destruct = other.self_destruct;
            func = other.func;
            task_name = other.task_name;
            pool_name = other.pool_name;
            executing_thread_id = other.executing_thread_id;
            error_message = other.error_message;
            enqueue_time = other.enqueue_time;
            start_processing_time = other.start_processing_time;
            end_processing_time = other.end_processing_time;
            wait_duration_ns = other.wait_duration_ns;
            execution_duration_ns = other.execution_duration_ns;
        }
        return *this;
    }

    /**
     * @brief Move assignment operator
     */
    Task &operator=(Task &&other) noexcept {
        if (this != &other) {
            task_id = std::move(other.task_id);
            creation_timestamp = other.creation_timestamp;
            status = other.status;
            self_destruct = other.self_destruct;
            func = std::move(other.func);
            task_name = std::move(other.task_name);
            pool_name = std::move(other.pool_name);
            executing_thread_id = other.executing_thread_id;
            error_message = std::move(other.error_message);
            enqueue_time = other.enqueue_time;
            start_processing_time = other.start_processing_time;
            end_processing_time = other.end_processing_time;
            wait_duration_ns = other.wait_duration_ns;
            execution_duration_ns = other.execution_duration_ns;
        }
        return *this;
    }

    /**
     * @brief Set the task as started processing
     */
    void setProcessingStarted() {
        start_processing_time = std::chrono::system_clock::now();
        if (enqueue_time != std::chrono::system_clock::time_point()) {
            // if enqueue_time was set
            wait_duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                start_processing_time - enqueue_time).count();
        }
        status = TaskStatus::PROCESSING;
    }

    /**
     * @brief Set the task as finished processing
     * 
     * @param final_status The final status of the task
     * @param err_msg Optional error message if the task failed
     */
    void setProcessingEnded(TaskStatus final_status, std::string err_msg = "") {
        end_processing_time = std::chrono::system_clock::now();
        if (start_processing_time != std::chrono::system_clock::time_point()) {
            // if start_processing_time was set
            execution_duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                end_processing_time - start_processing_time).count();
        }
        status = final_status;
        if (status == TaskStatus::FAILED && !err_msg.empty()) {
            error_message = std::move(err_msg);
        }
    }

    /**
     * @brief Virtual destructor
     *
     * Ensures proper cleanup of resources, especially the function object.
     * Virtual to allow proper destruction of derived classes.
     */
    virtual ~Task() {
        // Release any resources held by the function object
        if (func) {
            func = nullptr;
        }

        // Clear string members to release memory
        error_message.clear();
        error_message.shrink_to_fit();

        task_name.clear();
        task_name.shrink_to_fit();

        pool_name.clear();
        pool_name.shrink_to_fit();

        task_id.clear();
        task_id.shrink_to_fit();
    }

    /**
    * @brief Get the task ID
    * @return The task ID
    */
    [[nodiscard]] const std::string &getTaskId() const {
        return task_id;
    }

    /**
     * @brief Set the task ID
     * @param id The new task ID
     */
    void setTaskId(std::string id) {
        task_id = std::move(id);
    }

    /**
     * @brief Get the creation timestamp
     * @return The creation timestamp in milliseconds since epoch
     */
    [[nodiscard]] int64_t getCreationTimestamp() const {
        return creation_timestamp;
    }

    /**
     * @brief Set the creation timestamp
     * @param timestamp The new creation timestamp in milliseconds since epoch
     */
    void setCreationTimestamp(int64_t timestamp) {
        creation_timestamp = timestamp;
    }

    /**
     * @brief Get the task status
     * @return The current task status
     */
    [[nodiscard]] TaskStatus getStatus() const {
        return status;
    }

    /**
     * @brief Set the task status
     * @param new_status The new task status
     */
    void setStatus(TaskStatus new_status) {
        status = new_status;
    }

    /**
     * @brief Get the task function
     * @return The task function
     */
    [[nodiscard]] const std::function<void()> &getFunction() const {
        return func;
    }

    /**
     * @brief Set the function to be executed by this task
     * 
     * @param taskFunc The function to execute
     */
    void setFunction(std::function<void()> taskFunc) {
        func = std::move(taskFunc);
    }

    /**
     * @brief Get the task name
     * @return The task name
     */
    [[nodiscard]] const std::string &getTaskName() const {
        return task_name;
    }

    /**
     * @brief Set the task name
     * @param name The new task name
     */
    void setTaskName(std::string name) {
        task_name = std::move(name);
    }

    /**
     * @brief Get the pool name
     * @return The pool name
     */
    [[nodiscard]] const std::string &getPoolName() const {
        return pool_name;
    }

    /**
     * @brief Set the pool name
     * @param name The new pool name
     */
    void setPoolName(std::string name) {
        pool_name = std::move(name);
    }

    /**
     * @brief Get the executing thread ID
     * @return The executing thread ID
     */
    [[nodiscard]] std::thread::id getExecutingThreadId() const {
        return executing_thread_id;
    }

    /**
     * @brief Set the executing thread ID
     * @param thread_id The new executing thread ID
     */
    void setExecutingThreadId(std::thread::id thread_id) {
        executing_thread_id = thread_id;
    }

    /**
     * @brief Get the error message
     * @return The error message
     */
    [[nodiscard]] const std::string &getErrorMessage() const {
        return error_message;
    }

    /**
     * @brief Set the error message
     * @param message The new error message
     */
    void setErrorMessage(std::string message) {
        error_message = std::move(message);
    }

    /**
     * @brief Get the enqueue time
     * @return The enqueue time
     */
    [[nodiscard]] std::chrono::system_clock::time_point getEnqueueTime() const {
        return enqueue_time;
    }

    /**
     * @brief Set the enqueue time for this task
     * 
     * @param time Enqueue time
     */
    void setEnqueueTime(std::chrono::system_clock::time_point time) {
        this->enqueue_time = time;
    }

    /**
     * @brief Get the start processing time
     * @return The start processing time
     */
    [[nodiscard]] std::chrono::system_clock::time_point getStartProcessingTime() const {
        return start_processing_time;
    }

    /**
     * @brief Set the start processing time
     * @param time The new start processing time
     */
    void setStartProcessingTime(std::chrono::system_clock::time_point time) {
        start_processing_time = time;
    }

    /**
     * @brief Get the end processing time
     * @return The end processing time
     */
    [[nodiscard]] std::chrono::system_clock::time_point getEndProcessingTime() const {
        return end_processing_time;
    }

    /**
     * @brief Set the end processing time
     * @param time The new end processing time
     */
    void setEndProcessingTime(std::chrono::system_clock::time_point time) {
        end_processing_time = time;
    }

    /**
     * @brief Get the wait duration in nanoseconds
     * @return The wait duration in nanoseconds
     */
    [[nodiscard]] uint64_t getWaitDurationNs() const {
        return wait_duration_ns;
    }

    /**
     * @brief Set the wait duration in nanoseconds
     * @param duration The new wait duration in nanoseconds
     */
    void setWaitDurationNs(uint64_t duration) {
        wait_duration_ns = duration;
    }

    /**
     * @brief Get the execution duration in nanoseconds
     * @return The execution duration in nanoseconds
     */
    [[nodiscard]] uint64_t getExecutionDurationNs() const {
        return execution_duration_ns;
    }

    /**
     * @brief Set the execution duration in nanoseconds
     * @param duration The new execution duration in nanoseconds
     */
    void setExecutionDurationNs(uint64_t duration) {
        execution_duration_ns = duration;
    }

    /**
     * @brief Update wait duration based on current time and enqueue time
     */
    void updateWaitDuration() {
        auto now = std::chrono::system_clock::now();
        wait_duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - enqueue_time).count();
    }

    // Helper to write string to stream
    static bool write_string(std::ostream& os, const std::string& s) {
        size_t len = s.length();
        os.write(reinterpret_cast<const char*>(&len), sizeof(len));
        if (len > 0) {
            os.write(s.c_str(), static_cast<std::streamsize>(len));
        }
        return os.good();
    }

    // Helper to read string from stream
    static bool read_string(std::istream& is, std::string& s) {
        size_t len;
        is.read(reinterpret_cast<char*>(&len), sizeof(len));
        if (is.gcount() != sizeof(len)) return false;
        if (len > 0) {
            s.resize(len);
            is.read(&s[0], static_cast<std::streamsize>(len));
            if (is.gcount() != static_cast<std::streamsize>(len)) return false;
        }
        return is.good();
    }

    /**
     * @brief Serialize the Task object to an output stream.
     * Does not serialize std::function func.
     * @param os The output stream.
     * @return True if successful, false otherwise.
     */
    virtual bool serialize(std::ostream& os) const {
        if (!os.good()) return false;

        // Serialize basic task properties
        if (!write_string(os, task_id)) return false;
        os.write(reinterpret_cast<const char*>(&creation_timestamp), sizeof(creation_timestamp));
        os.write(reinterpret_cast<const char*>(&status), sizeof(status));
        os.write(reinterpret_cast<const char*>(&self_destruct), sizeof(self_destruct));

        // Serialize metrics properties
        if (!write_string(os, task_name)) return false;
        if (!write_string(os, pool_name)) return false;
        // std::thread::id is not directly serializable in a portable way, skip for now or use a hash/string representation if needed
        if (!write_string(os, error_message)) return false;

        // std::chrono::system_clock::time_point can be tricky; convert to duration since epoch
        auto enqueue_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(enqueue_time.time_since_epoch()).count();
        os.write(reinterpret_cast<const char*>(&enqueue_time_ms), sizeof(enqueue_time_ms));

        auto start_processing_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(start_processing_time.time_since_epoch()).count();
        os.write(reinterpret_cast<const char*>(&start_processing_time_ms), sizeof(start_processing_time_ms));

        auto end_processing_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_processing_time.time_since_epoch()).count();
        os.write(reinterpret_cast<const char*>(&end_processing_time_ms), sizeof(end_processing_time_ms));

        os.write(reinterpret_cast<const char*>(&wait_duration_ns), sizeof(wait_duration_ns));
        os.write(reinterpret_cast<const char*>(&execution_duration_ns), sizeof(execution_duration_ns));

        return os.good();
    }

    /**
     * @brief Deserialize the Task object from an input stream.
     * Does not deserialize std::function func (it will remain nullptr).
     * @param is The input stream.
     * @return True if successful, false otherwise.
     */
    virtual bool deserialize(std::istream& is) {
        if (!is.good()) return false;

        // Deserialize basic task properties
        if (!read_string(is, task_id)) return false;
        is.read(reinterpret_cast<char*>(&creation_timestamp), sizeof(creation_timestamp));
        if (is.gcount() != sizeof(creation_timestamp)) return false;
        is.read(reinterpret_cast<char*>(&status), sizeof(status));
        if (is.gcount() != sizeof(status)) return false;
        is.read(reinterpret_cast<char*>(&self_destruct), sizeof(self_destruct));
        if (is.gcount() != sizeof(self_destruct)) return false;

        // Deserialize metrics properties
        if (!read_string(is, task_name)) return false;
        if (!read_string(is, pool_name)) return false;
        // Skip thread::id deserialization for now
        if (!read_string(is, error_message)) return false;

        int64_t enqueue_time_ms;
        is.read(reinterpret_cast<char*>(&enqueue_time_ms), sizeof(enqueue_time_ms));
        if (is.gcount() != sizeof(enqueue_time_ms)) return false;
        enqueue_time = std::chrono::system_clock::time_point(std::chrono::milliseconds(enqueue_time_ms));

        int64_t start_processing_time_ms;
        is.read(reinterpret_cast<char*>(&start_processing_time_ms), sizeof(start_processing_time_ms));
        if (is.gcount() != sizeof(start_processing_time_ms)) return false;
        start_processing_time = std::chrono::system_clock::time_point(std::chrono::milliseconds(start_processing_time_ms));

        int64_t end_processing_time_ms;
        is.read(reinterpret_cast<char*>(&end_processing_time_ms), sizeof(end_processing_time_ms));
        if (is.gcount() != sizeof(end_processing_time_ms)) return false;
        end_processing_time = std::chrono::system_clock::time_point(std::chrono::milliseconds(end_processing_time_ms));

        is.read(reinterpret_cast<char*>(&wait_duration_ns), sizeof(wait_duration_ns));
        if (is.gcount() != sizeof(wait_duration_ns)) return false;
        is.read(reinterpret_cast<char*>(&execution_duration_ns), sizeof(execution_duration_ns));
        if (is.gcount() != sizeof(execution_duration_ns)) return false;

        func = nullptr; // Ensure func is nullptr after deserialization
        return is.good();
    }

    /**
    * @brief Converts TaskStatus enum to a string representation.
    *
    * @param status The TaskStatus enum value.
    * @return A string representation of the status.
    */
    static std::string taskStatusToString(TaskStatus status) {
        switch (status) {
            case TaskStatus::PENDING: return "PENDING";
            case TaskStatus::PROCESSING: return "PROCESSING";
            case TaskStatus::COMPLETED: return "COMPLETED";
            case TaskStatus::FAILED: return "FAILED";
            case TaskStatus::CANCELLED: return "CANCELLED";
            default: return "UNKNOWN";
        }
    }
};

#endif // TASK_H
