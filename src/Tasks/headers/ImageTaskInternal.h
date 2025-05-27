#ifndef IMAGE_TASK_INTERNAL_H
#define IMAGE_TASK_INTERNAL_H

#include <string>
#include <chrono>
#include <utility>
#include "_Task.h"
#include "../../Debug/headers/Debug.h"
#include <Image/headers/BitmapImage.h>


/**
 * ImageTaskInternal struct
 *
 * Represents an internal image processing task with serialization capabilities.
 * Contains a task ID, filename, BitmapImage, creation timestamp, and status.
 * Inherits from the Task for common task properties and behaviors.
 */
struct ImageTaskInternal : Task {
    std::string filename;
    BitmapImage image;

    /**
     * Default constructor is deleted - a task_id is always required
     */
    ImageTaskInternal(std::string filename, BitmapImage img, std::string id)
        : Task(std::move(id)),
          filename(std::move(filename)),
          image(std::move(img)) {
        // Set self_destruct flag if the image is invalid (already destroyed)
        if (image.getWidth() == 0 || image.getHeight() == 0) {
            this->self_destruct = true;
        }
    }

    /**
     * Move constructor for optimized transfers
     */
    ImageTaskInternal(ImageTaskInternal &&other) noexcept
        : Task(std::move(other)),
          filename(std::move(other.filename)),
          image(std::move(other.image)) {
        // Self-destruct if the image was already destroyed or invalid
        if (image.getWidth() == 0 || image.getHeight() == 0) {
            this->self_destruct = true;
        }
    }

    /**
     * Move assignment operator for optimized transfers
     */
    ImageTaskInternal &operator=(ImageTaskInternal &&other) noexcept {
        if (this != &other) {
            Task::operator=(std::move(other));
            filename = std::move(other.filename);
            image = std::move(other.image);
            
            // Self-destruct if the image was already destroyed or invalid
            if (image.getWidth() == 0 || image.getHeight() == 0) {
                this->self_destruct = true;
            }
        }
        return *this;
    }

    /**
     * Copy constructor (explicitly defined for clarity)
     */
    ImageTaskInternal(const ImageTaskInternal &other) 
        : Task(other),
          filename(other.filename),
          image(other.image) {
    }

    /**
     * Copy assignment operator (explicitly defined for clarity)
     */
    ImageTaskInternal &operator=(const ImageTaskInternal &other) {
        if (this != &other) {
            Task::operator=(other);
            filename = other.filename;
            image = other.image;
        }
        return *this;
    }

    /**
    * Destructor - ensures proper cleanup of resources
    */
    ~ImageTaskInternal() override {
        // Clear and release memory for the filename string
        filename.clear();
        filename.shrink_to_fit();

        // Clear image data
        image.~BitmapImage();
    }

    /**
     * Get the precise memory usage of this object with detailed breakdown
     * Provides a more accurate measurement of all memory used by this object
     *
     * @return Detailed memory usage in bytes
     */
    [[nodiscard]] size_t getMemoryUsage() const override {
        // Calculate base size of the object
        size_t total_size = sizeof(*this);

        // Typical SSO threshold is around 15-23 bytes depending on implementation
        constexpr size_t estimated_sso_threshold = 15;

        // For filename
        if (filename.length() > estimated_sso_threshold || filename.capacity() > estimated_sso_threshold) {
            // String is likely heap-allocated
            total_size += filename.capacity() + 1; // +1 for null terminator
        }

        // For task_id
        if (task_id.length() > estimated_sso_threshold || task_id.capacity() > estimated_sso_threshold) {
            total_size += task_id.capacity() + 1;
        }

        // For error_message from Task base class
        if (error_message.length() > estimated_sso_threshold || error_message.capacity() > estimated_sso_threshold) {
            total_size += error_message.capacity() + 1;
        }

        // For task_name from Task base class
        if (task_name.length() > estimated_sso_threshold || task_name.capacity() > estimated_sso_threshold) {
            total_size += task_name.capacity() + 1;
        }

        // For pool_name from Task base class
        if (pool_name.length() > estimated_sso_threshold || pool_name.capacity() > estimated_sso_threshold) {
            total_size += pool_name.capacity() + 1;
        }

        // Add the precise memory usage of the image
        total_size += image.get_allocated_memory_size();

        // Account for any memory used by the function object if present
        if (func) {
            // This is an approximation as we can't know the exact size of the captured variables
            // Assuming a typical std::function implementation with small buffer optimization
            constexpr size_t estimated_function_size = 64;
            total_size += estimated_function_size;
        }

        return total_size;
    }

    /**
     * Reserve memory for strings to avoid reallocations
     *
     * @param filename_capacity Expected capacity for filename
     * @param task_id_capacity Expected capacity for task_id
     */
    void reserveMemory(size_t filename_capacity, size_t task_id_capacity) {
        if (filename_capacity > filename.capacity()) {
            filename.reserve(filename_capacity);
        }
        if (task_id_capacity > task_id.capacity()) {
            task_id.reserve(task_id_capacity);
        }
    }

    /**
     * Release heavy resources to reduce memory usage
     */
    void releaseHeavyResources(const std::string &spill_path) override {
        image.clear();
        setSpillFile(spill_path);
    }

    /**
     * Serialize the object to the output stream
     *
     * @param os Output stream to serialize to
     * @return True if serialization succeeded, false otherwise
    */
    bool serialize(std::ostream &os) const override {
        // Serialize base Task members first
        if (!Task::serialize(os)) {
            std::cerr << "[ERROR] ImageTaskInternal::serialize: Base Task::serialize failed." << std::endl;
            return false;
        }
        if (!Task::write_string(os, filename)) {
            // Using Task::write_string as the helper
            std::cerr << "[ERROR] ImageTaskInternal::serialize: Task::write_string for filename failed." << std::endl;
            return false;
        }
        // Serialize BitmapImage
        if (!image.serialize(os)) {
            std::cerr << "[ERROR] ImageTaskInternal::serialize: image.serialize(os) failed." << std::endl;
            return false;
        }
        return os.good(); // Final check on stream state
    }

    /**
     * Deserialize the object from the input stream
     *
     * @param is Input stream to deserialize from
     * @return True if deserialization succeeded, false otherwise
     */
    bool deserialize(std::istream &is) override {
        if (!is.good()) return false;

        // Deserialize base Task members first
        if (!Task::deserialize(is)) {
            printError("ImageTaskInternal::deserialize: Base Task deserialization failed.");
            return false;
        }

        // Helper to read string from stream (consistent with Task::read_string)
        auto read_string_local = [&](std::istream &input_stream, std::string &s) {
            size_t len;
            input_stream.read(reinterpret_cast<char *>(&len), sizeof(len));
            if (input_stream.gcount() != sizeof(len)) return false;
            if (len > 0) {
                // Basic sanity check for length to prevent excessive allocation
                if (len > 1024 * 1024 * 10) {
                    // Max 10MB for a filename string, adjust as needed
                    printError("ImageTaskInternal::deserialize: Excessive string length detected for filename.");
                    return false;
                }
                s.resize(len);
                input_stream.read(&s[0], static_cast<std::streamsize>(len));
                if (input_stream.gcount() != static_cast<std::streamsize>(len)) return false;
            }
            return input_stream.good();
        };

        // Deserialize filename
        if (!read_string_local(is, filename)) {
            printError("ImageTaskInternal::deserialize: Failed to read filename.");
            return false;
        }
        // Deserialize BitmapImage
        if (!image.deserialize(is)) {
            printError("ImageTaskInternal::deserialize: BitmapImage deserialization failed.");
            return false;
        }

        return is.good();
    }
};

#endif // IMAGE_TASK_INTERNAL_H
