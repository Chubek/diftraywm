cmake_minimum_required(VERSION 3.16)

if(NOT PROJECT_NAME)
    project(libsh_api_examples LANGUAGES CXX)
endif()

set(LIBSH_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

set(LIBSH_API_COMMON
    "${CMAKE_CURRENT_LIST_DIR}/common.hpp"
)

function(add_libsh_api_example name)
    set(options WITH_PARSER)
    cmake_parse_arguments(ARG "${options}" "" "" ${ARGN})

    set(source "${CMAKE_CURRENT_LIST_DIR}/${name}.cpp")
    set(extra_sources)
    if(ARG_WITH_PARSER)
        list(APPEND extra_sources "${LIBSH_ROOT}/cli/libsh-parse.cpp")
    endif()

    add_executable(${name} ${source} ${extra_sources})
    target_include_directories(${name} PRIVATE "${LIBSH_ROOT}/include" "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_features(${name} PRIVATE cxx_std_20)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror -pedantic)
    target_link_libraries(${name} PRIVATE libshell::libshell)
endfunction()

add_libsh_api_example(01_dry_run_plan)
add_libsh_api_example(02_echo_capture)
add_libsh_api_example(03_variable_expansion)
add_libsh_api_example(04_environment_overlay)
add_libsh_api_example(05_export_and_child_env)
add_libsh_api_example(06_command_working_directory)
add_libsh_api_example(07_stdout_to_file)
add_libsh_api_example(08_append_to_file)
add_libsh_api_example(09_stderr_capture)
add_libsh_api_example(10_discard_stderr)
add_libsh_api_example(11_pipeline_count_lines)
add_libsh_api_example(12_pipefail_any_failed)
add_libsh_api_example(13_and_if_short_circuit)
add_libsh_api_example(14_or_if_recovery)
add_libsh_api_example(15_subshell_copy_isolation)
add_libsh_api_example(16_subshell_shared_environment)
add_libsh_api_example(17_arithmetic_expansion)
add_libsh_api_example(18_command_substitution)
add_libsh_api_example(19_command_substitution_parser WITH_PARSER)
add_libsh_api_example(20_glob_expansion)
add_libsh_api_example(21_validation_diagnostics)
add_libsh_api_example(22_lambda_sinklet)
add_libsh_api_example(23_grep_sinklet)
add_libsh_api_example(24_json_lines_sinklet)
add_libsh_api_example(25_function_kernel)

add_custom_target(libsh-api-examples SOURCES
    ${LIBSH_API_COMMON}
    "${CMAKE_CURRENT_LIST_DIR}/README.md"
)
