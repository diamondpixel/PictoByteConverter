#ifndef PARSE_FROM_IMAGE_H
#define PARSE_FROM_IMAGE_H

#include <string>
#include <fstream>
#include <vector>
#include <mutex>

/**
 * Get the size of a file in bytes.
 *
 * @param filename Path to the file
 * @return Size of the file in bytes
 */
std::ifstream::pos_type filesize(const char *filename);

/**
 * Parse data from an image created by parseToImage.
 * This function can handle both single images and multi-part image sets.
 * When given any image from a multi-part set, it will automatically locate and process all parts.
 *
 * The image header is expected to have the following format:
 *   <output filename>#<10-digit data size>#<index info>#
 * where the index info is in the format "XXXX-YYYY" for multi-part images.
 *
 * @param filename Path to the image file (any part if split across multiple images)
 */
void parseFromImage(const std::string &filename);

// Internal functions in image_parser namespace (not part of public API)
namespace image_parser {
    /**
     * Structure to hold metadata extracted from image file header
     */
    struct ImageMetadata {
        std::string output_filename;
        size_t expected_data_size;
        std::string index_info;
        int current_chunk;
        int total_chunks;
    };

    /**
    * Structure to hold file chunk information
    */
    struct ChunkInfo {
        int chunk_index;
        int total_chunks;
        size_t chunk_size;
        std::vector<unsigned char> payload;
        std::string filename;
    };

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
                          std::mutex &output_mutex, std::mutex &print_mutex);

    /**
     * Find sub-bmp files when the main file isn't found
     *
     * @param base_filename The base filename (without extension)
     * @return Vector of matching sub-bmp files sorted in proper order
     */
    std::vector<std::string> findSubBmpFiles(const std::string &base_filename);

    /**
     * Get all files to process based on input pattern or filename
     *
     * @param filename Path to the image file, or pattern for multiple files
     * @return Vector of files to process
     */
    std::vector<std::string> getFilesToProcess(const std::string &filename);
}

#endif // PARSE_FROM_IMAGE_H
