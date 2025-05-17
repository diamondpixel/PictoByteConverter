/**
 * @file PictoByteConverter.h
 * @brief Public API for the PictoByteConverter library
 * 
 * This header provides the public interface for using PictoByteConverter as a library.
 * Applications can include this header and link against the pictobyte shared library
 * to integrate file-to-image and image-to-file conversion functionality.
 */

#ifndef PICTOBYTE_CONVERTER_H
#define PICTOBYTE_CONVERTER_H

#include <string>
#include <vector>
#include <filesystem>

#ifdef _WIN32
    #ifdef PICTOBYTE_EXPORTS
        #define PICTOBYTE_API __declspec(dllexport)
    #else
        #define PICTOBYTE_API __declspec(dllimport)
    #endif
#else
    #define PICTOBYTE_API
#endif

namespace PictoByteConverter {

/**
 * @brief Convert a file to one or more BMP images
 * 
 * @param inputFilePath Path to the input file to convert
 * @param outputFilePath Path to the output BMP file (base name)
 * @param maxThreads Maximum number of CPU threads to use (0 = auto)
 * @param maxMemoryMB Maximum memory usage in MB
 * @param maxChunkSizeMB Maximum chunk size in MB
 * @param debugMode Enable debug output
 * @param newMaxImageSizeMB Maximum BMP image size in MB (0 = default 100MB)
 * @return bool True if conversion was successful, false otherwise
 */
PICTOBYTE_API bool FileToImage(
    const std::string& inputFilePath,
    const std::string& outputFilePath,
    int maxThreads = 0,
    int maxMemoryMB = 1024,
    int maxChunkSizeMB = 9,
    bool debugMode = false,
    int newMaxImageSizeMB = 0
);

/**
 * @brief Extract original file from one or more BMP images
 * 
 * @param inputFilePath Path to the input BMP image (if multi-part, specify first chunk)
 * @param outputDirectory Directory where the extracted file should be saved
 * @param maxThreads Maximum number of CPU threads to use (0 = auto)
 * @param maxMemoryMB Maximum memory usage in MB
 * @param debugMode Enable debug output
 * @return bool True if extraction was successful, false otherwise
 */
PICTOBYTE_API bool ImageToFile(
    const std::string& inputFilePath,
    const std::string& outputDirectory,
    int maxThreads = 0,
    int maxMemoryMB = 1024,
    bool debugMode = false
);

/**
 * @brief Set the logging callback function for the library
 * 
 * @param callback Function pointer to a logging callback function
 */
PICTOBYTE_API void SetLogCallback(void (*callback)(const char* message));

/**
 * @brief Get the version string of the PictoByteConverter library
 * 
 * @return const char* Version string in the format "Major.Minor.Patch"
 */
PICTOBYTE_API const char* GetLibraryVersion();

} // namespace PictoByteConverter

#endif // PICTOBYTE_CONVERTER_H
