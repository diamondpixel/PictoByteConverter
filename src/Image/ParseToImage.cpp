/*
 * ParseToImage.cpp
 * This file contains the implementation for converting binary data into bitmap images
 * and back. It provides functionality to split large files into multiple chunks,
 * encode them as bitmap images, and process them with resource management.
 */

#include <string>        // For string operations and filename handling
#include <fstream>       // For file input/output operations
#include <cmath>         // For mathematical functions like sqrt
#include <iostream>      // For standard I/O operations
#include <vector>        // For dynamic arrays to store binary data
#include <filesystem>    // For path and file system operations
#include <thread>        // For multi-threading support
#include <mutex>         // For thread synchronization primitives
#include <optional>      // For optional return values
#include <queue>         // For queue data structure
#include "headers/ParseToImage.h"       // Header defining the interface
#include "headers/ResourceManager.h"    // Resource management utilities
#include "../Debug/headers/Debug.h"     // Debugging and logging utilities

/**
 * BitmapImage class is a specialized implementation for creating bitmap images
 * from binary data with specific memory management considerations.
 * It provides functionality to create, manipulate, and save bitmap images
 * with embedded data for storage and later retrieval.
 */
class BitmapImage {
public:
    /**
     * Constructs a BitmapImage with the specified dimensions
     * @param width Width of the image in pixels
     * @param height Height of the image in pixels
     */
    BitmapImage(int width, int height);

    /**
     * Sets binary data into the pixel buffer at the specified offset
     * @param data Binary data to be embedded in the image
     * @param offset Starting position in the pixel buffer
     */
    void setData(const std::vector<uint8_t> &data, size_t offset);

    /**
     * Saves the image to a BMP file
     * @param filename Path where the BMP file will be saved
     */
    void save(const std::string &filename);

    /** Default destructor */
    ~BitmapImage() = default;

private:
    /** Raw pixel data (RGB format, 3 bytes per pixel) */
    std::vector<unsigned char> pixels;

    /** Width of the image in pixels */
    int width;

    /** Height of the image in pixels */
    int height;
};

/**
 * Internal task structure used for passing image data to the writer thread.
 * Contains both the target filename and the image data to be written.
 */
struct ImageTaskInternal {
    /** Path where the image should be saved */
    std::string filename;

    /** The bitmap image data to save */
    BitmapImage image;
};

/**
 * Thread-safe queue implementation for managing producer-consumer pattern
 * between chunk processing and image writing threads. Provides synchronized
 * access to queue operations to prevent data races.
 *
 * @tparam T Type of items stored in the queue
 */
template<typename T>
class ThreadSafeQueueTemplate {
public:
    /**
     * Adds an item to the queue in a thread-safe manner
     * @param item The item to add to the queue
     */
    void push(const T &item) {
        std::lock_guard<std::mutex> lock(mutex); // Lock the mutex during the operation
        queue.push(item); // Add the item to the queue
        cv.notify_one(); // Notify one waiting thread
    }

    /**
     * Checks if the queue is empty
     * @return True if the queue is empty, false otherwise
     */
    [[nodiscard]] bool empty() const {
        std::lock_guard<std::mutex> lock(mutex); // Lock the mutex during the check
        return queue.empty(); // Return the queue state
    }

    /**
     * Tries to pop an item from the queue with timeout
     * @param timeout Maximum duration to wait for an item
     * @return An optional containing the popped item or nullopt if timeout expired
     */
    [[nodiscard]] std::optional<T> try_pop(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex); // Unique lock for condition variable

        // Wait for the condition or timeout
        if (cv.wait_for(lock, timeout, [this] { return !queue.empty(); })) {
            T item = std::move(queue.front()); // Get the front item
            queue.pop(); // Remove it from the queue
            return item; // Return the item
        }

        return std::nullopt; // Return empty optional if timeout expired
    }

private:
    /** Underlying queue to store the items */
    std::queue<T> queue;

    /** Mutex for synchronizing access to the queue */
    mutable std::mutex mutex;

    /** Condition variable for signaling queue state changes */
    std::condition_variable cv;
};

/**
 * Calculates optimal rectangular dimensions for a bitmap that needs to store a specific
 * amount of data while respecting maximum file size constraints.
 *
 * @param total_bytes Total bytes of data to store in the image
 * @param bytes_per_pixel Number of bytes per pixel (typically 3 for RGB)
 * @param bmp_header_size Size of the BMP header in bytes
 * @param max_size_bytes Maximum allowed file size in bytes
 * @param aspect_ratio Desired width:height ratio (default 1.0 for square)
 * @return A pair of width and height dimensions
 */
std::pair<size_t, size_t> calculateOptimalRectDimensions(size_t total_bytes, size_t bytes_per_pixel,
                                                         size_t bmp_header_size,
                                                         size_t max_size_bytes, float aspect_ratio = 1.0f) {
    // Add buffer for ensuring all data fits with no truncation
    // Extra 500 pixels provide safety margin for rounding errors
    size_t total_pixels = (total_bytes + bytes_per_pixel - 1) / bytes_per_pixel + 500;

    // Calculate initial dimensions based on desired aspect ratio
    auto width = static_cast<size_t>(std::sqrt(static_cast<double>(total_pixels * aspect_ratio)));
    auto height = static_cast<size_t>(width / aspect_ratio);

    // Ensure dimensions provide enough pixels for all data
    // This loop increases dimensions until they can accommodate all data
    while (width * height < total_pixels) {
        width++;
        height = static_cast<size_t>(width / aspect_ratio);
    }

    // Enforce minimum dimensions for practical reasons
    // Images that are too small may not be practical to work with
    constexpr size_t min_dimension = 64;
    if (width < min_dimension) width = min_dimension;
    if (height < min_dimension) height = min_dimension;

    // Cap at BMP format limits (65,535 x 65,535)
    // This is the maximum size supported by the BMP format
    constexpr size_t max_bitmap_dimension = 65535;
    if (width > max_bitmap_dimension) width = max_bitmap_dimension;
    if (height > max_bitmap_dimension) height = max_bitmap_dimension;

    // Calculate row padding and actual file size
    // BMP rows must be padded to multiple of 4 bytes
    size_t row_bytes = width * bytes_per_pixel;
    size_t padding = (4 - (row_bytes % 4)) % 4;
    size_t actual_image_size = bmp_header_size + (height * (row_bytes + padding));

    // Shrink dimensions if image would exceed max size
    // This loop ensures the final image is within size limits
    while (actual_image_size > max_size_bytes && width > min_dimension && height > min_dimension) {
        if (width > height) {
            width--;
        } else {
            height--;
        }
        row_bytes = width * bytes_per_pixel;
        padding = (4 - (row_bytes % 4)) % 4;
        actual_image_size = bmp_header_size + (height * (row_bytes + padding));
    }

    return {width, height};
}

/**
 * Optimizes dimensions specifically for the last image, which may have different
 * requirements than regular chunks. The last image might contain metadata or
 * have special handling requirements.
 *
 * @param total_bytes Total bytes of data to store
 * @param bytes_per_pixel Number of bytes per pixel
 * @param metadata_size Size of metadata to be included
 * @param bmp_header_size Size of the BMP header in bytes
 * @param aspect_ratio Desired width:height ratio
 * @return A pair of width and height dimensions
 */
std::pair<size_t, size_t> optimizeLastImageDimensions(size_t total_bytes, size_t bytes_per_pixel, size_t metadata_size,
                                                      size_t bmp_header_size, float aspect_ratio = 1.0f) {
    // For last chunk, we need to account for both metadata and file data
    size_t total_data_size = metadata_size + total_bytes;
    size_t total_pixels_needed = (total_data_size + bytes_per_pixel - 1) / bytes_per_pixel;

    // Calculate initial dimensions based on desired aspect ratio
    auto width = static_cast<size_t>(std::sqrt(static_cast<double>(total_pixels_needed * aspect_ratio)));
    auto height = static_cast<size_t>(width / aspect_ratio);

    // Ensure dimensions provide enough pixels
    // Increase dimensions until they can fit all required data
    while (width * height < total_pixels_needed) {
        width++;
        height = static_cast<size_t>(width / aspect_ratio);
    }

    // Apply size constraints
    // Minimum and maximum dimension limits
    constexpr size_t min_dimension = 64;
    if (width < min_dimension) width = min_dimension;
    if (height < min_dimension) height = min_dimension;

    constexpr size_t max_bitmap_dimension = 65535;
    if (width > max_bitmap_dimension) width = max_bitmap_dimension;
    if (height > max_bitmap_dimension) height = max_bitmap_dimension;

    return {width, height};
}

/**
 * Reads a segment of a file with memory management handled by ResourceManager.
 * Attempts to allocate memory for the requested segment, and falls back to
 * smaller allocations if necessary.
 *
 * @param file Open input file stream
 * @param start_pos Starting position in the file
 * @param length Number of bytes to read
 * @return Vector containing the read data, or empty on failure
 */
std::vector<uint8_t> readFileSegment(std::ifstream &file, size_t start_pos, size_t length) {
    // Get singleton instance of the resource manager
    auto &resManager = ResourceManager::getInstance();

    // Try to allocate the requested memory
    bool memory_allocated = resManager.allocateMemory(length);

    // If full allocation failed, try with reduced size
    if (!memory_allocated) {
        printWarning("Memory allocation for " + std::to_string(length / (1024 * 1024)) +
                     " MB failed, reducing buffer size");
        // Try with half the size if full allocation failed
        length /= 2;
        memory_allocated = resManager.allocateMemory(length);

        // If still can't allocate, return empty result
        if (!memory_allocated) {
            printError("Could not allocate memory for file segment");
            return {};
        }
    }

    // Create buffer and read data from file
    std::vector<uint8_t> buffer(length);

    file.seekg(start_pos, std::ios::beg);
    file.read(reinterpret_cast<char *>(buffer.data()), length);

    // If EOF reached, resize buffer to actual bytes read and free unused memory
    if (file.eof()) {
        auto actual_size = file.gcount();
        buffer.resize(actual_size);
        resManager.freeMemory(length - actual_size);
    }

    return buffer;
}

/**
 * Processes a single chunk of the input file, converting it to a bitmap image.
 * Each chunk includes metadata about the original file and chunk position.
 * This function handles the core logic of reading a file segment, adding
 * metadata headers, and preparing the image for storage.
 *
 * @param chunk_index Index of the current chunk (0-based)
 * @param chunk_size Size of each chunk in bytes
 * @param total_chunks Total number of chunks in the file
 * @param original_file_size Original file size in bytes
 * @param input_file Path to the input file
 * @param output_base Base path for output images
 * @param task_queue Thread-safe queue for writer tasks
 * @param max_image_file_size Maximum size in bytes for output image files
 */
void processChunk(int chunk_index, size_t chunk_size, size_t total_chunks, size_t original_file_size,
                  const std::string &input_file, const std::string &output_base,
                  ThreadSafeQueueTemplate<ImageTaskInternal> &task_queue, size_t max_image_file_size) {
    // Get the current debug mode setting
    auto localDebugMode = getDebugMode();

    // Generate output path for this chunk's image
    // Format: basename_XofY.bmp where X is current chunk and Y is total chunks
    auto formatted_path = output_base + "_" + std::to_string(chunk_index + 1) + "of" + std::to_string(total_chunks) +
                          ".bmp";

    // Log the chunk processing start
    printHighlight("Processing chunk " + std::to_string(chunk_index + 1) + " of " + std::to_string(total_chunks));
    printFilePath("Chunk output: " + formatted_path);

    // Calculate chunk boundaries
    // Determine start position and actual size of this chunk
    auto start_pos = chunk_index * chunk_size;
    auto actual_chunk_size = (chunk_index == total_chunks - 1) ? (original_file_size - start_pos) : chunk_size;

    try {
        // Open the input file in binary mode
        std::ifstream input(input_file, std::ios::binary);
        if (!input.is_open()) {
            throw std::runtime_error("Failed to open input file: " + input_file);
        }

        // Read the chunk data using resource-managed function
        auto chunk_data = readFileSegment(input, start_pos, actual_chunk_size);
        if (chunk_data.empty()) {
            throw std::runtime_error("Failed to read data from file: " + input_file);
        }

        // Create metadata header for this chunk
        // This will store information about the file and chunk
        std::vector<uint8_t> metadata;

        // 1. Header length (3 bytes) - fixed at 48 bytes
        // This allows the parser to know how much metadata to read
        constexpr uint32_t header_length = 48;
        metadata.push_back((header_length >> 16) & 0xFF); // High byte
        metadata.push_back((header_length >> 8) & 0xFF); // Middle byte
        metadata.push_back(header_length & 0xFF); // Low byte

        // 2. Filename length (2 bytes)
        // Store the length of the original filename
        auto filename = std::filesystem::path(input_file).filename().string();
        auto filename_length = static_cast<uint16_t>(filename.length());
        metadata.push_back((filename_length >> 8) & 0xFF); // High byte
        metadata.push_back(filename_length & 0xFF); // Low byte

        // 3. Store original filename
        // Each character is converted to a byte
        for (char c: filename) {
            metadata.push_back(static_cast<uint8_t>(c));
        }

        // 4. Data size (10 bytes, zero-padded string)
        // Stores the actual chunk size as a fixed-width string
        auto data_size_str = std::to_string(chunk_data.size());
        data_size_str = std::string(10 - data_size_str.length(), '0') + data_size_str;
        for (char c: data_size_str) {
            metadata.push_back(static_cast<uint8_t>(c));
        }

        // 5. Current chunk number (4 bytes, zero-padded string)
        // Stores the 1-based chunk index (user-friendly numbering)
        auto current_chunk_str = std::to_string(chunk_index + 1);
        current_chunk_str = std::string(4 - current_chunk_str.length(), '0') + current_chunk_str;
        for (char c: current_chunk_str) {
            metadata.push_back(static_cast<uint8_t>(c));
        }

        // 6. Total chunks (4 bytes, zero-padded string)
        // Total number of chunks needed to reconstruct the file
        auto total_chunks_str = std::to_string(total_chunks);
        total_chunks_str = std::string(4 - total_chunks_str.length(), '0') + total_chunks_str;
        for (char c: total_chunks_str) {
            metadata.push_back(static_cast<uint8_t>(c));
        }

        // 7. Add padding to reach exactly header_length bytes
        // This ensures the header has a consistent size
        auto current_metadata_size = 3 + 2 + filename_length + 10 + 4 + 4;
        if (current_metadata_size < header_length) {
            auto padding_needed = header_length - current_metadata_size;
            for (size_t i = 0; i < padding_needed; i++) {
                metadata.push_back(0);
            }
        }

        // Constants for BMP format
        constexpr size_t bytes_per_pixel = 3; // RGB format (3 bytes)
        constexpr size_t bmp_header_size = 54; // Standard BMP header size

        // Determine appropriate dimensions based on chunk data size
        // Different calculation for last chunk vs. regular chunks
        std::pair<size_t, size_t> dimensions;
        if (chunk_index == total_chunks - 1) {
            // Last chunk may have special handling
            dimensions = optimizeLastImageDimensions(chunk_data.size(), bytes_per_pixel,
                                                     metadata.size(), bmp_header_size);
        } else {
            dimensions = calculateOptimalRectDimensions(
                metadata.size() + chunk_data.size(),
                bytes_per_pixel, bmp_header_size,
                max_image_file_size
            );
        }

        // Extract dimensions
        auto [width, height] = dimensions;

        // Log the dimensions for tracking/debugging
        printMessage("Chunk " + std::to_string(chunk_index + 1) + " image dimensions: " +
                     std::to_string(width) + " x " + std::to_string(height) + " pixels");

        // Calculate memory requirements for the bitmap
        auto total_pixels = width * height;
        auto bitmap_memory_size = total_pixels * bytes_per_pixel;

        // Request memory allocation from the resource manager
        auto memory_allocated = ResourceManager::getInstance().allocateMemory(bitmap_memory_size);
        if (!memory_allocated) {
            throw std::runtime_error("Not enough memory to create image");
        }

        // Create and populate the bitmap image
        // First with metadata, then with actual file data
        BitmapImage image(width, height);
        image.setData(metadata, 0);
        image.setData(chunk_data, metadata.size());

        // Queue the image for saving by the writer thread
        // This allows processing to continue without waiting for disk I/O
        task_queue.push(ImageTaskInternal{
            formatted_path,
            image
        });

        // Log successful processing
        printStatus("Processed chunk " + std::to_string(chunk_index + 1) + " of " +
                    std::to_string(total_chunks) + " (" +
                    std::to_string(chunk_data.size()) + " bytes)");
    } catch (const std::exception &e) {
        // Log error and propagate exception
        printError("Error processing chunk " + std::to_string(chunk_index + 1) + ": " + e.what());
        throw;
    }
}

/**
 * Saves an image task to disk as a BMP file.
 * This function encapsulates the process of saving an image to disk,
 * with appropriate error handling.
 *
 * @param task The image task containing the filename and bitmap image
 */
void saveImage(const ImageTaskInternal &task) {
    try {
        // Get the image from the task
        BitmapImage img = task.image;

        // Save to the specified path
        img.save(task.filename);

        // Log successful save
        printStatus("Saved image: " + task.filename);
    } catch (const std::exception &e) {
        // Log any errors during the save operation
        printError("Failed to save image: " + std::string(e.what()));
    }
}

/**
 * Background thread that waits for image tasks and saves them to disk.
 * Continues running until explicitly terminated and queue is empty.
 * This thread is responsible for handling all disk I/O operations to save images.
 *
 * @param task_queue Thread-safe queue containing image tasks to process
 * @param should_terminate Atomic flag indicating whether the thread should exit
 */
void imageWriterThread(ThreadSafeQueueTemplate<ImageTaskInternal> &task_queue, std::atomic<bool> &should_terminate) {
    try {
        // Continue running until told to terminate AND queue is empty
        while (!should_terminate || !task_queue.empty()) {
            // Try to get an item from the queue, with timeout
            auto task = task_queue.try_pop(std::chrono::milliseconds(100));
            if (task) {
                // If a task was retrieved, save the image
                saveImage(*task);
            }
            // If no task was retrieved, loop will continue checking
        }
    } catch (const std::exception &e) {
        // Log any thread errors
        printError("Image writer thread error: " + std::string(e.what()));
    }
}

/**
 * Main function that handles the conversion of a file to a series of BMP images.
 * The file is split into chunks, each chunk is processed in parallel, and
 * the resulting BMP images contain the original file data.
 *
 * @param input_file Path to the file to convert
 * @param output_base Base name for output BMP files
 * @param maxChunkSizeMB Maximum size of each chunk in megabytes
 * @param maxThreads Maximum number of processing threads to use
 * @param maxMemoryMB Maximum memory usage in megabytes
 * @return True if conversion was successful, false otherwise
 */
bool parseToImage(const std::string &input_file, const std::string &output_base, int maxChunkSizeMB, int maxThreads,
                  int maxMemoryMB) {
    try {
        // Initialize and configure resource manager
        auto &resManager = ResourceManager::getInstance();

        // Configure thread pool size
        if (maxThreads > 0) {
            // Use explicitly specified thread count
            resManager.setMaxThreads(maxThreads);
        } else {
            // Default to half of available CPU cores if not specified
            // This provides a balanced approach to multithreading
            auto default_threads = std::max(1u, std::thread::hardware_concurrency() / 2);
            resManager.setMaxThreads(default_threads);
        }

        // Configure memory limits
        if (maxMemoryMB > 0) {
            // Use explicitly specified memory limit
            resManager.setMaxMemory(static_cast<size_t>(maxMemoryMB) * 1024 * 1024);
        } else {
            // Default to 1GB if not specified
            // This is a reasonable default for most systems
            constexpr size_t DEFAULT_MEMORY_LIMIT = 1024 * 1024 * 1024; // 1GB
            resManager.setMaxMemory(DEFAULT_MEMORY_LIMIT);
        }

        // Log the configured resource limits
        printMessage("Resource limits: " + std::to_string(resManager.getMaxThreads()) +
                     " threads, " + std::to_string(resManager.getMaxMemory() / (1024 * 1024)) + " MB memory");

        // Use references to avoid unnecessary copies
        const std::string &cleanInputPath = input_file;
        const std::string &cleanOutputPath = output_base;

        // Validate input file
        if (!std::filesystem::exists(cleanInputPath)) {
            printError("Input file does not exist: " + cleanInputPath);
            return false;
        }

        // Create output directory if needed
        std::filesystem::path output_path(cleanOutputPath);
        std::filesystem::path output_dir = output_path.parent_path();
        if (!output_dir.empty() && !std::filesystem::exists(output_dir)) {
            // Create the directory structure recursively
            std::filesystem::create_directories(output_dir);
            printStatus("Created output directory: " + output_dir.string());
        }

        // Get file size and validate
        auto file_size = std::filesystem::file_size(cleanInputPath);
        if (file_size == 0) {
            printError("Input file is empty: " + cleanInputPath);
            return false;
        }

        // Calculate chunk size and number of chunks
        // Convert MB to bytes and ensure chunk size doesn't exceed file size
        auto chunk_size = static_cast<size_t>(maxChunkSizeMB) * 1024 * 1024;
        chunk_size = std::min(chunk_size, file_size);

        // Calculate how many chunks we'll need based on chunk size
        auto total_chunks = (file_size + chunk_size - 1) / chunk_size;

        // Log input/output information
        printFilePath("Input: " + cleanInputPath);
        printFilePath("Output: " + cleanOutputPath);
        printStats("File size: " + std::to_string(file_size) + " bytes, Chunks: " + std::to_string(total_chunks) +
                   ", Chunk size: " + std::to_string(chunk_size / (1024 * 1024)) + " MB");

        // Create task queue for image processing
        // This queue will hold tasks that need to be written to disk
        ThreadSafeQueueTemplate<ImageTaskInternal> task_queue;

        // Start the background image writer thread
        // This thread will run concurrently with processing threads
        std::atomic<bool> should_terminate(false);
        std::thread writer_thread(imageWriterThread, std::ref(task_queue), std::ref(should_terminate));

        // Calculate maximum size for output image files
        // Default is 100MB, but we may need to reduce based on available memory
        constexpr size_t DEFAULT_MAX_IMAGE_SIZE = 100 * 1024 * 1024; // 100MB
        auto max_image_file_size = std::min(
            DEFAULT_MAX_IMAGE_SIZE,
            resManager.getMaxMemory() / (total_chunks > 0 ? total_chunks : 1) / 2
        );

        // Process each chunk in parallel using the resource manager
        for (size_t i = 0; i < total_chunks; ++i) {
            // Log which chunk we're processing
            auto chunkInfo = "Chunk " + std::to_string(i + 1) + " of " + std::to_string(total_chunks);
            printProcessingStep(chunkInfo);

            // Run the chunk processing in a worker thread
            // The resource manager handles thread allocation and management
            resManager.runWithThread(processChunk, i, chunk_size, total_chunks, file_size,
                                     input_file, output_base, std::ref(task_queue), max_image_file_size);

            // Brief pause to allow thread initialization
            // This helps prevent thread creation overhead causing resource contention
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Wait for all processing to complete
        // This ensures all chunks have been processed before we continue
        resManager.waitForAllThreads();

        // Signal the writer thread to terminate and wait for it
        // Only after processing is complete do we terminate the writer thread
        should_terminate = true;
        if (writer_thread.joinable()) {
            writer_thread.join();
        }

        // Log success and return
        printSuccess("File processing completed successfully");
        return true;
    } catch (const std::exception &e) {
        // Handle any exceptions and log the error
        printError("Error in parseToImage: " + std::string(e.what()));
        return false;
    }
}

/**
 * Constructor for BitmapImage class.
 * Initializes a new bitmap image with the specified dimensions.
 *
 * @param width Width of the image in pixels
 * @param height Height of the image in pixels
 */
BitmapImage::BitmapImage(int width, int height) {
    this->width = width;
    this->height = height;
    pixels.resize(width * height * 3, 0); // 3 bytes per pixel (RGB)
}

/**
 * Sets binary data into the pixel buffer at the specified offset.
 * This method is used to embed file data and metadata into the image pixels.
 *
 * @param data Binary data to be embedded in the image
 * @param offset Starting position in the pixel buffer
 */
void BitmapImage::setData(const std::vector<uint8_t> &data, size_t offset) {
    // Calculate how many bytes we can actually copy
    auto bytesToCopy = std::min(data.size(), pixels.size() - offset);

    // Copy the data into the pixel buffer
    std::copy_n(data.begin(), bytesToCopy, pixels.begin() + offset);
}

/**
 * Saves the image to a BMP file.
 * This function handles the low-level details of writing the image data
 * to a file in the BMP format.
 *
 * @param filename Path where the BMP file will be saved
 */
void BitmapImage::save(const std::string &filename) {
    try {
        // Open the output file in binary mode
        std::ofstream outFile(filename, std::ios::binary);
        if (!outFile) {
            printError("Cannot open file for writing: " + filename);
            return;
        }

        // BMP file header
        unsigned char fileHeader[14] = {
            'B', 'M',
            0, 0, 0, 0,
            0, 0, 0, 0,
            54, 0, 0, 0
        };

        // BMP info header
        unsigned char infoHeader[40] = {
            40, 0, 0, 0,
            0, 0, 0, 0,
            0, 0, 0, 0,
            1, 0,
            24, 0,
            0, 0, 0, 0,
            0, 0, 0, 0,
            0, 0, 0, 0,
            0, 0, 0, 0
        };

        // Calculate padding size for each row
        int paddingSize = (4 - (width * 3) % 4) % 4;

        // Calculate total file size
        int fileSize = 14 + 40 + (width * 3 + paddingSize) * height;

        // Set file size in the file header
        fileHeader[2] = static_cast<unsigned char>(fileSize);
        fileHeader[3] = static_cast<unsigned char>(fileSize >> 8);
        fileHeader[4] = static_cast<unsigned char>(fileSize >> 16);
        fileHeader[5] = static_cast<unsigned char>(fileSize >> 24);

        // Set image dimensions in the info header
        infoHeader[4] = static_cast<unsigned char>(width);
        infoHeader[5] = static_cast<unsigned char>(width >> 8);
        infoHeader[6] = static_cast<unsigned char>(width >> 16);
        infoHeader[7] = static_cast<unsigned char>(width >> 24);

        infoHeader[8] = static_cast<unsigned char>(height);
        infoHeader[9] = static_cast<unsigned char>(height >> 8);
        infoHeader[10] = static_cast<unsigned char>(height >> 16);
        infoHeader[11] = static_cast<unsigned char>(height >> 24);

        // Set image size in the info header
        int imageSize = (width * 3 + paddingSize) * height;
        infoHeader[20] = static_cast<unsigned char>(imageSize);
        infoHeader[21] = static_cast<unsigned char>(imageSize >> 8);
        infoHeader[22] = static_cast<unsigned char>(imageSize >> 16);
        infoHeader[23] = static_cast<unsigned char>(imageSize >> 24);

        // Write the file and info headers
        outFile.write(reinterpret_cast<char *>(fileHeader), 14);
        outFile.write(reinterpret_cast<char *>(infoHeader), 40);

        // Write the image data
        unsigned char padding[3] = {0, 0, 0};
        for (int y = height - 1; y >= 0; y--) {
            for (int x = 0; x < width; x++) {
                unsigned char color[3] = {
                    pixels[(y * width + x) * 3 + 2],
                    pixels[(y * width + x) * 3 + 1],
                    pixels[(y * width + x) * 3]
                };
                outFile.write(reinterpret_cast<char *>(color), 3);
            }

            outFile.write(reinterpret_cast<char *>(padding), paddingSize);
        }

        outFile.close();
        printMessage("Successfully saved " + filename);
    } catch (const std::exception &e) {
        printError("Error saving bitmap: " + std::string(e.what()));
    } catch (...) {
        printError("Unknown error while saving bitmap");
    }
}
