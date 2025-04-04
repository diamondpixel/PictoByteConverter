#ifndef IMAGE_TASK_H
#define IMAGE_TASK_H

#include <string>
#include <fstream>

// Structure to hold information for image processing tasks
struct ImageTask {
    size_t img_index; // Index of this image
    size_t total_images; // Total number of images
    size_t bytes_capacity; // Total bytes capacity per image
    size_t bytes_to_read; // Bytes to read for this particular image
    size_t width; // Image width
    size_t height; // Image height
    std::streampos file_position; // Position in file to read from
    std::string base_outfile; // Base output filename
    std::string extension; // File extension
    std::string filename; // Original input filename
};

#endif // IMAGE_TASK_H
