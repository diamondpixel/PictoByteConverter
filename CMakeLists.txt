cmake_minimum_required(VERSION 3.29)
project(ConvertToImage VERSION 1.0)

# Set C++ standard to C++20 which is well-supported
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add compiler-specific flags if needed
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -std=c++20)
elseif(MSVC)
    add_compile_options(/W4 /std:c++20)
endif()

file(GLOB_RECURSE SOURCES
        ${CMAKE_SOURCE_DIR}/src/*.cpp
        ${CMAKE_SOURCE_DIR}/src/*/*.h
        ${CMAKE_SOURCE_DIR}/src/*/*.hpp
        ${CMAKE_SOURCE_DIR}/src/*/*/*.h
        ${CMAKE_SOURCE_DIR}/src/*/*/*.hpp
)

add_executable(${PROJECT_NAME} ${CMAKE_SOURCE_DIR} ${SOURCES})
