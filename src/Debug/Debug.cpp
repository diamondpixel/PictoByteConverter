// Minimal debug implementation
#include "headers/Debug.h"

// Global debug mode flag
bool gDebugMode = false;

// Global mutex for thread-safe printing
std::mutex gConsoleMutex; 