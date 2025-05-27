#include "headers/BitmapImage.h"
#include <algorithm>
#include <filesystem>
#include <windows.h>
#include <cassert>
#include <format>
#include "Debug/headers/LogMacros.h"
#include "Threading/headers/ResourceManager.h"

// Initialize static ResourceManager reference
ResourceManager& BitmapImage::rm_ = ResourceManager::getInstance();

/**
 * @brief Constructor for the `BitmapImage` class.
 *
 * Initializes a new bitmap image with the specified dimensions.
 *
 * @param width Width of the image in pixels
 * @param height Height of the image in pixels
 */
BitmapImage::BitmapImage(int width, int height) {
    // Guard against excessive allocations (width/height may overflow the multiplication)
    if (width <= 0 || height <= 0) {
        LOG_ERR("BitmapImage", "BitmapImage: invalid dimensions", debug::LogContext::Error);
        this->~BitmapImage();
        return;
    }

    uint64_t total_bytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * bytes_per_pixel();
    if (total_bytes > MAX_IMAGE_BYTES) {
        std::string msg = std::format("BitmapImage: requested {}x{} exceeds {} MB cap", width, height, MAX_IMAGE_BYTES / (1024 * 1024));
        LOG_ERR("BitmapImage", msg, debug::LogContext::Error);
        this->~BitmapImage();
        return;
    }

    // Check ResourceManager capacity before allocating
    if (total_bytes > rm_.getMaxMemory() || rm_.getCurrentMemoryUsage() + total_bytes > rm_.getMaxMemory()) {
        LOG_WARN("BitmapImage", "BitmapImage: allocation would exceed global memory limit; image ignored", debug::LogContext::Error);
        this->~BitmapImage();
        return;
    }

    this->width = width;
    this->height = height;
    pixels.resize(static_cast<size_t>(total_bytes), 0);
    update_memory_tracking(pixels.capacity());
    
    // Draw a smiley face in the middle of the image
    draw_smiley_face();
}

/**
 * @brief Draws a smiley face in the center of the image
 */
void BitmapImage::draw_smiley_face() {
    if (width < 100 || height < 100) {
        return; // Skip if image is too small
    }
    
    const int face_radius = std::min(width, height) / 8;
    const int center_x = width / 2;
    const int center_y = height / 2;
    
    // Draw yellow circle for face
    for (int y = center_y - face_radius; y <= center_y + face_radius; y++) {
        for (int x = center_x - face_radius; x <= center_x + face_radius; x++) {
            // Skip if out of bounds
            if (x < 0 || x >= width || y < 0 || y >= height) continue;
            
            // Calculate distance from center
            int dx = x - center_x;
            int dy = y - center_y;
            int distance_squared = dx * dx + dy * dy;
            
            // If inside the circle, color it yellow
            if (distance_squared <= face_radius * face_radius) {
                int pixel_index = (y * width + x) * bytes_per_pixel();
                pixels[pixel_index] = 255;     // R
                pixels[pixel_index + 1] = 255; // G
                pixels[pixel_index + 2] = 0;   // B
            }
        }
    }
    
    // Draw eyes (black circles)
    const int eye_radius = face_radius / 5;
    const int eye_offset_x = face_radius / 2;
    const int eye_offset_y = face_radius / 3;
    
    // Left eye
    for (int y = center_y - eye_offset_y - eye_radius; y <= center_y - eye_offset_y + eye_radius; y++) {
        for (int x = center_x - eye_offset_x - eye_radius; x <= center_x - eye_offset_x + eye_radius; x++) {
            if (x < 0 || x >= width || y < 0 || y >= height) continue;
            
            int dx = x - (center_x - eye_offset_x);
            int dy = y - (center_y - eye_offset_y);
            int distance_squared = dx * dx + dy * dy;
            
            if (distance_squared <= eye_radius * eye_radius) {
                int pixel_index = (y * width + x) * bytes_per_pixel();
                pixels[pixel_index] = 0;     // R
                pixels[pixel_index + 1] = 0; // G
                pixels[pixel_index + 2] = 0; // B
            }
        }
    }
    
    // Right eye
    for (int y = center_y - eye_offset_y - eye_radius; y <= center_y - eye_offset_y + eye_radius; y++) {
        for (int x = center_x + eye_offset_x - eye_radius; x <= center_x + eye_offset_x + eye_radius; x++) {
            if (x < 0 || x >= width || y < 0 || y >= height) continue;
            
            int dx = x - (center_x + eye_offset_x);
            int dy = y - (center_y - eye_offset_y);
            int distance_squared = dx * dx + dy * dy;
            
            if (distance_squared <= eye_radius * eye_radius) {
                int pixel_index = (y * width + x) * bytes_per_pixel();
                pixels[pixel_index] = 0;     // R
                pixels[pixel_index + 1] = 0; // G
                pixels[pixel_index + 2] = 0; // B
            }
        }
    }
    
    // Draw smile (semicircle)
    const int smile_radius = face_radius / 2;
    const int smile_offset_y = face_radius / 4;
    
    for (int y = center_y + smile_offset_y; y <= center_y + smile_offset_y + smile_radius; y++) {
        for (int x = center_x - smile_radius; x <= center_x + smile_radius; x++) {
            if (x < 0 || x >= width || y < 0 || y >= height) continue;
            
            int dx = x - center_x;
            int dy = y - (center_y + smile_offset_y);
            int distance_squared = dx * dx + dy * dy;
            
            // Draw only the bottom half of the circle (smile)
            if (distance_squared <= smile_radius * smile_radius && 
                distance_squared >= (smile_radius - 2) * (smile_radius - 2) && 
                dy > 0) {
                int pixel_index = (y * width + x) * bytes_per_pixel();
                pixels[pixel_index] = 0;     // R
                pixels[pixel_index + 1] = 0; // G
                pixels[pixel_index + 2] = 0; // B
            }
        }
    }
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
    update_memory_tracking(0);
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
        LOG_ERR("BitmapImage", "BitmapImage::resize: Invalid dimensions, clearing image", debug::LogContext::Error);
        clear();
        return;
    }

    size_t new_total = static_cast<size_t>(new_width) * static_cast<size_t>(new_height) * bytes_per_pixel();
    if (new_total > rm_.getMaxMemory() || rm_.getCurrentMemoryUsage() - tracked_bytes_ + new_total > rm_.getMaxMemory()) {
        LOG_ERR("BitmapImage", "BitmapImage::resize: would exceed memory limit; resize aborted", debug::LogContext::Error);
        return;
    }

    width = new_width;
    height = new_height;
    pixels.resize(new_total, 0);
    update_memory_tracking(pixels.capacity());
}

/**
 * @brief Saves the image to a BMP file.
 *
 * This function handles the low-level details of writing the image data
 * to a file in the BMP format.
 *
 * @param filename Path where the BMP file will be saved
 */
bool BitmapImage::save(const std::string &filename) const {
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
        std::string msg = "Failed to open temporary file for writing: " + temp_filename + ". Error: " + std::to_string(GetLastError());
        LOG_ERR("BitmapImage", msg, debug::LogContext::Error);
        return false;
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
        std::string msg = "Failed to rename temporary file " + temp_filename + " to " + filename + ". Error: " + e.what();
        LOG_ERR("BitmapImage", msg, debug::LogContext::Error);
        try {
            if (std::filesystem::exists(temp_filename)) {
                std::filesystem::remove(temp_filename);
            }
        } catch (const std::filesystem::filesystem_error &e_remove) {
            std::string msg_remove = "BitmapImage::save: Failed to remove temp file after rename error: " + temp_filename + ". Error: " + e_remove.what();
            LOG_ERR("BitmapImage", msg_remove, debug::LogContext::Error);
            return false;
        }
    }
    return true;
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
    if (!os.good()) {
        LOG_ERR("BitmapImage", "BitmapImage::serialize: Stream not good at entry.", debug::LogContext::Error);
        return false;
    }

    os.write(reinterpret_cast<const char *>(&width), sizeof(width));
    if (!os.good()) {
        LOG_ERR("BitmapImage", "BitmapImage::serialize: Stream error after writing width.", debug::LogContext::Error);
        return false;
    }

    os.write(reinterpret_cast<const char *>(&height), sizeof(height));
    if (!os.good()) {
        LOG_ERR("BitmapImage", "BitmapImage::serialize: Stream error after writing height.", debug::LogContext::Error);
        return false;
    }

    size_t pixel_data_size = pixels.size();
    os.write(reinterpret_cast<const char *>(&pixel_data_size), sizeof(pixel_data_size));
    if (!os.good()) {
        LOG_ERR("BitmapImage", "BitmapImage::serialize: Stream error after writing pixel_data_size.", debug::LogContext::Error);
        return false;
    }

    if (pixel_data_size > 0) {
        os.write(reinterpret_cast<const char *>(pixels.data()), pixel_data_size);
        if (!os.good()) {
            LOG_ERR("BitmapImage", "BitmapImage::serialize: Stream error after writing pixels.", debug::LogContext::Error);
            return false;
        }
    }
    return os.good();
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
        LOG_ERR("BitmapImage", "BitmapImage::deserialize: Failed to read dimensions", debug::LogContext::Error);
        return false;
    }

    // Validate dimensions
    if (new_width <= 0 || new_height <= 0 ||
        new_width > 65535 || new_height > 65535) { // Reasonable limits
        std::string msg = "BitmapImage::deserialize: Invalid dimensions: " + std::to_string(new_width) + "x" + std::to_string(new_height);
        LOG_ERR("BitmapImage", msg, debug::LogContext::Error);
        return false;
    }

    // Read pixel data size
    size_t pixels_size = 0;
    if (!is.read(reinterpret_cast<char *>(&pixels_size), sizeof(pixels_size))) {
        LOG_ERR("BitmapImage", "BitmapImage::deserialize: Failed to read pixel data size", debug::LogContext::Error);
        return false;
    }

    // Validate pixel data size
    constexpr size_t MAX_IMAGE_SIZE = 100 * 1024 * 1024; // 100MB sanity check
    if (is.fail() || pixels_size > MAX_IMAGE_SIZE) {
        pixels.clear();
        std::string msg = "BitmapImage::deserialize: Pixel size too large or read failed. Size: " + std::to_string(pixels_size);
        LOG_ERR("BitmapImage", msg, debug::LogContext::Error);
        return false;
    }

    // Calculate expected size based on dimensions
    const size_t expected_size = static_cast<size_t>(new_width) *
                                 static_cast<size_t>(new_height) *
                                 static_cast<size_t>(bytes_per_pixel());

    // Validate pixel data size against dimensions
    if (pixels_size > 0 && pixels_size != expected_size) {
        std::string msg = "BitmapImage::deserialize: Pixel data size (" + std::to_string(pixels_size) + ") doesn't match expected size (" + std::to_string(expected_size) + ") for dimensions " + std::to_string(new_width) + "x" + std::to_string(new_height);
        LOG_ERR("BitmapImage", msg, debug::LogContext::Error);
        // Potentially an error, but some formats might have padding/extra data.
        // For strict BMP, this would be an error. For now, we'll log and try to proceed if pixels_size is what we read.
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
            size_t bytes_read = 0;

            while (bytes_read < pixels_size && is.good()) {
                constexpr size_t CHUNK_SIZE = 4096;
                const size_t bytes_to_read = std::min(CHUNK_SIZE, pixels_size - bytes_read);
                is.read(reinterpret_cast<char *>(pixels.data() + bytes_read),
                        static_cast<std::streamsize>(bytes_to_read));

                const auto chunk_bytes_read = static_cast<size_t>(is.gcount());
                if (chunk_bytes_read != bytes_to_read) {
                    std::string msg = "BitmapImage::deserialize: Unexpected end of data. Expected " + std::to_string(bytes_to_read) + " bytes, got " + std::to_string(chunk_bytes_read);
                    LOG_ERR("BitmapImage", msg, debug::LogContext::Error);
                    return false;
                }
                bytes_read += chunk_bytes_read;
            }
        } catch (const std::bad_alloc &e) {
            std::string msg = "BitmapImage::deserialize: Failed to allocate memory for pixels: " + std::string(e.what());
            LOG_ERR("BitmapImage", msg, debug::LogContext::Error);
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

/**
 * @brief Returns the memory usage of the bitmap image.
 *
 * Calculates the total memory used by the image, including pixel data and metadata.
 *
 * @return Size in
 */
size_t BitmapImage::getMemoryUsage() const {
    // Calculate the memory used by the pixel data - this is the dominant factor
    // For a 5000x5000 RGB image, this would be 5000*5000*3 = 75,000,000 bytes
    size_t pixel_memory = static_cast<size_t>(width) * static_cast<size_t>(height) * bytes_per_pixel();

    // Add memory for the vector's capacity (which might be larger than width*height*3)
    if (pixels.capacity() > pixel_memory) {
        pixel_memory = pixels.capacity();
    }

    // Add memory for the class members (width, height, etc.)
    size_t metadata_memory = sizeof(BitmapImage);

    return pixel_memory + metadata_memory;
}

// Helper to adjust ResourceManager accounting
void BitmapImage::update_memory_tracking(size_t new_capacity_bytes) {
    if (new_capacity_bytes > tracked_bytes_) {
        rm_.increaseMemory(new_capacity_bytes - tracked_bytes_);
    } else if (new_capacity_bytes < tracked_bytes_) {
        rm_.decreaseMemory(tracked_bytes_ - new_capacity_bytes);
    }
    tracked_bytes_ = new_capacity_bytes;
}
