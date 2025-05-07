#include "Image/headers/ParseToImage.h"
#include "Image/headers/ParseFromImage.h"
#include "Debug/headers/Debug.h"
#include <filesystem>
#include <iostream>
#include <string>

using namespace std;

void printUsage() {
    cout << "Usage: ConvertToImage [options]" << endl;
    cout << "Options:" << endl;
    cout << "  --debug            Enable debug output mode" << endl;
    cout << "  --mode=<mode>      Select operation mode (0: File to Image, 1: Image to File)" << endl;
    cout << "  --input=<file>     Specify input file" << endl;
    cout << "  --output=<file>    Specify output file" << endl;
    cout << "  --maxCPU=<num>     Maximum number of CPU threads to use (default: auto)" << endl;
    cout << "  --maxMemory=<MB>   Maximum memory to use in MB (default: 1024)" << endl;
    cout << "  --maxChunkSize=<MB> Maximum chunk size in MB (default: 9)" << endl;
    cout << "  --help             Display this help message" << endl;
}

int main(int argc, char* argv[]) {
    int mode = -1; // Default: File to Image
    string inputFile;  // Path on D: drive
    string outputFile;  // Path on D: drive
    bool debugMode = false;
    int maxThreads = 0;  // Default: auto (determined by system)
    int maxMemoryMB = 1024;  // Default: 1GB
    int maxChunkSizeMB = 9;  // Default: 9MB per chunk
    
    // Parse command-line arguments
    printMessage("Parsing " + std::to_string(argc) + " command line arguments");
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        printMessage("Processing argument: " + arg);
        
        if (arg == "--debug") {
            debugMode = true;
        }
        else if (arg.substr(0, 7) == "--mode=") {
            string modeStr = arg.substr(7);
            if (modeStr == "0") mode = 0;
            else if (modeStr == "1") mode = 1;
            else {
                cerr << "Invalid mode: " << modeStr << endl;
                printUsage();
                return 1;
            }
        }
        else if (arg.substr(0, 8) == "--input=") {
            inputFile = arg.substr(8);
            printMessage("Set input file to: " + inputFile);
        }
        else if (arg.substr(0, 9) == "--output=") {
            outputFile = arg.substr(9);
            printMessage("Set output file to: " + outputFile);
        }
        else if (arg.substr(0, 9) == "--maxCPU=") {
            try {
                maxThreads = std::stoi(arg.substr(9));
                printMessage("Set max CPU threads to: " + std::to_string(maxThreads));
                if (maxThreads < 0) {
                    cerr << "Invalid maxCPU value: " << maxThreads << " (must be >= 0)" << endl;
                    maxThreads = 0; // Reset to default
                }
            } catch (const std::exception& e) {
                cerr << "Invalid maxCPU value, using default" << endl;
                maxThreads = 0;
            }
        }
        else if (arg.substr(0, 12) == "--maxMemory=") {
            try {
                maxMemoryMB = std::stoi(arg.substr(12));
                printMessage("Set max memory to: " + std::to_string(maxMemoryMB) + " MB");
                if (maxMemoryMB < 64) {
                    cerr << "Invalid maxMemory value: " << maxMemoryMB << " MB (must be >= 64 MB)" << endl;
                    maxMemoryMB = 1024; // Reset to default
                }
            } catch (const std::exception& e) {
                cerr << "Invalid maxMemory value, using default (1024 MB)" << endl;
                maxMemoryMB = 1024;
            }
        }
        else if (arg.substr(0, 14) == "--maxChunkSize=") {
            try {
                maxChunkSizeMB = std::stoi(arg.substr(14));
                printMessage("Set max chunk size to: " + std::to_string(maxChunkSizeMB) + " MB");
                if (maxChunkSizeMB < 1) {
                    cerr << "Invalid maxChunkSize value: " << maxChunkSizeMB << " MB (must be >= 1 MB)" << endl;
                    maxChunkSizeMB = 9; // Reset to default
                }
                if (maxChunkSizeMB > 50) {
                    cerr << "Warning: Large chunk sizes (>" << 50 << " MB) may lead to memory issues." << endl;
                }
            } catch (const std::exception& e) {
                cerr << "Invalid maxChunkSize value, using default (9 MB)" << endl;
                maxChunkSizeMB = 9;
            }
        }
        else if (arg == "--help") {
            printUsage();
            return 0;
        }
        else {
            cerr << "Unknown option: " << arg << endl;
            printUsage();
            return 1;
        }
    }
    
    // Set debug mode based on command line flag
    setDebugMode(debugMode);
    
    // Display current working directory
    cout << "Current working directory: " << std::filesystem::current_path().string() << endl;
    cout << "Mode: " << (mode == 0 ? "File to Image" : "Image to File") << endl;
    if (debugMode) {
        cout << "Debug mode: Enabled" << endl;
    }
    cout << "Resource limits: " << (maxThreads == 0 ? "Auto" : std::to_string(maxThreads)) << " threads, " 
         << maxMemoryMB << " MB memory, " << maxChunkSizeMB << " MB max chunk size" << endl;

    switch (mode) {
        case 0:
            cout << "Converting file to image..." << endl;
            cout << "Input: " << inputFile << ", Output: " << outputFile << endl;
            parseToImage(inputFile, outputFile, maxChunkSizeMB, maxThreads, maxMemoryMB);  // Use maxChunkSizeMB instead of hardcoded 9
            break;
        case 1:
            cout << "Extracting file from image..." << endl;
            cout << "Input image: " << inputFile << endl;
            if (!outputFile.empty()) {
                cout << "Output path: " << outputFile << endl;
                parseFromImage(inputFile, outputFile, maxThreads, maxMemoryMB);
            } else {
                parseFromImage(inputFile, "", maxThreads, maxMemoryMB);
            }
            break;
        default:
            return 1;
    }

    return 0;
}
