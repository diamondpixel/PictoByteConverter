#include "headers/ResourceManager.h"
#include <iostream> 
#include "../Debug/headers/LogBufferManager.h"  
#include "../../Debug/headers/LogBuffer.h"         
#include "../../Debug/headers/Debug.h"             

// Static member definitions
ResourceManager* ResourceManager::instance = nullptr;
std::mutex ResourceManager::singleton_mutex; 

ResourceManager& ResourceManager::getInstance() {
    std::lock_guard<std::mutex> lock(singleton_mutex);
    if (!instance) {
        instance = new ResourceManager();
    }
    return *instance;
}

// Definition of the debugLog method
void ResourceManager::debugLog(const std::string &message) {
    // Check the thread-local flag from LogBufferManager
    if (debug::LogBufferManager::tls_inside_resource_manager_log_append) {
        return; // Avoid recursive logging if we're already in a ResourceManager log append operation
    }

    if (getDebugMode()) { // Ensure logging only happens if debug mode is active
        debug::LogBufferManager::getInstance().appendTo(
            "ResourceManager",
            message,
            debug::LogContext::Debug);
    }
}
