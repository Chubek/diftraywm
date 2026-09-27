cmake_minimum_required(VERSION 3.16)

project(AurocksGLR LANGUAGES NONE)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "Install prefix" FORCE)
endif()

set(_source_dir "${CMAKE_CURRENT_SOURCE_DIR}")
set(_binary_dir "${CMAKE_CURRENT_BINARY_DIR}")
set(_source_targets "${_source_dir}/target")
set(_project_prefix "${CMAKE_INSTALL_PREFIX}")
set(_path_prefix "$ENV{DESTDIR}${_project_prefix}")
set(_bindir "${_path_prefix}/bin")
set(_libdir "${_path_prefix}/lib")
set(_targetdir "${_libdir}/aurocksglr-target")
set(_mandir "${_path_prefix}/man/man1")

install(PROGRAMS "${_source_dir}/AurocksGLR.sh" DESTINATION "bin")
install(PROGRAMS "${_source_dir}/AurocksGLR.pl" DESTINATION "lib")
file(GLOB _target_files "${_source_targets}/*.m4")
install(FILES ${_target_files} DESTINATION "lib/aurocksglr-target")

find_program(POD2MAN_EXECUTABLE pod2man)
if(POD2MAN_EXECUTABLE)
    set(_manpage "${_binary_dir}/AurocksGLR.1")
    if(NOT EXISTS "${_manpage}" OR "${_source_dir}/AurocksGLR.pl" IS_NEWER_THAN "${_manpage}")
        execute_process(
            COMMAND ${POD2MAN_EXECUTABLE}
                --section=1
                --center="AurocksGLR"
                --release="AurocksGLR"
                "${_source_dir}/AurocksGLR.pl"
                "${_manpage}"
            RESULT_VARIABLE _pod2man_result
            ERROR_VARIABLE _pod2man_error
            OUTPUT_QUIET
            ERROR_QUIET
        )
        if(NOT _pod2man_result EQUAL 0)
            message(WARNING "pod2man failed while generating manpage: ${_pod2man_error}")
        endif()
    endif()
    if(EXISTS "${_manpage}")
        install(FILES "${_manpage}" DESTINATION "man/man1")
    endif()
else()
    message(STATUS "pod2man not found; skipping manpage generation")
endif()

install(CODE [[
    if(NOT ":$ENV{PATH}:" MATCHES ":${CMAKE_INSTALL_PREFIX}/bin:")
        if(DEFINED ENV{SHELL})
            get_filename_component(_shell_name "$ENV{SHELL}" NAME)
        else()
            set(_shell_name "sh")
        endif()
        if(_shell_name STREQUAL "fish")
            message(STATUS "Add ${CMAKE_INSTALL_PREFIX}/bin to PATH (fish): set -Ux PATH ${CMAKE_INSTALL_PREFIX}/bin $PATH")
        elseif(_shell_name STREQUAL "csh" OR _shell_name STREQUAL "tcsh")
            message(STATUS "Add ${CMAKE_INSTALL_PREFIX}/bin to PATH: setenv PATH \"${CMAKE_INSTALL_PREFIX}/bin:$PATH\"")
        else()
            message(STATUS "Add ${CMAKE_INSTALL_PREFIX}/bin to PATH (${_shell_name}): export PATH=\"${CMAKE_INSTALL_PREFIX}/bin:$PATH\"")
        endif()
    endif()
]])

add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -E rm -f "${_bindir}/AurocksGLR.sh"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${_libdir}/AurocksGLR.pl"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${_mandir}/AurocksGLR.1"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_targetdir}"
    WORKING_DIRECTORY "${_source_dir}"
    COMMENT "Removing installed AurocksGLR files from ${_path_prefix}"
)

message(STATUS "AurocksGLR is ready. Run 'cmake --install . --prefix=...' to install it.")
