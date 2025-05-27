cmake_minimum_required(VERSION 3.20)
project(PictoByteConverter VERSION 1.0)

include(FetchContent)

# Fetch Catch2
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.5.2 # Or any other recent stable tag
)
FetchContent_MakeAvailable(Catch2)

if (MSVC)
    # Add AddressSanitizer flags
    #add_compile_options("/fsanitize=address")
    #add_link_options("/fsanitize=address")

endif()

# Set C++ standard to C++20 which is well-supported
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Configure output directories to use cmake-build-<mode>/build/ structure
string(TOLOWER "${CMAKE_BUILD_TYPE}" BUILD_TYPE_LOWER)
set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/cmake-build-${BUILD_TYPE_LOWER}/build")

# For single configuration generators
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${OUTPUT_DIR}")

# For multi-configuration generators (e.g. Visual Studio, Xcode)
foreach (CONFIG_TYPE ${CMAKE_CONFIGURATION_TYPES})
    string(TOLOWER "${CONFIG_TYPE}" CONFIG_TYPE_LOWER)
    set(OUTPUT_DIR_CONFIG "${CMAKE_SOURCE_DIR}/cmake-build-${CONFIG_TYPE_LOWER}/build")

    string(TOUPPER "${CONFIG_TYPE}" CONFIG_TYPE_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_TYPE_UPPER} "${OUTPUT_DIR_CONFIG}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG_TYPE_UPPER} "${OUTPUT_DIR_CONFIG}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_TYPE_UPPER} "${OUTPUT_DIR_CONFIG}")
endforeach ()

# Add compiler-specific flags if needed
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
elseif (MSVC)
    add_compile_options(/W4)
endif ()

# Collect library source files (excluding main.cpp)
file(GLOB_RECURSE LIB_SOURCES
        "${CMAKE_SOURCE_DIR}/src/Tasks/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/Debug/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/Threading/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/FileSystem/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/Queue/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/Image/*.cpp"
)

# Remove main.cpp from the library sources if it was included
list(FILTER LIB_SOURCES EXCLUDE REGEX ".*main\\.cpp$")

# Create the executable
add_executable(PictoByteTest
        "${CMAKE_SOURCE_DIR}/src/main.cpp"
        ${LIB_SOURCES}
)

# Set public include directories for the executable
# This allows source files to find headers, and headers to find .inl files
target_include_directories(PictoByteTest PRIVATE 
    "${CMAKE_SOURCE_DIR}/src"
)

# Link the executable with any necessary system libraries (if any)
# target_link_libraries(PictoByteTest PRIVATE some_system_lib)

# Build options
option(BUILD_TESTING "Build tests" ON)

# If testing is enabled
if (BUILD_TESTING)
    enable_testing()
    # add_subdirectory(tests) # We will use main.cpp for tests for now
    target_link_libraries(PictoByteTest PRIVATE Catch2::Catch2WithMain)
endif ()

# Define the path to your custom DLLs directory
set(CUSTOM_DEBUGGING_DLLS_DIR ${CMAKE_SOURCE_DIR}/debugging_dlls)

# Check if the custom DLLs directory exists
if(EXISTS "${CUSTOM_DEBUGGING_DLLS_DIR}")
    message(STATUS "Custom debugging DLLs directory found: ${CUSTOM_DEBUGGING_DLLS_DIR}")

    # List of ASan runtime files to copy from your custom directory
    # Adjust these filenames if yours are different or if some are not present
    set(ASAN_FILES_TO_COPY)
    foreach(base_name "clang_rt.asan_dynamic-x86_64" "clang_rt.asan_dynamic_runtime_thunk-x86_64")
        foreach(ext ".dll" ".pdb")
            set(current_file "${CUSTOM_DEBUGGING_DLLS_DIR}/${base_name}${ext}")
            if(EXISTS "${current_file}")
                list(APPEND ASAN_FILES_TO_COPY "${current_file}")
            endif()
        endforeach()
    endforeach()

    if(NOT ASAN_FILES_TO_COPY)
        message(WARNING "No ASan files found in ${CUSTOM_DEBUGGING_DLLS_DIR} to copy.")
    else()
        # Ensure your main executable target is named PictoByteTest
        if(TARGET PictoByteTest)
            add_custom_command(TARGET PictoByteTest POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${ASAN_FILES_TO_COPY}
                $<TARGET_FILE_DIR:PictoByteTest>
                COMMENT "Copying ASan DLLs/PDBs to build directory"
            )
            message(STATUS "Added post-build command to copy ASan files for PictoByteTest.")
        else()
            message(WARNING "Target PictoByteTest not found. Cannot add post-build command for ASan files.")
        endif()
    endif()
else()
    message(STATUS "Custom debugging DLLs directory not found: ${CUSTOM_DEBUGGING_DLLS_DIR}. Skipping ASan DLL copy.")
endif()
