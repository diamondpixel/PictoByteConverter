#ifndef MEMORY_MAPPED_FILE_H
#define MEMORY_MAPPED_FILE_H

#include <string>

// Define NOMINMAX to prevent windows.h from defining min and max macros
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../../Debug/headers/Debug.h" // For printError, printWarning

/**
 * MemoryMappedFile class
 * 
 * Provides memory-mapped file access for efficient reading of large files.
 * Uses Windows API for memory mapping.
 */
class MemoryMappedFile {
public:
    MemoryMappedFile() : hFile(INVALID_HANDLE_VALUE), hMapping(NULL), pMappedView(nullptr), fileSize(0) {
    }

    ~MemoryMappedFile() {
        close();
    }

    // Disable copy operations
    MemoryMappedFile(const MemoryMappedFile &) = delete;
    MemoryMappedFile &operator=(const MemoryMappedFile &) = delete;

    // Enable move operations
    MemoryMappedFile(MemoryMappedFile &&other) noexcept
        : hFile(other.hFile), hMapping(other.hMapping), pMappedView(other.pMappedView), fileSize(other.fileSize) {
        other.hFile = INVALID_HANDLE_VALUE;
        other.hMapping = nullptr;
        other.pMappedView = nullptr;
        other.fileSize = 0;
    }

    MemoryMappedFile &operator=(MemoryMappedFile &&other) noexcept {
        if (this != &other) {
            close();
            hFile = other.hFile;
            hMapping = other.hMapping;
            pMappedView = other.pMappedView;
            fileSize = other.fileSize;

            other.hFile = INVALID_HANDLE_VALUE;
            other.hMapping = nullptr;
            other.pMappedView = nullptr;
            other.fileSize = 0;
        }
        return *this;
    }

    bool open(const std::string &filePath) {
        close();
        hFile = CreateFileA(
            filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            printError("MemoryMappedFile: CreateFile failed. Error code: " + std::to_string(GetLastError()));
            return false;
        }
        LARGE_INTEGER liFileSize;
        if (!GetFileSizeEx(hFile, &liFileSize)) {
            printError("MemoryMappedFile: GetFileSizeEx failed. Error code: " + std::to_string(GetLastError()));
            CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; return false;
        }
        fileSize = static_cast<size_t>(liFileSize.QuadPart);
        if (fileSize == 0) {
            printWarning("MemoryMappedFile: Input file is empty. Closing file handle without mapping.");
            CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; return true;
        }
        hMapping = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (hMapping == nullptr) {
            printError("MemoryMappedFile: CreateFileMapping failed. Error code: " + std::to_string(GetLastError()));
            CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; return false;
        }
        pMappedView = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
        if (pMappedView == nullptr) {
            printError("MemoryMappedFile: MapViewOfFile failed. Error code: " + std::to_string(GetLastError()));
            if (hMapping) CloseHandle(hMapping); hMapping = nullptr;
            CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; return false;
        }
        return true;
    }

    void close() {
        if (pMappedView) { UnmapViewOfFile(pMappedView); pMappedView = nullptr; }
        if (hMapping) { CloseHandle(hMapping); hMapping = nullptr; }
        if (hFile != INVALID_HANDLE_VALUE) { CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; }
        fileSize = 0;
    }

    [[nodiscard]] const unsigned char *getData() const { return static_cast<const unsigned char *>(pMappedView); }
    [[nodiscard]] size_t getSize() const { return fileSize; }
    [[nodiscard]] bool isOpen() const { return pMappedView != nullptr || (hFile == INVALID_HANDLE_VALUE && fileSize == 0); }

private:
    HANDLE hFile;
    HANDLE hMapping;
    void *pMappedView;
    size_t fileSize;
};

#endif // MEMORY_MAPPED_FILE_H
