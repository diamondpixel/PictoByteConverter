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
#include "headers/bitmap.hpp"
#include "headers/parse_from_image.h"
#include <ranges>

namespace image_parser {

/**
 * Get the size of a file in bytes.
 *
 * @param filename Path to the file
 * @return Size of the file in bytes
 */
std::ifstream::pos_type filesize(const char *filename) {
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

    // Read total_header_length (3 bytes)
    if (std::distance(data_iter, data.end()) < 3) {
        std::lock_guard<std::mutex> lock(print_mutex);
        std::cerr << "Error: Not enough data for header length in " << filename << std::endl;
        return {metadata, false};
    }
    size_t total_header_length = (static_cast<size_t>(*data_iter) << 16) |
                                 (static_cast<size_t>(*(data_iter + 1)) << 8) |
                                 static_cast<size_t>(*(data_iter + 2));
    data_iter += 3;

    // Read filename_length (2 bytes)
    if (std::distance(data_iter, data.end()) < 2) {
        std::lock_guard<std::mutex> lock(print_mutex);
        std::cerr << "Error: Not enough data for filename length in " << filename << std::endl;
        return {metadata, false};
    }
    size_t filename_length = (static_cast<size_t>(*data_iter) << 8) | static_cast<size_t>(*(data_iter + 1));
    data_iter += 2;

    // Read filename
    if (std::distance(data_iter, data.end()) < static_cast<long>(filename_length)) {
        std::lock_guard<std::mutex> lock(print_mutex);
        std::cerr << "Error: Not enough data for filename in " << filename << std::endl;
        return {metadata, false};
    }
    metadata.output_filename = std::string(data_iter, data_iter + filename_length);
    data_iter += filename_length;

    // Read data_size (10 bytes)
    if (std::distance(data_iter, data.end()) < 10) {
        std::lock_guard<std::mutex> lock(print_mutex);
        std::cerr << "Error: Not enough data for data size in " << filename << std::endl;
        return {metadata, false};
    }
    std::string data_size_str(data_iter, data_iter + 10);
    try {
        metadata.expected_data_size = std::stoull(data_size_str);
    } catch (const std::exception &e) {
        std::lock_guard<std::mutex> lock(print_mutex);
        std::cerr << "Error parsing data size '" << data_size_str << "' in " << filename << ": " << e.what() << std::endl;
        return {metadata, false};
    }
    data_iter += 10;

    // Read current_chunk (4 bytes)
    if (std::distance(data_iter, data.end()) < 4) {
        std::lock_guard<std::mutex> lock(print_mutex);
        std::cerr << "Error: Not enough data for current chunk in " << filename << std::endl;
        return {metadata, false};
    }
    std::string current_chunk_str(data_iter, data_iter + 4);
    try {
        metadata.current_chunk = std::stoi(current_chunk_str);
    } catch (const std::exception &e) {
        std::lock_guard<std::mutex> lock(print_mutex);
        std::cerr << "Error parsing current chunk '" << current_chunk_str << "' in " << filename << ": " << e.what() << std::endl;
        return {metadata, false};
    }
    data_iter += 4;

    // Read total_chunks (4 bytes)
    if (std::distance(data_iter, data.end()) < 4) {
        std::lock_guard<std::mutex> lock(print_mutex);
        std::cerr << "Error: Not enough data for total chunks in " << filename << std::endl;
        return {metadata, false};
    }
    std::string total_chunks_str(data_iter, data_iter + 4);
    try {
        metadata.total_chunks = std::stoi(total_chunks_str);
    } catch (const std::exception &e) {
        std::lock_guard<std::mutex> lock(print_mutex);
        std::cerr << "Error parsing total chunks '" << total_chunks_str << "' in " << filename << ": " << e.what() << std::endl;
        return {metadata, false};
    }
    data_iter += 4;

    // Calculate padding and advance iterator
    size_t bytes_read = 2 + filename_length + 10 + 4 + 4;
    size_t pad_length = total_header_length - 3 - bytes_read;
    if (std::distance(data_iter, data.end()) < static_cast<long>(pad_length)) {
        std::lock_guard<std::mutex> lock(print_mutex);
        std::cerr << "Error: Not enough data for header padding in " << filename << std::endl;
        return {metadata, false};
    }
    data_iter += pad_length;

    return {metadata, true};
}

/**
 * Extract pixel data from the image
 *
 * @param image The bitmap image to process
 * @return Vector containing the extracted pixel data
 */
std::vector<unsigned char> extractPixelData(const bitmap_image& image) {
    unsigned int width = image.width();
    unsigned int height = image.height();

    // Reserve enough space for all pixel data - explicit calculation
    size_t total_pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    size_t data_size_estimate = total_pixels * 3; // 3 bytes per pixel (RGB)
    std::vector<unsigned char> data;
    data.reserve(data_size_estimate);

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
            std::cerr << "Error: Failed to load image " << filename << std::endl;
            return std::nullopt;
        }

        // Get image dimensions
        unsigned int width = image.width();
        unsigned int height = image.height();

        // Extract all pixel data from the image
        std::vector<unsigned char> data = extractPixelData(image);

        {
            std::lock_guard<std::mutex> print_lock(print_mutex);
            std::cout << "Processing image: " << filename
                      << " (" << width << "x" << height << ") - Estimated capacity: "
                      << data.size() << " bytes" << std::endl;
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
            std::cout << "Extracted metadata:" << std::endl
                      << "  Output file: " << metadata.output_filename << std::endl
                      << "  Data size: " << metadata.expected_data_size << " bytes" << std::endl
                      << "  Index info: " << metadata.index_info;

            if (metadata.current_chunk != -1 && metadata.total_chunks != -1) {
                std::cout << " (Chunk " << metadata.current_chunk << " of " << metadata.total_chunks << ")";
            }
            std::cout << std::endl;
        }

        // Calculate the distance from data_iter to data.end()
        size_t available_data = std::distance(data_iter, data.end());

        // Determine actual payload size for this chunk
        size_t payload_size = std::min(available_data, metadata.expected_data_size);

        // Create payload result
        ChunkInfo chunk;
        chunk.chunk_index = metadata.current_chunk;
        chunk.total_chunks = metadata.total_chunks;
        chunk.chunk_size = payload_size;
        chunk.filename = metadata.output_filename;

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
            std::lock_guard<std::mutex> print_lock(print_mutex);
            std::cout << "Extracted " << chunk.payload.size() << " bytes of payload data from "
                      << filename << std::endl;
        }

        return chunk;
    }
    catch (const std::exception &e) {
        std::lock_guard<std::mutex> print_lock(print_mutex);
        std::cerr << "Exception processing " << filename << ": " << e.what() << std::endl;
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
        std::vector<std::pair<int, std::string>> indexed_files;

        // Extract directory path and filename from base_filename
        std::filesystem::path full_path(base_filename);
        std::filesystem::path directory = full_path.parent_path();
        std::string file_basename = full_path.filename().string();

        // If directory is empty, use current directory
        if (directory.empty()) {
            directory = ".";
        }

        std::cout << "Searching for sub-BMP files with base name: " << file_basename
                  << " in directory: " << directory << std::endl;

        // Search specified directory instead of current directory
        for (const auto &entry : std::filesystem::directory_iterator(directory)) {
            if (!entry.is_regular_file()) continue;

            std::string filename = entry.path().filename().string();
            std::string full_path = entry.path().string();

            // Convert filename to lowercase for case-insensitive comparison
            std::string lower_filename = filename;
            std::ranges::transform(lower_filename, lower_filename.begin(),
                                   [](unsigned char c){ return std::tolower(c); });

            // Convert base filename to lowercase for comparison
            std::string lower_basename = file_basename;
            std::ranges::transform(lower_basename, lower_basename.begin(),
                                   [](unsigned char c){ return std::tolower(c); });

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
                        std::cout << "  Found file: " << filename << " (index: " << index << ")" << std::endl;
                        indexed_files.emplace_back(index, full_path);
                    }
                    catch (const std::exception &e) {
                        std::cout << "  Skipping " << filename << ": " << e.what() << std::endl;
                    }
                }
            }
        }

        // Sort by index
        std::ranges::sort(indexed_files);

        // Extract just the paths
        for (const auto &val: indexed_files | std::views::values) {
            result.push_back(val);
        }

        std::cout << "Found " << result.size() << " sub-BMP files." << std::endl;
    }
    catch (const std::exception &e) {
        std::cerr << "Error finding sub-bmp files: " << e.what() << std::endl;
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

    // Handle the case of main file not found but sub-bmps might exist
    if (!is_pattern && !is_directory && !std::filesystem::exists(filename)) {
        std::cout << "Main file not found: " << filename << std::endl;
        std::string base_filename = filename;

        // Remove extension if present
        size_t dot_pos = base_filename.find_last_of('.');
        if (dot_pos != std::string::npos) {
            base_filename = base_filename.substr(0, dot_pos);
        }

        std::cout << "Looking for sub-bmp files with base name: " << base_filename << std::endl;

        files_to_process = findSubBmpFiles(base_filename);

        if (files_to_process.empty()) {
            std::cerr << "No sub-bmp files found for: " << base_filename << std::endl;
            return files_to_process;
        }

        std::cout << "Found " << files_to_process.size() << " sub-bmp files to process:" << std::endl;
        for (const auto &file : files_to_process) {
            std::cout << "  - " << file << std::endl;
        }
    }
    else if (is_pattern) {
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
            return files_to_process;
        }

        // Sort the files
        std::ranges::sort(files_to_process);
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
            return files_to_process;
        }

        // Sort the files
        std::ranges::sort(files_to_process);
    }
    else {
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
    const std::string& output_filename,
    const std::map<int, ChunkInfo>& chunks,
    std::mutex& print_mutex) {

    try {
        std::ofstream outfile(output_filename, std::ios::binary | std::ios::trunc);
        if (!outfile) {
            std::lock_guard<std::mutex> print_lock(print_mutex);
            std::cerr << "Error: Could not create output file " << output_filename << std::endl;
            return false;
        }

        // Calculate total expected size
        size_t total_size = 0;
        size_t expected_chunks = 0;

        if (!chunks.empty()) {
            expected_chunks = chunks.begin()->second.total_chunks;
            for (const auto& [idx, chunk] : chunks) {
                total_size += chunk.payload.size();
            }
        }

        {
            std::lock_guard<std::mutex> print_lock(print_mutex);
            std::cout << "Writing assembled file " << output_filename
                      << " from " << chunks.size() << "/" << expected_chunks
                      << " chunks, total size: " << total_size << " bytes" << std::endl;

            // Check if we have all expected chunks
            if (chunks.size() != expected_chunks) {
                std::cout << "Warning: Missing " << (expected_chunks - chunks.size())
                          << " chunks. Output file may be incomplete." << std::endl;
            }
        }

        // Write chunks in order
        size_t bytes_written = 0;
        for (const auto& [idx, chunk] : chunks) {
            if (!chunk.payload.empty()) {
                outfile.write(reinterpret_cast<const char*>(chunk.payload.data()),
                             chunk.payload.size());

                if (!outfile) {
                    std::lock_guard<std::mutex> print_lock(print_mutex);
                    std::cerr << "Error writing chunk " << idx << " to output file" << std::endl;
                    return false;
                }

                bytes_written += chunk.payload.size();
            }
        }

        outfile.close();

        {
            std::lock_guard<std::mutex> print_lock(print_mutex);
            std::cout << "Successfully wrote " << bytes_written << " bytes to "
                      << output_filename << std::endl;
        }

        return true;
    }
    catch (const std::exception &e) {
        std::lock_guard<std::mutex> print_lock(print_mutex);
        std::cerr << "Exception writing output file: " << e.what() << std::endl;
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
    std::vector<std::string> files_to_process = getFilesToProcess(filename);

    if (files_to_process.empty()) {
        std::cerr << "No files found to process." << std::endl;
        return;
    }

    std::cout << "Processing " << files_to_process.size() << " files" << std::endl;

    // Setup for single-threaded processing
    std::mutex print_mutex;

    // Map to store chunks by their index for proper ordering
    std::map<int, ChunkInfo> chunks;
    std::string output_filename;
    bool output_filename_set = false;

    // Extract payloads from all files
    for (const auto& file : files_to_process) {
        auto chunk_opt = extractChunkPayload(file, print_mutex);

        if (!chunk_opt) {
            std::lock_guard<std::mutex> print_lock(print_mutex);
            std::cerr << "Failed to extract payload from " << file << std::endl;
            continue;
        }

        auto chunk = *chunk_opt;

        // Set output filename from first successful chunk if not set already
        // Ensure we preserve the exact filename from metadata including extension
        if (!output_filename_set) {
            output_filename = chunk.filename;
            output_filename_set = true;

            std::lock_guard<std::mutex> print_lock(print_mutex);
            std::cout << "Using output filename from metadata: " << output_filename << std::endl;
        } else if (output_filename != chunk.filename) {
            // Warn about inconsistent output filenames
            std::lock_guard<std::mutex> print_lock(print_mutex);
            std::cout << "Warning: Inconsistent output filename in " << file
                      << " ('" << chunk.filename << "' vs '" << output_filename << "')" << std::endl;
        }

        // Store chunk by its index
        if (chunk.chunk_index > 0) {
            chunks[chunk.chunk_index] = std::move(chunk);
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
            std::cout << "Successfully assembled output file: " << output_filename << std::endl;
        } else {
            std::cerr << "Failed to assemble output file: " << output_filename << std::endl;
        }
    } else {
        std::cerr << "No valid chunks extracted, cannot create output file." << std::endl;
    }
}

} // namespace image_parser

// Export the public interface functions
std::ifstream::pos_type filesize(const char *filename) {
    return image_parser::filesize(filename);
}

void parseFromImage(const std::string &filename) {
    image_parser::parseFromImage(filename);
}