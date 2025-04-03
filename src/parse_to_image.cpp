#include <string>
#include <fstream>
#include <cmath>
#include <iostream>
#include <vector>
#include <filesystem>
#include "headers/specialvector.h"
#include "headers/bitmap.hpp"
#include "headers/parse_to_image.h"

void parseToImage(const std::string& filename, const std::string& outfile, size_t max_size_mb) {
    // Open file in binary mode
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        std::cerr << "Error: Could not open file " << filename << std::endl;
        return;
    }

    // Determine file size
    file.seekg(0, std::ios::end);
    std::streamsize file_size_stream = file.tellg();
    if (file_size_stream < 0) {
        std::cerr << "Error: Could not determine file size" << std::endl;
        return;
    }
    auto file_size = static_cast<size_t>(file_size_stream);
    file.seekg(0, std::ios::beg);

    std::cout << "File size: " << file_size << " bytes (" << (file_size / (1024.0 * 1024.0)) << " MB)" << std::endl;

    // Calculate max size in bytes - exact limit
    size_t max_size_bytes = max_size_mb * 1024 * 1024;
    
    // BMP file header and info header size (typically 14 + 40 = 54 bytes)
    constexpr size_t bmp_header_size = 54;
    
    // Calculate our metadata header size
    size_t header_size = filename.size() + 1 + 10 + 1 + 10 + 1; // filename + # + size + # + index/count + #
    
    // Calculate the actual maximum data we can store per image
    // Each pixel is 3 bytes (RGB) and each row has padding to make it a multiple of 4 bytes
    size_t data_bytes_per_image = max_size_bytes - bmp_header_size - header_size;
    
    // Each pixel holds 3 bytes (RGB)
    size_t bytes_per_pixel = 3;
    
    // Calculate optimal dimensions that don't waste space
    auto optimal_dim = static_cast<size_t>(std::sqrt(static_cast<double>(data_bytes_per_image / bytes_per_pixel)));
    
    // Limit dimensions to reasonable values
    if (optimal_dim > 65535) {
        optimal_dim = 65535;
    }
    
    // Calculate row padding (BMP rows must be aligned to 4 bytes)
    size_t row_bytes = optimal_dim * bytes_per_pixel;
    size_t padding = (4 - (row_bytes % 4)) % 4;
    
    // Calculate actual bytes capacity including padding
    size_t actual_image_size = bmp_header_size + (optimal_dim * (optimal_dim * bytes_per_pixel + padding));
    
    // If we're exceeding the limit, reduce dimensions
    while (actual_image_size > max_size_bytes) {
        optimal_dim--;
        row_bytes = optimal_dim * bytes_per_pixel;
        padding = (4 - (row_bytes % 4)) % 4;
        actual_image_size = bmp_header_size + (optimal_dim * (optimal_dim * bytes_per_pixel + padding));
    }
    
    // Recalculate with final dimensions
    size_t pixels_capacity = optimal_dim * optimal_dim;
    size_t bytes_capacity = pixels_capacity * bytes_per_pixel;
    
    // Calculate number of images needed based on file size
    size_t total_images = (file_size + bytes_capacity - 1) / bytes_capacity;
    if (total_images == 0) total_images = 1;  // At least one image even for empty files
    
    std::cout << "Will create " << total_images << " image(s) of size " << optimal_dim << "x" << optimal_dim << std::endl;
    std::cout << "Each image size will be approximately " << (actual_image_size / (1024.0 * 1024.0)) << " MB" << std::endl;
    std::cout << "Each image can hold approximately " << (bytes_capacity / (1024.0 * 1024.0)) << " MB of data" << std::endl;

    // Read and process the file in chunks corresponding to each image
    std::string base_outfile = outfile;
    std::string extension;

    // Extract extension
    auto dot_pos = base_outfile.find_last_of('.');
    if (dot_pos != std::string::npos) {
        extension = base_outfile.substr(dot_pos);
        base_outfile = base_outfile.substr(0, dot_pos);
    }

    // Process each segment of the file
    for (size_t img_index = 0; img_index < total_images; ++img_index) {
        // Create a vector for this image segment
        VectorWrapper<unsigned char> image_data;

        // Reserve appropriate space
        image_data.reserve(header_size + bytes_capacity);

        // Add filename to image data
        for (unsigned char character : filename) {
            image_data.push_back(character);
        }
        image_data.push_back('#');

        // Add placeholder for file size (10 digits)
        for (size_t i = 0; i < 10; ++i) {
            image_data.push_back('0');
        }
        image_data.push_back('#');

        // Add image index and total count (X of Y format)
        char index_info[11];
        snprintf(index_info, sizeof(index_info), "%04zu-%04zu", img_index + 1, total_images);
        for (size_t i = 0; i < 10 && index_info[i] != '\0'; ++i) {
            image_data.push_back(index_info[i]);
        }
        image_data.push_back('#');

        // Calculate how much data to read for this image
        size_t bytes_to_read = bytes_capacity;
        if ((img_index + 1) * bytes_capacity > file_size) {
            // Last image might not be full
            bytes_to_read = file_size - (img_index * bytes_capacity);
        }

        // Read this segment of data
        size_t actual_size = 0;
        constexpr size_t chunk_size = 1024 * 1024; // 1MB chunks for reading
        std::vector<char> buffer(chunk_size);

        // Seek to the right position
        file.seekg(static_cast<std::streamoff>(img_index * bytes_capacity), std::ios::beg);

        // Read data in chunks
        while (file && actual_size < bytes_to_read) {
            size_t current_chunk = std::min(chunk_size, bytes_to_read - actual_size);

            if (file.read(buffer.data(), static_cast<std::streamsize>(current_chunk))) {
                auto bytes_read = static_cast<size_t>(file.gcount());
                for (size_t i = 0; i < bytes_read; ++i) {
                    image_data.push_back(static_cast<unsigned char>(buffer[i]));
                }
                actual_size += bytes_read;
            } else {
                break;
            }
        }

        // Update the size information in the header (actual data bytes stored in this image)
        char buffer_size[21];
        snprintf(buffer_size, sizeof(buffer_size), "%010zu", actual_size);

        size_t size_offset = filename.size() + 1;
        for (size_t i = 0; i < 10; ++i) {
            image_data[size_offset + i] = buffer_size[i];
        }

        // Calculate optimal dimensions for this specific image if it's the last one
        size_t current_optimal_dim = optimal_dim;
        if (img_index == total_images - 1 && actual_size < bytes_capacity) {
            // For the last image, calculate more appropriate dimensions if it's not full
            size_t required_pixels = (actual_size + header_size + bytes_per_pixel - 1) / bytes_per_pixel;
            auto required_dim = static_cast<size_t>(std::ceil(std::sqrt(static_cast<double>(required_pixels))));
            
            // Ensure the dimensions don't make the file exceed the limit
            size_t row_bytes_test = required_dim * bytes_per_pixel;
            size_t padding_test = (4 - (row_bytes_test % 4)) % 4;
            size_t test_size = bmp_header_size + (required_dim * (required_dim * bytes_per_pixel + padding_test));
            
            if (test_size <= max_size_bytes) {
                current_optimal_dim = required_dim;
                std::cout << "Optimized last image to size " << current_optimal_dim << "x" << current_optimal_dim << std::endl;
            }
        }

        // Create bitmap with dimensions for this image
        bitmap_image image(static_cast<unsigned int>(current_optimal_dim), static_cast<unsigned int>(current_optimal_dim));

        // Fill bitmap with data
        size_t data_size = image_data.size();
        for (size_t i = 0; i < current_optimal_dim; ++i) {
            for (size_t j = 0; j < current_optimal_dim; ++j) {
                if (size_t pixel_index = i * current_optimal_dim + j; pixel_index * 3 + 2 < data_size) {
                    // We have enough data for this pixel
                    auto [e0, e1, e2] = image_data.get_triplet(pixel_index, 0);
                    image.set_pixel(static_cast<unsigned int>(j), static_cast<unsigned int>(i),
                                  rgb_t{e0, e1, e2});
                } else {
                    // Fill remaining pixels with zeros (black)
                    image.set_pixel(static_cast<unsigned int>(j), static_cast<unsigned int>(i),
                                  rgb_t{0, 0, 0});
                }
            }
        }

        // Create output filename with index if multiple images
        std::string current_outfile;
        if (total_images > 1) {
            current_outfile = base_outfile + "_" + std::to_string(img_index + 1) + "of" +
                             std::to_string(total_images) + extension;
        } else {
            current_outfile = outfile;
        }

        // Save image and report success
        if (image.save_image(current_outfile)) {
            // Get actual file size after saving
            std::ifstream check_file(current_outfile, std::ios::binary | std::ios::ate);
            std::streamsize actual_file_size = check_file.tellg();
            check_file.close();
            
            std::cout << "Created image " << (img_index + 1) << " of " << total_images << ": "
                     << current_outfile << std::endl;
            std::cout << "  Stored " << actual_size << " bytes of data" << std::endl;
            std::cout << "  Image file size: " << actual_file_size << " bytes ("
                     << (actual_file_size / (1024.0 * 1024.0)) << " MB)" << std::endl;
                     
            // Verify we're under the limit
            if (actual_file_size > static_cast<std::streamsize>(max_size_bytes)) {
                std::cerr << "Warning: The image exceeds the specified size limit!" << std::endl;
            }
        } else {
            std::cerr << "Error: Failed to save image " << (img_index + 1) << " to "
                     << current_outfile << std::endl;
        }
    }

    std::cout << "Conversion complete! " << total_images << " image(s) created." << std::endl;
}