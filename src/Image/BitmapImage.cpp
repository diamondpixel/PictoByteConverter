#include "headers/BitmapImage.h"
#include <algorithm>
#include <filesystem>
#include <windows.h>
#include <cassert>
#include <Debug/headers/Debug.h>

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
    pixels.resize(width * height * bytes_per_pixel(), 0); // 3 bytes per pixel (RGB)
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
    std::copy_n(data.begin(), static_cast<std::ptrdiff_t>(bytesToCopy), pixels.begin() + offset);
}
/**
 * @brief Clears the image data and resets dimensions.
 *
 * Releases the pixel data and sets width and height to 0.
 */
void BitmapImage::clear() {
    pixels.clear();
    pixels.shrink_to_fit(); // Attempt to release memory
    width = 0;
    height = 0;
}

/**
 * @brief Resizes the image to new dimensions.
 *
 * @param new_width New width of the image in pixels
 * @param new_height New height of the image in pixels
 */
void BitmapImage::resize(int new_width, int new_height) {
    if (new_width <= 0 || new_height <= 0) {
        printWarning("BitmapImage::resize: Invalid dimensions, clearing image");
        clear();
        return;
    }

    width = new_width;
    height = new_height;
    pixels.resize(width * height * bytes_per_pixel(), 0);
}

/**
 * @brief Saves the image to a BMP file.
 *
 * This function handles the low-level details of writing the image data
 * to a file in the BMP format.
 *
 * @param filename Path where the BMP file will be saved
 */
void BitmapImage::save(const std::string &filename) const {
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
        assert(row_stride_in_file <= std::numeric_limits<DWORD>::max() && "row_stride_in_file exceeds DWORD limits!");
        WriteFile(hFile, row_buffer.data(), static_cast<DWORD>(row_stride_in_file), &bytes_written, nullptr);
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

/**
 * @brief Serializes the bitmap image to an output stream.
 *
 * Optimized implementation that uses buffered writes to improve performance.
 *
 * @param os Output stream to serialize to
 * @return True if serialization succeeded, false otherwise
 */
bool BitmapImage::serialize(std::ostream &os) const {
    if (!os.good()) return false;

    // Use a buffer to reduce the number of write operations
    constexpr size_t BUFFER_SIZE = 4096;
    char buffer[BUFFER_SIZE];
    size_t buffer_pos = 0;

    // Helper to write to buffer and flush when needed
    auto write_to_buffer = [&](const void *data, size_t size) {
        // If data doesn't fit in buffer, flush buffer first
        if (buffer_pos + size > BUFFER_SIZE) {
            os.write(buffer, static_cast<std::streamsize>(buffer_pos));
            if (!os.good()) return false;
            buffer_pos = 0;
        }

        // If data is larger than buffer, write directly to stream
        if (size > BUFFER_SIZE) {
            if (buffer_pos > 0) {
                os.write(buffer, static_cast<std::streamsize>(buffer_pos));
                if (!os.good()) return false;
                buffer_pos = 0;
            }
            os.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
            return os.good();
        }

        // Otherwise copy to buffer
        std::memcpy(buffer + buffer_pos, data, size);
        buffer_pos += size;
        return true;
    };

    // Helper to flush buffer
    auto flush_buffer = [&]() {
        if (buffer_pos > 0) {
            os.write(buffer, static_cast<std::streamsize>(buffer_pos));
            buffer_pos = 0;
            return os.good();
        }
        return true;
    };

    // Write dimensions
    if (!write_to_buffer(&width, sizeof(width)) ||
        !write_to_buffer(&height, sizeof(height))) {
        return false;
    }

    // Write pixel data size
    const size_t pixels_size = pixels.size();
    if (!write_to_buffer(&pixels_size, sizeof(pixels_size))) {
        return false;
    }

    // Write pixel data - directly to stream if large
    if (pixels_size > 0) {
        if (pixels_size > BUFFER_SIZE - buffer_pos) {
            // Flush buffer and write pixels directly
            if (!flush_buffer()) return false;
            os.write(reinterpret_cast<const char *>(pixels.data()),
                     static_cast<std::streamsize>(pixels_size));
        } else {
            // Write to buffer
            if (!write_to_buffer(pixels.data(), pixels_size)) {
                return false;
            }
        }
    }

    // Final flush
    return flush_buffer();
}

/**
 * @brief Deserializes the bitmap image from an input stream.
 *
 * Optimized implementation with improved error handling and memory management.
 *
 * @param is Input stream to deserialize from
 * @return True if deserialization succeeded, false otherwise
 */
bool BitmapImage::deserialize(std::istream &is) {
    if (!is.good()) return false;

    // Read dimensions
    int new_width, new_height;
    if (!is.read(reinterpret_cast<char *>(&new_width), sizeof(new_width)) ||
        !is.read(reinterpret_cast<char *>(&new_height), sizeof(new_height))) {
        printError("BitmapImage::deserialize: Failed to read dimensions");
        return false;
    }

    // Validate dimensions
    if (new_width <= 0 || new_height <= 0 ||
        new_width > 65535 || new_height > 65535) {
        // Reasonable limits
        printError("BitmapImage::deserialize: Invalid dimensions: " +
                   std::to_string(new_width) + "x" + std::to_string(new_height));
        return false;
    }

    // Read pixel data size
    size_t pixels_size = 0;
    if (!is.read(reinterpret_cast<char *>(&pixels_size), sizeof(pixels_size))) {
        printError("BitmapImage::deserialize: Failed to read pixel data size");
        return false;
    }

    // Validate pixel data size
    constexpr size_t MAX_IMAGE_SIZE = 100 * 1024 * 1024; // 100MB sanity check
    if (is.fail() || pixels_size > MAX_IMAGE_SIZE) {
        pixels.clear();
        printError("BitmapImage::deserialize: Pixel size too large or read failed. Size: " +
                   std::to_string(pixels_size));
        return false;
    }

    // Calculate expected size based on dimensions
    const size_t expected_size = static_cast<size_t>(new_width) *
                                 static_cast<size_t>(new_height) *
                                 static_cast<size_t>(bytes_per_pixel());

    // Validate pixel data size against dimensions
    if (pixels_size > 0 && pixels_size != expected_size) {
        printWarning("BitmapImage::deserialize: Pixel data size (" +
                     std::to_string(pixels_size) + ") doesn't match expected size (" +
                     std::to_string(expected_size) + ") for dimensions " +
                     std::to_string(new_width) + "x" + std::to_string(new_height));
    }

    // Read pixel data
    if (pixels_size > 0) {
        try {
            // Reserve memory first to avoid multiple reallocations
            pixels.reserve(pixels_size);
            pixels.resize(pixels_size);

            // Update dimensions
            width = new_width;
            height = new_height;

            // Read data in chunks for better performance with large images
            constexpr size_t CHUNK_SIZE = 8192;
            size_t bytes_read = 0;

            while (bytes_read < pixels_size && is.good()) {
                const size_t bytes_to_read = std::min(CHUNK_SIZE, pixels_size - bytes_read);
                is.read(reinterpret_cast<char *>(pixels.data() + bytes_read),
                        static_cast<std::streamsize>(bytes_to_read));

                const auto chunk_bytes_read = static_cast<size_t>(is.gcount());
                if (chunk_bytes_read != bytes_to_read) {
                    printError("BitmapImage::deserialize: Unexpected end of data. Expected " +
                               std::to_string(bytes_to_read) + " bytes, got " +
                               std::to_string(chunk_bytes_read));
                    return false;
                }
                bytes_read += chunk_bytes_read;
            }
        } catch (const std::bad_alloc &e) {
            printError("BitmapImage::deserialize: Failed to allocate memory for pixels: " +
                       std::string(e.what()));
            pixels.clear();
            width = 0;
            height = 0;
            return false;
        }
    } else {
        // Empty image
        pixels.clear();
        width = 0;
        height = 0;
    }
    return is.good();
}
