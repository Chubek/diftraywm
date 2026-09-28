cmake_minimum_required(VERSION 3.16)

project(libshell VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

option(LIBSH_BUILD_TESTS "Build LibShell test targets" ON)
option(LIBSH_BUILD_EXAMPLES "Build LibShell example targets" ON)
option(LIBSH_BUILD_STDKERN "Enable libsh_stdkern interface target" ON)
option(LIBSH_BUILD_CLI "Build LibShell CLI target" ON)
option(LIBSHELL_GENERATE_DOCS "Generate and install LibShell documentation" ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(shell_qamrpp STATIC
    src/LibShell-Qamrpp.cpp
)
set_target_properties(shell_qamrpp PROPERTIES OUTPUT_NAME shell_qamrpp)
target_compile_features(shell_qamrpp PUBLIC cxx_std_20)
target_link_libraries(shell_qamrpp PUBLIC "${CMAKE_DL_LIBS}")
target_include_directories(shell_qamrpp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_include_directories(shell_qamrpp SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/QaMRpp/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/QaMRpp/third_party/SerdeTk/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/QaMRpp/third_party"
)

add_library(libshell INTERFACE)
add_library(libshell::libshell ALIAS libshell)
target_compile_features(libshell INTERFACE cxx_std_20)
target_link_libraries(libshell INTERFACE shell_qamrpp)
target_include_directories(libshell INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    include(CTest)
endif()

if(LIBSH_BUILD_STDKERN)
    add_subdirectory(stdkern)
endif()

if(LIBSH_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(LIBSH_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(LIBSH_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(LIBSHELL_GENERATE_DOCS)
    add_subdirectory(docs)
endif()

set(LIBSHELL_PC_PRIVATE_LIBS "")
if(CMAKE_DL_LIBS)
    set(LIBSHELL_PC_PRIVATE_LIBS "-l${CMAKE_DL_LIBS}")
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/libshell.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/libshell.pc"
    @ONLY
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/libshell.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
)
install(TARGETS shell_qamrpp
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)

install(DIRECTORY
    "${CMAKE_CURRENT_SOURCE_DIR}/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
