#include "headers/PictoByteConverter.h"
#include "../Image/headers/ParseToImage.h"
#include "../Image/headers/ParseFromImage.h"
#include "../Debug/headers/Debug.h"

#include <string>
#include <functional>
#include <mutex>

#define PICTOBYTE_EXPORTS

namespace {
    // Internal logging callback function pointer
    std::function<void(const char*)> g_logCallback = nullptr;
    std::mutex g_callbackMutex;

    // Version string
    const char* VERSION = "1.0.4";

    // Wrapper for calling the callback if it's set
    void LogMessage(const std::string& message) {
        std::lock_guard<std::mutex> lock(g_callbackMutex);
        if (g_logCallback) {
            // Format message for better spacing and readability
            std::string formattedMessage = formatMessage(message);
            g_logCallback(formattedMessage.c_str());
        }
    }
}

namespace PictoByteConverter {

bool FileToImage(
    const std::string& inputFilePath,
    const std::string& outputFilePath,
    int maxThreads,
    int maxMemoryMB,
    int maxChunkSizeMB,
    bool debugMode
) {
    // Set debug mode using the setDebugMode function
    setDebugMode(debugMode);
    
    try {
        if (debugMode) {
            LogMessage("Starting file-to-image conversion");
            LogMessage("Input file: " + inputFilePath);
            LogMessage("Output base name: " + outputFilePath);
            LogMessage("Resources: " + 
                      (maxThreads == 0 ? "Auto" : std::to_string(maxThreads)) + " threads, " + 
                      std::to_string(maxMemoryMB) + " MB memory, " + 
                      std::to_string(maxChunkSizeMB) + " MB chunk size");
        }
        
        bool result = parseToImage(
            inputFilePath,
            outputFilePath,
            maxChunkSizeMB,  
            maxThreads,      
            maxMemoryMB      
        );
        
        if (debugMode) {
            if (result) {
                LogMessage("FileToImage operation completed successfully");
            } else {
                LogMessage("FileToImage operation failed");
            }
        }
        
        return result;
    }
    catch (const std::exception& e) {
        if (debugMode) {
            LogMessage("Exception in FileToImage: " + std::string(e.what()));
        }
        return false;
    }
}

bool ImageToFile(
    const std::string& inputFilePath,
    const std::string& outputDirectory,
    int maxThreads,
    int maxMemoryMB,
    bool debugMode
) {
    // Set debug mode using the setDebugMode function
    setDebugMode(debugMode);
    
    try {
        if (debugMode) {
            LogMessage("Starting image-to-file extraction");
            LogMessage("Input image: " + inputFilePath);
            LogMessage("Output directory: " + (outputDirectory.empty() ? "Current directory" : outputDirectory));
            LogMessage("Resources: " + 
                      (maxThreads == 0 ? "Auto" : std::to_string(maxThreads)) + " threads, " + 
                      std::to_string(maxMemoryMB) + " MB memory");
        }
        
        // parseFromImage returns void, so we can't assign it to a bool result
        parseFromImage(
            inputFilePath,
            outputDirectory,
            maxThreads,
            maxMemoryMB
        );
        
        if (debugMode) {
            LogMessage("ImageToFile operation completed successfully");
        }
        
        return true; // If no exception was thrown, consider it successful
    }
    catch (const std::exception& e) {
        if (debugMode) {
            LogMessage("Exception in ImageToFile: " + std::string(e.what()));
        }
        return false;
    }
}

void SetLogCallback(void (*callback)(const char* message)) {
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    if (callback) {
        g_logCallback = callback;
    } else {
        g_logCallback = nullptr;
    }
}

const char* GetLibraryVersion() {
    return VERSION;
}

} // namespace PictoByteConverter
