cmake_minimum_required(VERSION 3.29)
project(ConvertToImage VERSION 1.0)

set(CMAKE_CXX_STANDARD 26)

file(GLOB_RECURSE SOURCES
        ${CMAKE_SOURCE_DIR}/src/*.cpp
        ${CMAKE_SOURCE_DIR}/src/headers/*.h
        ${CMAKE_SOURCE_DIR}/src/headers/*.hpp
)

add_executable(${PROJECT_NAME} ${CMAKE_SOURCE_DIR} ${SOURCES})
