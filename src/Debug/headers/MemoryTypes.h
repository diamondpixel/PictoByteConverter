#pragma once

#include <cstdint>

namespace memory {

/**
 * @brief Enum for categorizing memory blocks.
 */
enum class MemoryBlockCategory : uint8_t {
    GENERIC = 0,   /**< General-purpose allocation. */
    BUFFER  = 1,    /**< Buffer memory, e.g., for I/O or image data. */
    COMPUTE = 2     /**< Compute-related memory. */
};

/**
 * @brief Struct representing a unique identifier for a memory block in the pool.
 *
 * Encodes pool index, size bucket, and category for fast lookup and validation.
 */
struct MemoryBlockId {
    uint32_t pool_index{0};   /**< Index in the pool vector. */
    uint8_t  size_bucket{0};   /**< Size bucket for this block. */
    MemoryBlockCategory category{MemoryBlockCategory::GENERIC}; /**< Category of the block. */

    /**
     * @brief Default constructor (invalid ID).
     */
    MemoryBlockId() = default;

    /**
     * @brief Constructor with fields.
     * @param idx Pool index.
     * @param bucket Size bucket.
     * @param cat Category.
     */
    MemoryBlockId(uint32_t idx, uint8_t bucket, MemoryBlockCategory cat)
        : pool_index(idx), size_bucket(bucket), category(cat) {}

    /**
     * @brief Returns the ID as a 32-bit unsigned integer.
     * @return The ID as a 32-bit unsigned integer.
     */
    [[nodiscard]] uint32_t toUint32() const {
        return (static_cast<uint32_t>(pool_index) |
                (static_cast<uint32_t>(size_bucket) << 24) |
                (static_cast<uint32_t>(category) << 29));
    }

    /**
     * @brief Creates a MemoryBlockId from a 32-bit unsigned integer.
     * @param value The 32-bit unsigned integer to create the ID from.
     * @return The created MemoryBlockId.
     */
    static MemoryBlockId fromUint32(uint32_t value) {
        MemoryBlockId id;
        id.pool_index = value & 0x00FFFFFF;
        id.size_bucket = (value >> 24) & 0x1F;
        id.category = static_cast<MemoryBlockCategory>((value >> 29) & 0x07);
        return id;
    }

    /**
     * @brief Comparison operator for equality.
     * @param other The other MemoryBlockId to compare with.
     * @return True if the IDs are equal, false otherwise.
     */
    bool operator==(const MemoryBlockId &other) const {
        return pool_index == other.pool_index &&
               size_bucket == other.size_bucket &&
               category == other.category;
    }

    /**
     * @brief Checks if the ID is valid.
     * @return True if the ID is valid, false otherwise.
     */
    bool isValid() const { return pool_index > 0; }
};

} // namespace memory
