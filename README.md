# PictoByteConverter

A powerful and efficient utility for converting binary files to BMP images and back, available both as a command-line tool and a library.

## Overview

PictoByteConverter is a versatile tool designed to convert any binary file into a series of BMP images and reconstruct the original file from those images. This tool is perfect for data archiving, steganography experiments, or creative ways of backing up important data.

## Features

- **Binary to Image Conversion** - Transform any file into one or more BMP images
- **Image to Binary Reconstruction** - Accurately reconstruct original files from converted images
- **Automatic File Chunking** - Intelligently splits large files into multiple images of configurable size
- **Seamless Chunked File Handling** - Automatically detects and processes all related chunks
- **Metadata Preservation** - Embeds original filename and file structure in the image metadata
- **Multi-threaded Processing** - Utilizes parallel processing for optimal performance
- **Resource Management** - Controls memory usage and thread count to prevent system overload
- **Customizable Chunk Size** - Adjust chunk size for optimal performance on different systems
- **Library Integration** - Use as a library in your own applications via the C++ API
- **Memory-Mapped File Access** - Efficient handling of large files without excessive memory usage
- **Thread-Safe Queue Implementation** - Robust handling of concurrent processing tasks

## Command-Line Usage

```
Usage: ConvertToImage [options]
Options:
  --debug            Enable debug output mode
  --mode=<mode>      Select operation mode (0: File to Image, 1: Image to File)
  --input=<file>     Specify input file
  --output=<file>    Specify output file
  --maxCPU=<num>     Maximum number of CPU threads to use (default: auto)
  --maxMemory=<MB>   Maximum memory to use in MB (default: 1024)
  --maxChunkSize=<MB> Maximum chunk size in MB (default: 9)
  --newMaxImageSize=<MB> Define max UPPER chunk size limit in MB (default: 100) [Override for funsies]
  --help             Display this help message
```

### Converting a File to Image

To convert a binary file to BMP image(s):

```bash
ConvertToImage --mode=0 --input=D:\test.iso --output=D:\test\test --maxCPU=2 --maxMemory=1024 --maxChunkSize=9 --debug
```

Result:
```
Mode: File to Image
Debug mode: Enabled
Processing: Converting file to image...
Input: D:\test.iso
Output: D:\test\test
ResourceManager: Max threads set to 2
ResourceManager: Max memory set to 1024 MB
ResourceManager configured. Max Threads: 2, Max Memory: 1024 MB
Target output BMP file size per chunk: 9437184 bytes (9 MB).
Estimated internal metadata overhead per chunk: 86 bytes.
Available area in BMP for (payload + padding): 9437130 bytes.
Estimated total combined payload (internal metadata + raw data) capacity after padding allowance: 9390179 bytes.
Estimated raw data payload capacity per chunk (after internal metadata estimate): 9390093 bytes.
Total file size: 3280273408 bytes. Number of chunks: 350
Spill path for image task queue: D:\test\.spill_tasks
Creating 4 writer thread(s) with 16 available threads
Enqueueing Task for Chunk 0: Offset=0, SizeToRead=9390093, InputFileTotalSize=3280273408
Enqueueing task for chunk: 0, Offset: 0, ActualDataSize: 9390093
Enqueueing Task for Chunk 1: Offset=9390093, SizeToRead=9390093, InputFileTotalSize=3280273408
Processing chunk 0 / 349
Enqueueing task for chunk: 1, Offset: 9390093, ActualDataSize: 9390093
...
---- BEGIN CHUNK PROCESSING: 1 / 349 ----
  Chunk Index: 1
  Total Chunks: 350
  Actual Data Length for this chunk: 9390093
  Original File Total Size: 3280273408
  Max Image File Size Param: 104857600
---- END CHUNK PROCESSING: 1 (Successfully Enqueued) ----
Chunk 1/350 processed. Output: D:\test\test_1of350.bmp (Data size: 9390093, Header: 48)
...
Chunk 350/350 processed. Output: D:\test\test_350of350.bmp (Data size: 9390093, Header: 48)
All chunks processed successfully
Memory-mapped file closed
Conversion completed successfully
```

By default, the converter will chunk files larger than 9MB into multiple images, with each image having a maximum size of 9MB. You can adjust this with the `--maxChunkSize` parameter. The output BMP files will be named as follows:
- `test_1of350.bmp`, `test_2of350.bmp`, ..., `test_350of350.bmp` (for multi-file output)

### Extracting a File from Image(s)

To extract the original file from BMP image(s):

```bash
ConvertToImage --mode=1 --input=D:\test\test.bmp --output=D:\ --maxCPU=2 --maxMemory=1024 --debug
```

Result:
```
Mode: Image to File
Debug mode: Enabled
Processing: Extracting file from image...
Input image: D:\test\test_1of350.bmp
ResourceManager: Max threads set to 2
ResourceManager: Max memory set to 1024 MB
ResourceManager configured. Max Threads: 12, Max Memory: 16384 MB
Detected multi-part file:
  Base name: test.bmp
  Chunk index: 1 of 350
  Total chunks: 350
Searching for all chunks in directory...
Found chunk file: D:\test\test_1of350.bmp (Chunk 0)
Found chunk file: D:\test\test_2of350.bmp (Chunk 1)
Found chunk file: D:\test\test_3of350.bmp (Chunk 2)
Found all 350 chunk files in directory
Reading metadata from chunks...
Original filename from metadata: test.iso
Original file size: 3280273408 bytes
Creating output directory: D:\
Starting thread pool with 2 worker threads
Processing chunk 0 of 350 (9390093 bytes)
Processing chunk 1 of 350 (9390093 bytes)
Processing chunk 2 of 350 (9390093 bytes)
...
Assembling chunks into original file...
Writing chunk 0 to offset 0
Writing chunk 1 to offset 9390093
Writing chunk 2 to offset 18780186
...
Successfully assembled output file: D:\test.iso
Verified file integrity: MD5 checksum matches original
Extraction completed successfully
```

## Library API Usage

PictoByteConverter can also be used as a library in your own applications. The API provides straightforward functions for conversion operations.

### Linking with Your Project

#### Using CMake:

```cmake
find_package(PictoByteConverter REQUIRED)
target_link_libraries(YourApp PRIVATE PictoByteConverter::pictobyte)
```

#### Using Visual Studio:

1. Add the include directory to your project
2. Link against `pictobyte.lib`
3. Ensure `pictobyte.dll` is in your application's path at runtime

### API Functions

The library provides the following key functions:

```cpp
#include <PictoByteConverter.h>

// Convert a file to an image
bool FileToImage(
    const std::string& inputFilePath,
    const std::string& outputFilePath,
    int maxThreads = 0,
    int maxMemoryMB = 1024,
    int maxChunkSizeMB = 9,
    bool debugMode = false
);

// Extract a file from an image
bool ImageToFile(
    const std::string& inputFilePath,
    const std::string& outputDirectory,
    int maxThreads = 0,
    int maxMemoryMB = 1024,
    bool debugMode = false
);

// Configure logging
void SetLogCallback(void (*callback)(const char* message));

// Get library version
std::string GetLibraryVersion();
```

### Example Library Usage

```cpp
#include <PictoByteConverter.h>
#include <iostream>

// Custom logging function
void LogHandler(const char* message) {
    std::cout << "PictoByteConverter: " << message << std::endl;
}

int main() {
    // Set up logging
    SetLogCallback(LogHandler);
    
    // Convert a file to image
    bool success = FileToImage(
        "D:\\test.iso",
        "D:\\test\\test",
        16,              // Use 16 threads
        32768,           // Use up to 32GB of memory
        9,               // 9MB max chunk size
        true            // Enable debug mode
    );
    
    if (success) {
        std::cout << "Conversion successful!" << std::endl;
        
        // Extract the file back
        success = ImageToFile(
            "D:\\test\\test_1of350.bmp",
            "D:\\",
            12,           // Use 12 threads
            16384,        // Use up to 16GB of memory
            true         // Enable debug mode
        );
        
        if (success) {
            std::cout << "Extraction successful!" << std::endl;
        }
    }
    
    return 0;
}
```

## How It Works

PictoByteConverter encodes binary data into the RGB values of BMP image pixels. It implements:

1. A customized metadata header embedded in each image
2. Parallel processing for efficient conversion
3. Built-in error detection and recovery
4. Automatic directory creation and file management
5. Advanced resource management to prevent memory issues with large files
6. Memory-mapped file access for efficient processing of large files
7. Thread-safe queue system for managing concurrent image processing tasks

## Code Architecture

The PictoByteConverter is structured with a clean, modular architecture:

### Core Components

- **ImageProcessingTypes.h** - Contains essential class definitions:
  - `MemoryMappedFile`: Efficient handling of large file access
  - `BitmapImage`: Representation and manipulation of bitmap images
  - `ImageTaskInternal`: Task structure for image processing operations
  - `ThreadSafeQueueTemplate`: Thread-safe queue implementation for concurrent processing

- **ParseToImage.cpp** - Implements the file-to-image conversion logic:
  - Chunk processing and parallel execution
  - Dimension calculation for optimal image storage
  - Image data embedding and metadata handling

- **ParseFromImage.cpp** - Implements the image-to-file reconstruction logic:
  - Chunk detection and assembly
  - Metadata extraction and validation
  - Original file reconstruction

### Key Algorithms

- **Optimal Rectangle Dimensions** - Calculates the most efficient dimensions for storing binary data in bitmap images
- **Chunk Processing** - Divides large files into manageable chunks for parallel processing
- **Memory Management** - Controls resource usage to prevent system overload during processing

## Implementation Details

### Image to File Conversion Process

The image-to-file conversion process involves several sophisticated steps:

1. **File Discovery and Validation**
   - Scans the input directory for related chunk files
   - Validates file naming patterns to identify all chunks of a multi-part file
   - Orders files correctly based on chunk index metadata

2. **Resource Management**
   - Calculates total memory requirements based on file sizes
   - Pre-allocates memory for efficient processing
   - Configures thread pool based on available CPU cores and memory constraints

3. **Metadata Extraction**
   - Reads embedded metadata from each chunk image
   - Extracts original filename, total file size, chunk index, and total chunks
   - Validates chunk sequence integrity

4. **Parallel Processing**
   - Processes multiple chunks simultaneously using thread pool
   - Extracts binary data from pixel values
   - Validates data integrity for each chunk

5. **File Reassembly**
   - Allocates output file with correct size
   - Writes chunks to precise offsets in the output file
   - Provides progress reporting during reassembly

6. **Verification**
   - Performs integrity checks on the reassembled file
   - Validates file size matches original metadata
   - Optionally verifies MD5 checksum against original (when available)

For large files, the application provides detailed progress reporting:

```
Mode: Image to File
Debug mode: Enabled
Processing: Extracting file from image...
Input image: D:\test\test_1of350.bmp
ResourceManager: Max threads set to 12
ResourceManager: Max memory set to 16384 MB
ResourceManager configured. Max Threads: 12, Max Memory: 16384 MB
Detected multi-part file:
  Base name: test.bmp
  Chunk index: 1 of 350
  Total chunks: 350
Searching for all chunks in directory...
Found chunk file: D:\test\test_1of350.bmp (Chunk 0)
Found chunk file: D:\test\test_2of350.bmp (Chunk 1)
Found chunk file: D:\test\test_3of350.bmp (Chunk 2)
Found all 350 chunk files in directory
Reading metadata from chunks...
Original filename from metadata: test.iso
Original file size: 3280273408 bytes
Creating output directory: D:\
Starting thread pool with 2 worker threads
Processing chunk 0 of 350 (9390093 bytes)
Processing chunk 1 of 350 (9390093 bytes)
Processing chunk 2 of 350 (9390093 bytes)
...
Assembling chunks into original file...
Writing chunk 0 to offset 0
Writing chunk 1 to offset 9390093
Writing chunk 2 to offset 18780186
...
Successfully assembled output file: D:\test.iso
Verified file integrity: MD5 checksum matches original
Extraction completed successfully
```

### File to Image Conversion Process

The file-to-image conversion process follows these key steps:

1. **Input File Analysis**
   - Memory-maps the input file for efficient access
   - Calculates optimal chunk sizes based on memory constraints
   - Determines total number of chunks needed

2. **Chunk Processing**
   - Divides the file into manageable chunks
   - Creates metadata headers for each chunk
   - Calculates optimal image dimensions for each chunk

3. **Image Generation**
   - Encodes binary data into RGB pixel values
   - Constructs proper BMP headers
   - Embeds metadata within the image

4. **Parallel Processing**
   - Uses thread pool for concurrent chunk processing
   - Implements thread-safe queue for image saving tasks
   - Balances CPU and memory usage

5. **Disk Operations**
   - Saves images with appropriate naming convention
   - Implements spill-to-disk mechanism for memory management
   - Provides progress reporting during conversion

For large files, the application provides detailed progress reporting:

```
Mode: File to Image
Debug mode: Enabled
Processing: Converting file to image...
Input: D:\test.iso
Output: D:\test\test
ResourceManager: Max threads set to 16
ResourceManager: Max memory set to 32768 MB
ResourceManager configured. Max Threads: 16, Max Memory: 32768 MB
Target output BMP file size per chunk: 9437184 bytes (9 MB).
Estimated internal metadata overhead per chunk: 86 bytes.
Available area in BMP for (payload + padding): 9437130 bytes.
Estimated total combined payload (internal metadata + raw data) capacity after padding allowance: 9390179 bytes.
Estimated raw data payload capacity per chunk (after internal metadata estimate): 9390093 bytes.
Total file size: 3280273408 bytes. Number of chunks: 350
Spill path for image task queue: D:\test\.spill_tasks
Creating 4 writer thread(s) with 16 available threads
Enqueueing Task for Chunk 0: Offset=0, SizeToRead=9390093, InputFileTotalSize=3280273408
Enqueueing task for chunk: 0, Offset: 0, ActualDataSize: 9390093
Enqueueing Task for Chunk 1: Offset=9390093, SizeToRead=9390093, InputFileTotalSize=3280273408
Processing chunk 0 / 349
Enqueueing task for chunk: 1, Offset: 9390093, ActualDataSize: 9390093
...
---- BEGIN CHUNK PROCESSING: 1 / 349 ----
  Chunk Index: 1
  Total Chunks: 350
  Actual Data Length for this chunk: 9390093
  Original File Total Size: 3280273408
  Max Image File Size Param: 104857600
---- END CHUNK PROCESSING: 1 (Successfully Enqueued) ----
Chunk 1/350 processed. Output: D:\test\test_1of350.bmp (Data size: 9390093, Header: 48)
...
Chunk 350/350 processed. Output: D:\test\test_350of350.bmp (Data size: 9390093, Header: 48)
All chunks processed successfully
Memory-mapped file closed
Conversion completed successfully
```

## Technical Details

- Written in modern C++ with filesystem and multithreading support
- BMP image format chosen for its simplicity and wide compatibility
- Careful handling of data integrity with size validation
- Advanced resource management to prevent memory issues with large files

## Building from Source

### Prerequisites

- CMake (3.14 or higher)
- C++17 compatible compiler
- Windows, Linux, or macOS

### Build Instructions

1. Clone the repository
   ```bash
   git clone https://github.com/yourusername/PictoByteConverter.git
   cd PictoByteConverter
   ```

2. Create build directory
   ```bash
   mkdir build
   cd build
   ```

3. Generate build files with CMake
   ```bash
   cmake ..
   ```

4. Build the project
   ```bash
   cmake --build . --config Release
   ```

The built files will be located in:
- Executable: `cmake-build-release/build/ConvertToImage.exe`
- Library: `cmake-build-release/build/pictobyte.dll` and `pictobyte.lib` (Windows)
- Library: `cmake-build-release/build/libpictobyte.so` (Linux) or `libpictobyte.dylib` (macOS)

## License

This project is licensed under the MIT License - see the [LICENSE.md](LICENSE.md) file for details.