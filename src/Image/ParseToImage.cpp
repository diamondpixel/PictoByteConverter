#include <string>
#include <fstream>
#include <cmath>
#include <iostream>
#include <vector>
#include <filesystem>
#include <thread>
#include <mutex>
#include <optional>
#include <queue>
#include "headers/ParseToImage.h"
#include "headers/ResourceManager.h"
#include "../Debug/headers/Debug.h"



class BitmapImage {
private:
    std::vector<unsigned char> pixels;
    int width;
    int height;

public:
    BitmapImage(int width, int height);
    void setData(const std::vector<uint8_t>& data, size_t offset);
    void save(const std::string& filename);
    ~BitmapImage() = default;
};

struct ImageTaskInternal {
    std::string filename;
    BitmapImage image;
};

template <typename T>
class ThreadSafeQueueTemplate {
private:
    std::queue<T> queue;
    mutable std::mutex mutex;
    std::condition_variable cv;

public:
    void push(const T& item) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push(item);
        cv.notify_one();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.empty();
    }

    std::optional<T> try_pop(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        if (cv.wait_for(lock, timeout, [this] { return !queue.empty(); })) {
            T item = std::move(queue.front());
            queue.pop();
            return item;
        }
        return std::nullopt;
    }
};

std::pair<size_t, size_t> calculateOptimalRectDimensions(size_t total_bytes, size_t bytes_per_pixel, size_t bmp_header_size,
                                                         size_t max_size_bytes, float aspect_ratio = 1.0f) {
    size_t total_pixels = (total_bytes + bytes_per_pixel - 1) / bytes_per_pixel + 500;

    auto width = static_cast<size_t>(std::sqrt(static_cast<double>(total_pixels * aspect_ratio)));
    auto height = static_cast<size_t>(width / aspect_ratio);

    while (width * height < total_pixels) {
        width++;
        height = static_cast<size_t>(width / aspect_ratio);
    }

    size_t min_dimension = 64;
    if (width < min_dimension) width = min_dimension;
    if (height < min_dimension) height = min_dimension;

    if (width > 65535) width = 65535;
    if (height > 65535) height = 65535;

    size_t row_bytes = width * bytes_per_pixel;
    size_t padding = (4 - (row_bytes % 4)) % 4;
    size_t actual_image_size = bmp_header_size + (height * (row_bytes + padding));

    while (actual_image_size > max_size_bytes && width > min_dimension && height > min_dimension) {
        if (width > height) {
            width--;
        } else {
            height--;
        }
        row_bytes = width * bytes_per_pixel;
        padding = (4 - (row_bytes % 4)) % 4;
        actual_image_size = bmp_header_size + (height * (row_bytes + padding));
    }

    return {width, height};
}

std::pair<size_t, size_t> optimizeLastImageDimensions(size_t total_bytes, size_t bytes_per_pixel, size_t metadata_size, 
                                                     size_t bmp_header_size, float aspect_ratio = 1.0f) {
    size_t total_data_size = metadata_size + total_bytes;
    size_t total_pixels_needed = (total_data_size + bytes_per_pixel - 1) / bytes_per_pixel;

    auto width = static_cast<size_t>(std::sqrt(static_cast<double>(total_pixels_needed * aspect_ratio)));
    auto height = static_cast<size_t>(width / aspect_ratio);

    while (width * height < total_pixels_needed) {
        width++;
        height = static_cast<size_t>(width / aspect_ratio);
    }

    size_t min_dimension = 64;
    if (width < min_dimension) width = min_dimension;
    if (height < min_dimension) height = min_dimension;

    if (width > 65535) width = 65535;
    if (height > 65535) height = 65535;

    return {width, height};
}

std::vector<uint8_t> readFileSegment(std::ifstream& file, size_t start_pos, size_t length) {
    auto& resManager = ResourceManager::getInstance();
    bool memory_allocated = resManager.allocateMemory(length);
    
    if (!memory_allocated) {
        printWarning("Memory allocation for " + std::to_string(length / (1024 * 1024)) + 
                    " MB failed, reducing buffer size");
        length = length / 2;
        memory_allocated = resManager.allocateMemory(length);
        if (!memory_allocated) {
            printError("Could not allocate memory for file segment");
            return {};
        }
    }
    
    std::vector<uint8_t> buffer(length);
    
    file.seekg(start_pos, std::ios::beg);
    file.read(reinterpret_cast<char*>(buffer.data()), length);
    
    if (file.eof()) {
        size_t actual_size = file.gcount();
        buffer.resize(actual_size);
        resManager.freeMemory(length - actual_size);
    }
    
    return buffer;
}

void processChunk(int chunk_index, size_t chunk_size, size_t total_chunks, size_t original_file_size, 
                 const std::string& input_file, const std::string& output_base, 
                 ThreadSafeQueueTemplate<ImageTaskInternal>& task_queue, size_t max_image_file_size) {
    
    bool localDebugMode = getDebugMode();

    std::string formatted_path = output_base + "_" + std::to_string(chunk_index + 1) + "of" + std::to_string(total_chunks) + ".bmp";
    
    printHighlight("Processing chunk " + std::to_string(chunk_index + 1) + " of " + std::to_string(total_chunks));
    printFilePath("Chunk output: " + formatted_path);

    size_t start_pos = chunk_index * chunk_size;
    size_t actual_chunk_size = (chunk_index == total_chunks - 1) ?
                               (original_file_size - start_pos) :
                               chunk_size;

    try {
        std::ifstream input(input_file, std::ios::binary);
        if (!input.is_open()) {
            throw std::runtime_error("Failed to open input file: " + input_file);
        }

        std::vector<uint8_t> chunk_data = readFileSegment(input, start_pos, actual_chunk_size);
        if (chunk_data.empty()) {
            throw std::runtime_error("Failed to read data from file: " + input_file);
        }

        std::vector<uint8_t> metadata;

        uint32_t header_length = 48;
        metadata.push_back((header_length >> 16) & 0xFF);
        metadata.push_back((header_length >> 8) & 0xFF);
        metadata.push_back(header_length & 0xFF);

        std::string filename = std::filesystem::path(input_file).filename().string();
        auto filename_length = static_cast<uint16_t>(filename.length());
        metadata.push_back((filename_length >> 8) & 0xFF);
        metadata.push_back(filename_length & 0xFF);

        for (char c : filename) {
            metadata.push_back(static_cast<uint8_t>(c));
        }

        std::string data_size_str = std::to_string(chunk_data.size());
        data_size_str = std::string(10 - data_size_str.length(), '0') + data_size_str; 
        for (char c : data_size_str) {
            metadata.push_back(static_cast<uint8_t>(c));
        }

        std::string current_chunk_str = std::to_string(chunk_index + 1);
        current_chunk_str = std::string(4 - current_chunk_str.length(), '0') + current_chunk_str; 
        for (char c : current_chunk_str) {
            metadata.push_back(static_cast<uint8_t>(c));
        }

        std::string total_chunks_str = std::to_string(total_chunks);
        total_chunks_str = std::string(4 - total_chunks_str.length(), '0') + total_chunks_str; 
        for (char c : total_chunks_str) {
            metadata.push_back(static_cast<uint8_t>(c));
        }

        size_t current_metadata_size = 3 + 2 + filename_length + 10 + 4 + 4;
        if (current_metadata_size < header_length) {
            size_t padding_needed = header_length - current_metadata_size;
            for (size_t i = 0; i < padding_needed; i++) {
                metadata.push_back(0);
            }
        }

        constexpr size_t bytes_per_pixel = 3;
        constexpr size_t bmp_header_size = 54;

        std::pair<size_t, size_t> dimensions;
        if (chunk_index == total_chunks - 1) {
            dimensions = optimizeLastImageDimensions(chunk_data.size(), bytes_per_pixel,
                                                    metadata.size(), bmp_header_size);
        } else {
            dimensions = calculateOptimalRectDimensions(
                metadata.size() + chunk_data.size(),
                bytes_per_pixel, bmp_header_size,
                max_image_file_size
            );
        }

        size_t width = dimensions.first;
        size_t height = dimensions.second;

        printMessage("Chunk " + std::to_string(chunk_index + 1) + " image dimensions: " +
                    std::to_string(width) + " x " + std::to_string(height) + " pixels");

        size_t total_pixels = width * height;
        size_t bitmap_memory_size = total_pixels * 3; 

        bool memory_allocated = ResourceManager::getInstance().allocateMemory(bitmap_memory_size);
        if (!memory_allocated) {
            throw std::runtime_error("Not enough memory to create image");
        }

        BitmapImage image(width, height);

        image.setData(metadata, 0);

        image.setData(chunk_data, metadata.size());

        task_queue.push(ImageTaskInternal{
            formatted_path,
            image
        });

        printStatus("Processed chunk " + std::to_string(chunk_index + 1) + " of " +
                   std::to_string(total_chunks) + " (" +
                   std::to_string(chunk_data.size()) + " bytes)");

    } catch (const std::exception& e) {
        printError("Error processing chunk " + std::to_string(chunk_index + 1) + ": " + e.what());
        throw;
    }
}

void saveImage(const ImageTaskInternal& task) {
    try {
        BitmapImage img = task.image;
        
        img.save(task.filename);
        printStatus("Saved image: " + task.filename);
    } catch (const std::exception& e) {
        printError("Failed to save image: " + std::string(e.what()));
    }
}

void imageWriterThread(ThreadSafeQueueTemplate<ImageTaskInternal>& task_queue, std::atomic<bool>& should_terminate) {
    try {
        while (!should_terminate || !task_queue.empty()) {
            auto task = task_queue.try_pop(std::chrono::milliseconds(100));
            if (task) {
                saveImage(*task);
            }
        }
    } catch (const std::exception& e) {
        printError("Image writer thread error: " + std::string(e.what()));
    }
}

BitmapImage::BitmapImage(int width, int height) {
    this->width = width;
    this->height = height;
    pixels.resize(width * height * 3, 0);
}

void BitmapImage::setData(const std::vector<uint8_t>& data, size_t offset) {
    size_t bytesToCopy = std::min(data.size(), pixels.size() - offset);
    std::copy_n(data.begin(), bytesToCopy, pixels.begin() + offset);
}

void BitmapImage::save(const std::string& filename) {
    try {
        std::ofstream outFile(filename, std::ios::binary);
        if (!outFile) {
            printError("Cannot open file for writing: " + filename);
            return;
        }

        unsigned char fileHeader[14] = {
            'B', 'M', 
            0, 0, 0, 0, 
            0, 0, 0, 0, 
            54, 0, 0, 0 
        };

        unsigned char infoHeader[40] = {
            40, 0, 0, 0, 
            0, 0, 0, 0, 
            0, 0, 0, 0, 
            1, 0, 
            24, 0, 
            0, 0, 0, 0, 
            0, 0, 0, 0, 
            0, 0, 0, 0, 
            0, 0, 0, 0 
        };

        int paddingSize = (4 - (width * 3) % 4) % 4;

        int fileSize = 14 + 40 + (width * 3 + paddingSize) * height;

        fileHeader[2] = static_cast<unsigned char>(fileSize);
        fileHeader[3] = static_cast<unsigned char>(fileSize >> 8);
        fileHeader[4] = static_cast<unsigned char>(fileSize >> 16);
        fileHeader[5] = static_cast<unsigned char>(fileSize >> 24);

        infoHeader[4] = static_cast<unsigned char>(width);
        infoHeader[5] = static_cast<unsigned char>(width >> 8);
        infoHeader[6] = static_cast<unsigned char>(width >> 16);
        infoHeader[7] = static_cast<unsigned char>(width >> 24);

        infoHeader[8] = static_cast<unsigned char>(height);
        infoHeader[9] = static_cast<unsigned char>(height >> 8);
        infoHeader[10] = static_cast<unsigned char>(height >> 16);
        infoHeader[11] = static_cast<unsigned char>(height >> 24);

        int imageSize = (width * 3 + paddingSize) * height;
        infoHeader[20] = static_cast<unsigned char>(imageSize);
        infoHeader[21] = static_cast<unsigned char>(imageSize >> 8);
        infoHeader[22] = static_cast<unsigned char>(imageSize >> 16);
        infoHeader[23] = static_cast<unsigned char>(imageSize >> 24);

        outFile.write(reinterpret_cast<char*>(fileHeader), 14);
        outFile.write(reinterpret_cast<char*>(infoHeader), 40);

        unsigned char padding[3] = {0, 0, 0};

        for (int y = height - 1; y >= 0; y--) {
            for (int x = 0; x < width; x++) {
                unsigned char color[3] = {
                    pixels[(y * width + x) * 3 + 2],  
                    pixels[(y * width + x) * 3 + 1],  
                    pixels[(y * width + x) * 3]       
                };
                outFile.write(reinterpret_cast<char*>(color), 3);
            }

            outFile.write(reinterpret_cast<char*>(padding), paddingSize);
        }

        outFile.close();
        printMessage("Successfully saved " + filename);
    } catch (const std::exception& e) {
        printError("Error saving bitmap: " + std::string(e.what()));
    } catch (...) {
        printError("Unknown error while saving bitmap");
    }
}

bool parseToImage(const std::string& input_file, const std::string& output_base, int maxChunkSizeMB, int maxThreads, int maxMemoryMB) {
    try {
        auto& resManager = ResourceManager::getInstance();

        if (maxThreads > 0) {
            resManager.setMaxThreads(maxThreads);
        } else {
            size_t default_threads = std::max(1u, std::thread::hardware_concurrency() / 2);
            resManager.setMaxThreads(default_threads);
        }

        if (maxMemoryMB > 0) {
            resManager.setMaxMemory(static_cast<size_t>(maxMemoryMB) * 1024 * 1024);
        } else {
            resManager.setMaxMemory(1024 * 1024 * 1024);
        }

        printMessage("Resource limits: " + std::to_string(resManager.getMaxThreads()) +
                    " threads, " + std::to_string(resManager.getMaxMemory() / (1024 * 1024)) + " MB memory");

        const std::string& cleanInputPath = input_file;
        const std::string& cleanOutputPath = output_base;

        if (!std::filesystem::exists(cleanInputPath)) {
            printError("Input file does not exist: " + cleanInputPath);
            return false;
        }

        std::filesystem::path output_path(cleanOutputPath);
        std::filesystem::path output_dir = output_path.parent_path();
        if (!output_dir.empty() && !std::filesystem::exists(output_dir)) {
            std::filesystem::create_directories(output_dir);
            printStatus("Created output directory: " + output_dir.string());
        }

        auto file_size = std::filesystem::file_size(cleanInputPath);
        if (file_size == 0) {
            printError("Input file is empty: " + cleanInputPath);
            return false;
        }

        size_t chunk_size = static_cast<size_t>(maxChunkSizeMB) * 1024 * 1024;
        chunk_size = std::min(chunk_size, file_size);

        size_t total_chunks = (file_size + chunk_size - 1) / chunk_size;
        printFilePath("Input: " + cleanInputPath);
        printFilePath("Output: " + cleanOutputPath);
        printStats("File size: " + std::to_string(file_size) + " bytes, Chunks: " + std::to_string(total_chunks) + 
                   ", Chunk size: " + std::to_string(chunk_size / (1024 * 1024)) + " MB");

        ThreadSafeQueueTemplate<ImageTaskInternal> task_queue;

        std::atomic<bool> should_terminate(false);
        std::thread writer_thread(imageWriterThread, std::ref(task_queue), std::ref(should_terminate));

        size_t max_image_file_size = std::min(
            static_cast<size_t>(100) * 1024 * 1024,
            resManager.getMaxMemory() / (total_chunks > 0 ? total_chunks : 1) / 2
        );

        std::vector<std::thread> processing_threads;
        for (size_t i = 0; i < total_chunks; ++i) {
            std::string chunkInfo = "Chunk " + std::to_string(i + 1) + " of " + std::to_string(total_chunks);
            printProcessingStep(chunkInfo);
            
            resManager.runWithThread(processChunk, i, chunk_size, total_chunks, file_size,
                                    input_file, output_base, std::ref(task_queue), max_image_file_size);

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        resManager.waitForAllThreads();

        should_terminate = true;
        if (writer_thread.joinable()) {
            writer_thread.join();
        }

        printSuccess("File processing completed successfully");
        return true;

    } catch (const std::exception& e) {
        printError("Error in parseToImage: " + std::string(e.what()));
        return false;
    }
}