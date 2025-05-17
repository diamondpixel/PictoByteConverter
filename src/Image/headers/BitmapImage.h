#ifndef BITMAP_IMAGE_H
#define BITMAP_IMAGE_H

#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <cstdint>
#include <filesystem>

// Define NOMINMAX to prevent windows.h from defining min and max macros
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../../Debug/headers/Debug.h" // For printError, printWarning

/**
 * BitmapImage class
 * 
 * Represents a bitmap image with serialization/deserialization capabilities.
 */
class BitmapImage {
public:
    BitmapImage() : width(0), height(0) {}
    BitmapImage(int width, int height);
    
    void setData(const std::vector<uint8_t> &data, size_t offset);
    void save(const std::string &filename);
    ~BitmapImage() = default;

    bool serialize(std::ostream &os) const {
        if (!os.good()) return false;
        os.write(reinterpret_cast<const char *>(&width), sizeof(width));
        os.write(reinterpret_cast<const char *>(&height), sizeof(height));
        size_t pixels_size = pixels.size();
        os.write(reinterpret_cast<const char *>(&pixels_size), sizeof(pixels_size));
        if (pixels_size > 0) {
            os.write(reinterpret_cast<const char *>(pixels.data()), pixels_size);
        }
        return os.good();
    }

    bool deserialize(std::istream &is) {
        if (!is.good()) return false;
        is.read(reinterpret_cast<char *>(&width), sizeof(width));
        is.read(reinterpret_cast<char *>(&height), sizeof(height));
        size_t pixels_size = 0;
        is.read(reinterpret_cast<char *>(&pixels_size), sizeof(pixels_size));
        if (is.fail() || pixels_size > (100 * 1024 * 1024)) { // 100MB sanity check
            pixels.clear(); 
            printError("BitmapImage::deserialize: Pixel size read from stream is too large or read failed. Size: " + std::to_string(pixels_size));
            return false;
        }
        if (pixels_size > 0) {
            try {
                pixels.resize(pixels_size);
            } catch (const std::bad_alloc &e) {
                printError("BitmapImage::deserialize: Failed to allocate memory for pixels: " + std::string(e.what()));
                return false;
            }
            is.read(reinterpret_cast<char *>(pixels.data()), pixels_size);
        } else {
            pixels.clear();
        }
        return is.good();
    }

    // Getters
    [[nodiscard]] int getWidth() const { return width; }
    [[nodiscard]] int getHeight() const { return height; }
    [[nodiscard]] const std::vector<unsigned char>& getPixels() const { return pixels; }

private:
    std::vector<unsigned char> pixels;
    int width;
    int height;
};

#endif // BITMAP_IMAGE_H
