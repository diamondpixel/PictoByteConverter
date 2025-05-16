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

// Define NOMINMAX to prevent windows.h from defining min and max macros,
// which conflict with std::min and std::max.
#define NOMINMAX
#include <windows.h>     // For Windows API (memory mapping)

#include <algorithm>     // For std::copy
#include "headers/ParseToImage.h"       // Header defining the interface
#include "headers/ResourceManager.h"    // Resource management utilities
#include "../Debug/headers/Debug.h"     // Debugging and logging utilities
#include "headers/ImageProcessingTypes.h" // Moved class definitions

// Define DEFAULT_MAX_IMAGE_SIZE here
constexpr size_t DEFAULT_MAX_IMAGE_SIZE = 100 * 1024 * 1024; // 100MB

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
    // Estimate minimum possible image size for total_bytes (excluding variable padding)
    size_t min_data_payload_size = total_bytes;
    size_t min_possible_image_file_size = bmp_header_size + min_data_payload_size;

    if (max_size_bytes < min_possible_image_file_size) {
        std::cerr << "DIAGNOSTIC_WARN: calculateOptimalRectDimensions: max_size_bytes (" << max_size_bytes
                  << ") is less than min required image file size (" << min_possible_image_file_size
                  << ") for data payload (" << total_bytes << " bytes). Returning {0,0}." << std::endl;
        return {0, 0};
    }

    // Add buffer for ensuring all data fits with no truncation
    // Extra 500 pixels provide safety margin for rounding errors
    size_t total_pixels = (total_bytes + bytes_per_pixel - 1) / bytes_per_pixel + 500;

    // Calculate initial dimensions based on desired aspect ratio
    auto width = static_cast<size_t>(std::sqrt(static_cast<double>(total_pixels * aspect_ratio)));
    auto height = static_cast<size_t>(width / aspect_ratio);

    // Ensure dimensions provide enough pixels for all data
    // This loop increases dimensions until they can accommodate all data
    int emergency_break_initial_sizing = 10000; // Prevent infinite loop on weird inputs
    while (width * height < total_pixels) {
        width++;
        height = static_cast<size_t>(width / aspect_ratio);
        if (width == 0 || height == 0 || --emergency_break_initial_sizing <= 0) { 
            std::cerr << "DIAGNOSTIC_WARN: calculateOptimalRectDimensions: Initial sizing loop failed or safety break. W:" << width << " H:" << height << std::endl;
            return {0,0};
        }
    }

    // Enforce minimum dimensions for practical reasons
    constexpr size_t min_dimension = 64;
    if (width < min_dimension) width = min_dimension;
    if (height < min_dimension) height = min_dimension;

    // Cap at BMP format limits (65,535 x 65,535)
    constexpr size_t max_bitmap_dimension = 65535;
    if (width > max_bitmap_dimension) width = max_bitmap_dimension;
    if (height > max_bitmap_dimension) height = max_bitmap_dimension;
    
    // Ensure again that after clamping, we can still hold the data (pixels perspective)
    if (width * height < total_pixels) {
         std::cerr << "DIAGNOSTIC_WARN: calculateOptimalRectDimensions: Dimensions " << width << "x" << height 
                   << " (clamped by min/max) cannot hold total_pixels " << total_pixels << ". Returning {0,0}." << std::endl;
        return {0, 0};
    }

    // Calculate row padding and actual file size
    size_t row_bytes = width * bytes_per_pixel;
    size_t padding = (4 - (row_bytes % 4)) % 4;
    size_t actual_image_size = bmp_header_size + (height * (row_bytes + padding));

    // Shrink dimensions if image would exceed max size
    int emergency_break_shrinking = 10000; // Prevent infinite loop
    while (actual_image_size > max_size_bytes && width > min_dimension && height > min_dimension) {
        if (--emergency_break_shrinking <= 0) {
             std::cerr << "DIAGNOSTIC_WARN: calculateOptimalRectDimensions: Shrinking loop safety break. AIS:" << actual_image_size << " MSB:" << max_size_bytes << std::endl;
             break; // Exit loop, proceed to final checks
        }

        size_t old_width = width;
        size_t old_height = height;

        // Check if current capacity is already too close to total_bytes before shrinking further
        // This is a heuristic; a perfect check is hard due to padding.
        if (width * height * bytes_per_pixel < total_bytes + (bytes_per_pixel * 100)) { // if capacity is within 100 pixels of data, be careful
             std::cerr << "DIAGNOSTIC_INFO: calculateOptimalRectDimensions: Shrinking might compromise data capacity. W:" << width << " H:" << height 
                       << " DataBytes: " << total_bytes << " MaxSizeBytes: " << max_size_bytes << std::endl;
            //  It's better to fail the max_size_bytes constraint than to corrupt data. So break and let final checks handle it.
            break;
        }

        if (width > height) {
            width--;
        } else {
            height--;
        }
        
        if (width == old_width && height == old_height) { // No change, stuck
            std::cerr << "DIAGNOSTIC_WARN: calculateOptimalRectDimensions: Shrinking loop stuck. W:" << width << " H:" << height << std::endl;
            break; 
        }

        row_bytes = width * bytes_per_pixel;
        padding = (4 - (row_bytes % 4)) % 4;
        actual_image_size = bmp_header_size + (height * (row_bytes + padding));
    }

    // Final check: Can the chosen dimensions actually hold the data?
    if (width == 0 || height == 0 || width * height * bytes_per_pixel < total_bytes) {
        std::cerr << "DIAGNOSTIC_WARN: calculateOptimalRectDimensions: Final dimensions " << width << "x" << height
                  << " are too small for data_payload (" << total_bytes << " bytes) after attempting to meet max_size_bytes "
                  << max_size_bytes << ". Returning {0,0}." << std::endl;
        return {0, 0};
    }
    
    // Another check: does it now respect max_size_bytes? If not, it was impossible to satisfy both.
    if (actual_image_size > max_size_bytes) {
        std::cerr << "DIAGNOSTIC_WARN: calculateOptimalRectDimensions: Final dimensions " << width << "x" << height
                  << " (image file size " << actual_image_size << " bytes) still exceed max_size_bytes ("
                  << max_size_bytes << "). Data payload was " << total_bytes << " bytes. Returning {0,0}." << std::endl;
        return {0,0};
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
 * Processes a single chunk of the input file, converting it to a bitmap image.
 * Each chunk includes metadata about the original file and chunk position.
 * This function handles the core logic of reading a file segment, adding
 * metadata headers, and preparing the image for storage.
 *
 * @param chunk_index Index of the current chunk (0-based)
 * @param chunk_data_max_size Maximum size of data for this chunk from original file
 * @param total_chunks Total number of chunks in the file
 * @param original_file_total_size Original total file size in bytes
 * @param all_file_data_ptr Pointer to the start of the memory-mapped input file data
 * @param original_input_filepath Path to the original input file (for metadata/logging)
 * @param output_base_path Base path for output images
 * @param image_task_queue Thread-safe queue for writer tasks
 * @param max_image_file_size_param Maximum size in bytes for output image files
 */
void processChunk(int chunk_index, size_t chunk_data_max_size, size_t total_chunks, size_t original_file_total_size,
                  const unsigned char *all_file_data_ptr, const std::string &original_input_filepath,
                  const std::string &output_base_path, ThreadSafeQueueTemplate<ImageTaskInternal> &image_task_queue,
                  size_t max_image_file_size_param) {
    printMessage("Processing chunk " + std::to_string(chunk_index) + " / " + std::to_string(total_chunks - 1));

    // Calculate the actual start and length for this chunk from the memory-mapped file
    size_t chunk_start_offset_in_file = static_cast<size_t>(chunk_index) * chunk_data_max_size;
    size_t current_chunk_actual_data_length = chunk_data_max_size;

    if (chunk_start_offset_in_file >= original_file_total_size) {
        printWarning(
            "processChunk: chunk_start_offset_in_file is beyond or at original_file_total_size. Skipping chunk " +
            std::to_string(chunk_index));
        return; // Should not happen if num_chunks is calculated correctly
    }

    if (chunk_start_offset_in_file + current_chunk_actual_data_length > original_file_total_size) {
        current_chunk_actual_data_length = original_file_total_size - chunk_start_offset_in_file;
    }

    if (current_chunk_actual_data_length == 0 && original_file_total_size > 0) {
        // This case should ideally be handled by the main loop not creating a task for an empty trailing chunk
        printWarning(
            "processChunk: current_chunk_actual_data_length is 0 for chunk " + std::to_string(chunk_index) +
            ". This might be an empty trailing chunk.");
        // Depending on requirements, we might still create an image with only metadata if this is the last chunk
        // For now, if there's no data, we don't proceed to create an image from it.
        // However, metadata part might still be useful for the last chunk marker.
        // Let's assume for now that a chunk with zero data (not header) shouldn't produce an image unless it's specifically to mark EOF.
        // The current logic will try to proceed and make an image, which might be very small.
    }

    std::vector<uint8_t> chunk_data_segment;
    if (current_chunk_actual_data_length > 0) {
        try {
            // ResourceManager could be involved here if we want it to track this memory
            // For now, direct allocation.
            chunk_data_segment.resize(current_chunk_actual_data_length);
            std::copy(all_file_data_ptr + chunk_start_offset_in_file,
                      all_file_data_ptr + chunk_start_offset_in_file + current_chunk_actual_data_length,
                      chunk_data_segment.begin());
        } catch (const std::bad_alloc &e) {
            printError("processChunk: Memory allocation failed for chunk_data_segment: " + std::string(e.what()));
            return; // Cannot proceed with this chunk
        } catch (const std::exception &e) {
            printError("processChunk: Error copying chunk data: " + std::string(e.what()));
            return;
        }
    }
    // chunk_data_segment now holds the raw data for the current chunk
    // The rest of the function proceeds as before, using chunk_data_segment

    // Create the header for this chunk
    std::string original_filename = std::filesystem::path(original_input_filepath).filename().string();
    size_t header_filename_len = original_filename.length();
    if (header_filename_len > 65535) {
        // Max 2 bytes for filename length
        printWarning("Original filename too long, truncating for header: " + original_filename);
        header_filename_len = 65535;
        original_filename = original_filename.substr(0, header_filename_len);
    }

    // Fixed header size is 48 bytes for now
    // 3 (total_header_length) + 2 (filename_length) + N (filename) + 10 (data_size) + 4 (current_chunk) + 4 (total_chunks) + P (padding)
    // Let's define fixed total_header_length
    const size_t FIXED_TOTAL_HEADER_LENGTH = 48;

    std::vector<uint8_t> header_data;
    header_data.reserve(FIXED_TOTAL_HEADER_LENGTH);

    // 1. Total Header Length (3 bytes)
    header_data.push_back(static_cast<uint8_t>((FIXED_TOTAL_HEADER_LENGTH >> 16) & 0xFF));
    header_data.push_back(static_cast<uint8_t>((FIXED_TOTAL_HEADER_LENGTH >> 8) & 0xFF));
    header_data.push_back(static_cast<uint8_t>(FIXED_TOTAL_HEADER_LENGTH & 0xFF));

    // 2. Filename Length (2 bytes)
    header_data.push_back(static_cast<uint8_t>((header_filename_len >> 8) & 0xFF));
    header_data.push_back(static_cast<uint8_t>(header_filename_len & 0xFF));

    // 3. Filename (N bytes)
    header_data.insert(header_data.end(), original_filename.begin(), original_filename.end());

    // 4. Original Data Size for this chunk (10 bytes as string)
    std::string data_size_str = std::to_string(current_chunk_actual_data_length);
    // This is the size of the data in *this* chunk
    if (data_size_str.length() > 10) {
        printError("Data size string too long!"); // Should not happen for typical chunk sizes
        data_size_str = data_size_str.substr(0, 10);
    }
    while (data_size_str.length() < 10) data_size_str.insert(0, "0"); // Pad with leading zeros
    header_data.insert(header_data.end(), data_size_str.begin(), data_size_str.end());

    // 5. Current Chunk Index (4 bytes as string)
    std::string current_chunk_str = std::to_string(chunk_index);
    if (current_chunk_str.length() > 4) {
        printError("Current chunk string too long!");
        current_chunk_str = "9999";
    }
    while (current_chunk_str.length() < 4) current_chunk_str.insert(0, "0");
    header_data.insert(header_data.end(), current_chunk_str.begin(), current_chunk_str.end());

    // 6. Total Chunks (4 bytes as string)
    std::string total_chunks_str = std::to_string(total_chunks);
    if (total_chunks_str.length() > 4) {
        printError("Total chunks string too long!");
        total_chunks_str = "9999";
    }
    while (total_chunks_str.length() < 4) total_chunks_str.insert(0, "0");
    header_data.insert(header_data.end(), total_chunks_str.begin(), total_chunks_str.end());

    // 7. Pad header to FIXED_TOTAL_HEADER_LENGTH
    size_t current_header_size = header_data.size();
    if (current_header_size < FIXED_TOTAL_HEADER_LENGTH) {
        for (size_t i = 0; i < FIXED_TOTAL_HEADER_LENGTH - current_header_size; ++i) {
            header_data.push_back(0x00); // Pad with null bytes
        }
    } else if (current_header_size > FIXED_TOTAL_HEADER_LENGTH) {
        printError("Logic error: Calculated header size exceeds FIXED_TOTAL_HEADER_LENGTH. Truncating.");
        header_data.resize(FIXED_TOTAL_HEADER_LENGTH);
    }

    // Combine header and chunk data
    std::vector<uint8_t> chunk_data_with_header = header_data;
    chunk_data_with_header.insert(chunk_data_with_header.end(), chunk_data_segment.begin(), chunk_data_segment.end());

    std::cout << "DIAGNOSTIC: processChunk " << chunk_index 
              << ": chunk_data_with_header.size() = " << chunk_data_with_header.size()
              << ", max_image_file_size_param = " << max_image_file_size_param << std::endl;

    // Calculate optimal dimensions for the image
    size_t width = 0, height = 0;
    constexpr size_t bytes_per_pixel = 3;
    constexpr size_t bmp_header_size = 54; // Standard BMP header size

    if (chunk_index == static_cast<int>(total_chunks - 1)) {
        // Special handling for the last chunk if needed (e.g., different aspect ratio or metadata)
        auto dims = optimizeLastImageDimensions(chunk_data_with_header.size(), bytes_per_pixel, 0, bmp_header_size, 1.0f);
        width = dims.first;
        height = dims.second;
    } else {
        auto dims = calculateOptimalRectDimensions(chunk_data_with_header.size(), bytes_per_pixel, bmp_header_size,
                                                 max_image_file_size_param, 1.0f);
        width = dims.first;
        height = dims.second;
    }

    if (width == 0 || height == 0 || (width * height * bytes_per_pixel < chunk_data_with_header.size())) {
        std::cerr << "DIAGNOSTIC_ERROR: Failed to calculate valid dimensions for chunk " << chunk_index 
                  << ". Data size: " << chunk_data_with_header.size() << std::endl;
        return; // Skips creating an image for this chunk
    }

    // Create BitmapImage
    // This allocation (width * height * 3) can be significant.
    // ResourceManager could track this if BitmapImage was adapted or if we allocated buffer externally.
    BitmapImage bmp(static_cast<int>(width), static_cast<int>(height));
    bmp.setData(chunk_data_with_header, 0); // Embed data (header + chunk data)

    // Generate output filename for this chunk
    std::string output_filename = output_base_path + "_" + std::to_string(chunk_index + 1) + "of" +
                                  std::to_string(total_chunks) + ".bmp";

    // Push task to writer queue
    image_task_queue.push({output_filename, std::move(bmp)});

    printMessage("Chunk " + std::to_string(chunk_index + 1) + "/" + std::to_string(total_chunks) +
                 " processed. Output: " + output_filename + " (Data size: " +
                 std::to_string(current_chunk_actual_data_length) + ", Header: " + std::to_string(
                     header_data.size()) + ")");
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
        if (maxMemoryMB >= 64) {
            // Use explicitly specified memory limit
            resManager.setMaxMemory(static_cast<size_t>(maxMemoryMB) * 1024 * 1024);
        } else {
            // Default to 1GB if not specified or 64 > X
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
        size_t chunk_size_bytes = static_cast<size_t>(maxChunkSizeMB) * 1024 * 1024;
        chunk_size_bytes = std::min(chunk_size_bytes, file_size); // Ensure chunk_size_bytes isn't larger than the file itself

        // Calculate how many chunks we'll need based on chunk size
        auto num_chunks = (file_size + chunk_size_bytes - 1) / chunk_size_bytes;
        if (chunk_size_bytes == 0 && file_size > 0) { // Avoid division by zero if maxChunkSizeMB was 0 but file not empty
            printError("maxChunkSizeMB is 0, leading to zero chunk_size_bytes. Cannot process non-empty file.");
            return false;
        }
        if (file_size == 0) { // Already handled, but good to be defensive for num_chunks
            num_chunks = 0;
        }

        printMessage("Processing file: " + input_file + " (Size: " + std::to_string(file_size) + " bytes)");
        printMessage(
            "Max chunk size: " + std::to_string(maxChunkSizeMB) + "MB (" + std::to_string(
                chunk_size_bytes) + " bytes)");
        printMessage("Number of chunks: " + std::to_string(num_chunks));
        printMessage(
            "Max threads: " + std::to_string(maxThreads) + ", Max memory: " + std::to_string(maxMemoryMB) + "MB");

        // max_image_file_size calculation adjustment
        size_t actual_data_per_chunk_nominal = chunk_size_bytes; 
        constexpr size_t internal_chunk_header_size = 48; // As defined in processChunk's header_data logic
        size_t min_payload_size_for_a_chunk = actual_data_per_chunk_nominal + internal_chunk_header_size;
        constexpr size_t bmp_disk_header_size = 54;
        // Estimate min image file size: BMP disk header + (data payload + internal header) + ~5% for padding/row alignment worst case
        size_t estimated_min_image_file_size_needed = bmp_disk_header_size + min_payload_size_for_a_chunk + (min_payload_size_for_a_chunk / 20); 

        size_t memory_derived_max_size = (resManager.getMaxMemory() / (num_chunks > 0 ? num_chunks : 1)) / 2;
        if (resManager.getMaxMemory() == 0 || (num_chunks > 0 && resManager.getMaxMemory() / num_chunks == 0)) { // Check for near-zero due to division
            memory_derived_max_size = DEFAULT_MAX_IMAGE_SIZE; // Fallback if memory calculation is too small
            std::cerr << "DIAGNOSTIC_WARN: memory_derived_max_size was calculated too low or zero. Defaulting part of max_image_file_size calc." << std::endl;
        }

        auto max_image_file_size = std::min(
            DEFAULT_MAX_IMAGE_SIZE, // Global cap (100MB)
            std::max(estimated_min_image_file_size_needed, memory_derived_max_size) // Ensure it's at least what a chunk needs
        );
        
        // Additional safety: If total_chunks is 1, memory_derived_max_size can be large.
        // Ensure max_image_file_size isn't excessively large if not needed.
        if (num_chunks == 1) {
            max_image_file_size = std::min(max_image_file_size, DEFAULT_MAX_IMAGE_SIZE);
            max_image_file_size = std::max(max_image_file_size, estimated_min_image_file_size_needed);
        } else if (num_chunks > 1 && max_image_file_size < estimated_min_image_file_size_needed) {
             // This case implies memory_derived_max_size was smaller than estimated_min_image_file_size_needed,
             // and DEFAULT_MAX_IMAGE_SIZE was also smaller (unlikely unless DEFAULT_MAX_IMAGE_SIZE is tiny).
             // The std::max should have handled it, but as a fallback, issue a warning if it's still too small.
             std::cerr << "DIAGNOSTIC_WARN: Calculated max_image_file_size (" << max_image_file_size 
                       << ") is still less than estimated minimum needed (" << estimated_min_image_file_size_needed
                       << "). Clamping up." << std::endl;
             max_image_file_size = estimated_min_image_file_size_needed;
        }
        // Final clamp to ensure it doesn't exceed DEFAULT_MAX_IMAGE_SIZE due to large estimates
        max_image_file_size = std::min(max_image_file_size, DEFAULT_MAX_IMAGE_SIZE);


        std::cout << "DIAGNOSTIC: actual_data_per_chunk_nominal (from maxChunkSizeMB): " << actual_data_per_chunk_nominal << std::endl;
        std::cout << "DIAGNOSTIC: estimated_min_image_file_size_needed (for a chunk's data + headers + BMP overhead estimate): " << estimated_min_image_file_size_needed << std::endl;
        std::cout << "DIAGNOSTIC: memory_derived_max_size (target per image based on RAM/chunks): " << memory_derived_max_size << std::endl;
        std::cout << "DIAGNOSTIC: Calculated max_image_file_size (final value passed to chunks): " << max_image_file_size << std::endl;

        // Open the input file using memory mapping
        MemoryMappedFile mapped_file;
        if (!mapped_file.open(input_file)) {
            printError("parseToImage: Failed to open or map input file: " + input_file);
            // Ensure writer thread is properly joined even on early exit
            return false;
        }

        size_t original_file_size = mapped_file.getSize();
        const unsigned char *all_file_data_ptr = mapped_file.getData();

        if (original_file_size == 0 && !mapped_file.isOpen() && input_file.length() > 0) {
            // Check if open failed and wasn't just an empty file that opened correctly
            printError(
                "parseToImage: Failed to get size or data ptr, possibly map failed silently after open for non-empty file: "
                + input_file);
            // MMF::open should have printed specific error from GetLastError
            // Ensure writer thread is properly joined
            return false; // Critical failure if file is not empty but we can't access it
        }

        if (original_file_size == 0) {
            printWarning("Input file is empty. No images will be generated.");
            // Let writer thread terminate gracefully
            return true;
        }


        // Create task queue for image processing
        // This queue will hold tasks that need to be written to disk
        std::string spill_dir_path_str;
        // output_dir is derived from output_base (cleanOutputPath) a few lines above.
        // output_dir is the parent directory where output images will be saved.
        if (!output_dir.empty()) {
            // Ensure output_dir itself exists before constructing a path within it.
            // The queue constructor will attempt to create the spill_dir_path_str.
            if (!std::filesystem::exists(output_dir)) {
                 // This should have been created already by lines 431-434 if output_dir was not empty.
                 // If it's still not there, it's an issue, but we'll let the queue try to create its spill path.
                 printWarning("Output directory '" + output_dir.string() + "' does not exist. Spill path might be problematic.");
            }
            spill_dir_path_str = (output_dir / ".spill_tasks").string();
        } else {
            // If output_dir is empty, it means output_base was likely just a filename
            // (e.g., "myarchive"), so output files go to CWD.
            // Spill tasks will also go to a subdirectory in CWD.
            spill_dir_path_str = ".spill_tasks";
            if (gDebugMode) { // Only print if debugging, as this is a normal case for relative paths
                printMessage("Output base path has no parent directory; spill tasks will be in './.spill_tasks'");
            }
        }

        // Default max_in_memory_items_ is 1000. The queue's constructor will attempt to create the spill directory.
        ThreadSafeQueueTemplate<ImageTaskInternal> task_queue(1000, spill_dir_path_str);

        // Start the background image writer thread
        // This thread will run concurrently with processing threads
        std::atomic<bool> should_terminate_writer(false);
        std::thread writer_thread(imageWriterThread, std::ref(task_queue), std::ref(should_terminate_writer));

        // Submit tasks to ResourceManager
        for (size_t i = 0; i < num_chunks; ++i) {
            // Capture variables for the lambda, ensuring correct lifetime and access
            // Pass all_file_data_ptr and original_file_size to the lambda and then to processChunk.
            resManager.runWithThread([
                    i, chunk_size_bytes, num_chunks, original_file_size, all_file_data_ptr,
                    input_file_copy = input_file, // Pass input_file by value (copy) for safety in lambda
                    output_base_copy = output_base, // Pass output_base by value (copy)
                    &task_queue, // Pass task_queue by reference (it's thread-safe)
                    max_image_file_size
                ]() {
                    processChunk(static_cast<int>(i), chunk_size_bytes, num_chunks, original_file_size,
                                 all_file_data_ptr, input_file_copy, output_base_copy,
                                 task_queue, max_image_file_size);
                });
        }

        // Wait for all processing tasks to complete
        resManager.waitForAllThreads();

        // Signal writer thread to terminate and wait for it
        should_terminate_writer = true;
        if (writer_thread.joinable()) {
            writer_thread.join();
        }

        // mapped_file will be closed automatically by its destructor.
        printMessage("File processing complete.");
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
