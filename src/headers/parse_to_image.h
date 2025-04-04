#ifndef PARSE_TO_IMAGE_H
#define PARSE_TO_IMAGE_H

#include <string>
#include <vector>
#include <fstream>
#include "image_task.h"
#include "thread_safe_queue.h"
#include "specialvector.h"

/**
 * Print a message to console in a thread-safe manner
 * @param message The message to print
 */
void printMessage(const std::string& message);

/**
 * Calculate optimal dimensions for an image based on data size and constraints
 * @param data_bytes Available data bytes per image
 * @param bytes_per_pixel Number of bytes per pixel
 * @param bmp_header_size Size of BMP header in bytes
 * @param max_size_bytes Maximum allowed image size in bytes
 * @return Optimal dimension (width/height) for a square image
 */
size_t calculateOptimalDimensions(size_t data_bytes, size_t bytes_per_pixel,
                                size_t bmp_header_size, size_t max_size_bytes);

/**
 * Optimize dimensions for the last image if it's not full
 * @param actual_size Actual size of data for this image
 * @param header_size Size of the metadata header in bytes
 * @param bytes_per_pixel Number of bytes per pixel
 * @param bmp_header_size Size of BMP header in bytes
 * @param max_size_bytes Maximum allowed image size in bytes
 * @return Optimized dimension or 0 if optimization not possible
 */
size_t optimizeLastImageDimensions(size_t actual_size, size_t header_size,
                                  size_t bytes_per_pixel, size_t bmp_header_size,
                                  size_t max_size_bytes);

/**
 * Read a segment of data from the input file
 * @param file Input file stream
 * @param position Starting position in the file
 * @param bytes_to_read Number of bytes to read
 * @return Vector containing the read data
 */
std::vector<unsigned char> readFileSegment(std::ifstream& file, size_t position, size_t bytes_to_read);

/**
 * Create header data for the image
 * @param image_data Vector to store the header data
 * @param filename Name of the original file
 * @param actual_size Actual size of data stored in this image
 * @param img_index Index of this image
 * @param total_images Total number of images
 */
void createImageHeader(VectorWrapper<unsigned char>& image_data, const std::string& filename,
                      size_t actual_size, size_t img_index, size_t total_images);

/**
 * Process a single image task
 * @param task The image task to process
 * @param file Input file stream
 * @param bytes_per_pixel Number of bytes per pixel
 * @param bmp_header_size Size of BMP header in bytes
 * @param max_size_bytes Maximum allowed image size in bytes
 * @param header_size Size of the metadata header in bytes
 */
void processImageTask(const ImageTask& task, std::ifstream& file, size_t bytes_per_pixel,
                     size_t bmp_header_size, size_t max_size_bytes, size_t header_size);

/**
 * Worker thread function for processing image tasks
 * @param tasks Queue of image tasks to process
 * @param file Input file stream
 * @param bytes_per_pixel Number of bytes per pixel
 * @param bmp_header_size Size of BMP header in bytes
 * @param max_size_bytes Maximum allowed image size in bytes
 * @param header_size Size of the metadata header in bytes
 */
void workerThread(ThreadSafeQueue& tasks, std::ifstream& file, size_t bytes_per_pixel,
                 size_t bmp_header_size, size_t max_size_bytes, size_t header_size);

/**
 * Converts a binary file into one or more BMP images using multithreading
 * The file data is encoded into the RGB values of each pixel
 * Metadata is stored in the header portion of the pixel data
 * Multiple threads are used to process images in parallel
 *
 * @param filename Path to the input file to be converted
 * @param outfile Base name for the output image file(s)
 * @param max_size_mb Maximum size of each output image in megabytes
 * @param aspect_ratio Desired width-to-height ratio of output images (default: 1.0 for square images)
 */
void parseToImage(const std::string& filename, const std::string& outfile, size_t max_size_mb = 100, float aspect_ratio = 1.0f);

#endif // PARSE_TO_IMAGE_H