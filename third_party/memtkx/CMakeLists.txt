cmake_minimum_required(VERSION 3.20)

project(MemTKX VERSION 1.0 LANGUAGES CXX)

include(GNUInstallDirs)

option(MemTKX_BUILD_TESTS "Build the MemTKX test suite" ON)
option(MemTKX_BUILD_BENCHMARKS "Build the MemTKX benchmarks" OFF)
option(MemTKX_ENABLE_WARNINGS "Enable strict compiler warnings" ON)

add_library(MemTKX INTERFACE)
add_library(MemTKX::MemTKX ALIAS MemTKX)

target_include_directories(MemTKX
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(MemTKX INTERFACE cxx_std_20)

if(MemTKX_ENABLE_WARNINGS)
    target_compile_options(MemTKX INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    )
endif()

find_package(Threads REQUIRED)
target_link_libraries(MemTKX INTERFACE Threads::Threads)

if(MemTKX_BUILD_TESTS)
    enable_testing()

    set(MemTKX_TESTS
        test_units
        test_allocator
        test_barrier
        test_immix
        test_large_object
        test_scheduler_edges
        test_dispatcher
        test_scheduler
        test_plans
    )

    foreach(test_name IN LISTS MemTKX_TESTS)
        add_executable(${test_name} tests/${test_name}.cpp)
        target_link_libraries(${test_name} PRIVATE MemTKX::MemTKX)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()

if(MemTKX_BUILD_BENCHMARKS)
    set(MemTKX_BENCHMARKS
        bench_alloc
        bench_barrier
    )

    foreach(bench_name IN LISTS MemTKX_BENCHMARKS)
        add_executable(${bench_name} benchmarks/${bench_name}.cpp)
        target_link_libraries(${bench_name} PRIVATE MemTKX::MemTKX)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${bench_name} PRIVATE -O3)
        endif()
    endforeach()
endif()

install(DIRECTORY include/memtkx
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp")

install(TARGETS MemTKX EXPORT MemTKXTargets)
install(EXPORT MemTKXTargets
        FILE MemTKXTargets.cmake
        NAMESPACE MemTKX::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MemTKX)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/MemTKXConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/MemTKXConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MemTKX
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/MemTKXConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/MemTKXConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/MemTKXConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MemTKX
)

install(FILES README.md GUIDE.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
