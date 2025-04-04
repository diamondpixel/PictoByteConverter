#include <string>
#include <fstream>
#include <cmath>
#include <iostream>
#include <vector>
#include <filesystem>
#include <thread>
#include <mutex>
#include <sstream>
#include "headers/specialvector.h"
#include "headers/bitmap.hpp"
#include "headers/parse_to_image.h"
#include "headers/image_task.h"
#include "headers/thread_safe_queue.h"

// Mutex for synchronized console output
std::mutex console_mutex;

// Function to print thread-safe messages
void printMessage(const std::string &message) {
    std::lock_guard<std::mutex> lock(console_mutex);
    std::cout << message << std::endl;
}

// Function to calculate optimal rectangular dimensions based on aspect ratio
std::pair<size_t, size_t> calculateOptimalRectDimensions(size_t data_bytes, size_t bytes_per_pixel, size_t bmp_header_size,
                                  size_t max_size_bytes, float aspect_ratio = 1.0f) {
    // Calculate total number of pixels needed
    size_t total_pixels = (data_bytes + bytes_per_pixel - 1) / bytes_per_pixel;

    // Calculate width and height based on aspect ratio
    auto width = static_cast<size_t>(std::sqrt(static_cast<double>(total_pixels * aspect_ratio)));
    auto height = static_cast<size_t>(width / aspect_ratio);

    // Ensure width and height multiply to have enough pixels
    while (width * height < total_pixels) {
        width++;
        height = static_cast<size_t>(width / aspect_ratio);
    }

    // Ensure minimum dimensions (but not too small)
    size_t min_dimension = 64; // Larger minimum to avoid tiny dimensions
    if (width < min_dimension) width = min_dimension;
    if (height < min_dimension) height = min_dimension;

    // Limit dimensions to reasonable values
    if (width > 65535) width = 65535;
    if (height > 65535) height = 65535;

    // Calculate row padding (BMP rows must be aligned to 4 bytes)
    size_t row_bytes = width * bytes_per_pixel;
    size_t padding = (4 - (row_bytes % 4)) % 4;

    // Calculate actual bytes capacity including padding
    size_t actual_image_size = bmp_header_size + (height * (width * bytes_per_pixel + padding));

    // If we're exceeding the limit, reduce dimensions
    while (actual_image_size > max_size_bytes) {
        if (width > height) {
            width--;
        } else {
            height--;
        }

        row_bytes = width * bytes_per_pixel;
        padding = (4 - (row_bytes % 4)) % 4;
        actual_image_size = bmp_header_size + (height * (width * bytes_per_pixel + padding));
    }

    return {width, height};
}

// Function to optimize dimensions for the last image
std::pair<size_t, size_t> optimizeLastImageDimensions(size_t actual_size, size_t header_size, size_t bytes_per_pixel,
                                                      size_t bmp_header_size, size_t max_size_bytes,
                                                      float aspect_ratio = 1.0f) {
    // Calculate exact number of pixels needed
    size_t total_data_size = actual_size + header_size;
    size_t pixels_needed = (total_data_size + bytes_per_pixel - 1) / bytes_per_pixel;

    // Calculate width and height based on aspect ratio
    auto width = static_cast<size_t>(std::sqrt(static_cast<double>(pixels_needed * aspect_ratio)));
    auto height = static_cast<size_t>(width / aspect_ratio);

    // Ensure width and height multiply to have enough pixels
    while (width * height < pixels_needed) {
        width++;
        height = static_cast<size_t>(width / aspect_ratio);
    }

    // Ensure minimum dimensions
    if (width < 16) width = 16;
    if (height < 16) height = 16;

    // Calculate row padding and check if dimensions are within limits
    size_t row_bytes = width * bytes_per_pixel;
    size_t padding = (4 - (row_bytes % 4)) % 4;
    size_t test_size = bmp_header_size + (height * (width * bytes_per_pixel + padding));

    if (test_size <= max_size_bytes) {
        return {width, height};
    } else {
        return {0, 0}; // Return 0,0 if optimization not possible
    }
}

// Function to read data from the input file
std::vector<unsigned char> readFileSegment(std::ifstream &file, size_t position, size_t bytes_to_read) {
    std::vector<unsigned char> data;
    constexpr size_t chunk_size = 1024 * 1024; // 1MB chunks for reading
    std::vector<char> buffer(chunk_size);

    std::unique_lock<std::mutex> lock(console_mutex); // Lock for file access
    file.seekg(static_cast<std::streamoff>(position), std::ios::beg);

    size_t actual_size = 0;
    while (file && actual_size < bytes_to_read) {
        size_t current_chunk = std::min(chunk_size, bytes_to_read - actual_size);

        if (file.read(buffer.data(), static_cast<std::streamsize>(current_chunk))) {
            auto bytes_read = static_cast<size_t>(file.gcount());
            data.insert(data.end(), buffer.begin(), buffer.begin() + bytes_read);
            actual_size += bytes_read;
        } else {
            break;
        }
    }
    lock.unlock();

    return data;
}

// Function to create header data for the image
void createImageHeader(VectorWrapper<unsigned char> &image_data, const std::string &filename,
                       size_t actual_size, size_t img_index, size_t total_images) {
    // Add filename to image data
    for (unsigned char character: filename) {
        image_data.push_back(character);
    }
    image_data.push_back('#');

    // Add file size (10 digits)
    char buffer_size[21];
    snprintf(buffer_size, sizeof(buffer_size), "%010zu", actual_size);
    for (size_t i = 0; i < 10; ++i) {
        image_data.push_back(buffer_size[i]);
    }
    image_data.push_back('#');

    // Add image index and total count (X of Y format)
    char index_info[11];
    snprintf(index_info, sizeof(index_info), "%04zu-%04zu", img_index + 1, total_images);
    for (size_t i = 0; i < 10 && index_info[i] != '\0'; ++i) {
        image_data.push_back(index_info[i]);
    }
    image_data.push_back('#');
}

// Modified processImageTask function for the last image
void processImageTask(const ImageTask &task, std::ifstream &file, size_t bytes_per_pixel,
                      size_t bmp_header_size, size_t max_size_bytes, size_t header_size, float aspect_ratio = 1.0f) {
    // Create output filename
    std::string current_outfile;
    if (task.total_images > 1) {
        current_outfile = task.base_outfile + "_" + std::to_string(task.img_index + 1) + "of" +
                          std::to_string(task.total_images) + task.extension;
    } else {
        current_outfile = task.base_outfile + task.extension;
    }

    // Read data from file
    std::vector<unsigned char> file_data = readFileSegment(file, task.file_position, task.bytes_to_read);
    size_t actual_size = file_data.size();

    // Create image data with header
    VectorWrapper<unsigned char> image_data;
    image_data.reserve(header_size + file_data.size());

    // Add header information
    createImageHeader(image_data, task.filename, actual_size, task.img_index, task.total_images);

    // Add file data
    for (unsigned char byte: file_data) {
        image_data.push_back(byte);
    }

    // Default to the task dimensions
    size_t current_width = task.width;
    size_t current_height = task.height;

    // Optimize the last image to better fit the data
    // But ensure dimensions remain reasonable
    if (task.img_index == task.total_images - 1 && actual_size < task.bytes_capacity && task.total_images > 1) {
        // Calculate the exact number of pixels needed
        size_t total_data_size = actual_size + header_size;
        size_t pixels_needed = (total_data_size + bytes_per_pixel - 1) / bytes_per_pixel;

        // Keep reasonable proportions - don't go below 1/4 of original dimensions
        size_t min_width = task.width / 4;
        size_t min_height = task.height / 4;

        if (min_width < 64) min_width = 64;
        if (min_height < 64) min_height = 64;

        // Calculate width and height based on aspect ratio
        auto required_width = static_cast<size_t>(std::sqrt(static_cast<double>(pixels_needed * aspect_ratio)));
        auto required_height = static_cast<size_t>(pixels_needed / required_width);

        // Ensure we have enough pixels
        while (required_width * required_height < pixels_needed) {
            required_width++;
            required_height = static_cast<size_t>(pixels_needed / required_width);
            if (required_height == 0) required_height = 1;
        }

        // Ensure minimum dimensions
        if (required_width < min_width) required_width = min_width;
        if (required_height < min_height) required_height = min_height;

        // Calculate if this size is within limits
        size_t row_bytes_test = required_width * bytes_per_pixel;
        size_t padding_test = (4 - (row_bytes_test % 4)) % 4;
        size_t test_size = bmp_header_size + (required_height * (required_width * bytes_per_pixel + padding_test));

        if (test_size <= max_size_bytes) {
            current_width = required_width;
            current_height = required_height;
            std::stringstream ss;
            ss << "Optimized last image to size " << current_width << "x" << current_height;
            printMessage(ss.str());
        }
    }

    // Create bitmap with calculated dimensions
    bitmap_image image(static_cast<unsigned int>(current_width), static_cast<unsigned int>(current_height));

    // Fill bitmap with data
    size_t data_size = image_data.size();
    size_t data_pixels = (data_size + bytes_per_pixel - 1) / bytes_per_pixel;
    size_t total_pixels = current_width * current_height;

    // Only fill the exact pixels we need
    for (size_t pixel_index = 0; pixel_index < total_pixels; ++pixel_index) {
        size_t i = pixel_index / current_width;
        size_t j = pixel_index % current_width;

        if (pixel_index < data_pixels) {
            // We have enough data for this pixel
            auto [e0, e1, e2] = image_data.get_triplet(pixel_index, 0);
            image.set_pixel(static_cast<unsigned int>(j), static_cast<unsigned int>(i),
                         rgb_t{e0, e1, e2});
        } else {
            // No more data - fill with a distinct padding color
            image.set_pixel(static_cast<unsigned int>(j), static_cast<unsigned int>(i),
                         rgb_t{200, 200, 200});  // Light gray padding
        }
    }

    // Calculate actual data usage ratio
    double usage_ratio = static_cast<double>(data_pixels) / static_cast<double>(total_pixels) * 100.0;

    // Save image and report success
    if (image.save_image(current_outfile)) {
        // Get actual file size after saving
        std::ifstream check_file(current_outfile, std::ios::binary | std::ios::ate);
        std::streamsize actual_file_size = check_file.tellg();
        check_file.close();

        std::stringstream ss;
        ss << "Created image " << (task.img_index + 1) << " of " << task.total_images << ": "
                << current_outfile << std::endl
                << "  Dimensions: " << current_width << "x" << current_height << std::endl
                << "  Stored " << actual_size << " bytes of data" << std::endl
                << "  Image pixel usage: " << usage_ratio << "%" << std::endl
                << "  Image file size: " << actual_file_size << " bytes ("
                << (actual_file_size / (1024.0 * 1024.0)) << " MB)";
        printMessage(ss.str());

        // Verify we're under the limit
        if (actual_file_size > static_cast<std::streamsize>(max_size_bytes)) {
            std::stringstream warning;
            warning << "Warning: The image exceeds the specified size limit!";
            printMessage(warning.str());
        }
    } else {
        std::stringstream error;
        error << "Error: Failed to save image " << (task.img_index + 1) << " to " << current_outfile;
        printMessage(error.str());
    }
}

// Worker thread function
void workerThread(ThreadSafeQueue &tasks, std::ifstream &file, size_t bytes_per_pixel,
                  size_t bmp_header_size, size_t max_size_bytes, size_t header_size, float aspect_ratio = 1.0f) {
    ImageTask task;
    while (tasks.pop(task)) {
        processImageTask(task, file, bytes_per_pixel, bmp_header_size, max_size_bytes, header_size, aspect_ratio);
    }
}

// Modified parseToImage function to support rectangular images with aspect ratio
void parseToImage(const std::string &filename, const std::string &outfile, size_t max_size_mb,
                  float aspect_ratio) {
    // Open file in binary mode
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        std::cerr << "Error: Could not open file " << filename << std::endl;
        return;
    }

    // Determine file size
    file.seekg(0, std::ios::end);
    std::streamsize file_size_stream = file.tellg();
    if (file_size_stream < 0) {
        std::cerr << "Error: Could not determine file size" << std::endl;
        return;
    }
    auto file_size = static_cast<size_t>(file_size_stream);
    file.seekg(0, std::ios::beg);

    printMessage("File size: " + std::to_string(file_size) + " bytes (" +
                 std::to_string(file_size / (1024.0 * 1024.0)) + " MB)");
    printMessage("Using aspect ratio: " + std::to_string(aspect_ratio));

    // Calculate max size in bytes - exact limit
    size_t max_size_bytes = max_size_mb * 1024 * 1024;

    // BMP file header and info header size (typically 14 + 40 = 54 bytes)
    constexpr size_t bmp_header_size = 54;

    // Calculate our metadata header size
    size_t header_size = filename.size() + 1 + 10 + 1 + 10 + 1; // filename + # + size + # + index/count + #

    // Calculate the actual maximum data we can store per image
    size_t data_bytes_per_image = max_size_bytes - bmp_header_size - header_size;

    // Each pixel holds 3 bytes (RGB)
    constexpr size_t bytes_per_pixel = 3;

    // Calculate optimal width and height based on aspect ratio
    auto [optimal_width, optimal_height] = calculateOptimalRectDimensions(
        data_bytes_per_image, bytes_per_pixel, bmp_header_size, max_size_bytes, aspect_ratio);

    // Calculate actual bytes capacity per image
    size_t pixels_capacity = optimal_width * optimal_height;
    size_t bytes_capacity = pixels_capacity * bytes_per_pixel;

    // If file size is smaller than a single image capacity, adjust the dimensions for the actual file size
    if (file_size <= bytes_capacity) {
        // File fits in a single image, recalculate dimensions based on actual file size plus header
        size_t actual_data_size = file_size + header_size;

        auto [adjusted_width, adjusted_height] = calculateOptimalRectDimensions(
            actual_data_size, bytes_per_pixel, bmp_header_size, max_size_bytes, aspect_ratio);

        // Recalculate with padding to make sure we have enough space
        size_t row_bytes = adjusted_width * bytes_per_pixel;
        size_t padding = (4 - (row_bytes % 4)) % 4;
        size_t actual_image_size = bmp_header_size + (adjusted_height * (adjusted_width * bytes_per_pixel + padding));

        // If dimensions are valid and within limit, use them
        if (actual_image_size <= max_size_bytes) {
            optimal_width = adjusted_width;
            optimal_height = adjusted_height;
            pixels_capacity = optimal_width * optimal_height;
            bytes_capacity = pixels_capacity * bytes_per_pixel;
        }
    }

    // Calculate number of images needed based on file size
    size_t total_images = (file_size + bytes_capacity - 1) / bytes_capacity;
    if (total_images == 0) total_images = 1; // At least one image even for empty files

    std::stringstream info;
    info << "Will create " << total_images << " image(s) of size " << optimal_width << "x" << optimal_height;

    // Calculate actual image file size
    size_t row_bytes = optimal_width * bytes_per_pixel;
    size_t padding = (4 - (row_bytes % 4)) % 4;
    size_t actual_image_size = bmp_header_size + (optimal_height * (optimal_width * bytes_per_pixel + padding));

    info << std::endl << "Each image size will be approximately "
            << (actual_image_size / (1024.0 * 1024.0)) << " MB" << std::endl
            << "Each image can hold approximately " << (bytes_capacity / (1024.0 * 1024.0)) << " MB of data";
    printMessage(info.str());

    // Extract base filename and extension
    std::string base_outfile = outfile;
    std::string extension;

    // Extract extension
    auto dot_pos = base_outfile.find_last_of('.');
    if (dot_pos != std::string::npos) {
        extension = base_outfile.substr(dot_pos);
        base_outfile = base_outfile.substr(0, dot_pos);
    }

    // Create task queue
    ThreadSafeQueue tasks;

    // Create tasks for each image segment
    for (size_t img_index = 0; img_index < total_images; ++img_index) {
        // Calculate how much data to read for this image
        size_t bytes_to_read = bytes_capacity;
        if ((img_index + 1) * bytes_capacity > file_size) {
            // Last image might not be full
            bytes_to_read = file_size - (img_index * bytes_capacity);
        }

        // Create task with width and height instead of optimal_dim
        ImageTask task{
            .img_index = img_index,
            .total_images = total_images,
            .bytes_capacity = bytes_capacity,
            .bytes_to_read = bytes_to_read,
            .width = optimal_width,
            .height = optimal_height,
            .file_position = static_cast<std::streampos>(img_index * bytes_capacity),
            .base_outfile = base_outfile,
            .extension = extension,
            .filename = filename
        };

        tasks.push(task);
    }

    // Determine number of worker threads (use hardware concurrency or fall back to 4)
    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4; // Fallback if hardware_concurrency fails

    // Limit threads to the number of tasks
    num_threads = std::min(num_threads, static_cast<unsigned int>(total_images));

    printMessage("Using " + std::to_string(num_threads) + " worker threads");

    // Start worker threads with aspect ratio parameter
    std::vector<std::thread> workers;
    for (unsigned int i = 0; i < num_threads; ++i) {
        workers.emplace_back(
            [&tasks, &file, bytes_per_pixel, bmp_header_size, max_size_bytes, header_size, aspect_ratio]() {
                workerThread(tasks, file, bytes_per_pixel, bmp_header_size, max_size_bytes, header_size, aspect_ratio);
            }
        );
    }

    // Signal that all tasks have been added
    tasks.finish();

    // Wait for all worker threads to finish
    for (auto &worker: workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    printMessage("Conversion complete! " + std::to_string(total_images) + " image(s) created.");
}
