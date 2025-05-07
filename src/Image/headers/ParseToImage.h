#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <mutex>
#include <latch>
#include <chrono>
#include <future>
#include "../../Threading/headers/ThreadSafeQueue.h"
#include "Bitmap.hpp"
#include "ImageTask.h"
#include "SpecialVector.h"

/**
 * Calculate optimal dimensions for an image based on data size and constraints
 * @param dataBytes Available data bytes per image
 * @param bytesPerPixel Number of bytes per pixel
 * @param bmpHeaderSize Size of BMP header in bytes
 * @param maxSizeBytes Maximum allowed image size in bytes
 * @return Optimal dimension (width/height) for a square image
 */
size_t calculateOptimalDimensions(size_t dataBytes, size_t bytesPerPixel,
                                size_t bmpHeaderSize, size_t maxSizeBytes);

/**
 * Optimize dimensions for the last image if it's not full
 * @param actualSize Actual size of data for this image
 * @param headerSize Size of the metadata header in bytes
 * @param bytesPerPixel Number of bytes per pixel
 * @param bmpHeaderSize Size of BMP header in bytes
 * @param maxSizeBytes Maximum allowed image size in bytes
 * @return Optimized dimension or 0 if optimization not possible
 */
size_t optimizeLastImageDimensions(size_t actualSize, size_t headerSize,
                                  size_t bytesPerPixel, size_t bmpHeaderSize,
                                  size_t maxSizeBytes);

/**
 * Read a segment of data from the input file
 * @param file Input file stream
 * @param position Starting position in the file
 * @param bytesToRead Number of bytes to read
 * @return Vector containing the read data
 */
std::vector<unsigned char> readFileSegment(std::ifstream& file, size_t position, size_t bytesToRead);

/**
 * Create header data for the image
 * @param imageData Vector to store the header data
 * @param fileName Name of the original file
 * @param actualSize Actual size of data stored in this image
 * @param imgIndex Index of this image
 * @param totalImages Total number of images
 */
void createImageHeader(VectorWrapper<unsigned char>& imageData, const std::string& fileName,
                      size_t actualSize, size_t imgIndex, size_t totalImages);

/**
 * Process a single image task
 * @param task The image task to process
 * @param file Input file stream
 * @param bytesPerPixel Number of bytes per pixel
 * @param bmpHeaderSize Size of BMP header in bytes
 * @param maxSizeBytes Maximum allowed image size in bytes
 * @param headerSize Size of the metadata header in bytes
 */
void processImageTask(const ImageTask& task, std::ifstream& file, size_t bytesPerPixel,
                     size_t bmpHeaderSize, size_t maxSizeBytes, size_t headerSize);

/**
 * Worker thread function for processing image tasks
 * @param tasks Queue of image tasks to process
 * @param file Input file stream
 * @param bytesPerPixel Number of bytes per pixel
 * @param bmpHeaderSize Size of BMP header in bytes
 * @param maxSizeBytes Maximum allowed image size in bytes
 * @param headerSize Size of the metadata header in bytes
 */
void workerThread(ThreadSafeQueue& tasks, std::ifstream& file, size_t bytesPerPixel,
                 size_t bmpHeaderSize, size_t maxSizeBytes, size_t headerSize);

/**
 * Converts a binary file into one or more BMP images using multithreading
 * The file data is encoded into the RGB values of each pixel
 * Metadata is stored in the header portion of the pixel data
 * Multiple threads are used to process images in parallel
 *
 * @param inputFilePath Path to the input file to be converted
 * @param outputBaseName Base name for the output image file(s)
 * @param maxChunkSizeMB Maximum size in megabytes for each chunk/image
 * @param maxThreads Maximum number of threads to use (0 for default)
 * @param maxMemoryMB Maximum memory to use in MB (0 for default)
 * @return True if conversion was successful, false otherwise
 */
bool parseToImage(const std::string& inputFilePath, const std::string& outputBaseName, int maxChunkSizeMB = 9, int maxThreads = 0, int maxMemoryMB = 0);