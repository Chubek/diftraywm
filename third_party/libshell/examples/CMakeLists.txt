cmake_minimum_required(VERSION 3.16)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(libsh_examples LANGUAGES CXX)
endif()

add_subdirectory(API)
