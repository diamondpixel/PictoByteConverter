cmake_minimum_required(VERSION 3.20)
project(PictoByteConverter VERSION 1.0)

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

# Define the installation directories conforming to GNU standards
include(GNUInstallDirs)
set(PBC_INSTALL_LIBDIR ${CMAKE_INSTALL_LIBDIR})
set(PBC_INSTALL_BINDIR ${CMAKE_INSTALL_BINDIR})
set(PBC_INSTALL_INCLUDEDIR ${CMAKE_INSTALL_INCLUDEDIR})

# Create the interface and public header path 
set(PUBLIC_HEADERS_DIR "${CMAKE_CURRENT_BINARY_DIR}/include")
file(MAKE_DIRECTORY ${PUBLIC_HEADERS_DIR})

# Collect library source files (excluding main.cpp)
file(GLOB_RECURSE LIB_SOURCES
        "${CMAKE_SOURCE_DIR}/src/Image/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/Tasks/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/Debug/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/Threading/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/API/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/FileSystem/*.cpp"
)

# Remove main.cpp from the library sources if it was included
list(FILTER LIB_SOURCES EXCLUDE REGEX ".*main\\.cpp$")

# Collect header files
file(GLOB_RECURSE HEADER_FILES
        "${CMAKE_SOURCE_DIR}/src/Image/headers/*.h"
        "${CMAKE_SOURCE_DIR}/src/Debug/headers/*.h"
        "${CMAKE_SOURCE_DIR}/src/Threading/headers/*.h"
        "${CMAKE_SOURCE_DIR}/src/API/headers/*.h"
        "${CMAKE_SOURCE_DIR}/src/FileSystem/headers/*.h"
        "${CMAKE_SOURCE_DIR}/src/Tasks/headers/*.h"
)

# Define the public API headers that will be installed
set(PUBLIC_API_HEADERS
        "${CMAKE_SOURCE_DIR}/src/API/headers/PictoByteConverter.h"
)

# Create the shared library
add_library(pictobyte SHARED ${LIB_SOURCES})
add_library(PictoByteConverter::pictobyte ALIAS pictobyte)

# Define PICTOBYTE_DLL_EXPORTS for the pictobyte library
target_compile_definitions(pictobyte PRIVATE PICTOBYTE_DLL_EXPORTS)

# Set library properties
set_target_properties(pictobyte PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        PUBLIC_HEADER "${PUBLIC_API_HEADERS}"
        POSITION_INDEPENDENT_CODE ON
        WINDOWS_EXPORT_ALL_SYMBOLS ON
)

# Set public include directories for consumers of the library
target_include_directories(pictobyte
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/API/headers>
        $<INSTALL_INTERFACE:${PBC_INSTALL_INCLUDEDIR}>
)

# Create the executable
add_executable(ConvertToImage
        "${CMAKE_SOURCE_DIR}/src/main.cpp"
)

# Link the executable with the library
target_link_libraries(ConvertToImage PRIVATE pictobyte)

# Installation instructions for library
install(TARGETS pictobyte
        EXPORT PictoByteConverterTargets
        LIBRARY DESTINATION ${PBC_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${PBC_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${PBC_INSTALL_BINDIR}
        PUBLIC_HEADER DESTINATION ${PBC_INSTALL_INCLUDEDIR}/PictoByteConverter
        INCLUDES DESTINATION ${PBC_INSTALL_INCLUDEDIR}
)

# Installation instructions for executable
install(TARGETS ConvertToImage
        RUNTIME DESTINATION ${PBC_INSTALL_BINDIR}
)

# Make sure the cmake directory exists
file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Create the config file template if it doesn't exist
if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PictoByteConverterConfig.cmake.in")
    file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PictoByteConverterConfig.cmake.in"
            "@PACKAGE_INIT@

include(\"\${CMAKE_CURRENT_LIST_DIR}/PictoByteConverterTargets.cmake\")
check_required_components(PictoByteConverter)
")
endif ()

# Generate and install package configuration files
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/PictoByteConverterConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PictoByteConverterConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/PictoByteConverterConfig.cmake"
        INSTALL_DESTINATION ${PBC_INSTALL_LIBDIR}/cmake/PictoByteConverter
)

install(EXPORT PictoByteConverterTargets
        FILE PictoByteConverterTargets.cmake
        NAMESPACE PictoByteConverter::
        DESTINATION ${PBC_INSTALL_LIBDIR}/cmake/PictoByteConverter
)

install(
        FILES
        "${CMAKE_CURRENT_BINARY_DIR}/PictoByteConverterConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/PictoByteConverterConfigVersion.cmake"
        DESTINATION ${PBC_INSTALL_LIBDIR}/cmake/PictoByteConverter
)

# Build options
option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(BUILD_TESTING "Build tests" OFF)

# If testing is enabled
if (BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif ()

# ---- Added by Cascade: Copy ASan DLLs/PDBs from debugging_dlls directory ----
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
        # Ensure your main executable target is named PictoByteConverter
        # If it's different, change "PictoByteConverter" below to your actual target name.
        if(TARGET ConvertToImage)
            add_custom_command(TARGET ConvertToImage POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E echo "Copying ASan runtime files to $<TARGET_FILE_DIR:ConvertToImage>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${ASAN_FILES_TO_COPY}
                    $<TARGET_FILE_DIR:ConvertToImage>
                COMMENT "Copying ASan runtime DLLs/PDBs to build directory"
                VERBATIM
            )
            message(STATUS "Added post-build command to ConvertToImage to copy ASan DLLs: ${ASAN_FILES_TO_COPY}")
        else()
            message(WARNING "Target ConvertToImage not found. Cannot add custom command to copy ASan DLLs.")
        endif()
    endif()
else()
    message(WARNING "Custom debugging DLLs directory not found: ${CUSTOM_DEBUGGING_DLLS_DIR}. ASan DLLs will not be copied automatically from here.")
endif()
# ---- End of Cascade addition ----
