#ifndef FILE_TASK_H
#define FILE_TASK_H

#include <string>
#include <vector>
#include "../../Image/headers/ChunkData.h"

/**
 * FileTask struct
 * 
 * Represents a file writing task for the ParseFromImage process.
 * Contains chunk information and output path details.
 */
struct FileTask {
    // Chunk data and metadata
    chunk_data::ChunkInfo chunk;
    
    // Output path information
    std::string outputPath;
    
    // Flags for task management
    bool isLastChunk = false;
    
    // Default constructor
    FileTask() = default;
    
    // Constructor with chunk info and output path
    FileTask(chunk_data::ChunkInfo chunkInfo, std::string outPath, bool last = false)
        : chunk(std::move(chunkInfo)), outputPath(std::move(outPath)), isLastChunk(last) {
    }
};

#endif // FILE_TASK_H
