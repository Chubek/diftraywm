cmake_minimum_required(VERSION 3.16)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(libsh_tests LANGUAGES CXX)
endif()

enable_testing()

set(LIBSH_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

add_executable(test_runtime
    test_runtime.cpp
    $<TARGET_OBJECTS:libsh_cli_parse>
)

add_executable(test_cli
    test_cli.cpp
)

target_link_libraries(test_runtime PRIVATE libshell::libshell)
target_link_libraries(test_cli PRIVATE libshell::libshell)
target_compile_features(test_runtime PRIVATE cxx_std_20)
target_compile_features(test_cli PRIVATE cxx_std_20)
target_compile_options(test_runtime PRIVATE -Wall -Wextra -Werror -pedantic)
target_compile_options(test_cli PRIVATE -Wall -Wextra -Werror -pedantic)

add_dependencies(test_runtime libsh_cli_parse)
add_dependencies(test_cli libsh)

add_test(NAME libsh_cli_binary COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "$<TARGET_FILE:libsh>"
    "${CMAKE_CURRENT_BINARY_DIR}/libsh"
)
set_tests_properties(libsh_cli_binary PROPERTIES FIXTURES_SETUP libsh_cli_binary)

add_test(NAME runtime COMMAND test_runtime WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME cli COMMAND test_cli WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(cli PROPERTIES FIXTURES_REQUIRED libsh_cli_binary)
