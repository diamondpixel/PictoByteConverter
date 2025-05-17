/*
 * ParseToImage.cpp
 * This file implements the core logic for converting large binary files into a series of 
 * bitmap (BMP) images. It is designed to handle files that may exceed typical memory limits 
 * by splitting them into manageable chunks. Each chunk, along with essential metadata, 
 * is then encoded into a BMP image.
 *
 * Key functionalities include:
 * - Memory-mapping input files for efficient read access to large file data.
 * - Calculating optimal dimensions for BMP images to store data chunks while adhering to specified maximum file size constraints.
 * - Embedding the actual data chunk and relevant metadata (such as original filename, chunk index, total chunks, and data size) into the pixel data of the BMP image.
 * - Managing a pool of worker threads to process multiple chunks concurrently, improving performance for large files.
 * - Utilizing a thread-safe queue to manage image saving tasks, thereby decoupling CPU-intensive image processing from I/O-bound disk writing operations.
 * - Constructing and writing BMP file headers (BITMAPFILEHEADER and BITMAPINFOHEADER) and pixel data according to the BMP format specification.
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

// Define DEFAULT_MAX_IMAGE_SIZE here. This constant specifies the default upper limit
// for the size of any single generated BMP image file. It helps in managing output file sizes
// and preventing excessively large images. This value can be overridden by user-provided parameters
// if the application's interface supports it.
constexpr size_t DEFAULT_MAX_IMAGE_SIZE = 100 * 1024 * 1024; // 100MB

/**
 * @brief Calculates optimal rectangular dimensions for a bitmap image.
 * 
 * This function determines the most suitable width and height for a BMP image that needs to store a 
 * given amount of `total_bytes` data. The calculation aims to:
 * 1. Ensure the image has enough pixel capacity for `total_bytes`.
 * 2. Ensure the final BMP file (including headers, pixel data, and row padding) does not exceed `max_size_bytes`.
 * 3. Optionally, try to maintain a desired `aspect_ratio` for the image dimensions.
 *
 * The process involves considering the BMP header size, bytes per pixel, and the BMP format's requirement 
 * for row padding (each row's byte count must be a multiple of 4). It uses an iterative approach to 
 * adjust dimensions to meet all constraints, prioritizing data integrity (not truncating `total_bytes`) 
 * and then the `max_size_bytes` limit.
 *
 * @param total_bytes The total number of payload bytes (e.g., file chunk data + metadata) that needs to be stored 
 *                    within the image's pixel data area.
 * @param bytes_per_pixel The number of bytes used to represent a single pixel in the image (e.g., 3 for 24-bit RGB).
 * @param bmp_header_size The fixed size of the BMP file header and info header in bytes (typically 54 bytes).
 * @param max_size_bytes The maximum permissible size for the resulting BMP file, including all headers, 
 *                       pixel data, and padding. This is a hard limit the function tries to adhere to.
 * @param aspect_ratio The desired width-to-height ratio for the image (e.g., 1.0f for a square image). 
 *                     Defaults to 1.0f.
 * @return A `std::pair<size_t, size_t>` containing the calculated optimal width and height. 
 *         Returns `{0, 0}` if no valid dimensions can be found that satisfy all constraints 
 *         (i.e., cannot fit `total_bytes` within `max_size_bytes` or other logical failures).
 */
std::pair<size_t, size_t> calculateOptimalRectDimensions(size_t total_bytes, size_t bytes_per_pixel,
                                                         size_t bmp_header_size,
                                                         size_t max_size_bytes, float aspect_ratio = 1.0f) {
    // Estimate the minimum possible size of the image data payload itself. This is just the raw data bytes.
    size_t min_data_payload_size = total_bytes;
    // Calculate the theoretical minimum file size: header + raw data payload. This doesn't account for BMP row padding yet.
    size_t min_possible_image_file_size = bmp_header_size + min_data_payload_size;

    // Quick check: If max_size_bytes is less than even this highly optimistic minimum, it's impossible.
    if (max_size_bytes < min_possible_image_file_size) {
        if (gDebugMode.load(std::memory_order_relaxed)) {
            std::ostringstream oss;
            oss << "calculateOptimalRectDimensions: max_size_bytes (" << max_size_bytes
                    << ") is less than min required theoretical image file size (" << min_possible_image_file_size
                    << ") for data payload (" << total_bytes << " bytes). Returning {0,0}.";
            printWarning(oss.str());
        }
        return {0, 0}; // Cannot fit data even in the most ideal scenario.
    }

    // Calculate the total number of pixels required to store `total_bytes` of data.
    // The `(bytes_per_pixel - 1)` part ensures ceiling division: if `total_bytes` is not a perfect multiple
    // of `bytes_per_pixel`, we allocate enough pixels for the remainder.
    // The `+ 500` pixels act as a safety margin. This buffer helps to:
    //   a) Accommodate potential BMP row padding, which can consume extra space not directly used by the payload data.
    //   b) Mitigate issues arising from discrete pixel dimensions (integer width/height) not perfectly matching continuous data needs.
    //   c) Provide a small leeway for potential header variations (though `bmp_header_size` should be fixed) or other minor overheads.
    size_t total_pixels = (total_bytes + bytes_per_pixel - 1) / bytes_per_pixel + 500;

    // Calculate initial width and height based on the total pixels needed and the desired aspect ratio.
    // The formulas are derived from: total_pixels = width * height and aspect_ratio = width / height.
    // So, width = sqrt(total_pixels * aspect_ratio) and height = width / aspect_ratio (or sqrt(total_pixels / aspect_ratio)).
    auto width = static_cast<size_t>(std::sqrt(static_cast<double>(total_pixels * aspect_ratio)));
    auto height = static_cast<size_t>(width / aspect_ratio); // More numerically stable to calculate height from width if aspect_ratio is not 1.

    // Ensure initial dimensions provide enough pixels for all data.
    // This loop iteratively increases dimensions (primarily width, then height via aspect ratio) if the initial calculation (width * height) is insufficient.
    int emergency_break_initial_sizing = 10000; // Safeguard against excessively long loops due to unusual inputs or floating point inaccuracies.
    while (width * height < total_pixels) {
        width++; // Increment width slightly.
        height = static_cast<size_t>(width / aspect_ratio); // Recalculate height to maintain the aspect ratio.
        
        // Safety check: if dimensions become invalid (e.g., height becomes zero due to extreme aspect_ratio or width hitting limits)
        // or if the loop runs too many times, abort.
        if (width == 0 || height == 0 || --emergency_break_initial_sizing <= 0) {
            if (gDebugMode.load(std::memory_order_relaxed)) {
                std::ostringstream oss;
                oss << "calculateOptimalRectDimensions: Initial sizing loop failed or hit safety break. W:" << width <<
                        " H:" << height << " TotalPixelsRequired: " << total_pixels;
                printWarning(oss.str());
            }
            return {0, 0}; // Failed to find suitable initial dimensions.
        }
    }

    // Enforce minimum practical dimensions for the bitmap.
    // Very small images (e.g., 1x1) might be unusable or cause issues with some image viewers/libraries.
    constexpr size_t min_dimension = 64; // A reasonable minimum dimension (e.g., 64 pixels).
    if (width < min_dimension) width = min_dimension;
    if (height < min_dimension) height = min_dimension;

    // Enforce maximum dimensions, typically limited by BMP format specifications (e.g., max value for WORD type in headers, often 65535)
    // or practical constraints (e.g., system resources, display capabilities of image viewers).
    constexpr size_t max_bitmap_dimension = 65535; // Maximum dimension for BMP (often tied to 16-bit fields in headers).
    if (width > max_bitmap_dimension) width = max_bitmap_dimension;
    if (height > max_bitmap_dimension) height = max_bitmap_dimension;

    // After clamping dimensions to min/max boundaries, re-check if they can still hold the required `total_pixels`.
    // Clamping might have reduced the pixel count below what's needed.
    if (width * height < total_pixels) {
        if (gDebugMode.load(std::memory_order_relaxed)) {
            std::ostringstream oss;
            oss << "calculateOptimalRectDimensions: Dimensions " << width << "x" << height
                    << " (clamped by min/max values) cannot hold total_pixels " << total_pixels << ". Returning {0,0}.";
            printWarning(oss.str());
        }
        return {0, 0}; // Clamped dimensions are too small for the pixel data.
    }

    // Calculate the actual file size based on the current width, height, and BMP padding requirements.
    // BMP format requires each row of pixel data to be padded to a multiple of 4 bytes.
    size_t row_bytes_unpadded = width * bytes_per_pixel;              // Bytes for pixel data in one row, without padding.
    size_t padding_per_row = (4 - (row_bytes_unpadded % 4)) % 4;    // Bytes needed to pad one row to a multiple of 4.
    size_t row_bytes_padded = row_bytes_unpadded + padding_per_row; // Total bytes for one padded row.
    size_t actual_image_size = bmp_header_size + (height * row_bytes_padded); // Total file size.

    // Iteratively shrink dimensions if the calculated `actual_image_size` exceeds `max_size_bytes`.
    // This loop attempts to reduce dimensions (and thus file size) while ensuring:
    //   1. Dimensions do not go below `min_dimension`.
    //   2. The capacity (width * height * bytes_per_pixel) still remains sufficient for `total_bytes` (with a small safety buffer).
    int emergency_break_shrinking = 10000; // Safeguard against excessively long loops.
    while (actual_image_size > max_size_bytes && width > min_dimension && height > min_dimension) {
        if (--emergency_break_shrinking <= 0) {
            if (gDebugMode.load(std::memory_order_relaxed)) {
                std::ostringstream oss;
                oss << "calculateOptimalRectDimensions: Shrinking loop safety break. ActualImageSize:" << actual_image_size <<
                        " MaxSizeBytes:" << max_size_bytes;
                printWarning(oss.str());
            }
            break; // Exit shrinking loop and proceed to final checks with current dimensions.
        }

        size_t old_width = width;
        size_t old_height = height;

        // Heuristic: Check if current pixel capacity is getting critically close to the required data payload.
        // If `width * height * bytes_per_pixel` is only slightly larger than `total_bytes`,
        // further shrinking might make it impossible to store the actual data, even if the file size constraint is met.
        // The `bytes_per_pixel * 100` term acts as a small buffer (e.g., 300 bytes for 3BPP, or about 100 pixels).
        // It's generally better to slightly exceed `max_size_bytes` (if unavoidable and allowed by subsequent checks)
        // than to truncate or be unable to store the data payload.
        if (width * height * bytes_per_pixel < total_bytes + (bytes_per_pixel * 100)) {
            // If current pixel capacity is within a small margin (e.g., ~100 pixels worth of data) of the required payload,
            // be very cautious with further shrinking as it might compromise data storage.
            if (gDebugMode.load(std::memory_order_relaxed)) {
                std::ostringstream oss;
                oss << "calculateOptimalRectDimensions: Shrinking might compromise data capacity. Current W:" << width << " H:"
                        << height << ". TotalBytes: " << total_bytes << ". MaxSizeBytes: " << max_size_bytes;
                printDebug(oss.str()); // Log as INFO/Debug level, not a warning yet.
            }
            // It's better to potentially fail the `max_size_bytes` constraint (and let final checks handle it)
            // than to risk making dimensions too small to hold the actual `total_bytes` payload.
            break; // Stop shrinking.
        }

        // Preferentially shrink the larger dimension to better maintain the aspect ratio (or get closer to square if aspect_ratio was ~1.0).
        if (width > height) {
            width--;
        } else {
            height--;
        }

        // If dimensions didn't change (e.g., both are at `min_dimension`, or one hit `min_dimension` and the other couldn't be reduced further based on aspect ratio),
        // break the loop to avoid getting stuck.
        if (width == old_width && height == old_height) {
            // No change in dimensions, implies we are stuck (e.g., at min_dimension or constrained by aspect ratio at min_dimension).
            if (gDebugMode.load(std::memory_order_relaxed)) {
                std::ostringstream oss;
                oss << "calculateOptimalRectDimensions: Shrinking loop stuck. W:" << width << " H:" << height;
                printWarning(oss.str());
            }
            break;
        }

        // Recalculate image size with the new (shrunken) dimensions.
        row_bytes_unpadded = width * bytes_per_pixel;
        padding_per_row = (4 - (row_bytes_unpadded % 4)) % 4;
        row_bytes_padded = row_bytes_unpadded + padding_per_row;
        actual_image_size = bmp_header_size + (height * row_bytes_padded);
    }

    // Final critical check 1: Can the chosen dimensions (after all adjustments) actually hold the `total_bytes` data payload?
    // This ensures that data isn't truncated due to dimension calculations, clamping, or shrinking attempts.
    // The condition `width * height * bytes_per_pixel` represents the raw pixel data capacity, which must be >= `total_bytes`.
    if (width * height * bytes_per_pixel < total_bytes) {
        if (gDebugMode.load(std::memory_order_relaxed)) {
            std::ostringstream oss;
            oss << "calculateOptimalRectDimensions: Final dimensions " << width << "x" << height
                    << " (raw pixel capacity " << (width * height * bytes_per_pixel) << " bytes) are too small for data_payload (" << total_bytes <<
                    " bytes) after attempting to meet max_size_bytes "
                    << max_size_bytes << ". Returning {0,0}.";
            printWarning(oss.str());
        }
        return {0, 0}; // Chosen dimensions are insufficient for the data payload.
    }

    // Final critical check 2: Does the image file (with chosen dimensions, headers, and padding) respect the `max_size_bytes` constraint?
    // If this fails, it means it was impossible to satisfy both the data capacity requirement and the file size limit with valid dimensions.
    if (actual_image_size > max_size_bytes) {
        if (gDebugMode.load(std::memory_order_relaxed)) {
            std::ostringstream oss;
            oss << "calculateOptimalRectDimensions: Final dimensions " << width << "x" << height
                    << " (actual image file size " << actual_image_size << " bytes) still exceed max_size_bytes (" 
                    << max_size_bytes << "). Data payload was " << total_bytes << " bytes. Returning {0,0}.";
            printWarning(oss.str());
        }
        return {0, 0}; // Cannot meet max file size constraint even with dimensions sufficient for data capacity.
    }

    // If all checks pass, the calculated width and height are considered valid and optimal under the given constraints.
    return {width, height};
}

/**
 * @brief Optimizes dimensions specifically for the last image in a sequence of chunked images.
 * 
 * The last image in a series (when a large file is split into multiple image chunks)
 * might have different sizing considerations. For instance, it might contain a smaller final
 * data segment or primarily consist of metadata. This function is similar to 
 * `calculateOptimalRectDimensions` but might be tailored for scenarios where `total_bytes` 
 * is relatively small (e.g., mostly metadata or a small remaining data part).
 *
 * A key difference or consideration here is how `max_size_bytes` (if applicable implicitly or via a default)
 * is handled for the last chunk. Typically, the last chunk has more flexibility as it doesn't need to 
 * reserve space for subsequent full-sized chunks.
 * This function aims to find compact yet sufficient dimensions for the `total_bytes` (data + metadata).
 *
 * @param total_bytes Total bytes of data (this should include actual data from the original file segment 
 *                    AND any metadata being embedded) to store in this last image.
 * @param bytes_per_pixel Number of bytes per pixel (e.g., 3 for 24-bit RGB).
 * @param metadata_size Size of the metadata portion that is included within `total_bytes`. This parameter 
 *                      might be used for more nuanced calculations or logging, but `total_bytes` is the 
 *                      primary driver for pixel capacity.
 * @param bmp_header_size Size of the BMP file and info headers in bytes (typically 54 bytes).
 * @param aspect_ratio Desired width:height ratio for the image (e.g., 1.0f for a square). Defaults to 1.0f.
 * @return A `std::pair<size_t, size_t>` containing the calculated width and height.
 *         Returns `{0,0}` if valid dimensions cannot be found (e.g., if `total_bytes` is excessively large 
 *         for reasonable image dimensions or if internal logic fails).
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
 * @brief Processes a single chunk of an input file and prepares it for conversion into a bitmap image.
 *
 * This function is a core part of the file-to-image conversion pipeline. It takes a segment (chunk)
 * of the memory-mapped input file data (`all_file_data_ptr` + offset and length), constructs a metadata 
 * header specific to this chunk (containing information like original filename, chunk index, total chunks, 
 * and data size of this chunk), and combines this metadata with the actual chunk data. 
 * It then calculates the optimal dimensions for a BMP image that can store this combined payload, 
 * respecting `max_image_file_size_param`.
 * Finally, it creates a `BitmapImage` object, populates it with the header and chunk data, and queues an 
 * `ImageTaskInternal` (containing the image and output filename) for asynchronous writing to disk by 
 * a separate writer thread.
 *
 * @param chunk_index The 0-based index of the current chunk being processed from the original file.
 * @param chunk_data_max_size The pre-calculated maximum size of original file data that each chunk (except possibly the last) 
 *                            should contain. The actual data read for this specific chunk might be less if it's the last one.
 * @param total_chunks The total number of chunks the original input file has been divided into.
 * @param original_file_total_size The total size of the original input file in bytes.
 * @param all_file_data_ptr A pointer to the beginning of the memory-mapped data of the entire input file.
 *                          This allows direct access to any part of the file's content without repeated reads.
 * @param original_input_filepath The full path to the original input file (used for extracting filename for metadata).
 * @param output_base_path The base directory and filename prefix where output BMP images will be saved. 
 *                         The chunk index and total chunks will be appended to this base to form unique filenames.
 * @param image_task_queue A reference to a thread-safe queue. `ImageTaskInternal` objects, ready for saving to disk, 
 *                         are pushed onto this queue. This allows image saving to be handled by a dedicated writer thread, 
 *                         decoupling it from the chunk processing logic.
 * @param max_image_file_size_param The maximum allowed size for any single generated BMP image file in bytes. 
 *                                  This is passed to `calculateOptimalRectDimensions` or `optimizeLastImageDimensions`.
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
            printError(
                std::string("processChunk: Memory allocation failed for chunk_data_segment: ") + e.what() +
                std::string(" for chunk ") + std::to_string(chunk_index));
            if (gDebugMode.load(std::memory_order_relaxed)) {
                printDebug("---- END CHUNK PROCESSING: " + std::to_string(chunk_index) + " (Early Exit: bad_alloc for chunk_data_segment) ----");
            }
            return; // Cannot proceed with this chunk
        } catch (const std::exception &e) {
            printError(
                std::string("processChunk: Error copying chunk data: ") + e.what() + std::string(" for chunk ") +
                std::to_string(chunk_index));
            if (gDebugMode.load(std::memory_order_relaxed)) {
                printDebug("---- END CHUNK PROCESSING: " + std::to_string(chunk_index) + " (Early Exit: std::exception copying chunk data) ----");
            }
            return;
        }
    }
    // else: current_chunk_actual_data_length is 0, chunk_data_segment remains empty, which is valid for a zero-sized final chunk.

    if (gDebugMode.load(std::memory_order_relaxed)) {
        printDebug("---- BEGIN CHUNK PROCESSING: " + std::to_string(chunk_index) + " / " + std::to_string(total_chunks - 1) + " ----");
        printDebug("  Chunk Index: " + std::to_string(chunk_index));
        printDebug("  Total Chunks: " + std::to_string(total_chunks));
        printDebug("  Actual Data Length for this chunk: " + std::to_string(current_chunk_actual_data_length));
        printDebug("  Original File Total Size: " + std::to_string(original_file_total_size));
        printDebug("  Max Image File Size Param: " + std::to_string(max_image_file_size_param));

        const unsigned char *current_segment_ptr_dbg = chunk_data_segment.empty() ? nullptr : chunk_data_segment.data();
        const unsigned char *end_ptr_dbg = current_segment_ptr_dbg
                                               ? current_segment_ptr_dbg + current_chunk_actual_data_length
                                               : nullptr;
        std::ostringstream oss_start_ptr_proc, oss_end_ptr_proc;
        oss_start_ptr_proc << static_cast<const void *>(current_segment_ptr_dbg);
        oss_end_ptr_proc << static_cast<const void *>(end_ptr_dbg);
        printDebug("  Data Segment Start Ptr (local copy): " + oss_start_ptr_proc.str());
        printDebug("  Data Segment End Ptr (local copy, exclusive): " + oss_end_ptr_proc.str());

        if (chunk_data_segment.empty() && current_chunk_actual_data_length > 0) {
            std::string error_msg = std::string("CRITICAL_ERROR_CHUNK_PROC: Chunk ") + std::to_string(chunk_index) +
                                    std::string(
                                        " has current_chunk_actual_data_length > 0 but chunk_data_segment is empty.");
            printError(error_msg);
            if (gDebugMode.load(std::memory_order_relaxed)) {
                printDebug("---- END CHUNK PROCESSING: " + std::to_string(chunk_index) + " (Early Exit: empty chunk_data_segment with positive length) ----");
            }
            return;
        }
    }

    // Construct header_data (internal metadata like chunk index, total chunks, etc.)
    std::string chunk_idx_str = "ChunkIndex:" + std::to_string(chunk_index) + "\0";
    std::string total_chunks_str = "TotalChunks:" + std::to_string(total_chunks) + "\0";
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
        printError(
            std::string("Data size string too long!") + std::string(" for chunk ") + std::to_string(chunk_index));
        // Should not happen for typical chunk sizes
        data_size_str = data_size_str.substr(0, 10);
    }
    while (data_size_str.length() < 10) data_size_str.insert(0, "0"); // Pad with leading zeros
    header_data.insert(header_data.end(), data_size_str.begin(), data_size_str.end());

    // 5. Current Chunk Index (4 bytes as string)
    std::string current_chunk_str = std::to_string(chunk_index);
    if (current_chunk_str.length() > 4) {
        printError(
            std::string("Current chunk string too long!") + std::string(" for chunk ") + std::to_string(chunk_index));
        current_chunk_str = "9999";
    }
    while (current_chunk_str.length() < 4) current_chunk_str.insert(0, "0");
    header_data.insert(header_data.end(), current_chunk_str.begin(), current_chunk_str.end());

    // 6. Total Chunks (4 bytes as string)
    total_chunks_str = std::to_string(total_chunks);
    if (total_chunks_str.length() > 4) {
        printError(
            std::string("Total chunks string too long!") + std::string(" for chunk ") + std::to_string(chunk_index));
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
        printError(
            std::string("Logic error: Calculated header size exceeds FIXED_TOTAL_HEADER_LENGTH. Truncating.") +
            std::string(" for chunk ") + std::to_string(chunk_index));
        header_data.resize(FIXED_TOTAL_HEADER_LENGTH);
    }

    std::vector<unsigned char> combined_data_for_image;
    try {
        // Reserve space first: header + actual chunk data
        // Ensure chunk_data_segment is used here, not chunk_data_ptr
        size_t required_size = header_data.size() + (chunk_data_segment.empty() ? 0 : chunk_data_segment.size());
        combined_data_for_image.reserve(required_size);

        // Insert header
        combined_data_for_image.insert(combined_data_for_image.end(), header_data.begin(), header_data.end());

        // Insert actual chunk data (if any)
        if (!chunk_data_segment.empty()) {
            if (gDebugMode.load(std::memory_order_relaxed)) {
                std::ostringstream oss_src_ptr;
                oss_src_ptr << static_cast<const void *>(chunk_data_segment.data());
                printDebug("  Action: Inserting chunk_data_segment into combined_data");
                printDebug("    Source Ptr: " + oss_src_ptr.str());
                printDebug("    Size: " + std::to_string(chunk_data_segment.size()));
            }
            combined_data_for_image.insert(combined_data_for_image.end(), chunk_data_segment.begin(),
                                           chunk_data_segment.end());
        } else if (current_chunk_actual_data_length > 0) {
            // This case should have been caught by the earlier check, but as a safeguard:
            std::string error_msg = std::string("CRITICAL_ERROR_CHUNK_PROC: Chunk ") + std::to_string(chunk_index) +
                                    std::string(
                                        " about to insert data, but chunk_data_segment is empty while current_chunk_actual_data_length > 0.");
            printError(error_msg);
            // Not returning, to see if it crashes, but this is bad.
        }
    } catch (const std::length_error &le) {
        std::string error_msg = std::string(
                                    "processChunk: Length error during vector operation for combined_data_for_image: ")
                                + le.what() + std::string(" for chunk ") + std::to_string(chunk_index);
        printError(error_msg);
        if (gDebugMode.load(std::memory_order_relaxed)) {
            printDebug("---- END CHUNK PROCESSING: " + std::to_string(chunk_index) + " (Early Exit: length_error) ----");
        }
        return; // Added return based on previous pattern for critical errors
    } catch (const std::bad_alloc &ba) {
        printError(
            std::string("processChunk: std::bad_alloc during vector reserve/insert: ") + ba.what() +
            std::string(" for chunk ") + std::to_string(chunk_index));
        if (gDebugMode.load(std::memory_order_relaxed)) {
            printDebug("---- END CHUNK PROCESSING: " + std::to_string(chunk_index) + " (Early Exit: bad_alloc) ----");
        }
        return; // Don't proceed
    } catch (const std::exception &e) {
        printError(
            std::string("processChunk: Generic std::exception during vector reserve/insert: ") + e.what() +
            std::string(" for chunk ") + std::to_string(chunk_index));
        if (gDebugMode.load(std::memory_order_relaxed)) {
            printDebug("---- END CHUNK PROCESSING: " + std::to_string(chunk_index) + " (Early Exit: std::exception) ----");
        }
        return; // Don't proceed
    }

    if (gDebugMode.load(std::memory_order_relaxed)) {
        printDebug("  Combined Data Payload Size (after header and data insert): " + std::to_string(combined_data_for_image.size()));
        // Max image file size parameter is already printed in the top block
        // printDebug("  Max Image File Size Parameter (context): " + std::to_string(max_image_file_size_param));
    }

    // Calculate optimal dimensions for the image
    size_t width = 0, height = 0;
    constexpr size_t bytes_per_pixel = 3;
    constexpr size_t bmp_header_size = 54; // Standard BMP header size

    if (chunk_index == static_cast<int>(total_chunks - 1)) {
        // Special handling for the last chunk if needed (e.g., different aspect ratio or metadata)
        auto dims = optimizeLastImageDimensions(combined_data_for_image.size(), bytes_per_pixel, 0, bmp_header_size,
                                                1.0f);
        width = dims.first;
        height = dims.second;
    } else {
        auto dims = calculateOptimalRectDimensions(combined_data_for_image.size(), bytes_per_pixel, bmp_header_size,
                                                   max_image_file_size_param, 1.0f);
        width = dims.first;
        height = dims.second;
    }

    if (width == 0 || height == 0 || (width * height * bytes_per_pixel < combined_data_for_image.size())) {
        if (gDebugMode.load(std::memory_order_relaxed)) {
            std::ostringstream oss;
            oss << "Failed to calculate valid dimensions for chunk " << chunk_index
                    << ". Data size: " << combined_data_for_image.size();
            printError(oss.str()); // Using printError for this diagnostic error
        }
        if (gDebugMode.load(std::memory_order_relaxed)) {
            printDebug("---- END CHUNK PROCESSING: " + std::to_string(chunk_index) + " (Early Exit: invalid dimensions calculated) ----");
        }
        return; // Skips creating an image for this chunk
    }

    // Create BitmapImage
    // This allocation (width * height * 3) can be significant.
    // ResourceManager could track this if BitmapImage was adapted or if we allocated buffer externally.
    BitmapImage bmp(static_cast<int>(width), static_cast<int>(height));
    bmp.setData(combined_data_for_image, 0); // Embed data (header + chunk data)

    // Generate output filename for this chunk
    std::string output_filename = output_base_path + "_" + std::to_string(chunk_index + 1) + "of" +
                                  std::to_string(total_chunks) + ".bmp";

    // Push task to writer queue
    image_task_queue.push({output_filename, std::move(bmp)});

    printMessage("Chunk " + std::to_string(chunk_index + 1) + "/" + std::to_string(total_chunks) +
                 " processed. Output: " + output_filename + " (Data size: " +
                 std::to_string(current_chunk_actual_data_length) + ", Header: " + std::to_string(
                     header_data.size()) + ")");

    if (gDebugMode.load(std::memory_order_relaxed)) {
        printDebug("---- END CHUNK PROCESSING: " + std::to_string(chunk_index) + " (Successfully Enqueued) ----");
    }
}

/**
 * @brief Saves a `BitmapImage` (encapsulated within an `ImageTaskInternal`) to disk as a BMP file.
 *
 * This function is typically called by the dedicated image writer thread (`imageWriterThread`).
 * It receives an `ImageTaskInternal` object, which contains both the `BitmapImage` data to be saved
 * and the target output filename. The function then invokes the `save` method of the `BitmapImage` class,
 * which handles the low-level details of writing the BMP headers and pixel data to the specified file.
 * Includes basic error handling for the save operation and logs success or failure.
 *
 * @param task A constant reference to the `ImageTaskInternal` object. This object contains the 
 *             `BitmapImage` (which itself holds pixel data and dimensions) and the `filename` 
 *             where the image should be saved.
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
 * @brief Function executed by the dedicated image writer thread.
 *
 * This thread continuously monitors the `task_queue` for `ImageTaskInternal` objects. 
 * When a task becomes available (i.e., a processed chunk is ready to be saved as an image),
 * this thread dequeues it and calls the `saveImage` function to write the corresponding BMP image to disk.
 * The thread continues to run and process tasks until the `should_terminate` flag is set to `true` 
 * AND the `task_queue` becomes empty, ensuring all pending images are saved before exiting.
 * This design decouples image processing (which can be CPU-bound) from image saving (which is I/O-bound),
 * potentially improving overall application throughput and responsiveness.
 *
 * @param task_queue A reference to the thread-safe queue (`ThreadSafeQueueTemplate<ImageTaskInternal>`) 
 *                   from which image saving tasks are retrieved.
 * @param should_terminate An atomic boolean flag. When the main processing logic decides that no more tasks 
 *                         will be added and work is complete, it sets this flag to `true`. The writer thread 
 *                         will then finish processing any remaining items in the queue and subsequently terminate.
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
 * @brief Main function to orchestrate the conversion of an input file into a series of BMP images.
 *
 * This function manages the entire file-to-image conversion process. Its responsibilities include:
 * 1. Validating the input file and parameters.
 * 2. Opening and memory-mapping the input file for efficient random access to its data.
 * 3. Calculating the number of chunks the input file will be divided into, based on `maxChunkSizeMB` and the total file size.
 * 4. Initializing a `ResourceManager` to manage and limit system resources like threads and memory used by the application.
 * 5. Setting up a thread pool (via `ResourceManager`) for concurrent processing of chunks and a dedicated image writer thread.
 * 6. Iterating through the calculated chunks of the input file:
 *    - For each chunk, it prepares parameters and dispatches a `processChunk` task to a worker thread from the pool.
 *    - `processChunk` will read data, create metadata, determine image dimensions, build the `BitmapImage`, and queue it.
 * 7. Waiting for all chunk processing tasks and image writing tasks to complete.
 * 8. Performing cleanup operations, such as unmapping the input file and ensuring all threads are properly joined.
 *
 * The output consists of one or more BMP images. Each image contains a segment of the original file's data, 
 * along with embedded metadata necessary for potential reassembly of the original file from these images.
 *
 * @param input_file The path to the binary file that needs to be converted into BMP images.
 * @param output_base The base name (and potentially path) for the output BMP files. A chunk index and total chunk count 
 *                    (e.g., "_1ofN.bmp") will be appended to this base to create unique filenames for each image chunk.
 * @param maxChunkSizeMB The maximum desired size for each data chunk (extracted from the original file) in megabytes. 
 *                       The actual BMP image file size might be larger due to BMP overhead (headers, padding) and embedded metadata.
 * @param maxThreads The maximum number of worker threads to use for processing file chunks concurrently. 
 *                   If non-positive, a default (often based on hardware concurrency) might be used.
 * @param maxMemoryMB The maximum estimated memory usage for the entire application in megabytes. This is used by the 
 *                    `ResourceManager` to throttle operations if memory limits are approached.
 * @param newMaxImageSizeMB The maximum size for each output BMP image file in megabytes. This overrides the default value.
 *                       If non-positive, a default of 100MB is used.
 * @return `true` if the conversion process completes successfully and all chunks are processed and saved. 
 *         `false` otherwise (e.g., if the input file is not found, memory allocation errors occur, or other critical failures).
 */
bool parseToImage(const std::string &input_file, const std::string &output_base, int maxChunkSizeMB, int maxThreads,
                  int maxMemoryMB, int newMaxImageSizeMB) {
    if (gDebugMode.load(std::memory_order_relaxed)) {
        printDebug("parseToImage started. Input: " + input_file + ", Output Base: " + output_base +
                   ", MaxChunkSizeMB: " + std::to_string(maxChunkSizeMB) + ", MaxThreads: " + std::to_string(maxThreads)
                   + ", MaxMemoryMB: " + std::to_string(maxMemoryMB) + 
                   ", MaxImageSizeMB: " + std::to_string(newMaxImageSizeMB));
    }

    // --- ResourceManager Setup (Singleton) ---
    auto &resManager = ResourceManager::getInstance();
    if (maxThreads > 0) {
        resManager.setMaxThreads(maxThreads);
    } else {
        auto default_threads = std::max(1u, std::thread::hardware_concurrency() / 2);
        resManager.setMaxThreads(default_threads);
    }
    if (maxMemoryMB >= 64) {
        resManager.setMaxMemory(static_cast<size_t>(maxMemoryMB) * 1024 * 1024);
    } else {
        constexpr size_t DEFAULT_MEMORY_LIMIT = 1024 * 1024 * 1024; // 1GB
        resManager.setMaxMemory(DEFAULT_MEMORY_LIMIT);
    }
    if (gDebugMode.load(std::memory_order_relaxed)) {
        printDebug(
            "ResourceManager configured. Max Threads: " + std::to_string(resManager.getMaxThreads()) + ", Max Memory: "
            + std::to_string(resManager.getMaxMemory() / (1024 * 1024)) + " MB");
    }
    // --- End ResourceManager Setup ---

    MemoryMappedFile fileToMap;
    if (!fileToMap.open(input_file)) {
        printError("Failed to open or map input file: " + input_file);
        return false;
    }
    if (fileToMap.getSize() == 0) {
        printWarning("Input file is empty: " + input_file + ". Nothing to process.");
        fileToMap.close();
        return true; // Technically successful, as an empty file results in no images.
    }

    size_t file_size = fileToMap.getSize();
    const unsigned char *all_file_data_ptr = fileToMap.getData();
    if (!all_file_data_ptr) {
        printError("Failed to get data pointer from memory mapped file: " + input_file);
        fileToMap.close();
        return false;
    }

    // New: maxChunkSizeMB is the target size for the *output BMP file*.
    if (maxChunkSizeMB <= 0) {
        printWarning(
            "Invalid maxChunkSizeMB specified (" + std::to_string(maxChunkSizeMB) +
            "). Using default of 9MB for target BMP size.");
        maxChunkSizeMB = 9;
    }
    size_t target_output_bmp_file_size_bytes = static_cast<size_t>(maxChunkSizeMB) * 1024 * 1024;

    const size_t BMP_FILE_AND_INFO_HEADER_SIZE = 14 + 40; // Standard BMP headers
    // Estimate internal metadata overhead (chunk_idx_str, total_chunks_str, filesize_str, original_filename_str, plus null terminators, separators etc.)
    // Increased from +50 to +75 for more robust estimation of numeric strings, separators, and potential EOF markers.
    size_t estimated_internal_metadata_overhead = input_file.length() + 75;

    if (target_output_bmp_file_size_bytes <= (BMP_FILE_AND_INFO_HEADER_SIZE + estimated_internal_metadata_overhead +
                                              1024)) {
        // + 1KB for minimal data
        printError("Target BMP file size (" + std::to_string(maxChunkSizeMB) +
                   "MB) is too small to hold BMP headers, internal metadata, and a reasonable amount of data. Minimum practical size is larger.");
        printError("Required for headers and estimated metadata approx: " +
                   std::to_string(BMP_FILE_AND_INFO_HEADER_SIZE + estimated_internal_metadata_overhead) + " bytes.");
        fileToMap.close();
        return false;
    }

    // This is the space available in the BMP after its own headers, for our combined payload (internal metadata + raw file data) AND BMP row padding.
    size_t available_area_for_payload_and_padding = target_output_bmp_file_size_bytes - BMP_FILE_AND_INFO_HEADER_SIZE;

    // Estimate that BMP row padding will be a small fraction of the data it accompanies.
    // Let D_combined = internal metadata + raw file data. Let P_bmp_padding = BMP row padding.
    // We need D_combined + P_bmp_padding <= available_area_for_payload_and_padding.
    // Assume P_bmp_padding = k * D_combined (e.g., padding is 0.5% of D_combined, so k=0.005).
    // Then D_combined * (1+k) <= available_area_for_payload_and_padding.
    // So, D_combined <= available_area_for_payload_and_padding / (1+k).
    // This D_combined is the target size for (actual internal metadata + raw file data for the chunk).
    const double padding_allowance_factor = 0.005; // Reserve 0.5% of the combined payload space for BMP row padding.
    size_t estimated_total_combined_payload_capacity = static_cast<size_t>(
        static_cast<double>(available_area_for_payload_and_padding) / (1.0 + padding_allowance_factor)
    );

    if (estimated_total_combined_payload_capacity <= estimated_internal_metadata_overhead) {
        printError("Target BMP file size (" + std::to_string(maxChunkSizeMB) +
                   "MB) is too small. After reserving space for BMP headers and estimated BMP row padding, the remaining space ("
                   +
                   std::to_string(estimated_total_combined_payload_capacity) +
                   " bytes) is not enough to even hold the estimated internal metadata (" +
                   std::to_string(estimated_internal_metadata_overhead) + " bytes).");
        fileToMap.close();
        return false;
    }

    size_t estimated_raw_data_payload_capacity_per_chunk =
            estimated_total_combined_payload_capacity - estimated_internal_metadata_overhead;

    if (estimated_raw_data_payload_capacity_per_chunk == 0) {
        printError(
            "Calculated raw data payload capacity per chunk is 0. Target BMP size might be too small or metadata/padding estimates too large.");
        fileToMap.close();
        return false;
    }

    size_t num_chunks = (file_size + estimated_raw_data_payload_capacity_per_chunk - 1) /
                        estimated_raw_data_payload_capacity_per_chunk;
    if (num_chunks == 0 && file_size > 0) num_chunks = 1; // Ensure at least one chunk if there's data

    if (gDebugMode.load(std::memory_order_relaxed)) {
        printDebug(
            "Target output BMP file size per chunk: " + std::to_string(target_output_bmp_file_size_bytes) + " bytes (" +
            std::to_string(maxChunkSizeMB) + " MB).");
        printDebug(
            "Estimated internal metadata overhead per chunk: " + std::to_string(estimated_internal_metadata_overhead) +
            " bytes.");
        printDebug(
            "Available area in BMP for (payload + padding): " + std::to_string(available_area_for_payload_and_padding) +
            " bytes.");
        printDebug(
            "Estimated total combined payload (internal metadata + raw data) capacity after padding allowance: " +
            std::to_string(estimated_total_combined_payload_capacity) + " bytes.");
        printDebug(
            "Estimated raw data payload capacity per chunk (after internal metadata estimate): " + std::to_string(
                estimated_raw_data_payload_capacity_per_chunk) + " bytes.");
        printDebug(
            "Total file size: " + std::to_string(file_size) + " bytes. Number of chunks: " + std::to_string(
                num_chunks));
    }

    std::string output_dir_str = std::filesystem::path(output_base).parent_path().string();
    std::string spill_path_str;
    if (!output_dir_str.empty()) {
        try {
            std::filesystem::path temp_spill_path = std::filesystem::path(output_dir_str) / ".spill_tasks";
            spill_path_str = temp_spill_path.string();
        } catch (const std::exception &e) {
            printWarning(
                "Could not form spill path from output_dir: " + output_dir_str + ". Error: " + e.what() +
                ". Spilling may be disabled or use CWD.");
            spill_path_str = ".spill_tasks"; // Fallback to CWD
        }
    } else {
        spill_path_str = ".spill_tasks"; // Default to CWD if output_base has no parent path
    }
    if (gDebugMode.load(std::memory_order_relaxed)) {
        printDebug("Spill path for image task queue: " + spill_path_str);
    }

    // Use 1000 for max_in_memory_items, matching ThreadSafeQueueTemplate's typical default
    ThreadSafeQueueTemplate<ImageTaskInternal> image_task_queue(1000, spill_path_str);
    std::atomic<bool> should_terminate_writer(false);
    std::atomic<size_t> processed_chunks_count(0); // To track completion
    std::thread writer_thread(imageWriterThread, std::ref(image_task_queue), std::ref(should_terminate_writer));

    for (size_t i = 0; i < num_chunks; ++i) {
        size_t current_chunk_offset = i * estimated_raw_data_payload_capacity_per_chunk;

        // Check 1: If the offset itself is already at or past the end of the file, stop.
        if (current_chunk_offset >= file_size) {
            if (gDebugMode.load(std::memory_order_relaxed)) {
                printDebug("Chunk Loop " + std::to_string(i) + ": offset " + std::to_string(current_chunk_offset) +
                           " is >= file_size " + std::to_string(file_size) + ". Ending chunk processing loop.");
            }
            break;
        }

        // Calculate data size for the current chunk
        // It's the minimum of the configured capacity or the remaining file data from the current offset.
        size_t remaining_file_data = file_size - current_chunk_offset; // Safe because current_chunk_offset < file_size
        size_t current_chunk_data_size = std::min(estimated_raw_data_payload_capacity_per_chunk, remaining_file_data);

        // Check 2: If the calculated data size for this chunk is zero, skip enqueueing a task.
        if (current_chunk_data_size == 0) {
            if (gDebugMode.load(std::memory_order_relaxed)) {
                printDebug("Chunk Loop " + std::to_string(i) + ": calculated current_chunk_data_size is 0. Offset: " +
                           std::to_string(current_chunk_offset) + ". Skipping task enqueue.");
            }
            continue;
        }

        // Assertion: current_chunk_offset + current_chunk_data_size should now always be <= file_size.
        // The redundant 'if (current_chunk_offset + current_chunk_data_size > file_size)' block was removed.

        if (gDebugMode.load(std::memory_order_relaxed)) {
            const unsigned char *data_start_ptr_for_log = all_file_data_ptr + current_chunk_offset;
            const unsigned char *data_end_ptr_one_past_last_for_log = data_start_ptr_for_log + current_chunk_data_size;

            // Use ostringstream to correctly convert pointer addresses to strings for logging
            std::ostringstream oss_start_ptr, oss_end_ptr;
            oss_start_ptr << static_cast<const void *>(data_start_ptr_for_log);
            oss_end_ptr << static_cast<const void *>(data_end_ptr_one_past_last_for_log);

            // Explicitly start with std::string
            std::string debug_msg = std::string("Enqueueing Task for Chunk ") + std::to_string(i) +
                                    std::string(": Offset=") + std::to_string(current_chunk_offset) +
                                    std::string(", SizeToRead=") + std::to_string(current_chunk_data_size) +
                                    std::string(", InputFileTotalSize=") + std::to_string(file_size) +
                                    std::string(", DataStartPtrToPass=") + oss_start_ptr.str() +
                                    std::string(", DataEndPtrToAccess (one past last)=") + oss_end_ptr.str();

            if (!all_file_data_ptr) {
                printError(
                    "CRITICAL_ASSERTION_FAIL_PRE_ENQUEUE: Chunk " + std::to_string(i) + ": all_file_data_ptr is null!");
            }
        }

        // Capture variables for the lambda
        int chunk_index_cap = i;
        size_t num_chunks_cap = num_chunks;
        size_t file_size_cap = file_size;
        // Capture the base pointer of the MMF, processChunk will calculate its own offsets.
        const unsigned char *base_mmf_ptr_for_lambda_cap = all_file_data_ptr;
        // Capture the uniform chunk payload capacity for offset calculation in processChunk.
        size_t uniform_chunk_payload_capacity_for_lambda_cap = estimated_raw_data_payload_capacity_per_chunk;

        // Capture input_file and output_base by value (copy) for safety in lambda
        const std::string& input_file_val_cap = input_file;
        const std::string& output_base_val_cap = output_base;
        // image_task_queue is assumed to be the original queue variable in parseToImage's scope
        size_t effective_max_image_size_bytes;
        if (newMaxImageSizeMB > 0) {
            effective_max_image_size_bytes = static_cast<size_t>(newMaxImageSizeMB) * 1024 * 1024;
        } else {
            effective_max_image_size_bytes = DEFAULT_MAX_IMAGE_SIZE;
        }

        if (gDebugMode.load(std::memory_order_relaxed)) {
            // Explicitly start with std::string
            std::string debug_msg = std::string("Enqueueing task for chunk: ") + std::to_string(chunk_index_cap) +
                                    std::string(", Offset: ") + std::to_string(current_chunk_offset) +
                                    std::string(", ActualDataSize: ") + std::to_string(current_chunk_data_size);
            printDebug(debug_msg);
        }

        // Assuming resManager and image_task_queue are available in this scope
        resManager.runWithThread([
                &image_task_queue, // Capture original queue by reference
                chunk_index_cap,
                num_chunks_cap,
                file_size_cap,
                base_mmf_ptr_for_lambda_cap,
                uniform_chunk_payload_capacity_for_lambda_cap,
                input_file_val_cap,
                output_base_val_cap,
                effective_max_image_size_bytes
            ]() {
                // This code runs in a worker thread
                processChunk(
                    chunk_index_cap,
                    uniform_chunk_payload_capacity_for_lambda_cap,
                    num_chunks_cap,
                    file_size_cap,
                    base_mmf_ptr_for_lambda_cap,
                    input_file_val_cap,
                    output_base_val_cap,
                    image_task_queue, // Pass the captured reference
                    effective_max_image_size_bytes
                );
            });

        processed_chunks_count++;
    }

    if (gDebugMode.load(std::memory_order_relaxed)) {
        printDebug("All chunk processing tasks submitted. Waiting for completion...");
    }

    // Wait for all processing tasks to complete via ResourceManager
    // This loop is a more robust way to wait if resManager.waitForAllThreads() isn't enough
    // or if we want to provide progress.
    // However, if resManager.waitForAllThreads() is blocking and correct, that's simpler.
    // Let's assume waitForAllThreads is sufficient and blocks until all runWithThread tasks are done.
    resManager.waitForAllThreads();

    if (gDebugMode.load(std::memory_order_relaxed)) {
        printDebug(
            "All chunks processed by threads. Processed count: " + std::to_string(processed_chunks_count.load()));
    }

    // Signal writer thread to terminate and wait for it
    should_terminate_writer = true;
    if (writer_thread.joinable()) {
        writer_thread.join();
    }

    fileToMap.close();

    printMessage("File processing complete.");
    return true;
}

/**
 * @brief Constructor for the `BitmapImage` class.
 *
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
 * @brief Sets binary data into the pixel buffer at the specified offset.
 *
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
 * @brief Saves the image to a BMP file.
 *
 * This function handles the low-level details of writing the image data
 * to a file in the BMP format.
 *
 * @param filename Path where the BMP file will be saved
 */
void BitmapImage::save(const std::string &filename) {
    std::string temp_filename = filename + ".tmp";

    std::ofstream file(temp_filename, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        printError("Failed to open temporary file for writing: " + temp_filename);
        return;
    }

    if (width <= 0 || height <= 0) {
        printError(
            "Invalid image dimensions for save: " + std::to_string(width) + "x" + std::to_string(height) + " for file "
            + temp_filename);
        file.close();
        try { std::filesystem::remove(temp_filename); } catch (const std::filesystem::filesystem_error &e) {
            printWarning(
                "BitmapImage::save: Failed to remove temp file (invalid dims): " + temp_filename + " Error: " + e.
                what());
        }
        return;
    }

    // BMP File Header
    BITMAPFILEHEADER bfh;
    bfh.bfType = 0x4d42; // 'BM'
    bfh.bfSize = 0; // Will be set later
    bfh.bfReserved1 = 0;
    bfh.bfReserved2 = 0;
    bfh.bfOffBits = 0; // Will be set later

    // BMP Info Header
    BITMAPINFOHEADER bih;
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = width;
    bih.biHeight = height;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = 0; // Will be set later
    bih.biXPelsPerMeter = 0;
    bih.biYPelsPerMeter = 0;
    bih.biClrUsed = 0;
    bih.biClrImportant = 0;

    // Calculate row padding and actual file size
    constexpr size_t bytes_per_pixel = 3; // For 24-bit BMP
    size_t data_row_size = static_cast<size_t>(width) * bytes_per_pixel;
    size_t padding = (4 - (data_row_size % 4)) % 4;
    size_t row_stride_in_file = data_row_size + padding; // Size of one row in the BMP file (data + padding)
    size_t actual_image_data_on_disk_size = static_cast<size_t>(height) * row_stride_in_file;
    size_t total_file_size = sizeof(bfh) + sizeof(bih) + actual_image_data_on_disk_size;

    // Set file size in the file header
    bfh.bfSize = static_cast<uint32_t>(total_file_size);
    bfh.bfOffBits = sizeof(bfh) + sizeof(bih);

    // Set image size in the info header (size of pixel data on disk including padding)
    bih.biSizeImage = static_cast<uint32_t>(actual_image_data_on_disk_size);

    // Write the file and info headers
    file.write(reinterpret_cast<const char *>(&bfh), sizeof(bfh));
    file.write(reinterpret_cast<const char *>(&bih), sizeof(bih));

    // BMPs store pixels bottom-to-top. Pixel data in `pixels` is assumed to be compact (no padding within it).
    // The original data source (e.g. file chunk) is raw bytes. For 24-bit BMP, we interpret 3 bytes as one pixel.
    // BMP format requires BGR order. We assume `pixels` holds R,G,B order and swap to B,G,R during write.

    unsigned char pixel_output_buffer[bytes_per_pixel]; // To hold B,G,R for one pixel
    unsigned char row_padding_buffer[3] = {0, 0, 0}; // Max padding is 3 bytes of zeros

    for (int y_bmp = height - 1; y_bmp >= 0; --y_bmp) {
        // Iterate image rows from bottom to top for BMP file structure
        for (int x_pixel = 0; x_pixel < width; ++x_pixel) {
            // Iterate pixels within a row
            // Calculate index for the current pixel in the source `pixels` array.
            // `pixels` is organized top-to-bottom, left-to-right from the source data.
            // For BMP bottom-to-top, y_bmp maps to source row y_img = y_bmp (if pixels were also bottom-up)
            // OR y_img = (height - 1 - y_bmp) if pixels are top-down.
            // Let's assume pixels is 1D array representing a 2D image scanline by scanline (top-to-bottom).
            // The y_bmp is the row index *in the output file* (bottom-up).
            // So, the source data row is y_bmp if pixels are stored bottom-up, or (height-1-y_bmp) if top-down.
            // The current `BitmapImage::setData` is just `std::copy(data.begin(), data.end(), pixels.begin() + offset);`
            // This means pixels is a flat buffer. The original code's `pixels[(y * width + x) * 3]` implied `y` was image row index.
            // Let's stick to the original interpretation: y is image row (0 to height-1), x is pixel column (0 to width-1).
            // So, for BMP row `y_bmp` (from height-1 down to 0), we're writing data from `pixels` that corresponds to image row `y_bmp`.
            size_t source_pixel_offset = (static_cast<size_t>(y_bmp) * width + x_pixel) * bytes_per_pixel;

            // Check bounds to prevent reading past the end of pixels, though resize should handle it.
            if (source_pixel_offset + bytes_per_pixel > pixels.size()) {
                // This should ideally not happen if resize was correct.
                // Fill with black if out of bounds.
                pixel_output_buffer[0] = 0; // B
                pixel_output_buffer[1] = 0; // G
                pixel_output_buffer[2] = 0; // R
            } else {
                // Assuming pixels has R,G,B order for each pixel from source data.
                // Swap to B,G,R for BMP file.
                pixel_output_buffer[0] = pixels[source_pixel_offset + 2]; // Blue component
                pixel_output_buffer[1] = pixels[source_pixel_offset + 1]; // Green component
                pixel_output_buffer[2] = pixels[source_pixel_offset + 0]; // Red component
            }
            file.write(reinterpret_cast<const char *>(pixel_output_buffer), bytes_per_pixel);
        }
        // After writing all pixels for a row, write padding if any.
        if (padding > 0) {
            file.write(reinterpret_cast<const char *>(row_padding_buffer), padding);
        }
    }

    if (!file.good()) {
        printError("Error occurred while writing pixel data to temporary file: " + temp_filename);
        file.close();
        try { std::filesystem::remove(temp_filename); } catch (const std::filesystem::filesystem_error &e) {
            printWarning(
                "BitmapImage::save: Failed to remove temp file (write error): " + temp_filename + " Error: " + e.
                what());
        }
        return;
    }

    file.close();
    if (!file.good()) {
        // Check state after closing
        printError("Error occurred after closing temporary file: " + temp_filename);
        try { std::filesystem::remove(temp_filename); } catch (const std::filesystem::filesystem_error &e) {
            printWarning(
                "BitmapImage::save: Failed to remove temp file (close error): " + temp_filename + " Error: " + e.
                what());
        }
        return;
    }

    // Attempt to rename the temporary file to the final filename
    try {
        // Remove target file if it exists, to prevent rename error on some systems
        if (std::filesystem::exists(filename)) {
            std::filesystem::remove(filename);
        }
        std::filesystem::rename(temp_filename, filename);
    } catch (const std::filesystem::filesystem_error &e) {
        printError("Failed to rename temporary file " + temp_filename + " to " + filename + ". Error: " + e.what());
        // Try to clean up the temporary file if rename fails
        try {
            if (std::filesystem::exists(temp_filename)) {
                std::filesystem::remove(temp_filename);
            }
        } catch (const std::filesystem::filesystem_error &e_remove) {
            printWarning(
                "BitmapImage::save: Failed to remove temp file after rename error: " + temp_filename + ". Error: " +
                e_remove.what());
        }
    }
}