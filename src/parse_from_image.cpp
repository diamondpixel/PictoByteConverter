#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <algorithm>
#include <filesystem>
#include <regex>
#include "headers/bitmap.hpp"
#include "headers/parse_from_image.h"

#ifndef PARSEFROM
#define PARSEFROM

/**
 * Get the file size of a file
 *
 * @param filename Path to the file
 * @return Size of the file in bytes
 */
std::ifstream::pos_type filesize(const char *filename) {
    std::ifstream in(filename, std::ifstream::ate | std::ifstream::binary);
    return in.tellg();
}

/**
 * Process a single image file to extract the encoded binary data
 *
 * @param filename Path to the image file
 * @param append_mode If true, will append to existing output file instead of overwriting
 * @param output_mutex Mutex for synchronizing file output operations
 * @param print_mutex Mutex for synchronizing console output
 * @return True if processing was successful
 */
bool processImageFile(const std::string &filename, bool append_mode,
                     std::mutex &output_mutex, std::mutex &print_mutex) {
    try {
        // Load the image file
        bitmap_image image(filename);

        if (!image) {
            std::lock_guard<std::mutex> print_lock(print_mutex);
            std::cerr << "Error: Failed to load image " << filename << std::endl;
            return false;
        }

        // Reserve enough space for all pixel data
        std::vector<unsigned char> data;
        data.reserve(static_cast<size_t>(image.width()) * static_cast<size_t>(image.height()) * 3);

        {
            std::lock_guard<std::mutex> print_lock(print_mutex);
            std::cout << "Processing image: " << filename
                      << " (" << image.width() << "x" << image.height() << ")" << std::endl;
        }

        // Read all pixels from the image in row-major order
        for (unsigned int i = 0; i < image.height(); ++i) {
            for (unsigned int j = 0; j < image.width(); ++j) {
                rgb_t rgb = image.get_pixel(j, i);
                data.push_back(rgb.red);
                data.push_back(rgb.green);
                data.push_back(rgb.blue);
            }
        }

        auto data_iter = data.begin();

        // 1. Extract the output filename from the encoded data (until first '#')
        std::string outfile = "";
        while (data_iter != data.end() && *data_iter != '#') {
            outfile += static_cast<char>(*data_iter);
            ++data_iter;
        }
        if (data_iter == data.end()) {
            std::lock_guard<std::mutex> print_lock(print_mutex);
            std::cerr << "Error: Header not found (outfile) in " << filename << std::endl;
            return false;
        }
        ++data_iter; // Skip the '#' delimiter

        // 2. Extract the data size (exactly 10 digits)
        char numbuf[11]; // 10 digits + null terminator
        for (int i = 0; i < 10; ++i) {
            if (data_iter == data.end()) {
                std::lock_guard<std::mutex> print_lock(print_mutex);
                std::cerr << "Error: Header incomplete (file size) in " << filename << std::endl;
                return false;
            }
            numbuf[i] = static_cast<char>(*data_iter);
            ++data_iter;
        }
        numbuf[10] = '\0';
        size_t data_size = static_cast<size_t>(std::stoull(numbuf));

        if (data_iter == data.end() || *data_iter != '#') {
            std::lock_guard<std::mutex> print_lock(print_mutex);
            std::cerr << "Error: Header missing delimiter after file size in " << filename << std::endl;
            return false;
        }
        ++data_iter; // Skip the '#' delimiter

        // 3. Extract the index info (e.g. "0001-0002")
        std::string index_info = "";
        while (data_iter != data.end() && *data_iter != '#') {
            index_info += static_cast<char>(*data_iter);
            ++data_iter;
        }
        if (data_iter == data.end()) {
            std::lock_guard<std::mutex> print_lock(print_mutex);
            std::cerr << "Error: Header incomplete (index info) in " << filename << std::endl;
            return false;
        }
        ++data_iter; // Skip the '#' delimiter

        // Parse index info to determine the image's position in the sequence
        std::regex index_pattern(R"(\d{4}-\d{4})");
        if (!std::regex_match(index_info, index_pattern)) {
            std::lock_guard<std::mutex> print_lock(print_mutex);
            std::cerr << "Warning: Invalid index format in " << filename
                      << ": " << index_info << std::endl;
        }

        // Prepare to write data to the output file
        {
            std::lock_guard<std::mutex> print_lock(print_mutex);
            std::cout << "Extracted metadata:" << std::endl
                      << "  Output file: " << outfile << std::endl
                      << "  Data size: " << data_size << " bytes" << std::endl
                      << "  Index info: " << index_info << std::endl;
        }

        // Calculate the distance from data_iter to data.end()
        size_t available_data = std::distance(data_iter, data.end());
        if (available_data < data_size) {
            std::lock_guard<std::mutex> print_lock(print_mutex);
            std::cerr << "Warning: Image contains less data than expected. "
                      << "Expected: " << data_size << ", Available: " << available_data << std::endl;
            // Continue with what we have
            data_size = available_data;
        }

        // Open and write to the output file with proper synchronization
        {
            std::lock_guard<std::mutex> output_lock(output_mutex);

            // Determine file open mode (append or overwrite)
            std::ios_base::openmode mode = std::ios::binary;
            if (append_mode) {
                mode |= std::ios::app; // Append mode
            } else {
                mode |= std::ios::trunc; // Overwrite mode
            }

            std::ofstream file(outfile, mode);
            if (!file) {
                std::lock_guard<std::mutex> print_lock(print_mutex);
                std::cerr << "Error: Could not open output file " << outfile << std::endl;
                return false;
            }

            // Get current position in file (for reporting)
            std::streampos start_pos = file.tellp();

            // Write the data to the output file
            for (size_t i = 0; i < data_size; ++i) {
                if (data_iter == data.end()) {
                    std::lock_guard<std::mutex> print_lock(print_mutex);
                    std::cerr << "Error: Not enough data in image" << std::endl;
                    break;
                }
                file.put(static_cast<char>(*data_iter));
                ++data_iter;
            }

            // Get end position to calculate bytes written
            std::streampos end_pos = file.tellp();
            size_t bytes_written = static_cast<size_t>(end_pos - start_pos);

            file.close();

            std::lock_guard<std::mutex> print_lock(print_mutex);
            std::cout << "Successfully wrote " << bytes_written << " bytes from "
                      << filename << " to " << outfile << std::endl;
        }

        return true;
    }
    catch (const std::exception &e) {
        std::lock_guard<std::mutex> print_lock(print_mutex);
        std::cerr << "Exception processing " << filename << ": " << e.what() << std::endl;
        return false;
    }
}

/**
 * Extract binary data from one or more image files
 * Can process a single image or multiple images that make up a larger file
 *
 * @param filename Path to the image file, or pattern for multiple files
 */
void parseFromImage(const std::string &filename) {
    // Check if this is a pattern with wildcards or a directory
    bool is_pattern = (filename.find('*') != std::string::npos ||
                       filename.find('?') != std::string::npos);
    bool is_directory = std::filesystem::is_directory(filename);

    std::vector<std::string> files_to_process;

    if (is_pattern) {
        // This is a glob pattern - get matching files from current directory
        std::string directory = ".";
        std::string pattern = filename;

        // If pattern contains a path separator, extract directory
        size_t last_separator = filename.find_last_of("/\\");
        if (last_separator != std::string::npos) {
            directory = filename.substr(0, last_separator);
            pattern = filename.substr(last_separator + 1);
        }

        // Compile regex pattern from glob pattern (simplified)
        std::string regex_pattern = "^";
        for (char c : pattern) {
            if (c == '*') regex_pattern += ".*";
            else if (c == '?') regex_pattern += ".";
            else if (c == '.') regex_pattern += "\\.";
            else regex_pattern += c;
        }
        regex_pattern += "$";
        std::regex file_regex(regex_pattern);

        // Find all matching files
        try {
            for (const auto &entry : std::filesystem::directory_iterator(directory)) {
                if (entry.is_regular_file()) {
                    std::string filename = entry.path().filename().string();
                    if (std::regex_match(filename, file_regex)) {
                        files_to_process.push_back(entry.path().string());
                    }
                }
            }
        }
        catch (const std::exception &e) {
            std::cerr << "Error accessing directory: " << e.what() << std::endl;
            return;
        }
    }
    else if (is_directory) {
        // Process all BMP files in directory
        try {
            for (const auto &entry : std::filesystem::directory_iterator(filename)) {
                if (entry.is_regular_file() &&
                    entry.path().extension() == ".bmp") {
                    files_to_process.push_back(entry.path().string());
                }
            }
        }
        catch (const std::exception &e) {
            std::cerr << "Error accessing directory: " << e.what() << std::endl;
            return;
        }
    }
    else {
        // Single file
        files_to_process.push_back(filename);
    }

    // Sort files by name to ensure correct order
    std::sort(files_to_process.begin(), files_to_process.end(),
             [](const std::string &a, const std::string &b) {
                 // Extract indices from filenames like "file_1of10.bmp"
                 std::regex index_pattern(R"(.*_(\d+)of\d+\..*)");
                 std::smatch match_a, match_b;

                 bool has_index_a = std::regex_match(a, match_a, index_pattern);
                 bool has_index_b = std::regex_match(b, match_b, index_pattern);

                 if (has_index_a && has_index_b) {
                     return std::stoi(match_a[1]) < std::stoi(match_b[1]);
                 }
                 return a < b;
             });

    if (files_to_process.empty()) {
        std::cerr << "No matching files found for: " << filename << std::endl;
        return;
    }

    std::cout << "Found " << files_to_process.size() << " files to process" << std::endl;

    // Determine if we should use multi-threading
    unsigned int num_threads = 1;
    bool parallel_mode = false;

    // Use multi-threading only if we have multiple files
    if (files_to_process.size() > 1) {
        // Use number of cores minus 1, but no more than files to process
        num_threads = std::min(static_cast<unsigned int>(files_to_process.size()),
                              std::max(1u, std::thread::hardware_concurrency() - 1));
        parallel_mode = (num_threads > 1);
    }

    if (parallel_mode) {
        std::cout << "Processing files using " << num_threads << " threads" << std::endl;

        // Set up threading resources
        std::mutex output_mutex;  // For synchronizing file output
        std::mutex print_mutex;   // For synchronizing console output
        std::vector<std::thread> threads;
        std::queue<size_t> work_queue;
        std::mutex queue_mutex;
        std::condition_variable cv;
        std::atomic<size_t> processed_files(0);
        std::atomic<size_t> successful_files(0);

        // Worker function
        auto process_files = [&](int thread_id) {
            while (true) {
                // Get next file index from queue
                size_t file_index;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex);
                    cv.wait(lock, [&]() {
                        return !work_queue.empty() || processed_files >= files_to_process.size();
                    });

                    if (processed_files >= files_to_process.size() && work_queue.empty()) {
                        break;  // No more work
                    }

                    file_index = work_queue.front();
                    work_queue.pop();
                }

                {
                    std::lock_guard<std::mutex> lock(print_mutex);
                    std::cout << "Thread " << thread_id << " processing file: "
                              << files_to_process[file_index] << std::endl;
                }

                // Process the file - append mode for any file after the first one
                bool success = processImageFile(
                    files_to_process[file_index],
                    file_index > 0, // First file overwrites, others append
                    output_mutex,
                    print_mutex
                );

                if (success) {
                    successful_files++;
                }

                processed_files++;
            }
        };

        // Start worker threads
        for (unsigned int i = 0; i < num_threads; ++i) {
            threads.emplace_back(process_files, i);
        }

        // Add all files to work queue
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            for (size_t i = 0; i < files_to_process.size(); ++i) {
                work_queue.push(i);
            }
        }

        // Notify threads that work is available
        cv.notify_all();

        // Wait for all threads to finish
        for (auto &thread : threads) {
            thread.join();
        }

        std::cout << "Finished processing " << files_to_process.size()
                  << " files. Successful: " << successful_files << std::endl;
    }
    else {
        // Single-threaded mode
        std::cout << "Processing files in single-threaded mode" << std::endl;

        std::mutex output_mutex;
        std::mutex print_mutex;
        size_t successful_files = 0;

        for (size_t i = 0; i < files_to_process.size(); ++i) {
            bool success = processImageFile(
                files_to_process[i],
                i > 0, // First file overwrites, others append
                output_mutex,
                print_mutex
            );

            if (success) {
                successful_files++;
            }
        }

        std::cout << "Finished processing " << files_to_process.size()
                  << " files. Successful: " << successful_files << std::endl;
    }
}

#endif