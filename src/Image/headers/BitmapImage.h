#ifndef BITMAP_IMAGE_H
#define BITMAP_IMAGE_H

#include <vector>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream> // Added for std::ofstream
#include <string>
#include "../../Threading/headers/ResourceManager.h"
#include "../../Debug/headers/MemoryTypes.h"  // for MemoryBlockId

class BitmapImage; // forward declaration for PixelBuffer

namespace detail {
    /**
     * @brief RAII wrapper over ResourceManager pooled memory for raw byte buffers.
     */
    class PixelBuffer {
    public:
        PixelBuffer() = default;

        // Move semantics
        PixelBuffer(PixelBuffer&& other) noexcept { block_ = other.block_; rm_ptr_ = other.rm_ptr_; other.block_ = {}; other.rm_ptr_=nullptr; }
        PixelBuffer& operator=(PixelBuffer&& other) noexcept {
            if (this != &other) {
                free();
                block_ = other.block_;
                rm_ptr_ = other.rm_ptr_;
                other.block_ = {};
                other.rm_ptr_ = nullptr;
            }
            return *this;
        }

        // Disable copy
        PixelBuffer(const PixelBuffer&) = delete;
        PixelBuffer& operator=(const PixelBuffer&) = delete;

        ~PixelBuffer() { free(); }

        void allocate(size_t bytes, ResourceManager& rm) {
            free();
            if (bytes == 0) return;
            block_ = rm.getPooledMemory(bytes, memory::MemoryBlockCategory::BUFFER);
            if (!block_.isValid()) throw std::bad_alloc();
            // Zero small buffers to prevent uninitialized reads; skip large blocks for speed.
            void* ptr = rm.getMemoryPtr(block_);
            constexpr size_t ZERO_THRESHOLD = 1 * 1024 * 1024; // 1 MiB
            if (bytes <= ZERO_THRESHOLD && ptr) {
                std::memset(ptr, 0, bytes);
            }
            rm_ptr_ = &rm;
        }

        void free() {
            if (block_.isValid() && rm_ptr_) {
                rm_ptr_->releasePooledMemory(block_);
                block_ = {};
            }
        }

        uint8_t* data(ResourceManager& rm) { return static_cast<uint8_t*>(rm.getMemoryPtr(block_)); }
        const uint8_t* data(ResourceManager& rm) const { return static_cast<const uint8_t*>(rm.getMemoryPtr(block_)); }
        size_t size(ResourceManager& rm) const { return rm.getMemorySize(block_); }

        uint8_t& operator[](size_t idx) {
            if (!rm_ptr_) throw std::runtime_error("PixelBuffer rm_ptr_ null");
            return data(*rm_ptr_)[idx];
        }
        const uint8_t& operator[](size_t idx) const {
            if (!rm_ptr_) throw std::runtime_error("PixelBuffer rm_ptr_ null");
            return data(*rm_ptr_)[idx];
        }

        [[nodiscard]] bool valid() const { return block_.isValid(); }
        [[nodiscard]] memory::MemoryBlockId id() const { return block_; }

    private:
        memory::MemoryBlockId block_{};
        ResourceManager* rm_ptr_{nullptr};
    };
}

// Maximum allowed image size in bytes to prevent runaway allocations
inline constexpr size_t MAX_IMAGE_BYTES = 100 * 1024 * 1024; // 100 MB cap

// Define NOMINMAX to prevent windows.h from defining min and max macros
#ifndef NOMINMAX
#define NOMINMAX
#endif

/**
 * @brief BitmapImage represents a 24-bit RGB bitmap image with basic manipulation and I/O.
 */
class BitmapImage {
public:
    /**
     * @brief Default constructor.
     */
    BitmapImage() : width(0), height(0) {
    }

    /**
     * @brief Construct a new BitmapImage with the given dimensions.
     * @param width Width of the image in pixels.
     * @param height Height of the image in pixels.
     */
    BitmapImage(int width, int height);

    /**
     * @brief Copy constructor.
     * @param other BitmapImage to copy from.
     */
    BitmapImage(const BitmapImage &other);

    /**
     * @brief Move constructor.
     * @param other BitmapImage to move from.
     */
    BitmapImage(BitmapImage &&other) noexcept
        : pixels(std::move(other.pixels)),
          width(other.width),
          height(other.height) {
        other.width = 0;
        other.height = 0;
    }

    /**
     * @brief Move assignment operator.
     * @param other BitmapImage to move from.
     * @return Reference to this BitmapImage.
     */
    BitmapImage &operator=(BitmapImage &&other) noexcept {
        if (this != &other) {
            pixels = std::move(other.pixels);
            width = other.width;
            height = other.height;

            other.width = 0;
            other.height = 0;
        }
        return *this;
    }

    /**
     * @brief Copy assignment operator.
     * @param other BitmapImage to copy from.
     * @return Reference to this BitmapImage.
     */
    BitmapImage &operator=(const BitmapImage &other);

    /**
     * @brief Sets binary data into the pixel buffer at the specified offset.
     * @param data Binary data to be embedded in the image.
     * @param offset Starting position in the pixel buffer.
     */
    void setData(const std::vector<uint8_t> &data, size_t offset);

    /**
     * @brief Saves the image to a BMP file.
     * @param filename Path where the BMP file will be saved.
     * @return True if the image was saved successfully, false otherwise.
     */
    [[nodiscard]] bool save(const std::string &filename) const;

    /**
     * @brief Clears the image data and resets dimensions.
     */
    void clear();

    /**
     * @brief Destructor.
     */
    ~BitmapImage() {
        this->clear();
    }

    /**
     * @brief Returns the width of the image.
     * @return Width of the image in pixels.
     */
    [[nodiscard]] int getWidth() const { return width; }

    /**
     * @brief Returns the height of the image.
     * @return Height of the image in pixels.
     */
    [[nodiscard]] int getHeight() const { return height; }

    /**
     * @brief Returns a copy of the pixel data of the image.
     * @return Pixel data as a vector of unsigned characters.
     */
    [[nodiscard]] std::vector<unsigned char> getPixelsCopy() const {
        const unsigned char* ptr = data();
        return std::vector<unsigned char>(ptr, ptr + size_bytes());
    }

    /**
     * @brief Returns the number of bytes per pixel in the image.
     * @return Number of bytes per pixel.
     */
    [[nodiscard]] static int bytes_per_pixel() { return 3; } // RGB format (3 bytes per pixel)

    /**
     * @brief Returns a pointer to the pixel data of the image.
     * @return Pointer to the pixel data.
     */
    [[nodiscard]] const unsigned char *get_pixel_data() const { return data(); }

    /**
     * @brief Returns a pointer to the pixel buffer for writing.
     * @return Pointer to the pixel buffer.
     */
    [[nodiscard]] unsigned char *get_pixel_buffer_for_writing() { return data(); }

    /**
     * @brief Returns the allocated memory size of the image.
     * @return Size in bytes of the allocated memory.
     */
    [[nodiscard]] size_t get_allocated_memory_size() const { return size_bytes(); }

    /**
     * @brief Draws a smiley face in the center of the image.
     */
    void draw_smiley_face();

    /**
     * @brief Serializes the bitmap image to an output stream.
     * @param os Output stream to serialize to.
     * @return True if serialization succeeded, false otherwise.
     */
    bool serialize(std::ostream &os) const;

    /**
     * @brief Deserializes the bitmap image from an input stream.
     * @param is Input stream to deserialize from.
     * @return True if deserialization succeeded, false otherwise.
     */
    [[nodiscard]] bool deserialize(std::istream &is);

    /**
     * @brief Returns the memory usage of the bitmap image.
     * @return Size in bytes used by the image data and metadata.
     */
    [[nodiscard]] size_t getMemoryUsage() const;

    /**
     * @brief Resizes the image to new dimensions.
     * @param new_width New width of the image in pixels.
     * @param new_height New height of the image in pixels.
     */
    void resize(int new_width, int new_height);

private:
    detail::PixelBuffer pixels;
    int width;
    int height;

    // Static reference to ResourceManager to avoid repeated getInstance() calls
    static ResourceManager &rm_;

    // Allow PixelBuffer to access rm_
    friend class detail::PixelBuffer;

    // Helper accessors
    inline unsigned char* data() { return pixels.data(rm_); }
    inline const unsigned char* data() const { return pixels.data(rm_); }
    inline size_t size_bytes() const { return pixels.size(rm_); }
};

#endif // BITMAP_IMAGE_H
