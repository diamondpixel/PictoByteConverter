#ifndef BITMAP_IMAGE_H
#define BITMAP_IMAGE_H

#include <vector>
#include <string>
#include <cstdint>
#include <filesystem>

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

    // Move constructor
    BitmapImage(BitmapImage&& other) noexcept
        : pixels(std::move(other.pixels)),
          width(other.width),
          height(other.height) {
        other.width = 0;
        other.height = 0;
    }

    // Move assignment operator
    BitmapImage& operator=(BitmapImage&& other) noexcept {
        if (this != &other) {
            pixels = std::move(other.pixels);
            width = other.width;
            height = other.height;
            other.width = 0;
            other.height = 0;
        }
        return *this;
    }

    // Copy constructor
    BitmapImage(const BitmapImage& other)
        : pixels(other.pixels),
          width(other.width),
          height(other.height) {
    }

    // Copy assignment operator
    BitmapImage& operator=(const BitmapImage& other) {
        if (this != &other) {
            pixels = other.pixels;
            width = other.width;
            height = other.height;
        }
        return *this;
    }

    // Set binary data into pixel buffer
    void setData(const std::vector<uint8_t> &data, size_t offset);

    // Save image to a BMP file
    void save(const std::string &filename) const;

    // Clear image data and reset dimensions
    void clear();

    // Destructor
    ~BitmapImage() = default;


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


    // Resize the image
    void resize(int new_width, int new_height);

    // Serialization to stream
    bool serialize(std::ostream &os) const;

    // Deserialization from stream
    bool deserialize(std::istream &is);

private:
    std::vector<unsigned char> pixels;
    int width;
    int height;
};

#endif // BITMAP_IMAGE_H
