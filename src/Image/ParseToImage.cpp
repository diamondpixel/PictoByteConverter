#include <string>
#include <fstream>
#include <cmath>
#include <iostream>
#include <vector>
#include <filesystem>
#include <thread>
#include <mutex>
#include <sstream>
#include <format>     // Add format header
#include <iomanip>    // For std::setw, std::setfill
#include <optional>   // For std::optional
#include <queue>      // For std::queue
#include "headers/ParseToImage.h"
#include "headers/Bitmap.hpp"
#include "headers/ImageTask.h"
#include "headers/SpecialVector.h"
#include "../Debug/headers/Debug.h"
#include "../Threading/headers/ThreadSafeQueue.h"

// Mutex for synchronized console output - use local instance for file operations
std::mutex console_mutex;

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
    ~BitmapImage() {}
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
    std::vector<uint8_t> buffer(length);
    
    file.seekg(start_pos, std::ios::beg);
    file.read(reinterpret_cast<char*>(buffer.data()), length);
    
    // If we reached EOF, resize buffer to actual read bytes
    if (file.eof()) {
        buffer.resize(file.gcount());
    }
    
    return buffer;
}

void processChunk(int chunk_index, size_t chunk_size, size_t total_chunks, size_t original_file_size, 
                 const std::string& input_file, const std::string& output_base, 
                 ThreadSafeQueueTemplate<ImageTaskInternal>& task_queue, size_t max_image_file_size) {
    
    // Generate output filename
    std::string output_filename;
    if (total_chunks > 1) {
        output_filename = output_base + "_" + std::to_string(chunk_index + 1) + "of" + std::to_string(total_chunks) + ".bmp";
    } else {
        output_filename = output_base + ".bmp";
    }
    
    printMessage("Processing chunk " + std::to_string(chunk_index + 1) + " of " + std::to_string(total_chunks) + " to " + output_filename);
    
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
        
        // Read data for this chunk
        std::vector<uint8_t> chunk_data = readFileSegment(input, start_pos, actual_chunk_size);
        
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
        
        // Create BMP with metadata and chunk data
        BitmapImage bmp(static_cast<int>(width), static_cast<int>(height));
        
        // Set metadata at beginning of image data
        bmp.setData(metadata, 0);
        
        // Set chunk data after metadata
        bmp.setData(chunk_data, metadata.size());
        
        // Queue the image for writing
        task_queue.push(ImageTaskInternal{
            output_filename,
            std::move(bmp)
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
        printError("Error saving image " + task.filename + ": " + e.what());
        throw;
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
        fileHeader[2] = (unsigned char)(fileSize);
        fileHeader[3] = (unsigned char)(fileSize >> 8);
        fileHeader[4] = (unsigned char)(fileSize >> 16);
        fileHeader[5] = (unsigned char)(fileSize >> 24);
        
        // Write width to header
        infoHeader[4] = (unsigned char)(width);
        infoHeader[5] = (unsigned char)(width >> 8);
        infoHeader[6] = (unsigned char)(width >> 16);
        infoHeader[7] = (unsigned char)(width >> 24);
        
        // Write height to header
        infoHeader[8] = (unsigned char)(height);
        infoHeader[9] = (unsigned char)(height >> 8);
        infoHeader[10] = (unsigned char)(height >> 16);
        infoHeader[11] = (unsigned char)(height >> 24);
        
        // Write image size to header
        int imageSize = (width * 3 + paddingSize) * height;
        infoHeader[20] = (unsigned char)(imageSize);
        infoHeader[21] = (unsigned char)(imageSize >> 8);
        infoHeader[22] = (unsigned char)(imageSize >> 16);
        infoHeader[23] = (unsigned char)(imageSize >> 24);
        
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
bool parseToImage(const std::string& input_file, const std::string& output_base, int maxChunkSizeMB) {
    printMessage("Starting conversion of file to image: " + input_file);
    printMessage("Output base name: " + output_base);
    
    try {
        // Get file size
        uintmax_t file_size = std::filesystem::file_size(input_file);
        printStatus("Input file size: " + std::to_string(file_size) + " bytes");
        
        if (file_size == 0) {
            printError("Input file is empty");
            return false;
        }
        
        // Convert maxChunkSizeMB to bytes
        const size_t MAX_CHUNK_SIZE = static_cast<size_t>(maxChunkSizeMB) * 1024 * 1024;
        
        // Determine chunk size and count
        size_t chunk_size;
        size_t total_chunks;
        
        const size_t MAX_IMAGE_SIZE = 100 * 1024 * 1024; // 100 MB max for BMP files
        
        if (file_size <= MAX_CHUNK_SIZE) {
            // Single chunk mode if file is smaller than max chunk size
            chunk_size = file_size;
            total_chunks = 1;
        } else {
            // Multi-chunk mode
            // Use specified max chunk size
            chunk_size = MAX_CHUNK_SIZE;
            
            // Ensure chunk size is reasonable (at least 1 MB but not more than half of MAX_IMAGE_SIZE)
            const size_t MIN_CHUNK_SIZE = 1 * 1024 * 1024; // 1 MB minimum
            
            if (chunk_size < MIN_CHUNK_SIZE) {
                // If specified chunk size is too small, use minimum size
                chunk_size = MIN_CHUNK_SIZE;
                printWarning("Specified chunk size too small, using 1MB minimum");
            } else if (chunk_size > MAX_IMAGE_SIZE / 2) {
                // If specified chunk size is too large, cap it
                chunk_size = MAX_IMAGE_SIZE / 2;
                printWarning("Specified chunk size too large, capping at " + 
                          std::to_string(MAX_IMAGE_SIZE / 2 / 1024 / 1024) + "MB");
            }
            
            // Calculate total chunks based on chunk size
            total_chunks = (file_size + chunk_size - 1) / chunk_size;
        }
        
        printStatus("Splitting file into " + std::to_string(total_chunks) + 
                  " chunks of approximately " + std::to_string(chunk_size / 1024) + " KB each");
        
        // Create task queue for image writing
        ThreadSafeQueueTemplate<ImageTaskInternal> task_queue;
        
        // Start image writer thread
        std::atomic<bool> should_terminate(false);
        std::thread writer_thread(imageWriterThread, std::ref(task_queue), std::ref(should_terminate));
        
        // Process chunks
        std::vector<std::thread> processing_threads;
        
        // Use at most std::thread::hardware_concurrency() - 1 threads (leave one for writer)
        size_t max_threads = std::max(1u, std::thread::hardware_concurrency() - 1);
        
        // Process chunks
        for (size_t i = 0; i < total_chunks; ++i) {
            // Process chunk in a new thread
            processing_threads.emplace_back(
                processChunk, i, chunk_size, total_chunks, file_size, 
                input_file, output_base, std::ref(task_queue), MAX_IMAGE_SIZE
            );
            
            // If we've reached max threads, wait for one to finish
            if (processing_threads.size() >= max_threads) {
                processing_threads.front().join();
                processing_threads.erase(processing_threads.begin());
            }
        }
        
        // Wait for all processing threads to complete
        for (auto& thread : processing_threads) {
            thread.join();
        }
        
        // Signal writer thread to terminate
        should_terminate = true;
        
        // Wait for writer thread to finish
        writer_thread.join();
        
        printStatus("Conversion completed successfully");
        return true;
        
    } catch (const std::exception& e) {
        printError("Error during file to image conversion: " + std::string(e.what()));
        return false;
    }
}
