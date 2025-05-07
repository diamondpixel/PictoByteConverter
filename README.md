# PictoByteConverter

A powerful and efficient utility for converting binary files to BMP images and back.

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

## Usage

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
  --help             Display this help message
```

### Converting a File to Image

To convert a binary file to BMP image(s):

```bash
ConvertToImage --mode=0 --input=MyFile.wav --output=MyFile.bmp
```

By default, the converter will chunk files larger than 9MB into multiple images, with each image having a maximum size of 9MB. You can adjust this with the `--maxChunkSize` parameter. The output BMP files will be named as follows:
- `MyFile.bmp.bmp` (for single file output)
- `MyFile.bmp_1of3.bmp`, `MyFile.bmp_2of3.bmp`, `MyFile.bmp_3of3.bmp` (for multi-file output)

### Extracting a File from Image(s)

To extract the original file from BMP image(s):

```bash
ConvertToImage --mode=1 --input=MyFile.bmp_1of3.bmp --output=OutputDirectory
```

The program will automatically:
1. Detect this is a chunk (part 1 of 3)
2. Find all related chunks in the same directory
3. Process them in the correct order
4. Reconstruct the original file with its original filename
5. Place it in the specified output directory

## How It Works

PictoByteConverter encodes binary data into the RGB values of BMP image pixels. It implements:

1. A customized metadata header embedded in each image
2. Parallel processing for efficient conversion
3. Built-in error detection and recovery
4. Automatic directory creation and file management
5. Advanced resource management to prevent memory issues with large files

## Examples

### Encoding a Large Audio File

```bash
ConvertToImage --mode=0 --input=EnchantedWaterfall.wav --output=EnchantedWaterfall.bmp --maxCPU=4 --maxMemory=2048 --maxChunkSize=12
```

Result:
```
Converting file to image...
Input: EnchantedWaterfall.wav, Output: EnchantedWaterfall.bmp
Resource limits: 4 threads, 2048 MB memory, 12 MB max chunk size
Input file size: 36857678 bytes
Splitting file into 3 chunks of approximately 12288 KB each
Saved image: EnchantedWaterfall.bmp_1of3.bmp
Saved image: EnchantedWaterfall.bmp_2of3.bmp
Saved image: EnchantedWaterfall.bmp_3of3.bmp
Conversion completed successfully
```

### Decoding from Images

```bash
ConvertToImage --mode=1 --input=EnchantedWaterfall.bmp_1of3.bmp --output=D:\Restored --maxCPU=2 --maxMemory=1024
```

Result:
```
Extracting file from image...
Input image: EnchantedWaterfall.bmp_1of3.bmp
Resource limits: 2 threads, 1024 MB memory
Detected multi-part file:
  Base name: EnchantedWaterfall.bmp
  Chunk index: 1 of 3
Found 3 chunk files
Successfully assembled output file: D:\Restored\EnchantedWaterfall.wav
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

The executable will be located in the `build/Release` directory (Windows) or `build` directory (Linux/macOS).

## Implementation Details

### File Structure

The project is organized as follows:

- `src/` - Source code files
  - `Main.cpp` - Program entry point, argument handling
  - `Image/` - Image processing functionality
    - `ParseToImage.cpp` - File to image conversion
    - `ParseFromImage.cpp` - Image to file extraction
    - `ResourceManager.h` - Resource management (memory and threads)
    - `headers/` - Header files
  - `Debug/` - Debug utilities

### Metadata Format

Each BMP image contains a metadata header with the following information:
- Original filename
- Data size
- Chunk index
- Total number of chunks

This metadata ensures proper reconstruction of the original file.

## License

This project is licensed under the MIT License