#ifndef DEBUG_H
#define DEBUG_H

#include <string>
#include <iostream>
#include <mutex>

// Global debug mode flag - set to false by default for clean output
extern bool gDebugMode;

// Mutex for synchronized console output
extern std::mutex gConsoleMutex;

/**
 * String utility function to safely convert const char* to std::string
 * 
 * @param textPtr Pointer to the text to convert
 * @return Safe string representation
 */
inline std::string makeSafeString(const char* textPtr) {
    return textPtr ? std::string(textPtr) : std::string("(null)");
}

/**
 * String utility function to handle std::string
 * 
 * @param textStr String to process
 * @return The same string (already safe)
 */
inline std::string makeSafeString(const std::string& textStr) {
    return textStr; // Already a string, just return it
}

/**
 * Print a message to the console only if debug mode is enabled
 * If force is true, print regardless of debug mode
 * 
 * @param messageText The message to print
 * @param forceOutput Whether to force printing even if debug mode is disabled
 */
inline void debugPrint(const std::string& messageText, bool forceOutput = false) {
    if (gDebugMode || forceOutput) {
        std::lock_guard<std::mutex> lock(gConsoleMutex);
        std::cout << messageText << std::endl;
    }
}

/**
 * Set debug mode on or off
 * 
 * @param isEnabled Whether to enable debug mode
 */
inline void setDebugMode(bool isEnabled) {
    gDebugMode = isEnabled;
}

/**
 * Get current debug mode state
 * 
 * @return Whether debug mode is enabled
 */
inline bool getDebugMode() {
    return gDebugMode;
}

/**
 * Print function for debug messages (only prints in debug mode)
 * 
 * @param messageText The message to print
 */
inline void printMessage(const std::string& messageText) {
    debugPrint(messageText, false);
}

/**
 * Print function for error messages (always shown)
 * 
 * @param errorText The error message to print
 */
inline void printError(const std::string& errorText) {
    debugPrint("Error: " + errorText, true);
}

/**
 * Print function for important status messages (always shown)
 * 
 * @param statusText The status message to print
 */
inline void printStatus(const std::string& statusText) {
    debugPrint(statusText, true);
}

/**
 * Print function for warnings (always shown)
 * 
 * @param warningText The warning message to print
 */
inline void printWarning(const std::string& warningText) {
    debugPrint("Warning: " + warningText, true);
}

#endif // DEBUG_H 