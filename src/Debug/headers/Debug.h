#ifndef DEBUG_H
#define DEBUG_H

#include <string>
#include <iostream>
#include <mutex>
#include <atomic>

// Global debug mode flag - atomic for thread safety
extern std::atomic<bool> gDebugMode;

// Mutex for synchronized console output
extern std::mutex gConsoleMutex;

// Color enum for Windows-compatible console coloring
enum class ANSIColor {
    BLACK,
    GRAY,
    BRIGHT_RED,
    RED,
    BRIGHT_GREEN,
    GREEN,
    BRIGHT_BLUE,
    BLUE,
    BRIGHT_CYAN,
    CYAN,
    BRIGHT_YELLOW,
    YELLOW,
    BRIGHT_MAGENTA,
    MAGENTA,
    BRIGHT_WHITE,
    WHITE,
    RESET,
    BG_BLUE,
    BG_MAGENTA,
    BOLD
};

// Windows console color enum (internal use)
enum class ConsoleColor {
    BLACK = 0,
    BLUE = 1,
    GREEN = 2,
    CYAN = 3,
    RED = 4,
    MAGENTA = 5,
    YELLOW = 6,
    WHITE = 7,
    GRAY = 8,
    BRIGHT_BLUE = 9,
    BRIGHT_GREEN = 10,
    BRIGHT_CYAN = 11,
    BRIGHT_RED = 12,
    BRIGHT_MAGENTA = 13,
    BRIGHT_YELLOW = 14,
    BRIGHT_WHITE = 15
};

// Legacy ANSI color code constants namespace
namespace ANSIColorConst {
    const std::string RESET   = "\033[0m";
    const std::string RED     = "\033[31m";
    const std::string GREEN   = "\033[32m";
    const std::string YELLOW  = "\033[33m";
    const std::string BLUE    = "\033[34m";
    const std::string MAGENTA = "\033[35m";
    const std::string CYAN    = "\033[36m";
    const std::string WHITE   = "\033[37m";
    const std::string BOLD    = "\033[1m";
    
    // Adding more color options
    const std::string BRIGHT_RED     = "\033[91m";
    const std::string BRIGHT_GREEN   = "\033[92m";
    const std::string BRIGHT_YELLOW  = "\033[93m";
    const std::string BRIGHT_BLUE    = "\033[94m";
    const std::string BRIGHT_MAGENTA = "\033[95m";
    const std::string BRIGHT_CYAN    = "\033[96m";
    const std::string BRIGHT_WHITE   = "\033[97m";
    
    // Background colors
    const std::string BG_BLACK   = "\033[40m";
    const std::string BG_RED     = "\033[41m";
    const std::string BG_GREEN   = "\033[42m";
    const std::string BG_YELLOW  = "\033[43m";
    const std::string BG_BLUE    = "\033[44m";
    const std::string BG_MAGENTA = "\033[45m";
    const std::string BG_CYAN    = "\033[46m";
    const std::string BG_WHITE   = "\033[47m";
    
    // Text effects
    const std::string UNDERLINE  = "\033[4m";
    const std::string ITALIC     = "\033[3m";  // Not supported in all terminals
}

/**
 * String utility function to safely convert const char* to std::string
 * 
 * @param textPtr Pointer to the text to convert
 * @return Safe string representation
 */
std::string makeSafeString(const char* textPtr);

/**
 * String utility function to handle std::string
 * 
 * @param textStr String to process
 * @return The same string (already safe)
 */
std::string makeSafeString(const std::string& textStr);

/**
 * Format message with proper spacing between steps
 */
std::string formatMessage(const std::string& message);

/**
 * Core debug print function with color support
 * 
 * @param messageText Text to print
 * @param forceOutput Whether to force output regardless of debug mode
 * @param color ANSI color code to use
 */
void debugPrint(const std::string& messageText, bool forceOutput, const ANSIColor color);

/**
 * Set debug mode on or off
 * Uses memory_order_release to ensure visibility to other threads
 * 
 * @param isEnabled Whether to enable debug mode
 */
void setDebugMode(bool isEnabled);

/**
 * Get current debug mode state
 * Uses memory_order_acquire to ensure visibility from other threads
 * 
 * @return Whether debug mode is enabled
 */
bool getDebugMode();

/**
 * Print function for status messages 
 * @param statusText Status message text
 * @param forceOutput Forces the message to display even if debug mode is off
 */
void printStatus(const std::string& statusText, bool forceOutput = false);
void printStatus(const char* statusText, bool forceOutput = false);

/**
 * Print function for warnings
 * @param warningText Warning message text
 * @param forceOutput Forces the message to display even if debug mode is off
 */
void printWarning(const std::string& warningText, bool forceOutput = false);
void printWarning(const char* warningText, bool forceOutput = false); 

/**
 * Print function for error messages
 * @param errorText Error message text
 * @param forceOutput Forces the message to display even if debug mode is off
 */
void printError(const std::string& errorText, bool forceOutput = true);
void printError(const char* errorText, bool forceOutput = true);

/**
 * Print function for debug messages
 * @param debugText Debug message text
 * @param forceOutput Forces the message to display even if debug mode is off
 */
void printDebug(const std::string& debugText, bool forceOutput = false);
void printDebug(const char* debugText, bool forceOutput = false);

/**
 * Print function for success messages
 * @param successText Success message text
 * @param forceOutput Forces the message to display even if debug mode is off
 */
void printSuccess(const std::string& successText, bool forceOutput = true);
void printSuccess(const char* successText, bool forceOutput = true);

/**
 * Print function for file paths
 * @param pathText Path message text
 * @param forceOutput Forces the message to display even if debug mode is off
 */
void printFilePath(const std::string& pathText, bool forceOutput = true);
void printFilePath(const char* pathText, bool forceOutput = true);

/**
 * Print function for processing steps
 * @param stepText Processing step description
 * @param forceOutput Forces the message to display even if debug mode is off
 */
void printProcessingStep(const std::string& stepText, bool forceOutput = true);
void printProcessingStep(const char* stepText, bool forceOutput = true);

/**
 * Print function for data statistics
 * @param statsText Statistics message text
 * @param forceOutput Forces the message to display even if debug mode is off
 */
void printStats(const std::string& statsText, bool forceOutput = true);
void printStats(const char* statsText, bool forceOutput = true);

/**
 * Print function for memory statistics
 * @param statsText Statistics message text
 * @param forceOutput Forces the message to display even if debug mode is off
 */
void printMemStats(const std::string& statsText, bool forceOutput = true);
void printMemStats(const char* statsText, bool forceOutput = true);

/**
 * Print function for highlighting important information
 * @param infoText Information text
 * @param forceOutput Forces the message to display even if debug mode is off
 */
void printHighlight(const std::string& infoText, bool forceOutput = true);
void printHighlight(const char* infoText, bool forceOutput = true);

/**
 * Print function for completion messages with optional byte count
 * 
 * @param taskText The completed task description
 * @param bytesProcessed Number of bytes processed (optional)
 * @param forceOutput Forces the message to display even if debug mode is off
 */
void printCompletion(const std::string& taskText, size_t bytesProcessed, bool forceOutput = true);
void printCompletion(const char* taskText, size_t bytesProcessed, bool forceOutput = true);

/**
 * Print function for generic messages
 * 
 * @param messageText The message text
 * @param forceOutput Forces the message to display even if debug mode is off
 */
void printMessage(const std::string& messageText, bool forceOutput = false);
void printMessage(const char* messageText, bool forceOutput = false);

/**
 * Print function for memory statistics with byte count
 * 
 * @param text The descriptive text
 * @param bytes The number of bytes
 * @param force_output Forces the message to display even if debug mode is off
 */
void printMemoryStats(const std::string& text, size_t bytes, bool force_output = false);
void printMemoryStats(const char* text, size_t bytes, bool force_output = false);

// Message queue processing function for thread-safe console output
// Used internally by debug print functions
void processMessageQueue();

// Internal function to print processed messages with proper formatting
// Do not call directly - this is used by the message queue system
void printProcessedMessage(const std::string& message, ConsoleColor color);

#endif // DEBUG_H