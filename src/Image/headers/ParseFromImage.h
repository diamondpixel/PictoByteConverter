#ifndef PARSE_FROM_IMAGE_H
#define PARSE_FROM_IMAGE_H

#include <string>
#include <fstream>
#include <vector>
#include <mutex>
#include <optional>

/**
 * Extracts binary data from one or more image files.
 * Can process either a single image or a pattern matching multiple images
 * that make up parts of a larger file.
 *
 * @param imageFilePath Path to the image file(s). Can be a specific file or a pattern.
 * @param outputPath Optional path where the extracted file should be saved.
 * @param maxThreads Maximum number of threads to use (0 for default)
 * @param maxMemoryMB Maximum memory to use in MB (0 for default)
 */
void parseFromImage(const std::string& imageFilePath, const std::string& outputPath = "", int maxThreads = 0, int maxMemoryMB = 0);

/**
 * Get the size of the specified file in bytes
 *
 * @param filePath Path to the file to check
 * @return Size of the file in bytes
 */
std::ifstream::pos_type fileSize(const char* filePath);

// Internal functions in image_parser namespace (not part of public API)
namespace image_parser {
    /**
     * Structure to hold metadata extracted from image file header
     */
    struct ImageMetadata {
        std::string outputFilename;
        size_t expectedDataSize;
        std::string indexInfo;
        int currentChunk;
        int totalChunks;
    };

    /**
    * Structure to hold file chunk information
    */
    struct ChunkInfo {
        int chunkIndex = -1;
        int totalChunks = -1;
        size_t chunkSize = 0;
        size_t expectedDataSize = 0;
        std::string filename;
        std::vector<unsigned char> payload;
    };

    /**
     * Process a single image file to extract the encoded binary data
     *
     * @param filePath Path to the image file
     * @param appendMode If true, will append to existing output file instead of overwriting
     * @param outputMutex Mutex for synchronizing file output operations
     * @param printMutex Mutex for synchronizing console output
     * @return True if processing was successful
     */
    bool processImageFile(const std::string &filePath, bool appendMode,
                          std::mutex &outputMutex, std::mutex &printMutex);

    /**
     * Find sub-bmp files when the main file isn't found
     *
     * @param baseFilename The base filename (without extension)
     * @return Vector of matching sub-bmp files sorted in proper order
     */
    std::vector<std::string> findSubBmpFiles(const std::string &baseFilename);

    /**
     * Get all files to process based on input pattern or filename
     *
     * @param filePattern Path to the image file, or pattern for multiple files
     * @return Vector of files to process
     */
    std::vector<std::string> getFilesToProcess(const std::string &filePattern);

    /**
     * Extract payload data from a single image file
     *
     * @param filename Path to the image file
     * @param print_mutex Mutex for synchronizing console output
     * @param debug_mode Explicitly pass debug mode state to ensure thread visibility
     * @return ChunkInfo structure with the extracted payload and metadata
     */
    std::optional<ChunkInfo> extractChunkPayload(
        const std::string &filename,
        std::mutex &print_mutex,
        bool debug_mode = false);
}

#endif // PARSE_FROM_IMAGE_H
