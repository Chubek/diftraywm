cmake_minimum_required(VERSION 3.16)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(libsh_cli LANGUAGES CXX)
endif()

set(LIBSH_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

add_library(libsh_cli_parse OBJECT
    libsh-parse.cpp
)
target_link_libraries(libsh_cli_parse PUBLIC libshell::libshell)
target_compile_features(libsh_cli_parse PUBLIC cxx_std_20)
target_compile_options(libsh_cli_parse PRIVATE -Wall -Wextra -Werror -pedantic)

add_executable(libsh
    libsh-cli.cpp
    $<TARGET_OBJECTS:libsh_cli_parse>
)
target_link_libraries(libsh PRIVATE libshell::libshell)
target_compile_features(libsh PRIVATE cxx_std_20)
target_compile_options(libsh PRIVATE -Wall -Wextra -Werror -pedantic)

install(TARGETS libsh RUNTIME DESTINATION bin)
