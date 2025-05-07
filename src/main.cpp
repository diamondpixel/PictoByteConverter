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
    cout << "  --debug           Enable debug output mode" << endl;
    cout << "  --mode=<mode>     Select operation mode (0: File to Image, 1: Image to File)" << endl;
    cout << "  --input=<file>    Specify input file" << endl;
    cout << "  --output=<file>   Specify output file" << endl;
    cout << "  --help            Display this help message" << endl;
}

int main(int argc, char* argv[]) {
    int mode = -1; // Default: File to Image
    string inputFile;  // Path on D: drive
    string outputFile;  // Path on D: drive
    bool debugMode = false;
    
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

    switch (mode) {
        case 0:
            cout << "Converting file to image..." << endl;
            cout << "Input: " << inputFile << ", Output: " << outputFile << endl;
            parseToImage(inputFile, outputFile, 9);  // Maximum 9MB per chunk
            break;
        case 1:
            cout << "Extracting file from image..." << endl;
            cout << "Input image: " << inputFile << endl;
            if (!outputFile.empty()) {
                cout << "Output path: " << outputFile << endl;
                parseFromImage(inputFile, outputFile);
            } else {
                parseFromImage(inputFile);
            }
            break;
        default:
            return 1;
    }

    return 0;
}
