#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <mutex>
#include <algorithm>
#include <filesystem>
#include <regex>
#include <cctype>
#include <map>
#include <optional>
#include <ranges>
#include <string_view>
#include <iomanip>
#include <sstream>
#include "headers/ParseFromImage.h"

#include <future>

#include "headers/Bitmap.hpp"
#include "headers/ResourceManager.h"
#include "../Debug/headers/Debug.h"

// No need for local printing functions - use the global ones from debug.h
namespace image_parser {
    /**
     * Get the size of a file in bytes.
     *
     * @param filename Path to the file
     * @return Size of the file in bytes
     */
    std::ifstream::pos_type fileSize(const char *filename) {
        std::ifstream in(filename, std::ifstream::ate | std::ifstream::binary);
        return in.tellg();
    }

    /**
     * Extract metadata from image data
     *
     * @param data Vector containing the raw image data
     * @param data_iter Iterator pointing to the beginning of the data
     * @param print_mutex Mutex for synchronizing console output
     * @param filename Original image filename (for error messages)
     * @return Tuple containing metadata and success status
     */
    std::pair<ImageMetadata, bool> extractMetadata(
    const std::vector<unsigned char> &data,
    std::vector<unsigned char>::const_iterator &data_iter,
    std::mutex &print_mutex,
    const std::string &filename) {
    ImageMetadata metadata;

    // Store the initial pointer location to rewind if needed
    auto initial_iter = data_iter;

    // Read total_header_length (3 bytes)
    if (std::distance(data_iter, data.end()) < 3) {
        std::lock_guard<std::mutex> lock(print_mutex);
        printError("Not enough data for header length in " + filename);
        return {metadata, false};
    }
    size_t total_header_length = (static_cast<size_t>(*data_iter) << 16) |
                                 (static_cast<size_t>(*(data_iter + 1)) << 8) |
                                 static_cast<size_t>(*(data_iter + 2));
    data_iter += 3;

    // Ensure total_header_length is exactly 48 bytes
    if (total_header_length != 48) {
        std::lock_guard<std::mutex> lock(print_mutex);
        printWarning("Header length is " + std::to_string(total_header_length) +
                    " bytes, expected 48 bytes in " + filename);
        // We continue anyway but this is a potential issue
    }

    // Read filename_length (2 bytes)
    if (std::distance(data_iter, data.end()) < 2) {
        std::lock_guard<std::mutex> lock(print_mutex);
        printError("Not enough data for filename length in " + filename);
        return {metadata, false};
    }
    size_t filename_length = (static_cast<size_t>(*data_iter) << 8) | static_cast<size_t>(*(data_iter + 1));
    data_iter += 2;

    // Read filename
    if (std::distance(data_iter, data.end()) < static_cast<long>(filename_length)) {
        std::lock_guard<std::mutex> lock(print_mutex);
        printError("Not enough data for filename in " + filename);
        return {metadata, false};
    }
    metadata.outputFilename = std::string(data_iter, data_iter + filename_length);
    data_iter += filename_length;

    // Read data_size (10 bytes)
    if (std::distance(data_iter, data.end()) < 10) {
        std::lock_guard<std::mutex> lock(print_mutex);
        printError("Not enough data for data size in " + filename);
        return {metadata, false};
    }
    metadata.expectedDataSize = std::stoull(std::string(data_iter, data_iter + 10));
    data_iter += 10;

    // Read current_chunk (4 bytes)
    if (std::distance(data_iter, data.end()) < 4) {
        std::lock_guard<std::mutex> lock(print_mutex);
        printError("Not enough data for current chunk in " + filename);
        return {metadata, false};
    }
    metadata.currentChunk = std::stoi(std::string(data_iter, data_iter + 4));
    data_iter += 4;

    // Read total_chunks (4 bytes)
    if (std::distance(data_iter, data.end()) < 4) {
        std::lock_guard<std::mutex> lock(print_mutex);
        printError("Not enough data for total chunks in " + filename);
        return {metadata, false};
    }
    metadata.totalChunks = std::stoi(std::string(data_iter, data_iter + 4));
    data_iter += 4;

    // Calculate the distance from data_iter to data.end()
    size_t bytes_read = 3 + 2 + filename_length + 10 + 4 + 4;

    // We need to skip the remainder of the fixed 48-byte header
    if (total_header_length > bytes_read) {
        size_t remaining_bytes = total_header_length - bytes_read;
        if (std::distance(data_iter, data.end()) < static_cast<long>(remaining_bytes)) {
            std::lock_guard<std::mutex> lock(print_mutex);
            printError("Not enough data for remaining header padding in " + filename);
            return {metadata, false};
        }
        data_iter += remaining_bytes;
    }

    return {metadata, true};
}

    /**
     * Extract pixel data from the image
     *
     * @param image The bitmap image to process
     * @return Vector containing the extracted pixel data
     */
    std::vector<unsigned char> extractPixelData(const bitmap_image &image) {
        unsigned int width = image.width();
        unsigned int height = image.height();

        // Reserve enough space for all pixel data - explicit calculation
        size_t total_pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        size_t data_size_estimate = total_pixels * 3; // 3 bytes per pixel (RGB)

        // Request memory allocation from ResourceManager
        auto& resManager = ResourceManager::getInstance();
        bool memory_allocated = resManager.allocateMemory(data_size_estimate);

        if (!memory_allocated) {
            printWarning("Memory allocation for " + std::to_string(data_size_estimate / (1024 * 1024)) +
                         " MB failed, will try to use less memory");
            // We'll still try to create the vector but with a smaller initial capacity
            data_size_estimate = std::min(data_size_estimate, resManager.getMaxMemory() / 2);
            memory_allocated = resManager.allocateMemory(data_size_estimate);
            if (!memory_allocated) {
                printError("Could not allocate memory for pixel data");
                return std::vector<unsigned char>();
            }
        }

        std::vector<unsigned char> data;
        data.reserve(data_size_estimate); // Use allocated size

        // Read all pixels from the image in row-major order
        for (unsigned int i = 0; i < height; ++i) {
            for (unsigned int j = 0; j < width; ++j) {
                rgb_t rgb = image.get_pixel(j, i);
                data.push_back(rgb.red);
                data.push_back(rgb.green);
                data.push_back(rgb.blue);
            }
        }

        // If we used less memory than allocated, release the excess
        if (data.size() < data_size_estimate) {
            resManager.freeMemory(data_size_estimate - data.size());
        }

        return data;
    }

    /**
     * Extract payload data from a single image file
     *
     * @param filename Path to the image file
     * @param print_mutex Mutex for synchronizing console output
     * @return ChunkInfo structure with the extracted payload and metadata
     */
    std::optional<image_parser::ChunkInfo> extractChunkPayload(
        const std::string &filename,
        std::mutex &print_mutex,
        bool debug_mode)
{
    try {
        // IMPORTANT: Force debug mode check at thread entry point
        bool mainDebugMode = debug_mode;
        
        // Load the image file
        bitmap_image image(filename);

        if (!image) {
            // Use the printError function which handles locking internally
            printError("Failed to load image " + filename);
            return std::nullopt;
        }

        // Get image dimensions
        unsigned int width = image.width();
        unsigned int height = image.height();

        // Request memory for image processing based on dimensions
        auto& resManager = ResourceManager::getInstance();
        size_t estimated_memory = static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
        bool memory_allocated = resManager.allocateMemory(estimated_memory);

        if (!memory_allocated) {
            // Use the printError function which handles locking internally
            printError("Not enough memory to process image: " + filename);
            return std::nullopt;
        }

        // Extract all pixel data from the image
        std::vector<unsigned char> data = extractPixelData(image);
        if (data.empty()) {
            // Free the allocated memory if pixel extraction failed
            resManager.freeMemory(estimated_memory);
            return std::nullopt;
        }

        // Use the printMessage function which handles locking internally
        printMessage("Processing image: " + filename +
                    " (" + std::to_string(width) + "x" + std::to_string(height) + ") - Estimated capacity: " +
                    std::to_string(data.size()) + " bytes");

        // Start reading from the beginning of the data
        auto data_iter = data.begin();

        // Extract metadata from image header
        auto [metadata, success] = extractMetadata(data, data_iter, print_mutex, filename);
        if (!success) {
            return std::nullopt;
        }

        // Print metadata info
        {
            // Use the printStats function which handles locking internally
            std::string metadataInfo = "Extracted metadata:\n"
                                      "  Output file: " + metadata.outputFilename + 
                                      "\n  Data size: " + std::to_string(metadata.expectedDataSize) + " bytes" +
                                      "\n  Index info: " + metadata.indexInfo +
                                      ((metadata.currentChunk != -1 && metadata.totalChunks != -1) ?
                                      " (Chunk " + std::to_string(metadata.currentChunk) + " of " +
                                      std::to_string(metadata.totalChunks) + ")" : "");
            
            printStats(metadataInfo);
        }

        // Calculate the distance from data_iter to data.end()
        size_t available_data = std::distance(data_iter, data.end());

        // IMPORTANT: Use exactly metadata.expectedDataSize instead of limiting by available data
        // This ensures we preserve the exact size recorded in the header
        size_t payload_size = metadata.expectedDataSize;

        // Warn if there's not enough data available
        if (available_data < payload_size) {
            // Use the printWarning function which handles locking internally
            printWarning("Available data (" + std::to_string(available_data) +
                       " bytes) is less than expected size (" +
                       std::to_string(payload_size) + " bytes) in chunk " +
                       std::to_string(metadata.currentChunk));

            // We continue extracting what we can, but we record the expected size separately
            payload_size = available_data;
        }

        // Create payload result
        image_parser::ChunkInfo chunk;
        chunk.chunkIndex = metadata.currentChunk;
        chunk.totalChunks = metadata.totalChunks;
        chunk.chunkSize = payload_size;
        chunk.expectedDataSize = metadata.expectedDataSize; // Store the expected size
        chunk.filename = metadata.outputFilename;

        // Copy the payload data (excluding header)
        chunk.payload.reserve(payload_size);
        for (size_t i = 0; i < payload_size; ++i) {
            if (data_iter == data.end()) {
                break;
            }
            chunk.payload.push_back(*data_iter);
            ++data_iter;
        }
        // Use the printMessage function which handles locking internally
        printMessage("Extracted " + std::to_string(chunk.payload.size()) + " bytes of payload data from " + filename);

        // Free memory used for image processing before returning
        resManager.freeMemory(estimated_memory);
        return chunk;
    } catch (const std::exception &e) {
        // Use the printError function which handles locking internally
        printError("Exception processing " + filename + ": " + e.what());
        return std::nullopt;
    }
}

    /**
     * Find sub-bmp files when the main file isn't found
     *
     * @param base_filename The base filename (without extension)
     * @return Vector of matching sub-bmp files sorted in proper order
     */
    std::vector<std::string> findSubBmpFiles(const std::string &base_filename) {
        std::vector<std::string> result;

        try {
            std::vector<std::pair<int, std::string> > indexed_files;

            // Extract directory path and filename from base_filename
            std::filesystem::path full_path(base_filename);
            std::filesystem::path directory = full_path.parent_path();
            std::string file_basename = full_path.filename().string();

            // If directory is empty, use current directory
            if (directory.empty()) {
                directory = ".";
            }

            std::string dir_str = directory.string();
            std::string filename_str = file_basename; // Create a std::string copy
            // std::cout << "Searching for sub-BMP files with base name: " << file_basename << " in directory: " << dir_str << std::endl;
            printMessage("Searching for sub-BMP files with base name: " + file_basename + " in directory: " + dir_str);

            // Search specified directory instead of current directory
            for (const auto &entry: std::filesystem::directory_iterator(directory)) {
                if (!entry.is_regular_file()) continue;

                std::string filename = entry.path().filename().string();
                std::string file_full_path = entry.path().string(); // Renamed to avoid shadowing

                // Skip non-BMP files
                if (entry.path().extension() != ".bmp") continue;

                // Convert filename to lowercase for case-insensitive comparison
                std::string lower_filename = filename;
                std::transform(lower_filename.begin(), lower_filename.end(), lower_filename.begin(),
                               [](unsigned char c) { return std::tolower(c); });

                // Convert base filename to lowercase for comparison
                std::string lower_basename = file_basename;
                std::transform(lower_basename.begin(), lower_basename.end(), lower_basename.begin(),
                               [](unsigned char c) { return std::tolower(c); });

                // Check if the filename matches the pattern: basefilename_XofY.bmp
                std::string pattern_prefix = lower_basename + "_";

                // Direct string search instead of regex
                if (lower_filename.find(pattern_prefix) == 0) {
                    size_t of_pos = lower_filename.find("of");
                    size_t ext_pos = lower_filename.rfind(".bmp");

                    // Make sure "of" exists and extension is ".bmp"
                    if (of_pos != std::string::npos && ext_pos != std::string::npos &&
                        (ext_pos == lower_filename.length() - 4)) {
                        try {
                            // Extract the index number (between pattern_prefix and "of")
                            std::string index_str = lower_filename.substr(
                                pattern_prefix.length(),
                                of_pos - pattern_prefix.length()
                            );

                            int index = std::stoi(index_str);
                            // std::cout << "  Found file: " << filename << " (index: " << std::to_string(index) << ")" << std::endl;
                            printMessage("  Found file: " + filename + " (index: " + std::to_string(index) + ")");
                            indexed_files.emplace_back(index, file_full_path);
                        } catch (const std::exception &e) {
                            // std::cout << "  Skipping " << filename << ": " << e.what() << std::endl;
                            printMessage("  Skipping " + filename + ": " + e.what());
                        }
                    }
                }
            }

            // If we didn't find any files with the exact base name, try looking for any files with _XofY pattern
            if (indexed_files.empty()) {
                // std::cout << "No direct matches found, searching for any chunked files in the directory" << std::endl;
                printMessage("No direct matches found, searching for any chunked files in the directory");

                for (const auto &entry: std::filesystem::directory_iterator(directory)) {
                    if (!entry.is_regular_file() || entry.path().extension() != ".bmp") continue;

                    std::string filename = entry.path().filename().string();
                    std::string file_full_path = entry.path().string();

                    // Look for the "of" pattern in the filename
                    size_t of_pos = filename.find("of");
                    if (of_pos != std::string::npos && of_pos > 1) {
                        size_t underscore_pos = filename.rfind('_', of_pos - 1);

                        if (underscore_pos != std::string::npos) {
                            try {
                                // Extract just the chunk number and total chunks
                                std::string index_str = filename.substr(underscore_pos + 1, of_pos - underscore_pos - 1);
                                std::string total_str = filename.substr(of_pos + 2);
                                size_t dot_pos = total_str.find('.');
                                if (dot_pos != std::string::npos) {
                                    total_str = total_str.substr(0, dot_pos);

                                    int index = std::stoi(index_str);
                                    int total = std::stoi(total_str);

                                    if (index > 0 && total > 0) {
                                        // std::cout << "  Found potential chunk file: " << filename <<
                                        //   " (index: " << std::to_string(index) << "/" << std::to_string(total) << ")" << std::endl;
                                        printMessage("  Found potential chunk file: " + filename +
                                                   " (index: " + std::to_string(index) + "/" + std::to_string(total) + ")");
                                        indexed_files.emplace_back(index, file_full_path);
                                    }
                                }
                            } catch (const std::exception &e) {
                                // std::cout << "  Skipping " << filename << ": " << e.what() << std::endl;
                                printMessage("  Skipping " + filename + ": " + e.what());
                            }
                        }
                    }
                }
            }

            // Sort by index
            std::sort(indexed_files.begin(), indexed_files.end());

            // Extract just the paths
            for (const auto &pair: indexed_files) {
                result.push_back(pair.second);
            }

            // std::cout << Color::BRIGHT_CYAN << "Found " << result.size() << " sub-BMP files." << Color::RESET << std::endl;
            printStatus("Found " + std::to_string(result.size()) + " sub-BMP files.");
        } catch (const std::exception &e) {
            // std::cout << Color::RED << "Error finding sub-bmp files: " << e.what() << Color::RESET << std::endl;
            printError("Error finding sub-bmp files: " + std::string(e.what()));
        }

        return result;
    }

    /**
     * Get all files to process based on input pattern or filename
     *
     * @param filename Path to the image file, or pattern for multiple files
     * @return Vector of files to process
     */
    std::vector<std::string> getFilesToProcess(const std::string &filename) {
        std::vector<std::string> files_to_process;
        bool is_pattern = (filename.find('*') != std::string::npos ||
                           filename.find('?') != std::string::npos);
        bool is_directory = std::filesystem::is_directory(filename);

        // Check if this is a chunk file (contains "of" in the filename followed by a number and .bmp)
        std::filesystem::path input_path(filename);
        std::string filename_only = input_path.filename().string();
        size_t of_pos = filename_only.find("of");

        if (!is_pattern && !is_directory && std::filesystem::exists(filename) && of_pos != std::string::npos && of_pos > 1) {
            // std::cout << "Detected chunked file pattern in: " << filename_only << std::endl;
            printHighlight("Detected chunked file pattern in: " + filename_only);
            size_t underscore_pos = filename_only.find_last_of('_', of_pos - 1);

            if (underscore_pos != std::string::npos) {
                std::string base_name = filename_only.substr(0, underscore_pos);
                std::filesystem::path dir_path = input_path.parent_path();

                // std::cout << "Looking for chunks with base name: " << base_name << " in " << dir_path.string() << std::endl;
                printStatus("Looking for sub-bmp files with base name: " + base_name);
                // std::cout << Color::BRIGHT_CYAN << "Looking for sub-bmp files with base name: " << base_filename << Color::RESET << std::endl;

                // Get all chunk files with this base name
                std::vector<std::string> chunk_files;
                for (const auto &entry : std::filesystem::directory_iterator(dir_path)) {
                    if (!entry.is_regular_file() || entry.path().extension() != ".bmp")
                        continue;

                    std::string entry_name = entry.path().filename().string();
                    if (entry_name.find(base_name + "_") == 0 && entry_name.find("of") != std::string::npos) {
                        chunk_files.push_back(entry.path().string());
                        // std::cout << "  Found chunk: " << entry_name << std::endl;
                        printMessage("  Found chunk: " + entry_name);
                    }
                }

                if (!chunk_files.empty()) {
                    // Sort numerically by the chunk number
                    std::sort(chunk_files.begin(), chunk_files.end(),
                        [](const std::string &a, const std::string &b) {
                            // Extract chunk numbers
                            std::filesystem::path a_path(a);
                            std::filesystem::path b_path(b);
                            std::string a_name = a_path.filename().string();
                            std::string b_name = b_path.filename().string();

                            size_t a_underscore = a_name.find_last_of('_');
                            size_t b_underscore = b_name.find_last_of('_');

                            if (a_underscore != std::string::npos && b_underscore != std::string::npos) {
                                size_t a_of = a_name.find("of", a_underscore);
                                size_t b_of = b_name.find("of", b_underscore);

                                if (a_of != std::string::npos && b_of != std::string::npos) {
                                    std::string a_index_str = a_name.substr(a_underscore + 1, a_of - a_underscore - 1);
                                    std::string b_index_str = b_name.substr(b_underscore + 1, b_of - b_underscore - 1);

                                    try {
                                        int a_index = std::stoi(a_index_str);
                                        int b_index = std::stoi(b_index_str);
                                        return a_index < b_index;
                                    } catch (...) {
                                        // Fall back to string comparison if conversion fails
                                    }
                                }
                            }

                            return a < b;
                        });

                    // std::cout << "Found and sorted " << std::to_string(chunk_files.size()) << " chunk files" << std::endl;
                    printStatus("Found and sorted " + std::to_string(chunk_files.size()) + " chunk files");
                    return chunk_files;
                }
            }

            // If no related chunks found or not a chunk file, just add the single file
            files_to_process.push_back(filename);
        } else if (is_pattern) {
            // Handle glob pattern
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
            for (char c: pattern) {
                if (c == '*') regex_pattern += ".*";
                else if (c == '?') regex_pattern += ".";
                else if (c == '.') regex_pattern += "\\.";
                else regex_pattern += c;
            }
            regex_pattern += "$";
            std::regex file_regex(regex_pattern);

            // Find all matching files
            try {
                for (const auto &entry: std::filesystem::directory_iterator(directory)) {
                    if (entry.is_regular_file()) {
                        std::string matched_filename = entry.path().filename().string();
                        if (std::regex_match(matched_filename, file_regex)) {
                            files_to_process.push_back(entry.path().string());
                        }
                    }
                }
            } catch (const std::exception &e) {
                // std::cout << Color::RED << "Error accessing directory: " << e.what() << Color::RESET << std::endl;
                printError("Error accessing directory: " + std::string(e.what()));
                return files_to_process;
            }
        } else if (is_directory) {
            // Process all BMP files in directory
            try {
                for (const auto &entry: std::filesystem::directory_iterator(filename)) {
                    if (entry.is_regular_file() &&
                        entry.path().extension() == ".bmp") {
                        files_to_process.push_back(entry.path().string());
                    }
                }
            } catch (const std::exception &e) {
                // std::cout << Color::RED << "Error accessing directory: " << e.what() << Color::RESET << std::endl;
                printError("Error accessing directory: " + std::string(e.what()));
                return files_to_process;
            }
        } else {
            // Single file
            files_to_process.push_back(filename);
        }

        // Handle the case of main file not found but sub-bmps might exist
        if (!is_pattern && !is_directory && !std::filesystem::exists(filename)) {
            std::string file_not_found = filename;  // Create a copy as std::string
            // std::cout << "Main file not found: " << filename << std::endl;
            std::string base_filename = filename;

            // Remove extension if present
            size_t dot_pos = base_filename.find_last_of('.');
            if (dot_pos != std::string::npos) {
                base_filename = base_filename.substr(0, dot_pos);
            }

            files_to_process = findSubBmpFiles(base_filename);

            if (files_to_process.empty()) {
                // std::cout << Color::RED << "No sub-bmp files found for: " << tmp_base << Color::RESET << std::endl;
                std::string tmp_base = base_filename;
                printError("No sub-bmp files found for: " + tmp_base);
                return files_to_process;
            }
        }
        // Check if this is a chunk file (contains "of" in the filename followed by a number and .bmp)
        else if (!is_pattern && !is_directory && std::filesystem::exists(filename)) {
            std::string file_path = filename;
            std::filesystem::path full_path(file_path);
            std::string filename_str = full_path.filename().string();
            std::filesystem::path directory = full_path.parent_path();

            // Check if filename contains _Xof pattern
            size_t of_pos = filename_str.find("of");
            if (of_pos != std::string::npos && of_pos > 1) {
                // Extract the base name (everything before _Xof)
                size_t underscore_pos = filename_str.rfind('_', of_pos - 1);

                if (underscore_pos != std::string::npos) {
                    std::string base_name = filename_str.substr(0, underscore_pos);

                    // Build the base filename path (directory + base name)
                    std::filesystem::path base_path = directory / base_name;
                    std::string base_filename = base_path.string();

                    // std::cout << "Detected chunk file, searching for all related chunks with base name: " << base_filename << std::endl;
                    printMessage("Detected chunk file, searching for all related chunks with base name: " + base_filename);
                    // std::cout << Color::BRIGHT_MAGENTA + Color::BOLD << "Detected chunked file pattern in: " 
                    //           << filename_only << Color::RESET << std::endl;
                    // std::cout << Color::BRIGHT_CYAN << "Looking for sub-bmp files with base name: " 
                    //           << base_filename << Color::RESET << std::endl;

                    // Find all related chunks
                    files_to_process = findSubBmpFiles(base_filename);

                    if (files_to_process.size() > 1) {
                        // std::cout << "Found " << std::to_string(files_to_process.size()) << " related chunk files to process:" << std::endl;
                        std::string files_count = std::to_string(files_to_process.size());
                        printMessage("Found " + files_count + " related chunk files to process:");
                        for (const auto &file_path: files_to_process) {
                            // std::cout << "  - " << file_path << std::endl;
                            printMessage("  - " + file_path);
                        }
                        return files_to_process;
                    }
                }
            }

            // If no related chunks found or not a chunk file, just add the single file
            files_to_process.push_back(filename);
        }

        std::sort(files_to_process.begin(), files_to_process.end());
        return files_to_process;
    }

    /**
     * Write the final assembled file from all chunks
     *
     * @param output_filename Output filename
     * @param chunks Map of chunk data indexed by chunk number
     * @param print_mutex Mutex for synchronizing console output
     * @return True if write was successful
     */
    bool writeAssembledFile(
        const std::string &output_filename,
        const std::map<int, image_parser::ChunkInfo> &chunks,
        std::mutex &print_mutex)
    {
        if (chunks.empty()) {
            printError("No data chunks to write.");
            return false;
        }

        try {
            std::ofstream outfile(output_filename, std::ios::binary | std::ios::trunc);
            if (!outfile) {
                printError("Could not create output file " + std::string(output_filename));
                return false;
            }

            // Get total expected size
            size_t total_expected_size = 0;
            size_t processed_chunks = 0;
            
            for (const auto &chunk_pair : chunks) {
                const auto &chunk = chunk_pair.second;
                total_expected_size += chunk.payload.size();
                processed_chunks++;
            }

            // Check if we have all chunks
            int highest_chunk_index = chunks.rbegin()->first;

            if (processed_chunks != static_cast<size_t>(highest_chunk_index)) {
                printWarning("Missing chunks: processed " + std::to_string(processed_chunks) +
                          " of " + std::to_string(highest_chunk_index) + " expected.");
            }

            printMessage("Writing " + std::to_string(total_expected_size) + 
                       " bytes from " + std::to_string(processed_chunks) + " chunks to " + 
                       output_filename);

            // Write each chunk in sequential order
            size_t total_bytes_written = 0;
            bool missing_chunks = false;

            for (int i = 1; i <= highest_chunk_index; ++i) {
                if (chunks.find(i) == chunks.end()) {
                    printWarning("Missing chunk " + std::to_string(i) + " - output file may be corrupt");
                    missing_chunks = true;
                    continue;
                }

                const auto &chunk = chunks.at(i);
                outfile.write(reinterpret_cast<const char *>(chunk.payload.data()),
                             static_cast<std::streamsize>(chunk.payload.size()));
                
                if (outfile.fail()) {
                    printError("Failed writing chunk " + std::to_string(i));
                    return false;
                }
                
                total_bytes_written += chunk.payload.size();

                if (i % 10 == 0 || i == highest_chunk_index) {
                    // Update progress every 10 chunks or on the last one
                    printStatus("Wrote chunk " + std::to_string(i) + "/" + 
                              std::to_string(highest_chunk_index) + " (" + 
                              std::to_string(total_bytes_written) + " bytes)");
                }
            }

            outfile.close();

            // Verify the file was created successfully
            if (!std::filesystem::exists(output_filename)) {
                printError("Failed to create output file " + output_filename);
                return false;
            }

            // Get actual file size
            size_t actual_file_size = std::filesystem::file_size(output_filename);

            if (actual_file_size != total_bytes_written) {
                printWarning("File size mismatch: expected " + std::to_string(total_bytes_written) +
                          " bytes, got " + std::to_string(actual_file_size) + " bytes");
            }

            printCompletion("File written successfully", total_bytes_written);

            return !missing_chunks;
        }
        catch (const std::exception &e) {
            printError("Exception writing file: " + std::string(e.what()));
            return false;
        }
    }

    /**
     * Parse from image implementation
     *
     * @param filename Path to the image file(s) to process
     */
    void parseFromImage(const std::string &filename) {
        std::vector<std::string> files_to_process = getFilesToProcess(filename);

        if (files_to_process.empty()) {
            // std::cout << Color::RED << "No files found to process." << Color::RESET << std::endl;
            printError("No files found to process.");
            return;
        }

        // std::cout << Color::CYAN << "Processing " << std::to_string(files_to_process.size()) << " files" << Color::RESET << std::endl;
        printStatus("Processing " + std::to_string(files_to_process.size()) + " files");

        // Setup for single-threaded processing
        std::mutex print_mutex;

        // Map to store chunks by their index for proper ordering
        std::map<int, image_parser::ChunkInfo> chunks;
        std::string output_filename;
        bool output_filename_set = false;

        // Extract payloads from all files
        for (const auto &file: files_to_process) {
            auto chunk_opt = extractChunkPayload(file, print_mutex, false);

            if (!chunk_opt) {
                std::lock_guard<std::mutex> print_lock(print_mutex);
                // std::cout << Color::RED << "Failed to extract payload from " << file << Color::RESET << std::endl;
                printError("Failed to extract payload from " + file);
                continue;
            }

            auto chunk = *chunk_opt;

            // Set output filename from first successful chunk if not set already
            // Ensure we preserve the exact filename from metadata including extension
            if (!output_filename_set && !chunk.filename.empty()) {
                output_filename = chunk.filename;
                output_filename_set = true;
                std::lock_guard<std::mutex> print_lock(print_mutex);
                // std::cout << Color::CYAN << "Using output filename from metadata: " << output_filename << Color::RESET << std::endl;
                printStatus("Using output filename from metadata: " + output_filename);
            } else if (!chunk.filename.empty() && output_filename != chunk.filename) {
                // Warn about inconsistent output filenames
                std::lock_guard<std::mutex> print_lock(print_mutex);
                // std::cout << Color::YELLOW << "Inconsistent output filename in " << file <<
                //   " ('" << chunk.filename << "' vs '" << output_filename << "')" << Color::RESET << std::endl;
                printWarning("Inconsistent output filename in " + file +
                          " ('" + chunk.filename + "' vs '" + output_filename + "')");
            }

            // Store chunk by its index
            if (chunk.chunkIndex > 0) {
                chunks[chunk.chunkIndex] = std::move(chunk);
            } else {
                // If index info is missing, use file order as fallback
                chunks[chunks.size() + 1] = std::move(chunk);
            }
        }

        // If we have any chunks, write the assembled file
        if (!chunks.empty() && output_filename_set) {
            std::mutex output_mutex; // Not strictly needed in single-threaded context
            bool success = writeAssembledFile(output_filename, chunks, print_mutex);
            if (success) {
                // std::cout << Color::GREEN << "File successfully extracted from image!" << Color::RESET << std::endl;
                printSuccess("File successfully extracted from image!");
            } else {
                // std::cout << Color::RED << "Failed to assemble output file: " << output_filename << Color::RESET << std::endl;
                printError("Failed to assemble output file: " + output_filename);
            }
        } else {
            // std::cout << Color::RED << "No valid chunks extracted, cannot create output file." << Color::RESET << std::endl;
            printError("No valid chunks extracted, cannot create output file.");
        }
    }

    /**
     * Create a full output path by combining a directory path with a filename
     *
     * @param outputPath Directory or full path where the file should be saved
     * @param filename Original filename from metadata
     * @return Complete path to use for saving the file
     */
    std::string createOutputPath(const std::string& outputPath, const std::string& filename) {
        if (outputPath.empty() || filename.empty()) {
            // Use filename as is if no output path specified or empty filename
            return filename.empty() ? outputPath : filename;
        }

        std::filesystem::path path(outputPath);
        std::filesystem::path filenamePath(filename);

        // Check if the filename has a name component
        if (!filenamePath.has_filename()) {
            // If filename doesn't have a name component, just return the output path
            return outputPath;
        }

        // Extract just the filename part without directory components
        std::string justFilename = filenamePath.filename().string();

        // Handle root directories like "D:\" 
        if (outputPath.back() == '\\' || outputPath.back() == '/') {
            // Already has trailing slash
            return outputPath + justFilename;
        } else {
            // Add a path separator
            return outputPath + "\\" + justFilename;
        }
    }
} // namespace image_parser

// Export the public interface functions
std::ifstream::pos_type filesize(const char *filename) {
    return image_parser::fileSize(filename);
}

void parseFromImage(const std::string &filename, const std::string &outputPath, int maxThreads, int maxMemoryMB) {
    try {
        // std::cout << Color::CYAN << "Extracting file from image..." << Color::RESET << std::endl;
        printStatus("Extracting file from image...");
        
        // Force debug mode reinitialization and verification from the start
        bool current_debug_mode = gDebugMode.load(std::memory_order_seq_cst);
        
        // Print current debug status
        {
            std::lock_guard<std::mutex> lock(gConsoleMutex);
            
            // Force set it if we detect it's off but should be on
            if (!current_debug_mode && getenv("DEBUG") != nullptr) {
                gDebugMode.store(true, std::memory_order_seq_cst);
                current_debug_mode = true;
            }
            
            // If debug mode is on, print a confirmation message
            if (current_debug_mode) {
                // std::cout << Color::CYAN << "-------- DEBUG MODE ACTIVE: Detailed progress will be shown --------" << Color::RESET << std::endl;
                printMessage("-------- DEBUG MODE ACTIVE: Detailed progress will be shown --------");
            }
        }
        
        // Explicitly check the debug mode status here and print it for verification
        bool isDebugMode = getDebugMode();
        
        // Use the global console mutex instead of a separate print_mutex
        std::map<int, image_parser::ChunkInfo> image_chunks;
        std::vector<std::future<bool>> futures;
        std::vector<std::string> failed_files;
        std::mutex failed_mutex;
        int total_chunks = 0;
        std::string target_output_file;
        std::mutex chunks_mutex;
                
        // Important: FORCE debug mode to be correctly visible here
        // Use the strongest memory ordering to ensure visibility in all threads
        bool main_thread_debug = gDebugMode.load(std::memory_order_seq_cst);
        
        // If we're in debug mode but the flag shows OFF, force it ON for all threads
        if (main_thread_debug != getDebugMode()) {
            gDebugMode.store(main_thread_debug, std::memory_order_seq_cst);
        }
        
        // Process all the files
        std::vector<std::string> files_to_process = image_parser::getFilesToProcess(filename);
        if (files_to_process.empty()) {
            // std::cout << Color::RED << "No image files found to process" << Color::RESET << std::endl;
            printError("No image files found to process");
            return;
        }

        // std::cout << "Found " << std::to_string(files_to_process.size()) << " files to process" << std::endl;

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

        // Set memory limit (default to 1GB if maxMemoryMB <= 0)
        if (maxMemoryMB > 0) {
            resManager.setMaxMemory(static_cast<size_t>(maxMemoryMB) * 1024 * 1024);
        } else {
            // Default to 1GB if not specified
            resManager.setMaxMemory(1024 * 1024 * 1024);
        }

        // Process each file in a separate thread
        for (const auto &file : files_to_process) {
            // std::cout << "Processing file: " << file << std::endl;
            printFilePath("Processing file: " + file);

            // Use ResourceManager to run threads with proper limits
            bool thread_scheduled = resManager.runWithThread(
                [&, file, main_thread_debug](const std::string &filepath) {
                    // Process the image file - use gConsoleMutex instead of print_mutex
                    auto chunk_data = image_parser::extractChunkPayload(filepath, gConsoleMutex, main_thread_debug);
                    if (!chunk_data) {
                        std::lock_guard<std::mutex> failed_lock(failed_mutex);
                        failed_files.push_back(filepath);
                        // Using printError directly handles console mutex internally
                        printError("Failed to extract payload from " + filepath);
                        return false;
                    }

                    // Update the chunks map
                    {
                        std::lock_guard<std::mutex> chunks_lock(chunks_mutex);
                        image_chunks[chunk_data->chunkIndex] = *chunk_data;

                        // Update metadata
                        if (target_output_file.empty() && !chunk_data->filename.empty()) {
                            target_output_file = chunk_data->filename;
                        }
                        total_chunks = std::max(total_chunks, chunk_data->totalChunks);
                    }
                    return true;
                },
                file);

            if (!thread_scheduled) {
                // Using printWarning directly handles console mutex internally
                printWarning("Could not schedule thread for file: " + file);
            }

            // Give a small delay to allow thread initialization
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Wait for all threads to complete
        resManager.waitForAllThreads();

        // Report on failed files
        if (!failed_files.empty()) {
            // std::cout << Color::YELLOW << "Failed to process " << std::to_string(failed_files.size()) << " files" << Color::RESET << std::endl;
            printWarning("Failed to process " + std::to_string(failed_files.size()) + " files");
            for (const auto &file : failed_files) {
                // std::cout << "  Failed: " << file << std::endl;
                printStatus("  Failed: " + file);
            }
        }

        // Check if we have any data
        if (image_chunks.empty()) {
            // std::cout << Color::RED << "No valid data found in processed images" << Color::RESET << std::endl;
            printError("No valid data found in processed images");
            return;
        }

        // std::cout << "Finished processing " << std::to_string(image_chunks.size()) << " chunks" << std::endl;
        printMessage("Finished processing " + std::to_string(image_chunks.size()) + " chunks");

        // Check if we have all chunks
        if (static_cast<int>(image_chunks.size()) != total_chunks) {
            // std::cout << Color::YELLOW << "Missing chunks: found " << std::to_string(image_chunks.size()) <<
            //   " of " << std::to_string(total_chunks) << Color::RESET << std::endl;
            printWarning("Missing chunks: found " + std::to_string(image_chunks.size()) +
                        " of " + std::to_string(total_chunks));
            for (int i = 1; i <= total_chunks; ++i) {
                if (image_chunks.find(i) == image_chunks.end()) {
                    // std::cout << "  Missing chunk " << std::to_string(i) << std::endl;
                    printStatus("  Missing chunk " + std::to_string(i));
                }
            }
        }

        // Create complete output path
        std::string output_file = image_parser::createOutputPath(outputPath, target_output_file);
        // std::cout << "Writing output file: " << output_file << std::endl;
        printMessage("Writing output file: " + output_file);

        // Write the output file - use gConsoleMutex instead of print_mutex
        bool write_success = image_parser::writeAssembledFile(output_file, image_chunks, gConsoleMutex);
        if (write_success) {
            // std::cout << Color::GREEN << "File successfully extracted from image!" << Color::RESET << std::endl;
            printSuccess("File successfully extracted from image!");
        } else {
            // std::cout << Color::RED << "Failed to write the extracted file" << Color::RESET << std::endl;
            printError("Failed to write the extracted file");
        }
    } catch (const std::exception &e) {
        // std::cout << Color::RED << "Error during image to file conversion: " << std::string(e.what()) << Color::RESET << std::endl;
        printError("Error during image to file conversion: " + std::string(e.what()));
    }
}