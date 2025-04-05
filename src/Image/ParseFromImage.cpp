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
#include "headers/Bitmap.hpp"
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
    printStatus("Chunk " + std::to_string(metadata.currentChunk) + " payload starts at offset " + 
                  std::to_string(data_iter - data.begin()));
    data_iter += 4;

    // Read total_chunks (4 bytes)
    if (std::distance(data_iter, data.end()) < 4) {
        std::lock_guard<std::mutex> lock(print_mutex);
        printError("Not enough data for total chunks in " + filename);
        return {metadata, false};
    }
    metadata.totalChunks = std::stoi(std::string(data_iter, data_iter + 4));
    data_iter += 4;

    // Calculate total bytes read so far
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
        std::vector<unsigned char> data;
        data.reserve(data_size_estimate + 100); // Add extra buffer space to prevent any truncation

        // Read all pixels from the image in row-major order
        for (unsigned int i = 0; i < height; ++i) {
            for (unsigned int j = 0; j < width; ++j) {
                rgb_t rgb = image.get_pixel(j, i);
                data.push_back(rgb.red);
                data.push_back(rgb.green);
                data.push_back(rgb.blue);
            }
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
    std::optional<ChunkInfo> extractChunkPayload(
        const std::string &filename,
        std::mutex &print_mutex) {
        try {
            // Load the image file
            bitmap_image image(filename);

            if (!image) {
                std::lock_guard<std::mutex> print_lock(print_mutex);
                printError("Failed to load image " + filename);
                return std::nullopt;
            }

            // Get image dimensions
            unsigned int width = image.width();
            unsigned int height = image.height();

            // Extract all pixel data from the image
            std::vector<unsigned char> data = extractPixelData(image);
            {
                std::lock_guard<std::mutex> print_lock(print_mutex);
                printMessage("Processing image: " + filename + 
                        " (" + std::to_string(width) + "x" + std::to_string(height) + ") - Estimated capacity: " +
                        std::to_string(data.size()) + " bytes");
            }

            // Start reading from the beginning of the data
            auto data_iter = data.begin();

            // Extract metadata from image header
            auto [metadata, success] = extractMetadata(data, data_iter, print_mutex, filename);
            if (!success) {
                return std::nullopt;
            }

            // Print metadata info
            {
                std::lock_guard<std::mutex> print_lock(print_mutex);
                printMessage("Extracted metadata:\n  Output file: " + metadata.outputFilename + 
                            "\n  Data size: " + std::to_string(metadata.expectedDataSize) + " bytes" +
                            "\n  Index info: " + metadata.indexInfo +
                            ((metadata.currentChunk != -1 && metadata.totalChunks != -1) ? 
                             " (Chunk " + std::to_string(metadata.currentChunk) + " of " + 
                             std::to_string(metadata.totalChunks) + ")" : ""));
            }

            // Calculate the distance from data_iter to data.end()
            size_t available_data = std::distance(data_iter, data.end());

            // IMPORTANT: Use exactly metadata.expectedDataSize instead of limiting by available data
            // This ensures we preserve the exact size recorded in the header
            size_t payload_size = metadata.expectedDataSize;
            
            // Warn if there's not enough data available
            if (available_data < payload_size) {
                std::lock_guard<std::mutex> print_lock(print_mutex);
                printWarning("Available data (" + std::to_string(available_data) + 
                           " bytes) is less than expected size (" + 
                           std::to_string(payload_size) + " bytes) in chunk " + 
                           std::to_string(metadata.currentChunk));
                
                // We continue extracting what we can, but we record the expected size separately
                payload_size = available_data;
            }

            // Create payload result
            ChunkInfo chunk;
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
            {
                std::lock_guard<std::mutex> lock(print_mutex);
                printMessage("Extracted " + std::to_string(chunk.payload.size()) + " bytes of payload data from " + filename);
                
                // Debug: Show the last few bytes of the chunk (only in debug mode)
                if (chunk.payload.size() > 10) {
                    std::string bytes_debug = "Last 10 bytes: ";
                    for (size_t i = 1; i <= 10; ++i) {
                        if (chunk.payload.size() >= i) {
                            std::stringstream ss;
                            ss << std::hex << std::setw(2) << std::setfill('0')
                               << static_cast<int>(chunk.payload[chunk.payload.size() - i]) << " ";
                            bytes_debug += ss.str();
                        }
                    }
                    printMessage(bytes_debug);
                }
            }

            return chunk;
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> print_lock(print_mutex);
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
            std::cout << "Searching for sub-BMP files with base name: " << file_basename << " in directory: " << dir_str << std::endl;

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
                            printMessage("  Found file: " + filename + " (index: " + std::to_string(index) + ")");
                            indexed_files.emplace_back(index, file_full_path);
                        } catch (const std::exception &e) {
                            printMessage("  Skipping " + filename + ": " + e.what());
                        }
                    }
                }
            }

            // If we didn't find any files with the exact base name, try looking for any files with _XofY pattern
            if (indexed_files.empty()) {
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
                                        printMessage("  Found potential chunk file: " + filename + 
                                                   " (index: " + std::to_string(index) + "/" + std::to_string(total) + ")");
                                        indexed_files.emplace_back(index, file_full_path);
                                    }
                                }
                            } catch (const std::exception &e) {
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

            // Temporarily comment out problematic messages
            std::string result_count = std::to_string(result.size());
            // statusPrint("Found " + result_count + " sub-BMP files.");
            std::cout << "Found " << result.size() << " sub-BMP files." << std::endl;
        } catch (const std::exception &e) {
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
            printMessage("Detected chunked file pattern in: " + filename_only);
            size_t underscore_pos = filename_only.find_last_of('_', of_pos - 1);
            
            if (underscore_pos != std::string::npos) {
                std::string base_name = filename_only.substr(0, underscore_pos);
                std::filesystem::path dir_path = input_path.parent_path();
                
                printMessage("Looking for chunks with base name: " + base_name + " in " + dir_path.string());
                
                // Get all chunk files with this base name
                std::vector<std::string> chunk_files;
                for (const auto &entry : std::filesystem::directory_iterator(dir_path)) {
                    if (!entry.is_regular_file() || entry.path().extension() != ".bmp")
                        continue;
                        
                    std::string entry_name = entry.path().filename().string();
                    if (entry_name.find(base_name + "_") == 0 && entry_name.find("of") != std::string::npos) {
                        chunk_files.push_back(entry.path().string());
                        printMessage("  Found chunk: " + entry.path().string());
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
                    
                    printMessage("Found and sorted " + std::to_string(chunk_files.size()) + " chunk files");
                    return chunk_files;
                }
            }
        }
        
        // Handle the case of main file not found but sub-bmps might exist
        if (!is_pattern && !is_directory && !std::filesystem::exists(filename)) {
            std::string file_not_found = filename;  // Create a copy as std::string
            std::cout << "Main file not found: " << filename << std::endl;
            std::string base_filename = filename;

            // Remove extension if present
            size_t dot_pos = base_filename.find_last_of('.');
            if (dot_pos != std::string::npos) {
                base_filename = base_filename.substr(0, dot_pos);
            }

            printStatus("Looking for sub-bmp files with base name: " + base_filename);

            // Temporarily comment out problematic messages
            // statusPrint("Looking for sub-bmp files with base name: " + base_filename);
            std::cout << "Looking for sub-bmp files with base name: " << base_filename << std::endl;

            files_to_process = findSubBmpFiles(base_filename);

            if (files_to_process.empty()) {
                std::string tmp_base = base_filename;
                printError("No sub-bmp files found for: " + tmp_base);
                return files_to_process;
            }

            std::string files_count = std::to_string(files_to_process.size());
            printStatus("Found " + files_count + " sub-bmp files to process:");
            for (const auto &file_path: files_to_process) {
                printMessage("  - " + file_path);
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
                    
                    printStatus("Detected chunk file, searching for all related chunks with base name: " + base_filename);
                    std::cout << "Searching for related chunks with base name: " << base_name << " in directory: " << directory.string() << std::endl;
                    
                    // Find all related chunks
                    files_to_process = findSubBmpFiles(base_filename);
                    
                    if (files_to_process.size() > 1) {
                        std::string files_count = std::to_string(files_to_process.size());
                        printStatus("Found " + files_count + " related chunk files to process:");
                        for (const auto &file_path: files_to_process) {
                            printMessage("  - " + file_path);
                        }
                        return files_to_process;
                    }
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
                printError("Error accessing directory: " + std::string(e.what()));
                return files_to_process;
            }

            // Sort the files
            std::sort(files_to_process.begin(), files_to_process.end());
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
                printError("Error accessing directory: " + std::string(e.what()));
                return files_to_process;
            }

            // Sort the files
            std::sort(files_to_process.begin(), files_to_process.end());
        } else {
            // Single file
            files_to_process.push_back(filename);
        }

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
        const std::map<int, ChunkInfo> &chunks,
        std::mutex &print_mutex) {
        try {
            std::ofstream outfile(output_filename, std::ios::binary | std::ios::trunc);
            if (!outfile) {
                std::lock_guard<std::mutex> print_lock(print_mutex);
                printError("Could not create output file " + std::string(output_filename));
                return false;
            }

            // Calculate total expected size and total available size
            size_t total_expected_size = 0;
            size_t total_available_size = 0;
            size_t expected_chunks = 0;

            if (!chunks.empty()) {
                expected_chunks = chunks.begin()->second.totalChunks;
                for (const auto &[idx, chunk]: chunks) {
                    total_expected_size += chunk.expectedDataSize;
                    total_available_size += chunk.payload.size();
                }
            } {
                std::lock_guard<std::mutex> print_lock(print_mutex);
                std::string msg = "Writing assembled file " + std::string(output_filename) +
                                " from " + std::to_string(chunks.size()) + "/" + std::to_string(expected_chunks) +
                                " chunks, expected size: " + std::to_string(total_expected_size) +
                                " bytes, available size: " + std::to_string(total_available_size) + " bytes";
                printStatus(msg);

                // Check if we have all expected chunks
                if (chunks.size() != expected_chunks) {
                    printWarning("Missing " + std::to_string(expected_chunks - chunks.size()) + 
                               " chunks. Output file may be incomplete.");
                }
            }

            // Write chunks in order
            size_t bytes_written = 0;
            for (const auto &[idx, chunk]: chunks) {
                if (!chunk.payload.empty()) {
                    // For all chunks, write exactly what we have - no truncation
                    outfile.write(reinterpret_cast<const char *>(chunk.payload.data()),
                                  chunk.payload.size());
                    bytes_written += chunk.payload.size();
                    
                    // If this chunk has less data than expected, pad with zeros
                    if (chunk.payload.size() < chunk.expectedDataSize) {
                        size_t padding_needed = chunk.expectedDataSize - chunk.payload.size();
                        std::vector<char> padding(padding_needed, 0);
                        outfile.write(padding.data(), padding_needed);
                        bytes_written += padding_needed;
                        
                        std::lock_guard<std::mutex> print_lock(print_mutex);
                        printMessage("Added " + std::to_string(padding_needed) + " bytes of padding for chunk " + std::to_string(idx));
                    }

                    if (!outfile) {
                        std::lock_guard<std::mutex> print_lock(print_mutex);
                        printError("Error writing chunk " + std::to_string(idx) + " to output file");
                        return false;
                    }
                }
            }

            outfile.close(); {
                std::lock_guard<std::mutex> print_lock(print_mutex);
                printStatus("Successfully wrote " + std::to_string(bytes_written) + " bytes to " + std::string(output_filename));
            }

            return true;
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> print_lock(print_mutex);
            printError("Exception writing output file: " + std::string(e.what()));
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
            printError("No files found to process.");
            return;
        }

        printStatus("Processing " + std::to_string(files_to_process.size()) + " files");

        // Setup for single-threaded processing
        std::mutex print_mutex;

        // Map to store chunks by their index for proper ordering
        std::map<int, ChunkInfo> chunks;
        std::string output_filename;
        bool output_filename_set = false;

        // Extract payloads from all files
        for (const auto &file: files_to_process) {
            auto chunk_opt = extractChunkPayload(file, print_mutex);

            if (!chunk_opt) {
                std::lock_guard<std::mutex> print_lock(print_mutex);
                printError("Failed to extract payload from " + file);
                continue;
            }

            auto chunk = *chunk_opt;

            // Set output filename from first successful chunk if not set already
            // Ensure we preserve the exact filename from metadata including extension
            if (!output_filename_set) {
                output_filename = chunk.filename;
                output_filename_set = true;

                std::lock_guard<std::mutex> print_lock(print_mutex);
                printStatus("Using output filename from metadata: " + output_filename);
            } else if (output_filename != chunk.filename) {
                // Warn about inconsistent output filenames
                std::lock_guard<std::mutex> print_lock(print_mutex);
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
                printStatus("Successfully assembled output file: " + output_filename);
            } else {
                printError("Failed to assemble output file: " + output_filename);
            }
        } else {
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
        if (outputPath.empty()) {
            return filename; // Use filename as is if no output path specified
        }
        
        std::filesystem::path path(outputPath);
        
        // Check if outputPath is a directory or a file path
        if (std::filesystem::is_directory(path) || outputPath.back() == '/' || outputPath.back() == '\\') {
            // It's a directory, append the original filename
            if (path.has_filename() && !std::filesystem::is_directory(path)) {
                path = path.parent_path();
            }
            return (path / filename).string();
        } else {
            // It's a file path, use it directly
            return outputPath;
        }
    }
} // namespace image_parser

// Export the public interface functions
std::ifstream::pos_type filesize(const char *filename) {
    return image_parser::fileSize(filename);
}

void parseFromImage(const std::string &filename, const std::string &outputPath) {
    // Add debug output to show what filename is being passed in
    printMessage("ParseFromImage called with filename: '" + filename + "'");
    if (!outputPath.empty()) {
        printMessage("Output path specified: '" + outputPath + "'");
    }
    std::cout << "Debug: Processing file: '" << filename << "'" << std::endl;
    
    // Check if this is a multi-chunk file (filename_XofY.bmp pattern)
    std::filesystem::path input_path(filename);
    if (!std::filesystem::exists(input_path)) {
        printError("Input file does not exist: " + filename);
        image_parser::parseFromImage(filename);  // Let the original function handle the error
        return;
    }
    
    std::string filename_only = input_path.filename().string();
    std::filesystem::path dir_path = input_path.parent_path();
    
    // If the directory path is empty, use the current directory
    if (dir_path.empty()) {
        dir_path = ".";
    }
    
    // Parse the filename to check if it's a multi-part file (pattern: name_XofY.bmp)
    size_t of_pos = filename_only.find("of");
    if (of_pos != std::string::npos && of_pos > 1) {
        size_t underscore_pos = filename_only.rfind('_', of_pos - 1);
        if (underscore_pos != std::string::npos) {
            // Extract the base name and number of parts
            std::string base_name = filename_only.substr(0, underscore_pos);
            std::string chunk_part = filename_only.substr(underscore_pos + 1);
            
            size_t dot_pos = chunk_part.rfind('.');
            if (dot_pos != std::string::npos) {
                // Extract X and Y from _XofY.bmp
                std::string of_part = chunk_part.substr(0, dot_pos);
                size_t of_text_pos = of_part.find("of");
                
                if (of_text_pos != std::string::npos && of_text_pos > 0) {
                    int chunk_index = std::stoi(of_part.substr(0, of_text_pos));
                    int total_chunks = std::stoi(of_part.substr(of_text_pos + 2));
                    
                    std::cout << "Detected multi-part file:" << std::endl;
                    std::cout << "  Base name: " << base_name << std::endl;
                    std::cout << "  Chunk index: " << chunk_index << " of " << total_chunks << std::endl;
                    std::cout << "  Directory: " << dir_path.string() << std::endl;
                    
                    // Find all related chunk files
                    std::vector<std::string> chunk_files;
                    
                    try {
                        for (const auto& entry : std::filesystem::directory_iterator(dir_path)) {
                            if (!entry.is_regular_file())
                                continue;
                                
                            std::string entry_name = entry.path().filename().string();
                            
                            // Simple string match:
                            // - Starts with the same base name 
                            // - Contains "ofX" where X is the total chunks
                            // - Ends with .bmp
                            if (entry_name.find(base_name + "_") == 0 && 
                                entry_name.find("of" + std::to_string(total_chunks)) != std::string::npos &&
                                entry_name.find(".bmp") != std::string::npos) {
                                
                                chunk_files.push_back(entry.path().string());
                                std::cout << "  Found chunk file: " << entry.path().string() << std::endl;
                            }
                        }
                    } catch (const std::exception& e) {
                        std::cout << "Error scanning directory: " << e.what() << std::endl;
                    }
                    
                    if (chunk_files.size() > 0) {
                        // Sort files by their chunk index
                        std::sort(chunk_files.begin(), chunk_files.end(), 
                            [&base_name](const std::string& a, const std::string& b) {
                                std::string a_filename = std::filesystem::path(a).filename().string();
                                std::string b_filename = std::filesystem::path(b).filename().string();
                                
                                // Extract the number between _ and of
                                size_t a_underscore = a_filename.find('_');
                                size_t b_underscore = b_filename.find('_');
                                
                                if (a_underscore != std::string::npos && b_underscore != std::string::npos) {
                                    size_t a_of = a_filename.find("of", a_underscore);
                                    size_t b_of = b_filename.find("of", b_underscore);
                                    
                                    if (a_of != std::string::npos && b_of != std::string::npos) {
                                        int a_index = std::stoi(a_filename.substr(a_underscore + 1, a_of - a_underscore - 1));
                                        int b_index = std::stoi(b_filename.substr(b_underscore + 1, b_of - b_underscore - 1));
                                        return a_index < b_index;
                                    }
                                }
                                
                                return a < b;
                            });
                        
                        std::cout << "Processing " << chunk_files.size() << " chunk files" << std::endl;
                        
                        // Setup for processing
                        std::mutex print_mutex;
                        std::map<int, image_parser::ChunkInfo> chunks;
                        std::string output_filename;
                        bool output_filename_set = false;
                        
                        // Process each chunk file
                        for (const auto& chunk_file : chunk_files) {
                            auto chunk_opt = image_parser::extractChunkPayload(chunk_file, print_mutex);
                            
                            if (!chunk_opt) {
                                std::lock_guard<std::mutex> lock(print_mutex);
                                printError("Failed to extract payload from " + chunk_file);
                                continue;
                            }
                            
                            auto chunk = *chunk_opt;
                            
                            // Set output filename from first successful chunk if not set already
                            if (!output_filename_set) {
                                // If outputPath is specified, create a full path
                                if (!outputPath.empty()) {
                                    output_filename = image_parser::createOutputPath(outputPath, chunk.filename);
                                } else {
                                    output_filename = chunk.filename;
                                }
                                output_filename_set = true;
                                
                                std::lock_guard<std::mutex> lock(print_mutex);
                                printStatus("Using output filename from metadata: " + chunk.filename);
                                if (!outputPath.empty()) {
                                    printStatus("Final output path: " + output_filename);
                                }
                            } else if (output_filename != chunk.filename && outputPath.empty()) {
                                // Only warn about inconsistent filenames if we're not using a custom output path
                                std::lock_guard<std::mutex> lock(print_mutex);
                                printWarning("Inconsistent output filename in " + chunk_file +
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
                        
                        // Write the assembled file
                        if (!chunks.empty() && output_filename_set) {
                            // Make sure the output directory exists
                            std::filesystem::path output_path(output_filename);
                            auto parent_path = output_path.parent_path();
                            if (!parent_path.empty() && !std::filesystem::exists(parent_path)) {
                                try {
                                    std::filesystem::create_directories(parent_path);
                                    printStatus("Created output directory: " + parent_path.string());
                                } catch (const std::exception& e) {
                                    printError("Failed to create output directory: " + std::string(e.what()));
                                }
                            }
                            
                            bool success = image_parser::writeAssembledFile(output_filename, chunks, print_mutex);
                            
                            if (success) {
                                printStatus("Successfully assembled output file: " + output_filename);
                            } else {
                                printError("Failed to assemble output file: " + output_filename);
                            }
                        } else {
                            printError("No valid chunks extracted, cannot create output file.");
                        }
                        
                        return;
                    } else {
                        std::cout << "No matching chunk files found." << std::endl;
                    }
                }
            }
        }
    }
    
    // If not a multi-chunk file or couldn't find related chunks, use original function
    // Pass the output path to the internal implementation if necessary
    if (!outputPath.empty()) {
        // Create a custom internal implementation that respects the output path
        std::vector<std::string> files_to_process = image_parser::getFilesToProcess(filename);
        
        if (files_to_process.empty()) {
            printError("No files found to process.");
            return;
        }
        
        printStatus("Processing " + std::to_string(files_to_process.size()) + " files");
        
        // Setup for single-threaded processing
        std::mutex print_mutex;
        
        // Map to store chunks by their index for proper ordering
        std::map<int, image_parser::ChunkInfo> chunks;
        std::string base_output_filename;
        bool output_filename_set = false;
        std::string final_output_path;
        
        // Extract payloads from all files
        for (const auto &file: files_to_process) {
            auto chunk_opt = image_parser::extractChunkPayload(file, print_mutex);
            
            if (!chunk_opt) {
                std::lock_guard<std::mutex> print_lock(print_mutex);
                printError("Failed to extract payload from " + file);
                continue;
            }
            
            auto chunk = *chunk_opt;
            
            // Set output filename from first successful chunk if not set already
            if (!output_filename_set) {
                base_output_filename = chunk.filename;
                // Create the full output path
                final_output_path = image_parser::createOutputPath(outputPath, base_output_filename);
                output_filename_set = true;
                
                std::lock_guard<std::mutex> print_lock(print_mutex);
                printStatus("Using output filename from metadata: " + base_output_filename);
                printStatus("Final output path: " + final_output_path);
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
            // Make sure the output directory exists
            std::filesystem::path output_path(final_output_path);
            auto parent_path = output_path.parent_path();
            if (!parent_path.empty() && !std::filesystem::exists(parent_path)) {
                try {
                    std::filesystem::create_directories(parent_path);
                    printStatus("Created output directory: " + parent_path.string());
                } catch (const std::exception& e) {
                    printError("Failed to create output directory: " + std::string(e.what()));
                }
            }
            
            bool success = image_parser::writeAssembledFile(final_output_path, chunks, print_mutex);
            
            if (success) {
                printStatus("Successfully assembled output file: " + final_output_path);
            } else {
                printError("Failed to assemble output file: " + final_output_path);
            }
        } else {
            printError("No valid chunks extracted, cannot create output file.");
        }
    } else {
        image_parser::parseFromImage(filename);
    }
} 