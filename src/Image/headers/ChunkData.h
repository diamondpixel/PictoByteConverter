#ifndef CHUNK_DATA_H
#define CHUNK_DATA_H

#include <string>
#include <vector>

namespace chunk_data {
    /**
     * ChunkInfo struct
     * 
     * Contains information about a single chunk of data extracted from an image
     * or to be written to an output file.
     */
    struct ChunkInfo {
        int chunkIndex = -1;
        int totalChunks = -1;
        size_t chunkSize = 0;
        size_t expectedDataSize = 0;
        std::string filename;
        std::vector<unsigned char> payload;
        
        // Default constructor
        ChunkInfo() = default;
        
        // Copy constructor
        ChunkInfo(const ChunkInfo& other) = default;
        
        // Move constructor
        ChunkInfo(ChunkInfo&& other) noexcept 
            : chunkIndex(other.chunkIndex),
              totalChunks(other.totalChunks),
              chunkSize(other.chunkSize),
              expectedDataSize(other.expectedDataSize),
              filename(std::move(other.filename)),
              payload(std::move(other.payload)) {
        }
        
        // Copy assignment operator
        ChunkInfo& operator=(const ChunkInfo& other) = default;
        
        // Move assignment operator
        ChunkInfo& operator=(ChunkInfo&& other) noexcept {
            if (this != &other) {
                chunkIndex = other.chunkIndex;
                totalChunks = other.totalChunks;
                chunkSize = other.chunkSize;
                expectedDataSize = other.expectedDataSize;
                filename = std::move(other.filename);
                payload = std::move(other.payload);
            }
            return *this;
        }
    };
}

#endif // CHUNK_DATA_H
