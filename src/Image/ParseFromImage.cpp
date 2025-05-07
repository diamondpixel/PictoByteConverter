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
        std::lock_guard lock(print_mutex);
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
     * @param pre_allocated_size Size that's already been allocated for this image (pass 0 if none)
     * @return Vector containing the extracted pixel data
     */
    std::vector<unsigned char> extractPixelData(const bitmap_image &image, size_t pre_allocated_size = 0) {
        unsigned int width = image.width();
        unsigned int height = image.height();

        // Reserve enough space for all pixel data - explicit calculation
        size_t total_pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        size_t data_size_estimate = total_pixels * 3; // 3 bytes per pixel (RGB)

        // We now rely on pre-allocated memory from the global pool
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

            // Extract all pixel data from the image - now always using global memory
            std::vector<unsigned char> data = extractPixelData(image, 0);
            if (data.empty()) {
                return std::nullopt;
            }

            // Use the printMessage function which handles locking internally
            printFilePath("Processing: " + filename);
            // printMessage("Processing image: " + filename +
            //             " (" + std::to_string(width) + "x" + std::to_string(height) + ") - Extracted data size: " +
            //             std::to_string(data.size()) + " bytes");

            // Start reading from the beginning of the data
            auto data_iter = data.begin();

            // Extract metadata from image header
            auto [metadata, success] = extractMetadata(data, data_iter, print_mutex, filename);
            if (!success) {
                return std::nullopt;
            }

            // Print metadata info directly here to ensure it's visible
            std::lock_guard<std::mutex> lock(print_mutex);
            std::cout << ANSIColorConst::CYAN << "=== Extracted metadata ===" << ANSIColorConst::RESET << std::endl;
            std::cout << "  " << ANSIColorConst::GREEN << "Output file:" << ANSIColorConst::RESET << " " << metadata.outputFilename << std::endl;
            std::cout << "  " << ANSIColorConst::GREEN << "Data size:" << ANSIColorConst::RESET << " " 
                     << ANSIColorConst::BRIGHT_YELLOW << metadata.expectedDataSize << ANSIColorConst::RESET << " bytes" << std::endl;
            
            if (metadata.currentChunk != -1 && metadata.totalChunks != -1) {
                std::cout << "  " << ANSIColorConst::GREEN << "Index info:" << ANSIColorConst::RESET << " " << metadata.indexInfo 
                        << " (Chunk " << ANSIColorConst::BRIGHT_YELLOW << metadata.currentChunk << ANSIColorConst::RESET << " of "
                        << ANSIColorConst::BRIGHT_YELLOW << metadata.totalChunks << ANSIColorConst::RESET << ")" << std::endl;
            } else if (!metadata.indexInfo.empty()) {
                std::cout << "  " << ANSIColorConst::GREEN << "Index info:" << ANSIColorConst::RESET << " " << metadata.indexInfo << std::endl;
            }
            std::cout << ANSIColorConst::CYAN << "=========================" << ANSIColorConst::RESET << std::endl;

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
            // printMessage("Extracted " + std::to_string(chunk.payload.size()) + " bytes of payload data from " + filename);

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
                            indexed_files.emplace_back(index, file_full_path);
                        } catch (const std::exception &e) {
                            printMessage("  Skipping " + filename + ": " + e.what());
                        }
                    }
                }
            }

            // If we didn't find any files with the exact base name, try looking for any chunked files in the directory
            if (indexed_files.empty()) {
                printMessage("No direct matches found, searching for any chunked files in the directory");

                // Use a specific regex to match patterns like file_10of16.bmp
                std::regex pattern(R"(.*_(\d+)of\d+\.bmp)");
                
                for (const auto &entry: std::filesystem::directory_iterator(directory)) {
                    if (!entry.is_regular_file() || entry.path().extension() != ".bmp") continue;

                    std::string filename = entry.path().filename().string();
                    std::string file_full_path = entry.path().string();
                    
                    std::smatch matches;
                    if (std::regex_match(filename, matches, pattern) && matches.size() > 1) {
                        try {
                            std::string index_str = matches[1].str();
                            int index = std::stoi(index_str);
                            indexed_files.emplace_back(index, file_full_path);
                        } catch (const std::exception& e) {
                            // Skip files with conversion errors
                        }
                    }
                }
                
                // Sort by index in ascending order
                std::sort(indexed_files.begin(), indexed_files.end(),
                    [](const std::pair<int, std::string> &a, const std::pair<int, std::string> &b) {
                        return a.first < b.first;
                    });
                    
                // Print the sorted files
                printMessage("Sorted files from secondary search:");
                for (const auto &pair : indexed_files) {
                    std::filesystem::path p(pair.second);
                    printMessage(" - " + p.filename().string());
                }
            }

            // Sort indexed files in ascending order by index
            std::sort(indexed_files.begin(), indexed_files.end(),
                [](const std::pair<int, std::string> &a, const std::pair<int, std::string> &b) {
                    // Simple numeric sorting of the index value
                    return a.first < b.first;
                });

            // Extract just the paths
            for (const auto &pair: indexed_files) {
                result.push_back(pair.second);
            }
            
            // Print the files in the order they will be processed
            printMessage("Files will be processed in this order:");
            for (const auto &path : result) {
                std::filesystem::path p(path);
                printMessage(" - " + p.filename().string());
            }
            
            printStatus("Found " + std::to_string(result.size()) + " sub-BMP files.");
        } catch (const std::exception &e) {
            printError("Error finding sub-bmp files: " + std::string(e.what()));
        }
        return result;
    }

    /**
     * Custom file name comparator that extracts and compares numeric portions
     * of filenames to sort in ascending numeric order, properly handling patterns like "file_10of16"
     * 
     * @param file_a First filename
     * @param file_b Second filename
     * @return True if file_a should come before file_b in sorting order
     */
    bool fileNameComparator(const std::string& file_a, const std::string& file_b) {
        // Extract just the filenames without path
        std::filesystem::path path_a(file_a);
        std::filesystem::path path_b(file_b);
        std::string name_a = path_a.filename().string();
        std::string name_b = path_b.filename().string();

        // Specifically handle the pattern file_Xof where X is a number
        // This is the format used for chunked files
        std::regex of_pattern(R"(.*_(\d+)of\d+\.bmp)");
        
        std::smatch matches_a, matches_b;
        bool has_of_a = std::regex_match(name_a, matches_a, of_pattern);
        bool has_of_b = std::regex_match(name_b, matches_b, of_pattern);
        
        // If both have the "of" pattern, extract and compare those numbers
        if (has_of_a && has_of_b && matches_a.size() > 1 && matches_b.size() > 1) {
            try {
                int index_a = std::stoi(matches_a[1].str());
                int index_b = std::stoi(matches_b[1].str());
                // Sort in ascending order (lowest first)
                return index_a < index_b;
            } catch (const std::exception& e) {
                // Fall back if conversion fails
            }
        }
        
        // Otherwise, check for numeric indexes at the end of the filename
        std::regex numeric_pattern(R"(.*_(\d+)\.bmp)");
        bool has_num_a = std::regex_match(name_a, matches_a, numeric_pattern);
        bool has_num_b = std::regex_match(name_b, matches_b, numeric_pattern);
        
        if (has_num_a && has_num_b && matches_a.size() > 1 && matches_b.size() > 1) {
            try {
                int num_a = std::stoi(matches_a[1].str());
                int num_b = std::stoi(matches_b[1].str());
                return num_a < num_b;
            } catch (const std::exception& e) {
                // Fall back if conversion fails
            }
        }
        
        // If one has pattern and other doesn't, or if extraction failed
        // Fall back to string comparison (ascending)
        return name_a < name_b;
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
                // std::cout << Color::BRIGHT_CYAN << "Looking for sub-bmp files with base name: " 
                //           << base_filename << Color::RESET << std::endl;

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
                    std::sort(chunk_files.begin(), chunk_files.end(), fileNameComparator);
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


                    printMessage("Detected chunk file, searching for all related chunks with base name: " + base_filename);

                    // Find all related chunks
                    files_to_process = findSubBmpFiles(base_filename);

                    if (files_to_process.size() > 1)
                        return files_to_process;
                }
            }

            // If no related chunks found or not a chunk file, just add the single file
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

            // Show a summary of chunk distribution at the start
            std::cout << ANSIColorConst::CYAN << "=== Chunk distribution summary ===" << ANSIColorConst::RESET << std::endl;
            std::cout << "  " << ANSIColorConst::GREEN << "Total chunks:" << ANSIColorConst::RESET << " " 
                      << ANSIColorConst::BRIGHT_YELLOW << highest_chunk_index << ANSIColorConst::RESET << std::endl;
            if (chunks.size() < highest_chunk_index) {
                std::cout << "  " << ANSIColorConst::RED << "Missing chunks:" << ANSIColorConst::RESET << " " 
                          << ANSIColorConst::BRIGHT_RED << (highest_chunk_index - chunks.size()) << ANSIColorConst::RESET << std::endl;
            }
            std::cout << ANSIColorConst::CYAN << "===============================" << ANSIColorConst::RESET << std::endl;

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

                // Update progress more frequently - every 5 chunks instead of 10
                if (i % 5 == 0 || i == highest_chunk_index || i == 1) {
                    // Use colored output for progress updates
                    std::cout << "  " << ANSIColorConst::GREEN << "Wrote chunk " << ANSIColorConst::RESET 
                              << ANSIColorConst::BRIGHT_YELLOW << i << ANSIColorConst::RESET << "/" 
                              << ANSIColorConst::BRIGHT_YELLOW << highest_chunk_index << ANSIColorConst::RESET
                              << " (" << ANSIColorConst::BRIGHT_YELLOW 
                              << total_bytes_written << ANSIColorConst::RESET << " bytes, " 
                              << ANSIColorConst::BRIGHT_CYAN << static_cast<int>((static_cast<float>(i) / highest_chunk_index) * 100)
                              << "%" << ANSIColorConst::RESET << ")" << std::endl;
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
            bool size_mismatch = false;

            if (actual_file_size != total_bytes_written) {
                size_mismatch = true;
                printWarning("File size mismatch: expected " + std::to_string(total_bytes_written) +
                          " bytes, got " + std::to_string(actual_file_size) + " bytes");
            }

            // Show final summary
            std::cout << ANSIColorConst::CYAN << "=== Writing complete ===" << ANSIColorConst::RESET << std::endl;
            std::cout << "  " << ANSIColorConst::GREEN << "Total bytes written:" << ANSIColorConst::RESET << " " 
                     << ANSIColorConst::BRIGHT_YELLOW << total_bytes_written << ANSIColorConst::RESET << std::endl;
            std::cout << "  " << ANSIColorConst::GREEN << "Output file:" << ANSIColorConst::RESET << " " 
                     << output_filename << std::endl;
            if (missing_chunks) {
                std::cout << "  " << ANSIColorConst::RED << "Warning: Some chunks were missing!" << ANSIColorConst::RESET << std::endl;
            }
            if (size_mismatch) {
                std::cout << "  " << ANSIColorConst::RED << "Warning: File size mismatch:" << ANSIColorConst::RESET << " expected " 
                         << ANSIColorConst::BRIGHT_YELLOW << total_bytes_written << ANSIColorConst::RESET
                         << " bytes, got " << ANSIColorConst::BRIGHT_YELLOW << actual_file_size << ANSIColorConst::RESET << " bytes" << std::endl;
            }
            std::cout << ANSIColorConst::CYAN << "======================" << ANSIColorConst::RESET << std::endl;
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
        
        // PRE-CALCULATE MEMORY REQUIREMENTS
        size_t total_memory_needed = 0;
        size_t safety_margin = 1024 * 1024; // 1MB safety margin
        
        // First, estimate the memory needs based on all the image files
        for (const auto &file : files_to_process) {
            try {
                std::ifstream::pos_type file_size = image_parser::fileSize(file.c_str());
                
                if (file_size > 0) {
                    // Estimate memory needed for this file (based on BMP dimensions and format)
                    // We allocate approximately 3x the file size as each pixel needs processing
                    size_t estimated_file_memory = static_cast<size_t>(file_size) * 3;
                    
                    // Add to total with boundary check
                    if (total_memory_needed + estimated_file_memory + safety_margin <= resManager.getMaxMemory()) {
                        total_memory_needed += estimated_file_memory;
                        printMessage("Adding " + std::to_string(estimated_file_memory / (1024*1024)) + 
                                   " MB for " + file + ", total now: " + 
                                   std::to_string(total_memory_needed / (1024*1024)) + " MB");
                    } else {
                        printWarning("Memory limit would be exceeded. Capping at maximum available.");
                        total_memory_needed = resManager.getMaxMemory() - safety_margin;
                        break;
                    }
                }
            } catch (const std::exception& e) {
                printWarning("Error estimating memory for " + file + ": " + e.what());
                // Add a conservative estimate if we can't determine size
                total_memory_needed += 10 * 1024 * 1024; // 10MB per file as fallback
                
                // Check if we hit the memory limit
                if (total_memory_needed + safety_margin >= resManager.getMaxMemory()) {
                    total_memory_needed = resManager.getMaxMemory() - safety_margin;
                    break;
                }
            }
        }
        
        // Allocate the entire memory block upfront
        printMessage("Pre-allocating " + std::to_string(total_memory_needed / (1024*1024)) + 
                   " MB for all image processing");
        
        bool memory_allocated = resManager.allocateMemory(
            total_memory_needed, 
            std::chrono::milliseconds(10000), 
            "TotalImageProcessingMemory"
        );
        
        if (!memory_allocated) {
            printWarning("Could not allocate requested memory. Will try to continue with available memory.");
        }

        // Process each file in a separate thread
        for (const auto &file : files_to_process) {

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