#ifndef BITMAP_IMAGE_H
#define BITMAP_IMAGE_H

#include <vector>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream> // Added for std::ofstream
#include <string>
#include "../../Threading/headers/ResourceManager.h"

// Maximum allowed image size in bytes to prevent runaway allocations
inline constexpr size_t MAX_IMAGE_BYTES = 1000 * 1024 * 1024; // 1000 MB cap

// Define NOMINMAX to prevent windows.h from defining min and max macros
#ifndef NOMINMAX
#define NOMINMAX
#endif


/**
 * BitmapImage class
 *
 * Represents a bitmap image with serialization/deserialization capabilities.
 */
class BitmapImage {
public:
    // Default constructor
    BitmapImage() : width(0), height(0) {
    }

    // Constructor with dimensions
    BitmapImage(int width, int height);

    // Copy constructor
    BitmapImage(const BitmapImage &other) : pixels(other.pixels), width(other.width), height(other.height) {
        tracked_bytes_ = 0;
        update_memory_tracking(pixels.capacity());
    }

    // Move constructor
    BitmapImage(BitmapImage &&other) noexcept
        : pixels(std::move(other.pixels)),
          width(other.width),
          height(other.height),
          tracked_bytes_(other.tracked_bytes_) { // transfer memory accounting state
        other.width = 0;
        other.height = 0;
        other.tracked_bytes_ = 0; // prevent double-free accounting on destructor
    }

    // Move assignment operator
    BitmapImage &operator=(BitmapImage &&other) noexcept {
        if (this != &other) {
            // Release current memory accounting for *this*
            update_memory_tracking(0);

            // Steal resources from other
            pixels = std::move(other.pixels);
            width = other.width;
            height = other.height;
            tracked_bytes_ = other.tracked_bytes_;

            // Reset other so its destructor won't modify accounting again
            other.width = 0;
            other.height = 0;
            other.tracked_bytes_ = 0;
        }
        return *this;
    }

    // Copy assignment operator
    BitmapImage &operator=(const BitmapImage &other) {
        if (this != &other) {
            // Release current accounting first
            update_memory_tracking(0);
            pixels = other.pixels;
            width = other.width;
            height = other.height;
            update_memory_tracking(pixels.capacity());
        }
        return *this;
    }

    // Set binary data into pixel buffer
    void setData(const std::vector<uint8_t> &data, size_t offset);

    // Save image to a BMP file
    [[nodiscard]] bool save(const std::string &filename) const;

    // Clear image data and reset dimensions
    void clear();

    // Destructor
    ~BitmapImage() {
        this->clear();
    }

    // Backward compatibility for existing code
    [[nodiscard]] int getWidth() const { return width; }
    [[nodiscard]] int getHeight() const { return height; }
    [[nodiscard]] const std::vector<unsigned char> &getPixels() const { return pixels; }

    // Pixel format
    [[nodiscard]] static int bytes_per_pixel() { return 3; } // RGB format (3 bytes per pixel)

    // Pixel data access
    [[nodiscard]] const unsigned char *get_pixel_data() const { return pixels.data(); }
    [[nodiscard]] unsigned char *get_pixel_buffer_for_writing() { return pixels.data(); }

    // Memory footprint
    [[nodiscard]] size_t get_allocated_memory_size() const { return pixels.capacity(); }

    // Draw a smiley face in the center of the image
    void draw_smiley_face();

    // Serialization to stream
    bool serialize(std::ostream &os) const;

    // Deserialization from stream
    [[nodiscard]] bool deserialize(std::istream &is);

    // Memory usage
    [[nodiscard]] size_t getMemoryUsage() const;

    // Resize the image
    void resize(int new_width, int new_height);

private:
    std::vector<unsigned char> pixels;
    int width;
    int height;

    // Static reference to ResourceManager to avoid repeated getInstance() calls
    static ResourceManager &rm_;

    // Bytes currently accounted in ResourceManager for this image's pixel buffer
    size_t tracked_bytes_{0};

    // Internal helper to sync ResourceManager accounting
    void update_memory_tracking(size_t new_capacity_bytes);
};

#endif // BITMAP_IMAGE_H
