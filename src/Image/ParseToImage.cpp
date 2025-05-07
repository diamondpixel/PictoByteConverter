#include <string>
#include <fstream>
#include <cmath>
#include <iostream>
#include <vector>
#include <filesystem>
#include <thread>
#include <mutex>
#include <sstream>
#include <iomanip>    // For std::setw, std::setfill
#include <optional>   // For std::optional
#include <queue>      // For std::queue
#include "headers/ParseToImage.h"
#include "headers/Bitmap.hpp"
#include "headers/ImageTask.h"
#include "headers/SpecialVector.h"
#include "headers/ResourceManager.h" // Add ResourceManager header
#include "../Debug/headers/Debug.h"
#include "../Threading/headers/ThreadSafeQueue.h"

// Use the global console mutex from Debug.h instead of a local one
extern std::mutex gConsoleMutex;

// Forward declaration of our adapted BitmapImage class
class BitmapImage {
private:
    std::vector<unsigned char> pixels;
    int width;
    int height;

public:
    BitmapImage(int width, int height);
    void setData(const std::vector<uint8_t>& data, size_t offset);
    void save(const std::string& filename);
    ~BitmapImage() = default;
};

// Our custom ImageTask structure for this file
struct ImageTaskInternal {
    std::string filename;
    BitmapImage image;
};

// Our custom queue for this file
template <typename T>
class ThreadSafeQueueTemplate {
private:
    std::queue<T> queue;
    mutable std::mutex mutex;
    std::condition_variable cv;

public:
    void push(const T& item) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push(item);
        cv.notify_one();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.empty();
    }

    std::optional<T> try_pop(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        if (cv.wait_for(lock, timeout, [this] { return !queue.empty(); })) {
            T item = std::move(queue.front());
            queue.pop();
            return item;
        }
        return std::nullopt;
    }
};

// Function to calculate optimal rectangular dimensions based on aspect ratio
std::pair<size_t, size_t> calculateOptimalRectDimensions(size_t total_bytes, size_t bytes_per_pixel, size_t bmp_header_size,
                                                         size_t max_size_bytes, float aspect_ratio = 1.0f) {
    // Total pixels needed for header + file data
    // Add a much larger buffer (500 pixels worth) to ensure all data fits with NO truncation
    size_t total_pixels = (total_bytes + bytes_per_pixel - 1) / bytes_per_pixel + 500;

    // Calculate initial width and height based on aspect ratio
    size_t width = static_cast<size_t>(std::sqrt(static_cast<double>(total_pixels * aspect_ratio)));
    size_t height = static_cast<size_t>(width / aspect_ratio);

    // Ensure enough pixels
    while (width * height < total_pixels) {
        width++;
        height = static_cast<size_t>(width / aspect_ratio);
    }

    // Set minimum dimensions to avoid tiny images
    size_t min_dimension = 64; // Increased from 16 to ensure capacity
    if (width < min_dimension) width = min_dimension;
    if (height < min_dimension) height = min_dimension;

    // Cap at BMP max dimensions (65,535 x 65,535)
    if (width > 65535) width = 65535;
    if (height > 65535) height = 65535;

    // Calculate actual file size with row padding
    size_t row_bytes = width * bytes_per_pixel;
    size_t padding = (4 - (row_bytes % 4)) % 4; // BMP rows pad to multiple of 4
    size_t actual_image_size = bmp_header_size + (height * (row_bytes + padding));

    // Shrink dimensions if over max_size_bytes
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

// Function to optimize dimensions for the last image
std::pair<size_t, size_t> optimizeLastImageDimensions(size_t total_bytes, size_t bytes_per_pixel, size_t metadata_size, 
                                                     size_t bmp_header_size, float aspect_ratio = 1.0f) {
    // Calculate the minimum number of pixels needed for header + metadata + file
    size_t total_data_size = metadata_size + total_bytes;
    size_t total_pixels_needed = (total_data_size + bytes_per_pixel - 1) / bytes_per_pixel;

    // Calculate width and height based on aspect ratio
    size_t width = static_cast<size_t>(std::sqrt(static_cast<double>(total_pixels_needed * aspect_ratio)));
    size_t height = static_cast<size_t>(width / aspect_ratio);

    // Ensure dimensions are sufficient
    while (width * height < total_pixels_needed) {
        width++;
        height = static_cast<size_t>(width / aspect_ratio);
    }

    // Apply minimum dimensions
    size_t min_dimension = 64;
    if (width < min_dimension) width = min_dimension;
    if (height < min_dimension) height = min_dimension;

    // Cap at BMP max dimensions
    if (width > 65535) width = 65535;
    if (height > 65535) height = 65535;

    return {width, height};
}

// Functions for reading file segments
std::vector<uint8_t> readFileSegment(std::ifstream& file, size_t start_pos, size_t length) {
    // Request memory allocation from ResourceManager
    auto& resManager = ResourceManager::getInstance();
    bool memory_allocated = resManager.allocateMemory(length);
    
    if (!memory_allocated) {
        printWarning("Memory allocation for " + std::to_string(length / (1024 * 1024)) + 
                    " MB failed, reducing buffer size");
        // Try with half the size if full allocation failed
        length = length / 2;
        memory_allocated = resManager.allocateMemory(length);
        if (!memory_allocated) {
            printError("Could not allocate memory for file segment");
            return std::vector<uint8_t>();
        }
    }
    
    std::vector<uint8_t> buffer(length);
    
    file.seekg(start_pos, std::ios::beg);
    file.read(reinterpret_cast<char*>(buffer.data()), length);
    
    // If we reached EOF, resize buffer to actual read bytes
    if (file.eof()) {
        size_t actual_size = file.gcount();
        buffer.resize(actual_size);
        // Free the memory we didn't use
        resManager.freeMemory(length - actual_size);
    }
    
    return buffer;
}

void processChunk(int chunk_index, size_t chunk_size, size_t total_chunks, size_t original_file_size, 
                 const std::string& input_file, const std::string& output_base, 
                 ThreadSafeQueueTemplate<ImageTaskInternal>& task_queue, size_t max_image_file_size) {
    
    // Store debug mode state at the beginning to ensure consistent debug messages
    bool localDebugMode = getDebugMode();

    // Generate output filename
    std::string formatted_path = output_base + "_" + std::to_string(chunk_index + 1) + "of" + std::to_string(total_chunks) + ".bmp";
    
    printHighlight("Processing chunk " + std::to_string(chunk_index + 1) + " of " + std::to_string(total_chunks));
    printFilePath("Chunk output: " + formatted_path);

    // Determine start position and size for this chunk
    size_t start_pos = chunk_index * chunk_size;
    size_t actual_chunk_size = (chunk_index == total_chunks - 1) ?
                               (original_file_size - start_pos) :
                               chunk_size;

    try {
        std::ifstream input(input_file, std::ios::binary);
        if (!input.is_open()) {
            throw std::runtime_error("Failed to open input file: " + input_file);
        }

        // Read data for this chunk with memory tracking
        auto& resManager = ResourceManager::getInstance();
        size_t actual_chunk_size = (chunk_index == total_chunks - 1) ?
                                  (original_file_size - start_pos) :
                                  chunk_size;

        // Read data for this chunk
        std::vector<uint8_t> chunk_data = readFileSegment(input, start_pos, actual_chunk_size);
        if (chunk_data.empty()) {
            throw std::runtime_error("Failed to read data from file: " + input_file);
        }

        // Create metadata for this chunk - match the format expected by ParseFromImage.cpp
        std::vector<uint8_t> metadata;

        // 1. Header length (3 bytes) - fixed at 48 bytes
        uint32_t header_length = 48;
        metadata.push_back((header_length >> 16) & 0xFF);
        metadata.push_back((header_length >> 8) & 0xFF);
        metadata.push_back(header_length & 0xFF);

        // 2. Filename length (2 bytes)
        std::string filename = std::filesystem::path(input_file).filename().string();
        uint16_t filename_length = static_cast<uint16_t>(filename.length());
        metadata.push_back((filename_length >> 8) & 0xFF);
        metadata.push_back(filename_length & 0xFF);

        // 3. Filename (variable length)
        for (char c : filename) {
            metadata.push_back(static_cast<uint8_t>(c));
        }

        // 4. Data size (10 bytes, as string)
        std::string data_size_str = std::to_string(chunk_data.size());
        data_size_str = std::string(10 - data_size_str.length(), '0') + data_size_str; // Zero-pad to 10 chars
        for (char c : data_size_str) {
            metadata.push_back(static_cast<uint8_t>(c));
        }

        // 5. Current chunk (4 bytes, as string)
        std::string current_chunk_str = std::to_string(chunk_index + 1);
        current_chunk_str = std::string(4 - current_chunk_str.length(), '0') + current_chunk_str; // Zero-pad to 4 chars
        for (char c : current_chunk_str) {
            metadata.push_back(static_cast<uint8_t>(c));
        }

        // 6. Total chunks (4 bytes, as string)
        std::string total_chunks_str = std::to_string(total_chunks);
        total_chunks_str = std::string(4 - total_chunks_str.length(), '0') + total_chunks_str; // Zero-pad to 4 chars
        for (char c : total_chunks_str) {
            metadata.push_back(static_cast<uint8_t>(c));
        }

        // 7. Padding to reach exactly 48 bytes for the header
        size_t current_metadata_size = 3 + 2 + filename_length + 10 + 4 + 4;
        if (current_metadata_size < header_length) {
            size_t padding_needed = header_length - current_metadata_size;
            for (size_t i = 0; i < padding_needed; i++) {
                metadata.push_back(0);
            }
        }

        // Determine dimensions for the BMP
        const size_t bytes_per_pixel = 3; // RGB
        const size_t bmp_header_size = 54; // Standard BMP header size

        // Get dimensions based on metadata and data size
        std::pair<size_t, size_t> dimensions;
        if (chunk_index == total_chunks - 1) {
            // Optimize dimensions for last chunk
            dimensions = optimizeLastImageDimensions(chunk_data.size(), bytes_per_pixel,
                                                    metadata.size(), bmp_header_size);
        } else {
            // Calculate dimensions for regular chunks
            dimensions = calculateOptimalRectDimensions(
                metadata.size() + chunk_data.size(),
                bytes_per_pixel, bmp_header_size,
                max_image_file_size
            );
        }

        size_t width = dimensions.first;
        size_t height = dimensions.second;

        printMessage("Chunk " + std::to_string(chunk_index + 1) + " image dimensions: " +
                    std::to_string(width) + " x " + std::to_string(height) + " pixels");

        // Create a bitmap with specified dimensions
        size_t total_pixels = width * height;
        size_t bitmap_memory_size = total_pixels * 3; // 3 bytes per pixel (RGB)

        // Request memory for the bitmap
        bool memory_allocated = resManager.allocateMemory(bitmap_memory_size);
        if (!memory_allocated) {
            throw std::runtime_error("Not enough memory to create image");
        }

        BitmapImage image(width, height);

        // Set metadata at beginning of image data
        image.setData(metadata, 0);

        // Set chunk data after metadata
        image.setData(chunk_data, metadata.size());

        // Add the task to the queue
        task_queue.push(ImageTaskInternal{
            formatted_path,
            std::move(image)
        });

        printStatus("Processed chunk " + std::to_string(chunk_index + 1) + " of " +
                   std::to_string(total_chunks) + " (" +
                   std::to_string(chunk_data.size()) + " bytes)");

    } catch (const std::exception& e) {
        printError("Error processing chunk " + std::to_string(chunk_index + 1) + ": " + e.what());
        // Re-throw to be caught by the main thread
        throw;
    }
}

void saveImage(const ImageTaskInternal& task) {
    try {
        // Make a non-const copy of the image to fix the const issue
        BitmapImage img = task.image;
        
        img.save(task.filename);
        printStatus("Saved image: " + task.filename);
    } catch (const std::exception& e) {
        printError("Failed to save image: " + std::string(e.what()));
    }
}

// Image writer thread function
void imageWriterThread(ThreadSafeQueueTemplate<ImageTaskInternal>& task_queue, std::atomic<bool>& should_terminate) {
    try {
        while (!should_terminate || !task_queue.empty()) {
            auto task = task_queue.try_pop(std::chrono::milliseconds(100));
            if (task) {
                saveImage(*task);
            }
        }
    } catch (const std::exception& e) {
        printError("Image writer thread error: " + std::string(e.what()));
    }
}

// BitmapImage implementation (simplified for direct BMP file output)
BitmapImage::BitmapImage(int width, int height) {
    printMessage("Creating bitmap with dimensions " + std::to_string(width) + "x" + std::to_string(height));
    this->width = width;
    this->height = height;
    // Initialize with black pixels (3 bytes per pixel - RGB)
    pixels.resize(width * height * 3, 0);
}

void BitmapImage::setData(const std::vector<uint8_t>& data, size_t offset) {
    printMessage("Setting data at offset " + std::to_string(offset) + ", size " + std::to_string(data.size()) + " bytes");

    // Check if there's enough space
    if (offset + data.size() > pixels.size()) {
        printWarning("Data exceeds image capacity, will be truncated");
    }

    // Copy data into pixels buffer
    size_t bytesToCopy = std::min(data.size(), pixels.size() - offset);
    std::copy(data.begin(), data.begin() + bytesToCopy, pixels.begin() + offset);
}

void BitmapImage::save(const std::string& filename) {
    printMessage("Saving bitmap to: " + filename);
    try {
        // Open file for writing
        std::ofstream outFile(filename, std::ios::binary);
        if (!outFile) {
            printError("Cannot open file for writing: " + filename);
            return;
        }

        // BMP file header (14 bytes)
        unsigned char fileHeader[14] = {
            'B', 'M',                   // Signature
            0, 0, 0, 0,                 // File size in bytes
            0, 0, 0, 0,                 // Reserved
            54, 0, 0, 0                 // Offset to pixel data
        };

        // BMP info header (40 bytes)
        unsigned char infoHeader[40] = {
            40, 0, 0, 0,                // Info header size
            0, 0, 0, 0,                 // Width
            0, 0, 0, 0,                 // Height
            1, 0,                       // Planes
            24, 0,                      // Bits per pixel
            0, 0, 0, 0,                 // Compression
            0, 0, 0, 0,                 // Image size
            0, 0, 0, 0,                 // X pixels per meter
            0, 0, 0, 0,                 // Y pixels per meter
            0, 0, 0, 0,                 // Colors in color table
            0, 0, 0, 0                  // Important color count
        };

        // Calculate row padding (rows must be aligned to 4 bytes)
        int paddingSize = (4 - (width * 3) % 4) % 4;

        // Calculate total file size
        int fileSize = 14 + 40 + (width * 3 + paddingSize) * height;

        // Write file size to header
        fileHeader[2] = static_cast<unsigned char>(fileSize);
        fileHeader[3] = static_cast<unsigned char>(fileSize >> 8);
        fileHeader[4] = static_cast<unsigned char>(fileSize >> 16);
        fileHeader[5] = static_cast<unsigned char>(fileSize >> 24);

        // Write width to header
        infoHeader[4] = static_cast<unsigned char>(width);
        infoHeader[5] = static_cast<unsigned char>(width >> 8);
        infoHeader[6] = static_cast<unsigned char>(width >> 16);
        infoHeader[7] = static_cast<unsigned char>(width >> 24);

        // Write height to header
        infoHeader[8] = static_cast<unsigned char>(height);
        infoHeader[9] = static_cast<unsigned char>(height >> 8);
        infoHeader[10] = static_cast<unsigned char>(height >> 16);
        infoHeader[11] = static_cast<unsigned char>(height >> 24);

        // Write image size to header
        int imageSize = (width * 3 + paddingSize) * height;
        infoHeader[20] = static_cast<unsigned char>(imageSize);
        infoHeader[21] = static_cast<unsigned char>(imageSize >> 8);
        infoHeader[22] = static_cast<unsigned char>(imageSize >> 16);
        infoHeader[23] = static_cast<unsigned char>(imageSize >> 24);

        // Write headers
        outFile.write(reinterpret_cast<char*>(fileHeader), 14);
        outFile.write(reinterpret_cast<char*>(infoHeader), 40);

        // Write pixel data with padding
        unsigned char padding[3] = {0, 0, 0};

        // In BMP, the pixel array starts from the bottom row
        for (int y = height - 1; y >= 0; y--) {
            for (int x = 0; x < width; x++) {
                // BMP uses BGR order
                int index = (y * width + x) * 3;
                unsigned char color[3] = {
                    pixels[index + 2],  // Blue
                    pixels[index + 1],  // Green
                    pixels[index]       // Red
                };
                outFile.write(reinterpret_cast<char*>(color), 3);
            }

            // Write padding for the current row
            outFile.write(reinterpret_cast<char*>(padding), paddingSize);
        }

        outFile.close();
        printMessage("Successfully saved " + filename);
    } catch (const std::exception& e) {
        printError("Error saving bitmap: " + std::string(e.what()));
    } catch (...) {
        printError("Unknown error while saving bitmap");
    }
}

// Main function to parse a file to BMP images
bool parseToImage(const std::string& input_file, const std::string& output_base, int maxChunkSizeMB, int maxThreads, int maxMemoryMB) {
    try {
        // Initialize ResourceManager with specified limits
        auto& resManager = ResourceManager::getInstance();

        // Set thread limit (default to hardware_concurrency if maxThreads <= 0)
        if (maxThreads > 0) {
            resManager.setMaxThreads(maxThreads);
        } else {
            // Default to half of available cores if not specified
            size_t default_threads = std::max(1u, std::thread::hardware_concurrency() / 2);
            resManager.setMaxThreads(default_threads);
        }

        // Set memory limit (default to 70% of specified if maxMemoryMB <= 0)
        if (maxMemoryMB > 0) {
            resManager.setMaxMemory(static_cast<size_t>(maxMemoryMB) * 1024 * 1024);
        } else {
            // Default to 1GB if not specified
            resManager.setMaxMemory(1024 * 1024 * 1024);
        }

        printMessage("Resource limits: " + std::to_string(resManager.getMaxThreads()) +
                    " threads, " + std::to_string(resManager.getMaxMemory() / (1024 * 1024)) + " MB memory");

        std::string cleanInputPath = input_file;
        std::string cleanOutputPath = output_base;

        // Check if input file exists
        if (!std::filesystem::exists(cleanInputPath)) {
            printError("Input file does not exist: " + cleanInputPath);
            return false;
        }

        // Create output directory if it doesn't exist
        std::filesystem::path output_path(cleanOutputPath);
        std::filesystem::path output_dir = output_path.parent_path();
        if (!output_dir.empty() && !std::filesystem::exists(output_dir)) {
            std::filesystem::create_directories(output_dir);
            printStatus("Created output directory: " + output_dir.string());
        }

        // Get input file size
        auto file_size = std::filesystem::file_size(cleanInputPath);
        if (file_size == 0) {
            printError("Input file is empty: " + cleanInputPath);
            return false;
        }

        // Calculate chunk size based on input parameter
        size_t chunk_size = static_cast<size_t>(maxChunkSizeMB) * 1024 * 1024;
        chunk_size = std::min(chunk_size, file_size); // Ensure chunk size isn't larger than file size

        // Calculate number of chunks needed
        size_t total_chunks = (file_size + chunk_size - 1) / chunk_size;
        printFilePath("Input: " + cleanInputPath);
        printFilePath("Output: " + cleanOutputPath);
        printStats("File size: " + std::to_string(file_size) + " bytes, Chunks: " + std::to_string(total_chunks) + 
                   ", Chunk size: " + std::to_string(chunk_size / (1024 * 1024)) + " MB");

        // Create a thread-safe queue for image tasks
        ThreadSafeQueueTemplate<ImageTaskInternal> task_queue;

        // Start the image writer thread
        std::atomic<bool> should_terminate(false);
        std::thread writer_thread(imageWriterThread, std::ref(task_queue), std::ref(should_terminate));

        // Calculate max image file size based on memory constraints
        size_t max_image_file_size = std::min(
            static_cast<size_t>(100) * 1024 * 1024,  // 100 MB default
            resManager.getMaxMemory() / (total_chunks > 0 ? total_chunks : 1) / 2  // Or half of available memory per chunk
        );

        // Process each chunk
        std::vector<std::thread> processing_threads;
        for (size_t i = 0; i < total_chunks; ++i) {
            // Format chunk info
            std::string chunkInfo = "Chunk " + std::to_string(i + 1) + " of " + std::to_string(total_chunks);
            printProcessingStep(chunkInfo);
            
            // Use ResourceManager to execute the thread
            resManager.runWithThread(processChunk, i, chunk_size, total_chunks, file_size,
                                    input_file, output_base, std::ref(task_queue), max_image_file_size);

            // Sleep briefly to allow thread to initialize
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Wait for all threads to complete
        resManager.waitForAllThreads();

        // Signal the writer thread to terminate once all chunks are processed
        should_terminate = true;
        if (writer_thread.joinable()) {
            writer_thread.join();
        }

        printSuccess("File processing completed successfully");
        return true;

    } catch (const std::exception& e) {
        printError("Error in parseToImage: " + std::string(e.what()));
        return false;
    }
}