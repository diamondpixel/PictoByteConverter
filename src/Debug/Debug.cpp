// Minimal debug implementation
#include "headers/Debug.h"
#include "../Image/headers/ResourceManager.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <mutex>
#include <atomic>  // Added for atomic flag
#include <iomanip>
#include <vector>  // Added for vector of extensions
#include <windows.h> // Added for Windows-specific console color support
#include <queue>    // Added for message queue
#include <sstream>  // Added for stringstream
#include <thread>   // Added for thread_local

// Forward declarations
std::string formatDataSize(size_t bytes);
void processMessageQueue();

/**
 * Format a data size in human-readable format (KB, MB, etc.)
 */
std::string formatDataSize(size_t bytes) {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(2);
    
    if (bytes < 1024) {
        ss << bytes << " bytes";
    } else if (bytes < 1024 * 1024) {
        ss << (bytes / 1024.0) << " KB";
    } else if (bytes < 1024 * 1024 * 1024) {
        ss << (bytes / (1024.0 * 1024.0)) << " MB";
    } else {
        ss << (bytes / (1024.0 * 1024.0 * 1024.0)) << " GB";
    }
    
    return ss.str();
}

// Global debug mode flag as atomic for thread safety - explicitly initialized
std::atomic<bool> gDebugMode{false};

// Global mutex for thread-safe printing
std::mutex gConsoleMutex;
std::mutex gQueueMutex;

// Message queue for buffered output
struct ColoredMessage {
    std::string text;
    ConsoleColor color;
    bool forceOutput;
};
std::queue<ColoredMessage> gMessageQueue;

// Thread-local buffer for collecting related messages
thread_local std::stringstream gThreadLocalBuffer;
thread_local ConsoleColor gThreadLocalColor = ConsoleColor::WHITE;
thread_local bool gThreadLocalForce = false;

// Global handle to the console output
static HANDLE g_consoleHandle = GetStdHandle(STD_OUTPUT_HANDLE);

// Default console attribute (save for restoring)
static WORD g_defaultConsoleAttributes = 0;

// Initialize console attributes
static bool initConsole() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(g_consoleHandle, &csbi)) {
        g_defaultConsoleAttributes = csbi.wAttributes;
        return true;
    }
    return false;
}

// Initialize console on startup
static bool g_consoleInitialized = initConsole();

// Set console text color
void setConsoleColor(ConsoleColor textColor, bool bold = false) {
    WORD attributes = (static_cast<WORD>(textColor) & 0x0F);
    if (bold || static_cast<int>(textColor) >= 8) {
        attributes |= FOREGROUND_INTENSITY;
    }
    SetConsoleTextAttribute(g_consoleHandle, attributes);
}

// Reset console to default colors
void resetConsoleColor() {
    if (g_consoleInitialized) {
        SetConsoleTextAttribute(g_consoleHandle, g_defaultConsoleAttributes);
    }
}

// Convert ANSIColor to Windows console attributes
WORD getWinColorAttribute(ANSIColor color) {
    WORD attributes = 0;
    
    switch (color) {
        case ANSIColor::BLACK:
            attributes = 0;
            break;
        case ANSIColor::BLUE:
            attributes = FOREGROUND_BLUE;
            break;
        case ANSIColor::GREEN:
            attributes = FOREGROUND_GREEN;
            break;
        case ANSIColor::CYAN:
            attributes = FOREGROUND_GREEN | FOREGROUND_BLUE;
            break;
        case ANSIColor::RED:
            attributes = FOREGROUND_RED;
            break;
        case ANSIColor::MAGENTA:
            attributes = FOREGROUND_RED | FOREGROUND_BLUE;
            break;
        case ANSIColor::YELLOW:
            attributes = FOREGROUND_RED | FOREGROUND_GREEN;
            break;
        case ANSIColor::WHITE:
            attributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
            break;
        case ANSIColor::GRAY:
            attributes = FOREGROUND_INTENSITY;
            break;
        case ANSIColor::BRIGHT_BLUE:
            attributes = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
            break;
        case ANSIColor::BRIGHT_GREEN:
            attributes = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
            break;
        case ANSIColor::BRIGHT_CYAN:
            attributes = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
            break;
        case ANSIColor::BRIGHT_RED:
            attributes = FOREGROUND_RED | FOREGROUND_INTENSITY;
            break;
        case ANSIColor::BRIGHT_MAGENTA:
            attributes = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
            break;
        case ANSIColor::BRIGHT_YELLOW:
            attributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
            break;
        case ANSIColor::BRIGHT_WHITE:
            attributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
            break;
        case ANSIColor::BG_BLUE:
            attributes = BACKGROUND_BLUE | BACKGROUND_INTENSITY;
            break;
        case ANSIColor::BG_MAGENTA:
            attributes = BACKGROUND_RED | BACKGROUND_BLUE | BACKGROUND_INTENSITY;
            break;
        case ANSIColor::BOLD:
            attributes = FOREGROUND_INTENSITY;
            break;
        default:
            attributes = g_defaultConsoleAttributes;
            break;
    }
    
    return attributes;
}

// String utility functions
std::string makeSafeString(const char* textPtr) {
    return textPtr ? std::string(textPtr) : std::string("(null)");
}

std::string makeSafeString(const std::string& textStr) {
    return textStr;
}

// Format message
std::string formatMessage(const std::string& message) {
    return message;
}

// Improved colored message printer that builds the entire output before displaying
std::string formatColoredMessageSegments(const std::string& message, ConsoleColor defaultColor, 
                                         std::vector<std::pair<size_t, ConsoleColor>>& colorChangePoints) {
    enum class State {
        NORMAL,
        IN_NUMBER,
        IN_EXTENSION
    };
    
    State state = State::NORMAL;
    std::string result = message;
    
    // First pass: identify color change points and store them
    for (size_t i = 0; i < message.length(); i++) {
        char c = message[i];
        
        // Handle file extensions
        if (c == '.' && state != State::IN_EXTENSION && i + 1 < message.length() && 
            (std::isalpha(message[i+1]))) {
            // This looks like a file extension, not a number
            state = State::IN_EXTENSION;
            colorChangePoints.push_back({i, ConsoleColor::BRIGHT_MAGENTA});
            continue;
        }
        
        // Exit extension mode on whitespace or certain characters
        if (state == State::IN_EXTENSION && (std::isspace(c) || c == ',' || c == ')' || c == '(' || c == ']' || c == '[')) {
            state = State::NORMAL;
            colorChangePoints.push_back({i, defaultColor});
        }
        
        // Handle numbers and their components
        if (state == State::IN_NUMBER) {
            // We're already in a number
            if (std::isdigit(c) || c == '.') {
                // Still part of the number
                continue;
            } else {
                // End of number
                state = State::NORMAL;
                colorChangePoints.emplace_back(i, defaultColor);
            }
        }
        
        // Check for start of a number
        if (state == State::NORMAL && (std::isdigit(c) || 
                                     (c == '.' && i + 1 < message.length() && std::isdigit(message[i+1])))) {
            // Starting a number (either with digit or decimal point followed by digit)
            state = State::IN_NUMBER;
            colorChangePoints.push_back({i, ConsoleColor::BRIGHT_YELLOW});
            continue;
        }
    }
    
    return result;
}

// Function to process and print the actual message with coloring
void printProcessedMessage(const std::string& message, ConsoleColor defaultColor) {
    // Process the message to find all color change points
    std::vector<std::pair<size_t, ConsoleColor>> colorChangePoints;
    std::string formattedMessage = formatColoredMessageSegments(message, defaultColor, colorChangePoints);
    
    // Sort color change points by position
    std::sort(colorChangePoints.begin(), colorChangePoints.end(), 
             [](const auto& a, const auto& b) { return a.first < b.first; });
    
    // Apply default color at the start
    setConsoleColor(defaultColor);
    
    // Print message with color changes at appropriate points
    size_t lastPos = 0;
    for (const auto& [pos, color] : colorChangePoints) {
        // Print segment up to this color change
        if (pos > lastPos) {
            std::cout << formattedMessage.substr(lastPos, pos - lastPos);
        }
        
        // Change color
        setConsoleColor(color);
        lastPos = pos;
    }
    
    // Print the remaining part of the message
    if (lastPos < formattedMessage.length()) {
        std::cout << formattedMessage.substr(lastPos);
    }
    
    // Reset color and add newline
    resetConsoleColor();
    std::cout << std::endl;
}

// Process the message queue
void processMessageQueue() {
    // Create a local copy of the queue to process
    std::queue<ColoredMessage> localQueue;
    
    // Only swap the queue if we can get the lock
    {
        // Try to lock the queue mutex, but don't block if we can't get it
        std::unique_lock<std::mutex> queueLock(gQueueMutex, std::try_to_lock);
        if (!queueLock.owns_lock() || gMessageQueue.empty()) {
            // If we can't get the lock or the queue is empty, just return
            return;
        }
        
        // We have the lock and there are messages, so swap the queue
        std::swap(localQueue, gMessageQueue);
    }
    
    // Only process messages if we have any after the swap
    if (!localQueue.empty()) {
        // Try to lock the console mutex, but don't block if we can't get it
        std::unique_lock<std::mutex> consoleLock(gConsoleMutex, std::try_to_lock);
        if (!consoleLock.owns_lock()) {
            // If we can't get the console lock, put the messages back in the queue
            std::lock_guard<std::mutex> queueLock(gQueueMutex);
            // Put the messages back at the front of the queue
            std::queue<ColoredMessage> tempQueue;
            while (!localQueue.empty()) {
                tempQueue.push(localQueue.front());
                localQueue.pop();
            }
            while (!gMessageQueue.empty()) {
                tempQueue.push(gMessageQueue.front());
                gMessageQueue.pop();
            }
            gMessageQueue = std::move(tempQueue);
            return;
        }
        
        // We have the console lock, process all messages
        while (!localQueue.empty()) {
            const auto& msg = localQueue.front();
            printProcessedMessage(msg.text, msg.color);
            localQueue.pop();
        }
    }
}

// Core debug print function - now uses the message grouping system
void debugPrint(const std::string& messageText, bool forceOutput, const ANSIColor color) {
    bool is_debug_on = gDebugMode.load(std::memory_order_acquire);
    if (is_debug_on || forceOutput) {
        ConsoleColor consoleColor;
        switch (color) {
            case ANSIColor::BRIGHT_RED:
            case ANSIColor::RED:
                consoleColor = ConsoleColor::BRIGHT_RED;
                break;
            case ANSIColor::BRIGHT_GREEN:
            case ANSIColor::GREEN:
                consoleColor = ConsoleColor::BRIGHT_GREEN;
                break;
            case ANSIColor::BRIGHT_BLUE:
            case ANSIColor::BLUE:
                consoleColor = ConsoleColor::BRIGHT_BLUE;
                break;
            case ANSIColor::BRIGHT_CYAN:
            case ANSIColor::CYAN:
                consoleColor = ConsoleColor::BRIGHT_CYAN;
                break;
            case ANSIColor::BRIGHT_YELLOW:
            case ANSIColor::YELLOW:
                consoleColor = ConsoleColor::BRIGHT_YELLOW;
                break;
            case ANSIColor::BRIGHT_MAGENTA:
            case ANSIColor::MAGENTA:
                consoleColor = ConsoleColor::BRIGHT_MAGENTA;
                break;
            case ANSIColor::BRIGHT_WHITE:
            case ANSIColor::WHITE:
                consoleColor = ConsoleColor::BRIGHT_WHITE;
                break;
            default:
                consoleColor = ConsoleColor::WHITE;
        }
        
        // Queue the message for atomic printing
        {
            std::lock_guard<std::mutex> queueLock(gQueueMutex);
            gMessageQueue.push({messageText, consoleColor, forceOutput});
        }
        
        // Process the queue in a separate call to avoid potential deadlocks
        processMessageQueue();
    }
}

// Print colored message with highlighted numbers - now just a wrapper
void printColoredMessage(const std::string& message, ConsoleColor defaultColor) {
    // Queue the message for atomic printing
    {
        std::lock_guard<std::mutex> queueLock(gQueueMutex);
        gMessageQueue.push({message, defaultColor, true}); // Force output since this is a direct call
    }
    
    // Process the queue in a separate call to avoid potential deadlocks
    processMessageQueue();
}

// Start a message group that will be printed atomically
void beginMessageGroup(ConsoleColor defaultColor /*= ConsoleColor::WHITE*/, bool forceOutput /*= false*/) {
    // Clear any existing buffer for this thread
    gThreadLocalBuffer.str("");
    gThreadLocalBuffer.clear();
    gThreadLocalColor = defaultColor;
    gThreadLocalForce = forceOutput;
}

// Add a message to the current group
void addToMessageGroup(const std::string& message) {
    gThreadLocalBuffer << message << std::endl;
}

// End the message group and queue it for output
void endMessageGroup() {
    // Only proceed if we have content
    if (gThreadLocalBuffer.str().empty()) {
        return;
    }
    
    // Queue the message
    {
        std::lock_guard<std::mutex> queueLock(gQueueMutex);
        gMessageQueue.push({gThreadLocalBuffer.str(), gThreadLocalColor, gThreadLocalForce});
    }
    
    // Process the queue in a separate call to avoid potential deadlocks
    processMessageQueue();
}

// Public debug printing methods
void printMessage(const std::string& messageText, bool forceOutput /*= false*/) {
    debugPrint(formatMessage(messageText), forceOutput, ANSIColor::WHITE);
}

void printMessage(const char* messageText, bool forceOutput /*= false*/) {
    printMessage(makeSafeString(messageText), forceOutput);
}

void printMessage(const std::string& messageText) {
    printMessage(messageText, false);
}

void printMessage(const char* messageText) {
    printMessage(messageText, false);
}

void printStatus(const std::string& statusText, bool forceOutput /*= false*/) {
    debugPrint(formatMessage(statusText), forceOutput, ANSIColor::GREEN);
}

void printStatus(const char* statusText, bool forceOutput /*= false*/) {
    printStatus(makeSafeString(statusText), forceOutput);
}

void printStatus(const std::string& statusText) {
    printStatus(statusText, false);
}

void printStatus(const char* statusText) {
    printStatus(statusText, false);
}

void printWarning(const std::string& warningText, bool forceOutput /*= false*/) {
    debugPrint(formatMessage(warningText), forceOutput, ANSIColor::YELLOW);
}

void printWarning(const char* warningText, bool forceOutput /*= false*/) {
    printWarning(makeSafeString(warningText), forceOutput);
}

void printWarning(const std::string& warningText) {
    printWarning(warningText, false);
}

void printWarning(const char* warningText) {
    printWarning(warningText, false);
}

void printError(const std::string& errorText, bool forceOutput /*= true*/) {
    debugPrint(formatMessage(errorText), forceOutput, ANSIColor::RED);
}

void printError(const char* errorText, bool forceOutput /*= true*/) {
    printError(makeSafeString(errorText), forceOutput);
}

void printError(const std::string& errorText) {
    printError(errorText, true);
}

void printError(const char* errorText) {
    printError(errorText, true);
}

void printDebug(const std::string& debugText, bool forceOutput /*= false*/) {
    debugPrint(formatMessage(debugText), forceOutput, ANSIColor::CYAN);
}

void printDebug(const char* debugText, bool forceOutput /*= false*/) {
    printDebug(makeSafeString(debugText), forceOutput);
}

void printDebug(const std::string& debugText) {
    printDebug(debugText, false);
}

void printDebug(const char* debugText) {
    printDebug(debugText, false);
}

void printSuccess(const std::string& successText, bool forceOutput) {
    debugPrint(formatMessage(successText), forceOutput, ANSIColor::BRIGHT_GREEN);
}

void printSuccess(const char* successText, bool forceOutput) {
    printSuccess(makeSafeString(successText), forceOutput);
}

void printFilePath(const std::string& pathText, bool forceOutput) {
    debugPrint(formatMessage(pathText), forceOutput, ANSIColor::BRIGHT_CYAN);
}

void printFilePath(const char* pathText, bool forceOutput) {
    printFilePath(makeSafeString(pathText), forceOutput);
}

void printProcessingStep(const std::string& stepText, bool forceOutput) {
    debugPrint("Processing: " + stepText, forceOutput, ANSIColor::BRIGHT_BLUE);
}

void printProcessingStep(const char* stepText, bool forceOutput) {
    printProcessingStep(makeSafeString(stepText), forceOutput);
}

void printHighlight(const std::string& infoText, bool forceOutput) {
    debugPrint("→ " + infoText, forceOutput, ANSIColor::BRIGHT_YELLOW);
}

void printHighlight(const char* infoText, bool forceOutput) {
    printHighlight(makeSafeString(infoText), forceOutput);
}

void printStats(const std::string& statsText, bool forceOutput) {
    debugPrint(formatMessage(statsText), forceOutput, ANSIColor::CYAN);
}

void printStats(const char* statsText, bool forceOutput) {
    printStats(makeSafeString(statsText), forceOutput);
}

void printMemStats(const std::string& statsText, bool force_output) {
    debugPrint(statsText, force_output, ANSIColor::BRIGHT_MAGENTA);
}

void printMemStats(const char* statsText, bool force_output) {
    printMemStats(makeSafeString(statsText), force_output);
}

// Debug mode control
bool getDebugMode() {
    return gDebugMode.load(std::memory_order_acquire);
}

void setDebugMode(bool isEnabled) {
    gDebugMode.store(isEnabled, std::memory_order_seq_cst);
    
    // Only print one debug mode message
    {
        std::lock_guard<std::mutex> lock(gConsoleMutex);
        
        if (isEnabled) {
            setConsoleColor(ConsoleColor::BRIGHT_CYAN);
        } else {
            setConsoleColor(ConsoleColor::BRIGHT_YELLOW);
        }
        
        std::cout << "Debug mode " << (isEnabled ? "enabled" : "disabled");
        resetConsoleColor();
        std::cout << std::endl;
        
        // Force print a test message if debug is enabled
        if (isEnabled) {
            setConsoleColor(ConsoleColor::BRIGHT_CYAN);
            std::cout << "Debug verification: If you can see detailed memory and metadata messages, debug mode is working correctly";
            resetConsoleColor();
            std::cout << std::endl;
        }
    }
}

// Implementation of printCompletion with byte count
void printCompletion(const std::string& taskText, size_t bytesProcessed, bool forceOutput) {
    std::string message = taskText;
    if (bytesProcessed > 0) {
        message += " (" + formatDataSize(bytesProcessed) + ")";
    }
    debugPrint("[OK] " + formatMessage(message), forceOutput, ANSIColor::BRIGHT_GREEN);
}

// C-string overload
void printCompletion(const char* taskText, size_t bytesProcessed, bool forceOutput) {
    printCompletion(makeSafeString(taskText), bytesProcessed, forceOutput);
}

// Implementation of printMemoryStats
void printMemoryStats(const std::string& text, size_t bytes, bool force_output) {
    std::string message = text + " (" + formatDataSize(bytes) + ")";
    debugPrint(formatMessage(message), force_output, ANSIColor::BRIGHT_MAGENTA);
}

// C-string overload for printMemoryStats
void printMemoryStats(const char* text, size_t bytes, bool force_output) {
    printMemoryStats(makeSafeString(text), bytes, force_output);
}