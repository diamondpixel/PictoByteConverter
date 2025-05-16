#include "Image/headers/ParseToImage.h"
#include "Image/headers/ParseFromImage.h"
#include "Debug/headers/Debug.h"
#include <filesystem>
#include <iostream>
#include <string>

using namespace std;

void printUsage() {
    printStatus("Usage: ConvertToImage [options]");
    std::cout << "Options:" << std::endl;
    std::cout << "  --debug            Enable debug output mode" << std::endl;
    std::cout << "  --mode=<mode>      Select operation mode (0: File to Image, 1: Image to File)" << std::endl;
    std::cout << "  --input=<file>     Specify input file" << std::endl;
    std::cout << "  --output=<file>    Specify output file" << std::endl;
    std::cout << "  --maxCPU=<num>     Maximum number of CPU threads to use (default: auto)" << std::endl;
    std::cout << "  --maxMemory=<MB>   Maximum memory to use in MB (default: 1024)" << std::endl;
    std::cout << "  --maxChunkSize=<MB> Maximum chunk size in MB (default: 9)" << std::endl;
    std::cout << "  --help             Display this help message" << std::endl;
}

int main(int argc, char* argv[]) {
    int mode = -1; // Default: File to Image
    string inputFile;  // Path on D: drive
    string outputFile;  // Path on D: drive
    bool debugMode = false;
    int maxThreads = 0;  // Default: auto (determined by system)
    int maxMemoryMB = 1024;  // Default: 1GB
    int maxChunkSizeMB = 9;  // Default: 9MB per chunk
    
    // First pass to check for debug mode
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--debug") {
            debugMode = true;
            break;
        }
    }
    
    // Ensure debug mode is globally visible with the strongest memory ordering
    // Sequential consistency guarantees all threads see the same value
    gDebugMode.store(debugMode, std::memory_order_seq_cst);
    
    // Set environment variable for child processes
    if (debugMode) {
        #ifdef _WIN32
        _putenv_s("DEBUG", "1");
        #else
        setenv("DEBUG", "1", 1);
        #endif
    }
    
    // Parse command-line arguments
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];

        if (arg == "--debug") {
            // Skip debug mode argument as it's already handled
            continue;
        }
        else if (arg == "--mode" || arg == "-m") {
            if (i + 1 < argc) {
                string modeStr = argv[i + 1];
                if (modeStr == "0" || modeStr == "file2img" || modeStr == "toimage") {
                    mode = 0;
                } else if (modeStr == "1" || modeStr == "img2file" || modeStr == "fromimage") {
                    mode = 1;
                } else {
                    printError("Invalid mode: " + modeStr);
                    printUsage();
                    return 1;
                }
                i++; // Skip the value
            }
        }
        else if (arg.substr(0, 7) == "--mode=") {
            string modeStr = arg.substr(7);
            if (modeStr == "0") mode = 0;
            else if (modeStr == "1") mode = 1;
            else {
                printError("Invalid mode: " + modeStr);
                printUsage();
                return 1;
            }
        }
        else if (arg.substr(0, 8) == "--input=") {
            inputFile = arg.substr(8);
        }
        else if (arg.substr(0, 9) == "--output=") {
            outputFile = arg.substr(9);
        }
        else if (arg.substr(0, 9) == "--maxCPU=") {
            try {
                maxThreads = std::stoi(arg.substr(9));
                if (maxThreads <= 0) {
                    printWarning("Invalid maxCPU value: " + std::to_string(maxThreads) + " (must be >= 0)");
                    maxThreads = 0; // Auto
                }
            } catch (const std::exception& e) {
                printWarning("Invalid maxCPU value, using default");
                maxThreads = 0;
            }
        }
        else if (arg.substr(0, 12) == "--maxMemory=") {
            try {
                maxMemoryMB = std::stoi(arg.substr(12));
                if (maxMemoryMB < 64) {
                    printWarning("Invalid maxMemory value: " + std::to_string(maxMemoryMB) + " MB (must be >= 64 MB)");
                    maxMemoryMB = 1024; // Default: 1GB
                }
            } catch (const std::exception& e) {
                printWarning("Invalid maxMemory value, using default (1024 MB)");
                maxMemoryMB = 1024;
            }
        }
        else if (arg.length() >= 15 && arg.substr(0, 15) == "--maxChunkSize=") {
            try {
                maxChunkSizeMB = std::stoi(arg.substr(15));
                if (maxChunkSizeMB < 1) {
                    printWarning("Invalid maxChunkSize value: " + std::to_string(maxChunkSizeMB) + " MB (must be >= 1 MB)");
                    maxChunkSizeMB = 9; // Default: 9MB
                } else if (maxChunkSizeMB > 50) {
                    printWarning("Large chunk sizes (>50 MB) may lead to memory issues");
                }
            } catch (const std::exception& e) {
                printWarning("Invalid maxChunkSize value, using default (9 MB)");
                maxChunkSizeMB = 9;
            }
        }
        else if (arg == "--help") {
            printUsage();
            return 0;
        }
        else {
            printError("Unknown option: " + arg);
            printUsage();
            return 1;
        }
    }
    
    // Display current working directory
    // printFilePath("Current working directory: " + std::filesystem::current_path().string());
    
    // Show operation mode in color
    if (mode == 0) {
        printStatus("Mode: File to Image");
    } else {
        printStatus("Mode: Image to File");
    }
    
    // Show debug status if enabled
    if (debugMode) {
        printStatus("Debug mode: Enabled");
    }
    
    // Show resource limits
    // printStats("Resource limits: " + 
    //             (maxThreads == 0 ? "Auto" : std::to_string(maxThreads)) + " threads, " + 
    //             std::to_string(maxMemoryMB) + " MB memory, " + 
    //             std::to_string(maxChunkSizeMB) + " MB max chunk size");

    bool success = false;
    switch (mode) {
        case 0:
            printProcessingStep("Converting file to image...");
            printFilePath("Input: " + inputFile);
            printFilePath("Output: " + outputFile);
            success = parseToImage(inputFile, outputFile, maxChunkSizeMB, maxThreads, maxMemoryMB);
            if (success) {
                printSuccess("File successfully converted to image!");
            }
            break;
        case 1:
            printProcessingStep("Extracting file from image...");
            if (!outputFile.empty()) {
                printFilePath("Input: " + inputFile);
                printFilePath("Output: " + outputFile);
                parseFromImage(inputFile, outputFile, maxThreads, maxMemoryMB);
            } else {
                printFilePath("Input: " + inputFile);
                parseFromImage(inputFile, "", maxThreads, maxMemoryMB);
            }
            // Success message is already printed in parseFromImage function
            // printSuccess("File successfully extracted from image!");
            break;
        default:
            printError("No valid mode selected");
            printUsage();
            return 1;
    }

    return 0;
}
