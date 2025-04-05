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
std::pair<size_t, size_t> calculateOptimalRectDimensions(size_t total_bytes, size_t bytes_per_pixel, size_t bmp_header_size,
                                                         size_t max_size_bytes, float aspect_ratio = 1.0f) {
    // Total pixels needed for header + file data
    size_t total_pixels = (total_bytes + bytes_per_pixel - 1) / bytes_per_pixel;

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

void createImageHeader(VectorWrapper<unsigned char> &image_data, const std::string &original_filename,
                       size_t actual_size, size_t img_index, size_t total_images) {
    // Calculate header components
    size_t filename_length = original_filename.size();
    std::string data_size_str = std::format("{:010d}", actual_size); // Requires C++20, else use snprintf
    std::string current_chunk_str = std::format("{:04d}", img_index + 1);
    std::string total_chunks_str = std::format("{:04d}", total_images);

    // Calculate header data size (excluding the 3-byte length field and padding)
    size_t header_data_size = 2 + filename_length + 10 + 4 + 4; // filename_length + filename + data_size + current_chunk + total_chunks

    // Calculate padding to make total header length a multiple of 3
    size_t total_header_size_without_padding = 3 + header_data_size;
    size_t pad_needed = (3 - (total_header_size_without_padding % 3)) % 3;
    size_t total_header_size = total_header_size_without_padding + pad_needed;

    // Write total header length (3 bytes)
    image_data.push_back(static_cast<unsigned char>((total_header_size >> 16) & 0xFF));
    image_data.push_back(static_cast<unsigned char>((total_header_size >> 8) & 0xFF));
    image_data.push_back(static_cast<unsigned char>(total_header_size & 0xFF));

    // Write filename length (2 bytes)
    image_data.push_back(static_cast<unsigned char>((filename_length >> 8) & 0xFF));
    image_data.push_back(static_cast<unsigned char>(filename_length & 0xFF));

    // Write filename
    for (char c : original_filename) {
        image_data.push_back(static_cast<unsigned char>(c));
    }

    // Write data_size (10 bytes)
    for (char c : data_size_str) {
        image_data.push_back(static_cast<unsigned char>(c));
    }

    // Write current_chunk (4 bytes)
    for (char c : current_chunk_str) {
        image_data.push_back(static_cast<unsigned char>(c));
    }

    // Write total_chunks (4 bytes)
    for (char c : total_chunks_str) {
        image_data.push_back(static_cast<unsigned char>(c));
    }

    // Add padding
    for (size_t i = 0; i < pad_needed; ++i) {
        image_data.push_back('\0');
    }
}

// Modified processImageTask function to maintain consistent headers
void processImageTask(const ImageTask &task, std::ifstream &file, size_t bytes_per_pixel,
                      size_t bmp_header_size, size_t max_size_bytes, size_t header_size, float aspect_ratio = 1.0f) {
    // Create output filename for the image file (.bmp extension)
    std::string current_outfile;
    if (task.total_images > 1) {
        current_outfile = task.base_outfile + "_" + std::to_string(task.img_index + 1) + "of" +
                          std::to_string(task.total_images) + ".bmp";
    } else {
        current_outfile = task.base_outfile + ".bmp";
    }

    // Read data from file
    std::vector<unsigned char> file_data = readFileSegment(file, task.file_position, task.bytes_to_read);
    size_t actual_size = file_data.size();

    // Create image data with header
    VectorWrapper<unsigned char> image_data;
    image_data.reserve(header_size + file_data.size());

    // Extract the original filename and extension from task.filename
    std::string original_filename = task.filename;
    std::string original_base;
    std::string original_extension;

    // Extract original extension
    auto dot_pos = original_filename.find_last_of('.');
    if (dot_pos != std::string::npos) {
        original_extension = original_filename.substr(dot_pos);
        original_base = original_filename.substr(0, dot_pos);
    } else {
        original_base = original_filename;
        original_extension = "";
    }

    // For the header, use original base name + original extension
    std::string header_filename = task.base_outfile + original_extension;

    // Add header information with original extension
    createImageHeader(image_data, header_filename, actual_size, task.img_index, task.total_images);

    // Append file data after the header
    for (unsigned char byte: file_data) {
        image_data.push_back(byte);
    }

    // Ensure the total image_data length is a multiple of 3 bytes
    size_t remainder = image_data.size() % 3;
    if (remainder != 0) {
        size_t pad = 3 - remainder;
        for (size_t i = 0; i < pad; ++i) {
            image_data.push_back(0); // Use 0 as a padding byte
        }
    }

    // The rest of the function remains unchanged...
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

    // Save image with .bmp extension
    if (image.save_image(current_outfile)) {
        // Get actual file size after saving
        std::ifstream check_file(current_outfile, std::ios::binary | std::ios::ate);
        std::streamsize actual_file_size = check_file.tellg();
        check_file.close();

        std::stringstream ss;
        ss << "Created image " << (task.img_index + 1) << " of " << task.total_images << ": "
                << current_outfile << std::endl
                << "  Original file: " << task.filename << std::endl
                << "  Header uses extension: " << original_extension << std::endl
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
    std::string extension = ".bmp";

    // Remove any existing extension from the output filename
    auto dot_pos = base_outfile.find_last_of('.');
    if (dot_pos != std::string::npos) {
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
            .filename = filename  // Store the original filename with extension
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
