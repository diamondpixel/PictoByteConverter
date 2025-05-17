#include "headers/BitmapImage.h"
#include <algorithm>
#include <filesystem>
#include <windows.h>

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

    HANDLE hFile = CreateFile
    (
        temp_filename.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        printError(
            "Failed to open temporary file for writing: " + temp_filename + ". Error: " +
            std::to_string(GetLastError()));
        return;
    }

    // BMP Headers
    BITMAPFILEHEADER bfh;
    bfh.bfType = 0x4d42;
    bfh.bfReserved1 = 0;
    bfh.bfReserved2 = 0;

    BITMAPINFOHEADER bih;
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = width;
    bih.biHeight = height;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;
    bih.biXPelsPerMeter = 0;
    bih.biYPelsPerMeter = 0;
    bih.biClrUsed = 0;
    bih.biClrImportant = 0;

    constexpr size_t bytes_per_pixel = 3;
    const size_t data_row_size = static_cast<size_t>(width) * bytes_per_pixel;
    const size_t padding = (4 - (data_row_size % 4)) % 4;
    const size_t row_stride_in_file = data_row_size + padding;
    const size_t actual_image_data_on_disk_size = static_cast<size_t>(height) * row_stride_in_file;
    const size_t total_file_size = sizeof(bfh) + sizeof(bih) + actual_image_data_on_disk_size;

    bfh.bfSize = static_cast<uint32_t>(total_file_size);
    bfh.bfOffBits = sizeof(bfh) + sizeof(bih);
    bih.biSizeImage = static_cast<uint32_t>(actual_image_data_on_disk_size);

    // Write headers
    DWORD bytes_written;
    WriteFile(hFile, &bfh, sizeof(bfh), &bytes_written, nullptr);
    WriteFile(hFile, &bih, sizeof(bih), &bytes_written, nullptr);
    OVERLAPPED overlapped = {0};
    overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    // Buffer for one row
    std::vector<unsigned char> row_buffer(row_stride_in_file, 0);
    for (int y_bmp = height - 1; y_bmp >= 0; --y_bmp) {
        for (int x_pixel = 0; x_pixel < width; ++x_pixel) {
            size_t source_pixel_offset = (static_cast<size_t>(y_bmp) * width + x_pixel) * bytes_per_pixel;
            size_t buffer_offset = x_pixel * bytes_per_pixel;
            if (source_pixel_offset + bytes_per_pixel <= pixels.size()) {
                row_buffer[buffer_offset + 0] = pixels[source_pixel_offset + 2]; // Blue
                row_buffer[buffer_offset + 1] = pixels[source_pixel_offset + 1]; // Green
                row_buffer[buffer_offset + 2] = pixels[source_pixel_offset + 0]; // Red
            }
        }
        WriteFile(hFile, row_buffer.data(), row_stride_in_file, &bytes_written, nullptr);
    }

    CloseHandle(hFile);
    CloseHandle(overlapped.hEvent);

    // Rename file
    try {
        if (std::filesystem::exists(filename)) {
            std::filesystem::remove(filename);
        }
        std::filesystem::rename(temp_filename, filename);
    } catch (const std::filesystem::filesystem_error &e) {
        printError("Failed to rename temporary file " + temp_filename + " to " + filename + ". Error: " + e.what());
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
