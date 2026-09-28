cmake_minimum_required(VERSION 3.16)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(libsh_stdkern LANGUAGES CXX)
endif()

add_library(libsh_stdkern INTERFACE)
target_include_directories(libsh_stdkern INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/../include"
)
target_compile_features(libsh_stdkern INTERFACE cxx_std_20)
