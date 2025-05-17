#ifndef IMAGE_PROCESSING_TYPES_H
#define IMAGE_PROCESSING_TYPES_H

#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <optional>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <stdexcept> // For std::bad_alloc etc.
#include <cstdint>   // For uint8_t

// Define NOMINMAX to prevent windows.h from defining min and max macros
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>     // For MemoryMappedFile
#include "../../Debug/headers/Debug.h" // For printError, printWarning

/**
 * BEGIN --- MemoryMappedFile class ---
 */
class MemoryMappedFile {
public:
    MemoryMappedFile() : hFile(INVALID_HANDLE_VALUE), hMapping(NULL), pMappedView(nullptr), fileSize(0) {
    }

    ~MemoryMappedFile() {
        close();
    }

    MemoryMappedFile(const MemoryMappedFile &) = delete;
    MemoryMappedFile &operator=(const MemoryMappedFile &) = delete;

    MemoryMappedFile(MemoryMappedFile &&other) noexcept
        : hFile(other.hFile), hMapping(other.hMapping), pMappedView(other.pMappedView), fileSize(other.fileSize) {
        other.hFile = INVALID_HANDLE_VALUE;
        other.hMapping = nullptr;
        other.pMappedView = nullptr;
        other.fileSize = 0;
    }

    MemoryMappedFile &operator=(MemoryMappedFile &&other) noexcept {
        if (this != &other) {
            close();
            hFile = other.hFile;
            hMapping = other.hMapping;
            pMappedView = other.pMappedView;
            fileSize = other.fileSize;

            other.hFile = INVALID_HANDLE_VALUE;
            other.hMapping = nullptr;
            other.pMappedView = nullptr;
            other.fileSize = 0;
        }
        return *this;
    }

    bool open(const std::string &filePath) {
        close();
        hFile = CreateFileA(
            filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            printError("MemoryMappedFile: CreateFile failed. Error code: " + std::to_string(GetLastError()));
            return false;
        }
        LARGE_INTEGER liFileSize;
        if (!GetFileSizeEx(hFile, &liFileSize)) {
            printError("MemoryMappedFile: GetFileSizeEx failed. Error code: " + std::to_string(GetLastError()));
            CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; return false;
        }
        fileSize = static_cast<size_t>(liFileSize.QuadPart);
        if (fileSize == 0) {
            printWarning("MemoryMappedFile: Input file is empty. Closing file handle without mapping.");
            CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; return true;
        }
        hMapping = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        if (hMapping == NULL) {
            printError("MemoryMappedFile: CreateFileMapping failed. Error code: " + std::to_string(GetLastError()));
            CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; return false;
        }
        pMappedView = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
        if (pMappedView == nullptr) {
            printError("MemoryMappedFile: MapViewOfFile failed. Error code: " + std::to_string(GetLastError()));
            if (hMapping) CloseHandle(hMapping); hMapping = NULL;
            CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; return false;
        }
        return true;
    }

    void close() {
        if (pMappedView) { UnmapViewOfFile(pMappedView); pMappedView = nullptr; }
        if (hMapping) { CloseHandle(hMapping); hMapping = NULL; }
        if (hFile != INVALID_HANDLE_VALUE) { CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; }
        fileSize = 0;
    }

    const unsigned char *getData() const { return static_cast<const unsigned char *>(pMappedView); }
    size_t getSize() const { return fileSize; }
    bool isOpen() const { return pMappedView != nullptr || (hFile == INVALID_HANDLE_VALUE && fileSize == 0); }

private:
    HANDLE hFile;
    HANDLE hMapping;
    void *pMappedView;
    size_t fileSize;
};
// END --- MemoryMappedFile class ---


/**
 * BitmapImage class
 */
class BitmapImage {
public:
    BitmapImage() : width(0), height(0) {}
    BitmapImage(int width, int height);
    void setData(const std::vector<uint8_t> &data, size_t offset);
    void save(const std::string &filename);
    ~BitmapImage() = default;

    bool serialize(std::ostream &os) const {
        if (!os.good()) return false;
        os.write(reinterpret_cast<const char *>(&width), sizeof(width));
        os.write(reinterpret_cast<const char *>(&height), sizeof(height));
        size_t pixels_size = pixels.size();
        os.write(reinterpret_cast<const char *>(&pixels_size), sizeof(pixels_size));
        if (pixels_size > 0) {
            os.write(reinterpret_cast<const char *>(pixels.data()), pixels_size);
        }
        return os.good();
    }

    bool deserialize(std::istream &is) {
        if (!is.good()) return false;
        is.read(reinterpret_cast<char *>(&width), sizeof(width));
        is.read(reinterpret_cast<char *>(&height), sizeof(height));
        size_t pixels_size = 0;
        is.read(reinterpret_cast<char *>(&pixels_size), sizeof(pixels_size));
        if (is.fail() || pixels_size > (100 * 1024 * 1024)) { // 100MB sanity check
            pixels.clear(); 
            printError("BitmapImage::deserialize: Pixel size read from stream is too large or read failed. Size: " + std::to_string(pixels_size));
            return false;
        }
        if (pixels_size > 0) {
            try {
                pixels.resize(pixels_size);
            } catch (const std::bad_alloc &e) {
                printError("BitmapImage::deserialize: Failed to allocate memory for pixels: " + std::string(e.what()));
                return false;
            }
            is.read(reinterpret_cast<char *>(pixels.data()), pixels_size);
        } else {
            pixels.clear();
        }
        return is.good();
    }

private:
    std::vector<unsigned char> pixels;
    int width;
    int height;
};


/**
 * ImageTaskInternal struct/class
 */
struct ImageTaskInternal {
    std::string filename;
    BitmapImage image;

    ImageTaskInternal() = default;
    ImageTaskInternal(std::string fname, BitmapImage img)
        : filename(std::move(fname)), image(std::move(img)) {}

    bool serialize(std::ostream &os) const {
        if (!os.good()) return false;
        size_t filename_len = filename.length();
        os.write(reinterpret_cast<const char *>(&filename_len), sizeof(filename_len));
        if (filename_len > 0) {
            os.write(filename.c_str(), static_cast<std::streamsize>(filename_len));
        }
        if (!os.good()) { 
            printError("ImageTaskInternal::serialize: Failed to write filename.");
            return false; 
        }
        return image.serialize(os);
    }

    bool deserialize(std::istream &is) {
        if (!is.good()) return false;
        size_t filename_len = 0;
        is.read(reinterpret_cast<char *>(&filename_len), sizeof(filename_len));
        if (is.fail() || filename_len > (1 * 1024 * 1024)) { // 1MB sanity check
             filename.clear(); 
             printError("ImageTaskInternal::deserialize: Filename length read from stream is too large or read failed. Length: " + std::to_string(filename_len));
             return false;
        }
        if (filename_len > 0) {
            try {
                filename.resize(filename_len);
            } catch (const std::bad_alloc &e) {
                printError("ImageTaskInternal::deserialize: Failed to allocate memory for filename: " + std::string(e.what()));
                return false;
            }
            is.read(&filename[0], static_cast<std::streamsize>(filename_len));
        } else {
            filename.clear();
        }
        if (!is.good()) { 
            printError("ImageTaskInternal::deserialize: Failed to read filename string data.");
            return false; 
        }
        return image.deserialize(is);
    }
};


/**
 * ThreadSafeQueueTemplate class
 */
template<typename T>
class ThreadSafeQueueTemplate {
public:
    explicit ThreadSafeQueueTemplate(size_t max_memory_items = 1000, const std::string &spill_path = "")
        : max_in_memory_items_(max_memory_items),
          spill_directory_path_(spill_path),
          spill_file_id_counter_(0),
          shutdown_flag_(false) {
        if (!spill_directory_path_.empty()) {
            try {
                if (!std::filesystem::exists(spill_directory_path_)) {
                    if (!std::filesystem::create_directories(spill_directory_path_)) {
                        printError("ThreadSafeQueue: Failed to create spill directory: " + spill_directory_path_);
                        spill_directory_path_.clear();
                    }
                } else if (!std::filesystem::is_directory(spill_directory_path_)) {
                    printError("ThreadSafeQueue: Spill path exists but is not a directory: " + spill_directory_path_);
                    spill_directory_path_.clear();
                }
            } catch (const std::filesystem::filesystem_error &e) {
                printError("ThreadSafeQueue: Filesystem error with spill directory '" + spill_directory_path_ + "': " + e.what());
                spill_directory_path_.clear();
            }
        }
    }

    ~ThreadSafeQueueTemplate() {
        request_stop();
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
                    printWarning("ThreadSafeQueue: Error deleting spill file '" + file_to_delete + "': " + e.what());
                }
            }
        }
    }

    void push(const T &item) {
        std::optional<T> item_to_serialize_optional;
        std::optional<std::string> spill_filename_optional;

        { // Scope for the first lock
            std::lock_guard<std::mutex> lock(mutex_);
            if (shutdown_flag_) {
                printWarning("ThreadSafeQueue: Attempted to push to a shutting down queue.");
                return;
            }

            if (queue_.size() >= max_in_memory_items_ && !spill_directory_path_.empty()) {
                std::ostringstream filename_stream;
                // Increment counter under lock
                filename_stream << spill_directory_path_ << "/task_" << spill_file_id_counter_++ << ".dat";
                spill_filename_optional = filename_stream.str();
                item_to_serialize_optional = item; // Make a copy for serialization outside lock
            } else {
                queue_.push(item);
                cv_.notify_one();
                return; // Added to in-memory queue, done.
            }
        } // First lock released

        if (spill_filename_optional && item_to_serialize_optional) {
            const std::string& filename = spill_filename_optional.value();
            bool serialization_succeeded = false;
            try {
                std::ofstream file(filename, std::ios::binary | std::ios::trunc);
                if (!file) {
                    printError("ThreadSafeQueue: Failed to open spill file for writing: " + filename);
                    // Item is lost, counter was incremented.
                } else {
                    // Check if the file stream is still good after opening, before serializing
                    if (!item_to_serialize_optional.value().serialize(file)) {
                        printError("ThreadSafeQueue: Failed to serialize item to spill file: " + filename);
                        file.close(); // Close before removing
                        try { if (std::filesystem::exists(filename)) std::filesystem::remove(filename); } catch (const std::filesystem::filesystem_error& e) { printWarning("ThreadSafeQueue: push: Filesystem error removing failed spill file '" + filename + "': " + e.what()); } catch (...) {}
                    } else {
                        file.close(); // Ensure file is closed before adding to queue
                        serialization_succeeded = true;
                    }
                }
            } catch (const std::exception &e) {
                printError("ThreadSafeQueue: Exception while spilling item to disk ('" + filename + "'): " + std::string(e.what()));
                try { if (std::filesystem::exists(filename)) std::filesystem::remove(filename); } catch (const std::filesystem::filesystem_error& e) { printWarning("ThreadSafeQueue: push: Filesystem error removing failed spill file (exception) '" + filename + "': " + e.what()); } catch (...) {}
            }

            if (serialization_succeeded) {
                std::lock_guard<std::mutex> lock(mutex_); // Re-acquire lock
                if (shutdown_flag_) { // Check shutdown again, in case it happened during serialization
                    printWarning("ThreadSafeQueue: Queue shutdown during spill operation for " + filename + ". Cleaning up.");
                    try { if (std::filesystem::exists(filename)) std::filesystem::remove(filename); } catch (const std::filesystem::filesystem_error& e) { printWarning("ThreadSafeQueue: push: Filesystem error removing spill file during shutdown '" + filename + "': " + e.what()); } catch(...) {}
                } else {
                    spilled_task_files_.push(filename);
                    cv_.notify_one();
                }
            }
            // If serialization_succeeded is false, the item is lost. The spill_file_id_counter_ was already incremented.
        }
    }

    // Overload for rvalue references (move semantics)
    void push(T &&item) {
        std::optional<T> item_to_serialize_optional;
        std::optional<std::string> spill_filename_optional;

        { // Scope for the first lock
            std::lock_guard<std::mutex> lock(mutex_);
            if (shutdown_flag_) {
                printWarning("ThreadSafeQueue: Attempted to push to a shutting down queue (move).");
                return;
            }

            if (queue_.size() >= max_in_memory_items_ && !spill_directory_path_.empty()) {
                std::ostringstream filename_stream;
                filename_stream << spill_directory_path_ << "/task_" << spill_file_id_counter_++ << ".dat";
                spill_filename_optional = filename_stream.str();
                item_to_serialize_optional = std::move(item); // Move item for serialization outside lock
            } else {
                queue_.push(std::move(item)); // Move item into in-memory queue
                cv_.notify_one();
                return; // Added to in-memory queue, done.
            }
        }

        if (spill_filename_optional && item_to_serialize_optional) {
            const std::string& filename = spill_filename_optional.value();
            bool serialization_succeeded = false;
            try {
                std::ofstream file(filename, std::ios::binary | std::ios::trunc);
                if (!file) {
                    printError("ThreadSafeQueue: Failed to open spill file for writing (move): " + filename);
                } else {
                    if (!item_to_serialize_optional.value().serialize(file)) { // .value() gives T& from optional<T>
                        printError("ThreadSafeQueue: Failed to serialize item to spill file (move): " + filename);
                        file.close();
                        try { if (std::filesystem::exists(filename)) std::filesystem::remove(filename); } catch (const std::filesystem::filesystem_error& e) { printWarning("ThreadSafeQueue: push(move): Filesystem error removing failed spill file '" + filename + "': " + e.what()); } catch (...) {}
                    } else {
                        file.close();
                        serialization_succeeded = true;
                    }
                }
            } catch (const std::exception &e) {
                printError("ThreadSafeQueue: Exception while spilling item to disk (move) ('" + filename + "'): " + std::string(e.what()));
                try { if (std::filesystem::exists(filename)) std::filesystem::remove(filename); } catch (const std::filesystem::filesystem_error& e) { printWarning("ThreadSafeQueue: push(move): Filesystem error removing failed spill file (exception) '" + filename + "': " + e.what()); } catch (...) {}
            }

            if (serialization_succeeded) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (shutdown_flag_) {
                    printWarning("ThreadSafeQueue: Queue shutdown during spill operation for (move) " + filename + ". Cleaning up.");
                    try { if (std::filesystem::exists(filename)) std::filesystem::remove(filename); } catch (const std::filesystem::filesystem_error& e) { printWarning("ThreadSafeQueue: push(move): Filesystem error removing spill file during shutdown '" + filename + "': " + e.what()); } catch(...) {}
                } else {
                    spilled_task_files_.push(filename);
                    cv_.notify_one();
                }
            }
        }
    }

    [[nodiscard]] bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty() && spilled_task_files_.empty();
    }

    [[nodiscard]] std::optional<T> try_pop(std::chrono::milliseconds timeout) {
        std::optional<std::string> filename_to_load_optional;
        T item_from_memory; 
        bool memory_item_popped = false;

        { // Scope for the unique_lock
            std::unique_lock<std::mutex> lock(mutex_);
            if (!cv_.wait_for(lock, timeout, [this] {
                return shutdown_flag_ || !queue_.empty() || !spilled_task_files_.empty();
            })) {
                return std::nullopt; // Timeout
            }

            if (shutdown_flag_ && queue_.empty() && spilled_task_files_.empty()) {
                return std::nullopt; // Shutdown and empty
            }

            if (!queue_.empty()) {
                item_from_memory = std::move(queue_.front());
                queue_.pop();
                memory_item_popped = true;
            } else if (!spilled_task_files_.empty()) {
                filename_to_load_optional = spilled_task_files_.front();
                spilled_task_files_.pop(); // Pop filename under lock
            } else {
                return std::nullopt; // Should be covered by cv_wait condition or shutdown check
            }
        } // unique_lock released here

        if (memory_item_popped) {
            return std::move(item_from_memory);
        }

        if (filename_to_load_optional) {
            const std::string& filename = filename_to_load_optional.value();
            T item_from_file;
            bool deserialization_succeeded = false;
            try {
                std::ifstream file(filename, std::ios::binary);
                if (!file) {
                    printError("ThreadSafeQueue: Failed to open spill file for reading: " + filename);
                } else {
                    if (!item_from_file.deserialize(file)) {
                        printError("ThreadSafeQueue: Failed to deserialize item from spill file: " + filename);
                    } else {
                        deserialization_succeeded = true;
                    }
                    // file is closed by RAII (ifstream destructor)
                }
            } catch (const std::exception &e) {
                printError("ThreadSafeQueue: Exception while loading item from spill file ('" + filename + "'): " + std::string(e.what()));
            }

            // Always attempt to delete the spill file, regardless of deserialization success
            try {
                if (std::filesystem::exists(filename)) {
                    std::filesystem::remove(filename);
                }
            } catch (const std::filesystem::filesystem_error &e) {
                printWarning("ThreadSafeQueue: try_pop: Error deleting spill file '" + filename + "': " + e.what());
            }

            if (deserialization_succeeded) {
                return item_from_file;
            } else {
                return std::nullopt; // Deserialization failed or file couldn't be opened
            }
        }
        return std::nullopt; // Fallback, should ideally not be reached if logic prior is exhaustive
    }

    void request_stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_flag_ = true;
        cv_.notify_all();
    }

private:
    std::queue<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    size_t max_in_memory_items_;
    std::string spill_directory_path_;
    std::queue<std::string> spilled_task_files_;
    uint64_t spill_file_id_counter_;
    bool shutdown_flag_;
};

#endif // IMAGE_PROCESSING_TYPES_H
