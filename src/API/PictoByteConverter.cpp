#include "headers/PictoByteConverter.h"
#include "../Image/headers/ParseToImage.h"
#include "../Image/headers/ParseFromImage.h"
#include "../Debug/headers/Debug.h"
#include "../Image/headers/ResourceManager.h"

#include <string>
#include <functional>
#include <mutex>

#define PICTOBYTE_EXPORTS

namespace {
    // Internal logging callback function pointer
    std::function<void(const char*)> g_logCallback = nullptr;
    std::mutex g_callbackMutex;

    // Version string
    const char* VERSION = "1.0.0";

    // Wrapper for calling the callback if it's set
    void LogMessage(const std::string& message) {
        std::lock_guard<std::mutex> lock(g_callbackMutex);
        if (g_logCallback) {
            g_logCallback(message.c_str());
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
        bool result = parseToImage(
            inputFilePath,
            outputFilePath,
            maxChunkSizeMB,  
            maxThreads,      
            maxMemoryMB      
        );
        
        if (debugMode) {
            LogMessage("FileToImage operation " + std::string(result ? "succeeded" : "failed"));
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
        // parseFromImage returns void, so we can't assign it to a bool result
        parseFromImage(
            inputFilePath,
            outputDirectory,
            maxThreads,
            maxMemoryMB
        );
        
        if (debugMode) {
            LogMessage("ImageToFile operation succeeded");
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
