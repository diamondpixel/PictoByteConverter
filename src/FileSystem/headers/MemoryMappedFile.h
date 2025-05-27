#ifndef MEMORY_MAPPED_FILE_H
#define MEMORY_MAPPED_FILE_H

#include <string>
#include <cstdint> // For uintptr_t and size_t

// Define NOMINMAX to prevent windows.h from defining min and max macros
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "Debug/headers/LogMacros.h"

// Define error codes for MemoryMappedFile operations
struct MmfError {
    static constexpr int Success = 0;
    static constexpr int FileOpenFailed = 1;         // CreateFileA failed
    static constexpr int FileSizeQueryFailed = 2;    // GetFileSizeEx failed
    static constexpr int FileIsEmpty = 3;            // File is empty, opened for read
    static constexpr int FileMappingFailed = 4;      // CreateFileMappingA failed
    static constexpr int MapViewFailed = 5;          // MapViewOfFile failed
    static constexpr int InvalidArguments = 6;       // e.g., zero size for writable map
    static constexpr int AlreadyOpen = 7;            // Attempted to open an already open MMF
    static constexpr int NotOpen = 8;                // Attempted operation on a closed MMF
    static constexpr int SetFileSizeFailed = 9;      // Failed to set file pointer/end of file for new size
};

// Define access modes for opening the memory-mapped file
enum class MmfAccessMode {
    ReadOnly,         // Open existing file for reading
    ReadWrite,        // Open existing file for reading and writing
    WriteCreate       // Create new or overwrite existing file for writing (and reading)
};

/**
 * MemoryMappedFile class
 *
 * Provides RAII-compliant memory-mapped file access.
 * Uses Windows API for memory mapping.
 */
class MemoryMappedFile {
public:
    MemoryMappedFile() :
        m_hFile(INVALID_HANDLE_VALUE),
        m_hMapping(NULL),
        m_pMappedView(nullptr),
        m_currentSize(0),
        m_accessMode(MmfAccessMode::ReadOnly) // Default, but will be set on open
    {}

    ~MemoryMappedFile() {
        close();
    }

    // Disable copy operations
    MemoryMappedFile(const MemoryMappedFile&) = delete;
    MemoryMappedFile& operator=(const MemoryMappedFile&) = delete;

    // Enable move operations
    MemoryMappedFile(MemoryMappedFile&& other) noexcept
        : m_hFile(other.m_hFile),
          m_hMapping(other.m_hMapping),
          m_pMappedView(other.m_pMappedView),
          m_currentSize(other.m_currentSize),
          m_accessMode(other.m_accessMode) {
        other.m_hFile = INVALID_HANDLE_VALUE;
        other.m_hMapping = nullptr;
        other.m_pMappedView = nullptr;
        other.m_currentSize = 0;
    }

    MemoryMappedFile& operator=(MemoryMappedFile&& other) noexcept {
        if (this != &other) {
            close(); // Release current resources

            m_hFile = other.m_hFile;
            m_hMapping = other.m_hMapping;
            m_pMappedView = other.m_pMappedView;
            m_currentSize = other.m_currentSize;
            m_accessMode = other.m_accessMode;

            other.m_hFile = INVALID_HANDLE_VALUE;
            other.m_hMapping = nullptr;
            other.m_pMappedView = nullptr;
            other.m_currentSize = 0;
        }
        return *this;
    }

    /**
     * Opens or creates a memory-mapped file.
     * @param filePath Path to the file.
     * @param mode Access mode (ReadOnly, ReadWrite, WriteCreate).
     * @param mappingSize The size of the mapping.
     *                    - For ReadOnly: Ignored. Size is determined by the file.
     *                    - For ReadWrite/WriteCreate: Required. Defines the size of the mapped region.
     *                      If the file is smaller, it will be extended. If larger, it will be truncated (for WriteCreate).
     * @return MmfError::Success on success, or an error code.
     */
    [[nodiscard]] int open(const std::string& filePath, MmfAccessMode mode, size_t mappingSize = 0) {
        if (isOpen()) {
             LOG_ERR("MMF", "MemoryMappedFile: Attempted to open an already open file instance.", debug::LogContext::Error);
            return MmfError::AlreadyOpen;
        }

        close(); // Ensure clean state

        m_accessMode = mode;
        DWORD dwDesiredAccess = 0;
        DWORD dwShareMode = 0;
        DWORD dwCreationDisposition = 0;
        DWORD flProtect = 0;
        DWORD dwMapViewDesiredAccess = 0;

        switch (mode) {
            case MmfAccessMode::ReadOnly:
                dwDesiredAccess = GENERIC_READ;
                dwShareMode = FILE_SHARE_READ;
                dwCreationDisposition = OPEN_EXISTING;
                flProtect = PAGE_READONLY;
                dwMapViewDesiredAccess = FILE_MAP_READ;
                break;
            case MmfAccessMode::ReadWrite:
                dwDesiredAccess = GENERIC_READ | GENERIC_WRITE;
                dwShareMode = FILE_SHARE_READ; // Or 0 for exclusive access if needed
                dwCreationDisposition = OPEN_EXISTING;
                flProtect = PAGE_READWRITE;
                dwMapViewDesiredAccess = FILE_MAP_WRITE; // FILE_MAP_ALL_ACCESS includes read
                break;
            case MmfAccessMode::WriteCreate:
                dwDesiredAccess = GENERIC_READ | GENERIC_WRITE;
                dwShareMode = 0; // Typically exclusive when creating/overwriting
                dwCreationDisposition = CREATE_ALWAYS; // Creates new or truncates existing
                flProtect = PAGE_READWRITE;
                dwMapViewDesiredAccess = FILE_MAP_WRITE;
                if (mappingSize == 0) {
                    LOG_ERR("MMF", "MemoryMappedFile: mappingSize cannot be 0 for WriteCreate mode.", debug::LogContext::Error);
                    return MmfError::InvalidArguments;
                }
                break;
            default:
                LOG_ERR("MMF", "MemoryMappedFile: Invalid access mode specified.", debug::LogContext::Error);
                return MmfError::InvalidArguments;
        }

        m_hFile = CreateFileA(
            filePath.c_str(),
            dwDesiredAccess,
            dwShareMode,
            nullptr,
            dwCreationDisposition,
            FILE_ATTRIBUTE_NORMAL, // Add FILE_FLAG_SEQUENTIAL_SCAN for ReadOnly if desired
            nullptr);

        if (m_hFile == INVALID_HANDLE_VALUE) {
            std::string msg = "MemoryMappedFile: CreateFileA failed for \"" + filePath + "\". Error code: " + std::to_string(GetLastError());
            LOG_ERR("MMF", msg, debug::LogContext::Error);
            return MmfError::FileOpenFailed;
        }

        // Determine or set the file size for mapping
        if (mode == MmfAccessMode::ReadOnly) {
            LARGE_INTEGER liFileSize;
            if (!GetFileSizeEx(m_hFile, &liFileSize)) {
                std::string msg = "MemoryMappedFile: GetFileSizeEx failed for \"" + filePath + "\". Error code: " + std::to_string(GetLastError());
                LOG_ERR("MMF", msg, debug::LogContext::Error);
                internalClose(); return MmfError::FileSizeQueryFailed;
            }
            m_currentSize = static_cast<size_t>(liFileSize.QuadPart);
            if (m_currentSize == 0) {
                // For ReadOnly, an empty file means no mapping.
                // SpillableQueue might treat this as an error or handle it.
                LOG_ERR("MMF", "MemoryMappedFile: Input file \"" + filePath + "\" is empty.", debug::LogContext::Error);
                // No need to map, but file handle is open. Close it if this is an error state.
                // For now, let's assume this is acceptable and close() will handle it if mapping fails.
                // For ReadOnly and empty file, we won't proceed to map.
                return MmfError::FileIsEmpty; // Specific status for empty readable file
            }
        } else { // ReadWrite or WriteCreate
            m_currentSize = mappingSize;
            // Ensure the file on disk matches the mappingSize for WriteCreate or if extending for ReadWrite.
            // CreateFileMapping will extend the file if mappingSize is larger than current file size,
            // and if it's smaller for CREATE_ALWAYS, the file is truncated by CreateFileA.
            // For robustly setting size if needed (e.g., if CreateFileMapping doesn't guarantee extension on all OS versions/scenarios):
            // SetFilePointerEx and SetEndOfFile could be used here if CreateFileMapping doesn't suffice.
            // However, for PAGE_READWRITE, CreateFileMapping *should* handle sizing if mappingSize is non-zero.
        }

        // Create the file mapping object.
        auto mapSizeHigh = (sizeof(size_t) > 4) ? static_cast<DWORD>(m_currentSize >> 32) : 0;
        auto mapSizeLow = static_cast<DWORD>(m_currentSize & 0xFFFFFFFF);

        m_hMapping = CreateFileMappingA(
            m_hFile,
            nullptr, // Default security attributes
            flProtect,
            mapSizeHigh,  // Maximum size high
            mapSizeLow,   // Maximum size low
            nullptr); // Name of mapping object (unnamed)

        if (m_hMapping == nullptr) {
            std::string msg = "MemoryMappedFile: CreateFileMappingA failed for \"" + filePath + "\". Error code: " + std::to_string(GetLastError());
            LOG_ERR("MMF", msg, debug::LogContext::Error);
            internalClose(); return MmfError::FileMappingFailed;
        }

        m_pMappedView = MapViewOfFile(
            m_hMapping,
            dwMapViewDesiredAccess,
            0, // File offset high
            0, // File offset low
            m_currentSize); // Number of bytes to map (0 means map from offset to end of file mapping)

        if (m_pMappedView == nullptr) {
            std::string msg = "MemoryMappedFile: MapViewOfFile failed for \"" + filePath + "\". Error code: " + std::to_string(GetLastError());
            LOG_ERR("MMF", msg, debug::LogContext::Error);
            internalClose(); return MmfError::MapViewFailed;
        }

        return MmfError::Success;
    }

    void close() {
        internalClose();
    }

    // Provides read-only access to the mapped data.
    [[nodiscard]] const unsigned char* getData() const {
        return static_cast<const unsigned char*>(m_pMappedView);
    }

    [[nodiscard]] unsigned char* getWritableData() const {
        if (m_accessMode == MmfAccessMode::ReadOnly) {
            LOG_ERR("MMF", "MemoryMappedFile: getWritableData() called on a read-only mapping.", debug::LogContext::Error);
            return nullptr;
        }
        return static_cast<unsigned char*>(m_pMappedView);
    }

    [[nodiscard]] size_t getSize() const {
        return m_currentSize;
    }

    [[nodiscard]] bool isOpen() const {
        return m_pMappedView != nullptr;
    }

    // Ensures data within a portion of the mapped view is written to disk.
    // Returns MmfError::Success or an error code.
    [[nodiscard]] int flush(size_t offset = 0, size_t bytesToFlush = 0) {
        if (!isOpen()) {
            LOG_ERR("MMF", "MemoryMappedFile: flush() called on a closed file.", debug::LogContext::Error);
            return MmfError::NotOpen;
        }
        if (m_accessMode == MmfAccessMode::ReadOnly) {
            LOG_ERR("MMF", "MemoryMappedFile: flush() called on a read-only file. No operation performed.", debug::LogContext::Error);
            return MmfError::Success; // Not an error, just no-op
        }

        if (bytesToFlush == 0) {
            bytesToFlush = m_currentSize - offset;
        }

        if (offset + bytesToFlush > m_currentSize) {
            LOG_ERR("MMF", "MemoryMappedFile: flush() range exceeds mapped size.", debug::LogContext::Error);
            return MmfError::InvalidArguments;
        }

        if (!FlushViewOfFile(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(reinterpret_cast<uintptr_t>(m_pMappedView)) + offset), bytesToFlush)) {
            LOG_ERR("MMF", "MemoryMappedFile: FlushViewOfFile failed. Error code: " + std::to_string(GetLastError()), debug::LogContext::Error);
            return MmfError::MapViewFailed; // Reusing, or add specific FlushFailed error
        }
        return MmfError::Success;
    }


private:
    void internalClose() {
        if (m_pMappedView) {
            UnmapViewOfFile(m_pMappedView);
            m_pMappedView = nullptr;
        }
        if (m_hMapping) {
            CloseHandle(m_hMapping);
            m_hMapping = nullptr;
        }
        if (m_hFile != INVALID_HANDLE_VALUE) {
            CloseHandle(m_hFile);
            m_hFile = INVALID_HANDLE_VALUE;
        }
        m_currentSize = 0;
    }

    HANDLE m_hFile;
    HANDLE m_hMapping;
    void* m_pMappedView;
    size_t m_currentSize;
    MmfAccessMode m_accessMode;
};

#endif // MEMORY_MAPPED_FILE_H