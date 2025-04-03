#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include "headers/bitmap.hpp"
#include "headers/parse_from_image.h"
#include <cstdlib>

#ifndef PARSEFROM
#define PARSEFROM

std::ifstream::pos_type filesize(const char *filename) {
    std::ifstream in(filename, std::ifstream::ate | std::ifstream::binary);
    return in.tellg();
}

void parseFromImage(const std::string &filename) {
    bitmap_image image(filename);

    std::vector<unsigned char> data;
    data.reserve(filesize(filename.data()));

    // Read the image pixels in row-major order.
    // (Assuming the image is square as created by the encoder)
    for (int i = 0; i < image.height(); ++i) {
        for (int j = 0; j < image.width(); ++j) {
            rgb_t rgb = image.get_pixel(j, i);
            data.push_back(rgb.red);
            data.push_back(rgb.green);
            data.push_back(rgb.blue);
        }
    }

    auto data_iter = data.begin();

    // 1. Retrieve output filename (until the first '#' delimiter)
    std::string outfile = "";
    while (data_iter != data.end() && *data_iter != '#') {
        outfile += *data_iter;
        ++data_iter;
    }
    if (data_iter == data.end()) {
        std::cerr << "Error: Header not found (outfile)" << std::endl;
        return;
    }
    ++data_iter; // Skip the '#' delimiter

    // 2. Retrieve file size (10 digits)
    char numbuf[11]; // 10 digits + null terminator
    for (int i = 0; i < 10; ++i) {
        if (data_iter == data.end()) {
            std::cerr << "Error: Header incomplete (file size)" << std::endl;
            return;
        }
        numbuf[i] = *data_iter;
        ++data_iter;
    }
    numbuf[10] = '\0';
    unsigned int data_size = std::atoi(numbuf);

    if (data_iter == data.end() || *data_iter != '#') {
        std::cerr << "Error: Header missing delimiter after file size" << std::endl;
        return;
    }
    ++data_iter; // Skip the '#' delimiter

    // 3. Retrieve index info (e.g., "0001-0002"), read until the next '#' delimiter
    std::string index_info = "";
    while (data_iter != data.end() && *data_iter != '#') {
        index_info += *data_iter;
        ++data_iter;
    }
    if (data_iter == data.end()) {
        std::cerr << "Error: Header incomplete (index info)" << std::endl;
        return;
    }
    ++data_iter; // Skip the '#' delimiter

    // Write the next data_size bytes to the output file
    std::ofstream file(outfile, std::ios::binary);
    if (!file) {
        std::cerr << "Error: Could not open output file " << outfile << std::endl;
        return;
    }

    for (unsigned int i = 0; i < data_size; ++i) {
        if (data_iter == data.end()) {
            std::cerr << "Error: Not enough data in image" << std::endl;
            break;
        }
        file.put(*data_iter);
        ++data_iter;
    }

    std::cout << "Successfully converted " << filename
            << " to the data saved at " << outfile << "!" << std::endl;

    file.close();
}

#endif
