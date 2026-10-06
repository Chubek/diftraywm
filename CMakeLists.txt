# DiftrayWM -- top-level build.
#
# Vendored libraries are built into a private prefix before pkg-config
# discovery. Only platform dependencies absent from third_party use the host.

cmake_minimum_required(VERSION 4.3)

project(DiftrayWM
  VERSION 0.1.0
  DESCRIPTION "Cell-based Wayland compositor with a notebook-style workflow"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# scripts/build-wlroots.sh always builds wlroots with meson's debugoptimized
# profile; default the C++ side to something equally debuggable instead of
# letting an unset CMAKE_BUILD_TYPE silently produce an -O0 no-config build.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Debug Release RelWithDebInfo MinSizeRel)
endif()

option(DIFTRAYWM_WERROR "Treat DiftrayWM compiler warnings as errors" OFF)

include(CTest)
include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Layout
# ---------------------------------------------------------------------------
set(DIFTRAYWM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(DIFTRAYWM_SRC "${DIFTRAYWM_ROOT}/src")
set(DIFTRAYWM_INCLUDE "${DIFTRAYWM_ROOT}/include")
set(DIFTRAY_THIRD_PARTY "${DIFTRAYWM_ROOT}/third_party")

if(NOT IS_DIRECTORY "${DIFTRAY_THIRD_PARTY}")
  message(FATAL_ERROR
    "DiftrayWM keeps its dependencies in ${DIFTRAY_THIRD_PARTY}, which does "
    "not exist. Restore the vendored trees described in AGENTS.md section 11.")
endif()

set(_diftraywm_vendored_required wlroots domterm termlib libtsm libptytty PEGTL oniguruma libshell lua kaguya dynalo libcyaml tomlc99 libevdev libudev)
set(_diftraywm_vendored_missing "")
foreach(_dep IN LISTS _diftraywm_vendored_required)
  if(NOT IS_DIRECTORY "${DIFTRAY_THIRD_PARTY}/${_dep}")
    list(APPEND _diftraywm_vendored_missing "third_party/${_dep}")
  endif()
endforeach()
if(_diftraywm_vendored_missing)
  string(REPLACE ";" "\n  " _diftraywm_missing "${_diftraywm_vendored_missing}")
  message(FATAL_ERROR
    "Missing vendored dependencies:\n  ${_diftraywm_missing}\n"
    "Restore them under ${DIFTRAY_THIRD_PARTY} (AGENTS.md section 11).")
endif()

# ---------------------------------------------------------------------------
# Host tools
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Perl REQUIRED)

# The vendored termlib generates the Termscript parser with its own script;
# a missing copy only shows up much later as an opaque
# "No rule to make target" from the generator.  Check it while we can still
# explain it.
set(DIFTRAY_AUROCKS "${DIFTRAY_THIRD_PARTY}/termlib/scripts/aurocks.pl")
if(NOT EXISTS "${DIFTRAY_AUROCKS}")
  message(FATAL_ERROR
    "${DIFTRAY_AUROCKS} is missing. third_party/termlib compiles "
    "termscript/Termscript.g with it to produce termscript_parser.c, and "
    "refuses to build without it.")
endif()

# ---------------------------------------------------------------------------
# wlroots (meson, out of tree)
# ---------------------------------------------------------------------------
set(WLROOTS_SOURCE_DIR "${DIFTRAY_THIRD_PARTY}/wlroots")
set(WLROOTS_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/wlroots-build")

# wlroots names its library libwlroots-<major>.<minor>.so; read the version out
# of its meson.build instead of hard-coding it, so bumping the vendored tree
# does not silently break the import below.
file(READ "${WLROOTS_SOURCE_DIR}/meson.build" _diftraywm_wlroots_meson)
string(REGEX MATCHALL "version:[ \t]*'[^']+'" _diftraywm_wlroots_versions
       "${_diftraywm_wlroots_meson}")
if(NOT _diftraywm_wlroots_versions)
  message(FATAL_ERROR
    "Could not read the wlroots version from ${WLROOTS_SOURCE_DIR}/meson.build")
endif()
list(GET _diftraywm_wlroots_versions 0 _diftraywm_wlroots_project_version)
string(REGEX MATCH "[0-9]+\\.[0-9]+" WLROOTS_ABI_VERSION
       "${_diftraywm_wlroots_project_version}")
if(NOT WLROOTS_ABI_VERSION)
  message(FATAL_ERROR
    "Could not derive the wlroots ABI version from "
    "'${_diftraywm_wlroots_project_version}'")
endif()

# Prefer the repository-local toolchain: meson and ninja are not expected to be
# on PATH, and the two must come from the same environment for meson to find a
# matching ninja.
set(_diftraywm_toolchain_hints "${DIFTRAYWM_ROOT}/.tools/meson-env/bin")
find_program(MESON_EXECUTABLE NAMES meson HINTS ${_diftraywm_toolchain_hints}
  DOC "meson, used to build the vendored wlroots")
find_program(NINJA_EXECUTABLE NAMES ninja HINTS ${_diftraywm_toolchain_hints}
  DOC "ninja, the backend meson uses to build the vendored wlroots")
if(NOT MESON_EXECUTABLE OR NOT NINJA_EXECUTABLE)
  message(FATAL_ERROR
    "meson and ninja are required to build the vendored wlroots. Install "
    "them, or create the repository-local toolchain at "
    "${DIFTRAYWM_ROOT}/.tools/meson-env.")
endif()
unset(_diftraywm_toolchain_hints)

set(DIFTRAY_VENDOR_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/vendor")
message(STATUS "Building vendored dependency chain (log: ${CMAKE_CURRENT_BINARY_DIR}/vendor-build.log)")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "MESON_BIN=${MESON_EXECUTABLE}"
    "${DIFTRAYWM_ROOT}/scripts/build-vendored.sh" "${CMAKE_CURRENT_BINARY_DIR}"
  OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/vendor-build.log"
  ERROR_FILE "${CMAKE_CURRENT_BINARY_DIR}/vendor-build.log"
  RESULT_VARIABLE _vendor_result)
if(NOT _vendor_result EQUAL 0)
  message(FATAL_ERROR "Vendored dependency build failed. See ${CMAKE_CURRENT_BINARY_DIR}/vendor-build.log; required host tools include Meson, Ninja, Autotools, gettext and gperf.")
endif()
set(ENV{PKG_CONFIG_PATH} "${DIFTRAY_VENDOR_PREFIX}/lib/pkgconfig:${DIFTRAY_VENDOR_PREFIX}/share/pkgconfig:$ENV{PKG_CONFIG_PATH}")
# Discard cached system paths when upgrading an existing build directory.
get_cmake_property(_cache_vars CACHE_VARIABLES)
foreach(_var IN LISTS _cache_vars)
  if(_var MATCHES "^(__pkg_config_checked_|pkgcfg_lib_)(WLROOTS_BUILD_DEPS|WLROOTS_PLATFORM_DEPS|FREETYPE|HARFBUZZ|FONTCONFIG|LIBUDEV|LIBEVDEV)")
    unset(${_var} CACHE)
  endif()
endforeach()
set(CMAKE_BUILD_RPATH "${DIFTRAY_VENDOR_PREFIX}/lib;${WLROOTS_BUILD_DIR}")
set(CMAKE_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}/diftraywm")

# ---------------------------------------------------------------------------
# Library discovery (private vendored prefix first)
# ---------------------------------------------------------------------------
# Keep host-only dependencies in a separate pkg-config query. FindPkgConfig
# resolves all libraries in a query using its combined -L directories; putting
# pixman/libdrm first also puts /usr/lib first and can select host copies of
# Wayland, xkbcommon and libinput despite finding their vendored .pc files.
pkg_check_modules(WLROOTS_PLATFORM_DEPS REQUIRED IMPORTED_TARGET pixman-1 libdrm)
pkg_check_modules(WLROOTS_BUILD_DEPS REQUIRED IMPORTED_TARGET
  wayland-server xkbcommon libinput)
target_link_libraries(PkgConfig::WLROOTS_BUILD_DEPS INTERFACE
  PkgConfig::WLROOTS_PLATFORM_DEPS)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(NCURSESW REQUIRED IMPORTED_TARGET ncursesw)
# The key remapper's system-wide backend. Both are vendored (see the libevdev and
# libudev steps in scripts/build-vendored.sh); libudev's headers stay on the
# include path as SYSTEM because they are third-party.
pkg_check_modules(LIBUDEV REQUIRED IMPORTED_TARGET libudev)
pkg_check_modules(LIBEVDEV REQUIRED IMPORTED_TARGET libevdev)

# Fail during configuration if an existing cache or host search path ever
# bypasses the mandatory vendored libraries. Auxiliary platform libraries
# (such as libm) are intentionally allowed to come from the host.
foreach(_dependency IN ITEMS
    "WLROOTS_BUILD_DEPS|wayland-server" "WLROOTS_BUILD_DEPS|xkbcommon"
    "WLROOTS_BUILD_DEPS|input" "FREETYPE|freetype" "HARFBUZZ|harfbuzz"
    "FONTCONFIG|fontconfig" "LIBUDEV|udev" "LIBEVDEV|evdev")
  string(REPLACE "|" ";" _parts "${_dependency}")
  list(GET _parts 0 _pkg)
  list(GET _parts 1 _library)
  set(_resolved "${pkgcfg_lib_${_pkg}_${_library}}")
  get_filename_component(_resolved_dir "${_resolved}" REALPATH)
  get_filename_component(_vendor_dir "${DIFTRAY_VENDOR_PREFIX}/lib" REALPATH)
  string(FIND "${_resolved_dir}" "${_vendor_dir}/" _vendor_match)
  if(NOT _vendor_match EQUAL 0)
    message(FATAL_ERROR
      "${_library} must resolve to ${DIFTRAY_VENDOR_PREFIX}/lib, got '${_resolved}'")
  endif()
endforeach()



set(WLROOTS_LIBRARY "${WLROOTS_BUILD_DIR}/libwlroots-${WLROOTS_ABI_VERSION}.so")

# This is a custom target rather than an OUTPUT-driven custom command on
# purpose: meson/ninja own wlroots' own incrementality, and a custom target
# re-invokes the script on every build so edits to the vendored tree (or a
# deleted .so) are picked up without re-configuring CMake.
add_custom_target(wlroots_ep
  COMMAND "${CMAKE_COMMAND}" -E env
          "MESON_BIN=${MESON_EXECUTABLE}"
          "PKG_CONFIG_PATH=$ENV{PKG_CONFIG_PATH}"
          "LD_LIBRARY_PATH=${DIFTRAY_VENDOR_PREFIX}/lib"
          "NINJA_BIN=${NINJA_EXECUTABLE}"
          "WLROOTS_SRC=${WLROOTS_SOURCE_DIR}"
          "WLROOTS_BUILD=${WLROOTS_BUILD_DIR}"
          "${DIFTRAYWM_ROOT}/scripts/build-wlroots.sh"
  WORKING_DIRECTORY "${DIFTRAYWM_ROOT}"
  BYPRODUCTS "${WLROOTS_LIBRARY}"
  COMMENT "Building vendored wlroots ${WLROOTS_ABI_VERSION}"
  USES_TERMINAL
  VERBATIM)

add_library(wlroots::wlroots SHARED IMPORTED GLOBAL)
# CMake checks that an imported target's interface include directories exist at
# generate time, long before wlroots has been built, so stand the directory up
# here; meson fills it in with the generated protocol and version headers.
file(MAKE_DIRECTORY "${WLROOTS_BUILD_DIR}/include")
set_target_properties(wlroots::wlroots PROPERTIES
  IMPORTED_LOCATION "${WLROOTS_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${WLROOTS_BUILD_DIR}/include")
add_dependencies(wlroots::wlroots wlroots_ep)

# ---------------------------------------------------------------------------
# termlib: Termscript plus its standard library
# ---------------------------------------------------------------------------
# Provides the `termscript`, `termscript_standalone`, `termscript_stdlib`,
# `termlib` and `termscript-driver` targets, and installs the stdlib .tsc
# companions that notelets import at run time.
# The supplied Termlib checkout lacks stdlib/env; the predecessor vendored
# alongside it has this module. Assemble an out-of-source build copy so the
# full stdlib is usable without editing either vendored tree.
set(DIFTRAY_TERMLIB "${DIFTRAY_THIRD_PARTY}/termlib")
set(DIFTRAY_TERMLIB_BUILD_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/termlib-source")
file(COPY "${DIFTRAY_TERMLIB}/" DESTINATION "${DIFTRAY_TERMLIB_BUILD_SOURCE}")
file(COPY "${DIFTRAY_THIRD_PARTY}/domterm/termscript/stdlib/env/"
  DESTINATION "${DIFTRAY_TERMLIB_BUILD_SOURCE}/termscript/stdlib/env")
add_subdirectory("${DIFTRAY_TERMLIB_BUILD_SOURCE}"
                 "${CMAKE_CURRENT_BINARY_DIR}/termlib")
# Termlib's optional stdlib registration is weak; make the static archive
# reachable even in small executables that only use the VM through Notelet.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_link_options(termscript INTERFACE "LINKER:-u,ts_stdlib_register_all")
endif()

# Help-page searches use the vendored Oniguruma implementation.  Keep its own
# test suite out of the compositor build; DiftrayWM covers its integration in
# tests/help_pager.cpp.
set(BUILD_TEST OFF CACHE BOOL "Build Oniguruma's upstream test suite" FORCE)
set(INSTALL_DOCUMENTATION OFF CACHE BOOL "Install Oniguruma upstream documentation" FORCE)
add_subdirectory("${DIFTRAY_THIRD_PARTY}/oniguruma"
                 "${CMAKE_CURRENT_BINARY_DIR}/oniguruma" EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# libtsm
# ---------------------------------------------------------------------------
add_library(diftray_libtsm STATIC
  "${DIFTRAY_THIRD_PARTY}/libtsm/src/shared/shl-htable.c"
  "${DIFTRAY_THIRD_PARTY}/libtsm/src/tsm/tsm-render.c"
  "${DIFTRAY_THIRD_PARTY}/libtsm/src/tsm/tsm-screen.c"
  "${DIFTRAY_THIRD_PARTY}/libtsm/src/tsm/tsm-selection.c"
  "${DIFTRAY_THIRD_PARTY}/libtsm/src/tsm/tsm-unicode.c"
  "${DIFTRAY_THIRD_PARTY}/libtsm/src/tsm/tsm-vte-charsets.c"
  "${DIFTRAY_THIRD_PARTY}/libtsm/src/tsm/tsm-vte.c"
  "${DIFTRAY_THIRD_PARTY}/libtsm/external/wcwidth/wcwidth.c"
)
target_include_directories(diftray_libtsm SYSTEM PUBLIC
  "${DIFTRAY_THIRD_PARTY}/libtsm/src/tsm"
  "${DIFTRAY_THIRD_PARTY}/libtsm/src/shared"
  "${DIFTRAY_THIRD_PARTY}/libtsm/external/wcwidth"
)
# libtsm wants the GNU extensions; it is third-party code, so keep its warnings
# out of our log rather than pretending it is warning-clean.
target_compile_definitions(diftray_libtsm PRIVATE _GNU_SOURCE)
target_compile_options(diftray_libtsm PRIVATE
  -w -Wno-unused-parameter)

# ---------------------------------------------------------------------------
# libptytty
# ---------------------------------------------------------------------------
# libptytty has no CMake build, so the handful of HAVE_* answers it needs are
# written here.  UTMP/WTMP/LASTLOG session logging stays off: NTerm spawns
# ordinary interactive shells, not login sessions, so the utmp code is compiled
# out entirely and the WTMP_SUPPORT/LASTLOG_SUPPORT switches are never reached.
set(DIFTRAYWM_PTYTTY_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/libptytty-gen")
file(MAKE_DIRECTORY "${DIFTRAYWM_PTYTTY_GEN_DIR}")
file(WRITE "${DIFTRAYWM_PTYTTY_GEN_DIR}/config.h"
"#ifndef DIFTRAY_LIBPTYTTY_CONFIG_H
#define DIFTRAY_LIBPTYTTY_CONFIG_H

#define HAVE_PTY_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_POSIX_OPENPT 1
#define HAVE_GETPT 1
#define HAVE_OPENPTY 1
#define HAVE_SETUID 1
#define HAVE_SETREUID 1
#define HAVE_SETRESUID 1
#define UNIX98_PTY 1
#define UTMP_SUPPORT 0

#endif /* DIFTRAY_LIBPTYTTY_CONFIG_H */
")

add_library(diftray_ptytty STATIC
  "${DIFTRAY_THIRD_PARTY}/libptytty/src/c-api.C"
  "${DIFTRAY_THIRD_PARTY}/libptytty/src/fdpass.C"
  "${DIFTRAY_THIRD_PARTY}/libptytty/src/logging.C"
  "${DIFTRAY_THIRD_PARTY}/libptytty/src/proxy.C"
  "${DIFTRAY_THIRD_PARTY}/libptytty/src/ptytty.C"
)
target_include_directories(diftray_ptytty SYSTEM PUBLIC
  "${DIFTRAY_THIRD_PARTY}/libptytty/src"
  "${DIFTRAYWM_PTYTTY_GEN_DIR}"
)
target_compile_definitions(diftray_ptytty PRIVATE PTYTTY_C_API=1)
target_compile_options(diftray_ptytty PRIVATE
  -w -Wno-unused-parameter)
target_link_libraries(diftray_ptytty PUBLIC util)

# LibShell is embedded in the PTY child. Compile its existing parser and
# scripting backend without enabling the vendored project's examples/tools.
add_library(diftray_libshell STATIC
  third_party/libshell/cli/lsh-lex.cpp
  third_party/libshell/cli/libsh-parse.cpp
  third_party/libshell/src/LibShell-Qamrpp.cpp)
target_include_directories(diftray_libshell SYSTEM PUBLIC third_party/libshell/include)
target_include_directories(diftray_libshell SYSTEM PRIVATE
  third_party/libshell/third_party/QaMRpp/include
  third_party/libshell/third_party/QaMRpp/third_party/SerdeTk/include
  third_party/libshell/third_party/QaMRpp/third_party)
target_link_libraries(diftray_libshell PUBLIC ${CMAKE_DL_LIBS})

file(GLOB DIFTRAY_LUA_SOURCES CONFIGURE_DEPENDS "${DIFTRAY_THIRD_PARTY}/lua/*.c")
list(FILTER DIFTRAY_LUA_SOURCES EXCLUDE REGEX "/(lua|luac|ltests|onelua)\\.c$")
add_library(diftray_lua_runtime STATIC ${DIFTRAY_LUA_SOURCES})
target_include_directories(diftray_lua_runtime SYSTEM PUBLIC third_party/lua)
target_compile_definitions(diftray_lua_runtime PRIVATE LUA_USE_LINUX)
target_link_libraries(diftray_lua_runtime PUBLIC ${CMAKE_DL_LIBS} m)

# Configuration formats use the supplied parsers. libyaml is libcyaml's
# platform dependency; it is not vendored in this checkout.
pkg_check_modules(LIBYAML REQUIRED IMPORTED_TARGET yaml-0.1)
add_library(diftray_cyaml STATIC
  third_party/libcyaml/src/mem.c third_party/libcyaml/src/free.c
  third_party/libcyaml/src/load.c third_party/libcyaml/src/save.c
  third_party/libcyaml/src/copy.c third_party/libcyaml/src/util.c
  third_party/libcyaml/src/utf8.c)
target_include_directories(diftray_cyaml SYSTEM PUBLIC third_party/libcyaml/include)
target_compile_definitions(diftray_cyaml PRIVATE VERSION_MAJOR=2 VERSION_MINOR=0 VERSION_PATCH=0 VERSION_DEVEL=1)
target_link_libraries(diftray_cyaml PUBLIC PkgConfig::LIBYAML m)
add_library(diftray_toml STATIC third_party/tomlc99/toml.c)
target_include_directories(diftray_toml SYSTEM PUBLIC third_party/tomlc99)

# ---------------------------------------------------------------------------
# Shared target setup
# ---------------------------------------------------------------------------
function(diftraywm_target_defaults target)
  target_include_directories(${target}
    PRIVATE
      "${DIFTRAYWM_SRC}"
      "${DIFTRAYWM_INCLUDE}"
      "${DIFTRAY_THIRD_PARTY}/wlroots/include"
      "${DIFTRAY_THIRD_PARTY}/libtsm/src/tsm"
      "${DIFTRAY_THIRD_PARTY}/libtsm/src/shared"
      "${DIFTRAY_THIRD_PARTY}/libptytty/src"
      "${DIFTRAY_THIRD_PARTY}/PEGTL/include"
  )
  # wlroots generates its protocol and version headers during its own build, so
  # nothing that includes a wlr header may compile before wlroots_ep has run.
  add_dependencies(${target} wlroots_ep)
  target_compile_definitions(${target} PRIVATE WLR_USE_UNSTABLE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wno-unused-parameter)
    if(DIFTRAYWM_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Notelet bundles
# ---------------------------------------------------------------------------
# notelet.tsc and the example sources are packed into a deterministic ustar
# archive, which is what the compositor loads at run time.
set(DIFTRAY_BUNDLED_NOTELETS_DIR "${CMAKE_CURRENT_BINARY_DIR}/notelets")
set(DIFTRAY_NOTELET_DEFAULT_PATH
  "${DIFTRAY_BUNDLED_NOTELETS_DIR}:${CMAKE_INSTALL_FULL_DATADIR}/diftraywm/notelets")

set(DIFTRAY_BUNDLED_NOTELETS "")
foreach(_notelet hello scratchpad desktop)
  file(GLOB_RECURSE _notelet_sources CONFIGURE_DEPENDS
    "${DIFTRAYWM_ROOT}/notelets/examples/${_notelet}/*")
  set(_bundle "${DIFTRAY_BUNDLED_NOTELETS_DIR}/${_notelet}.notelet")
  add_custom_command(OUTPUT "${_bundle}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${DIFTRAY_BUNDLED_NOTELETS_DIR}"
    COMMAND "${Python3_EXECUTABLE}" "${DIFTRAYWM_ROOT}/scripts/bundle-notelet.py"
      "${DIFTRAYWM_ROOT}/notelets/examples/${_notelet}" "${_bundle}"
    DEPENDS "${DIFTRAYWM_ROOT}/scripts/bundle-notelet.py"
      "${DIFTRAYWM_ROOT}/notelets/notelet.tsc" ${_notelet_sources}
    VERBATIM)
  list(APPEND DIFTRAY_BUNDLED_NOTELETS "${_bundle}")
endforeach()
add_custom_target(diftraywm_bundled_notelets ALL DEPENDS ${DIFTRAY_BUNDLED_NOTELETS})

# ---------------------------------------------------------------------------
# DiftrayWM libraries
# ---------------------------------------------------------------------------
add_library(diftraywm_notelet STATIC
  src/notelet/Notelet.cpp
  src/notelet/NoteletWorker.cpp
)
diftraywm_target_defaults(diftraywm_notelet)
target_link_libraries(diftraywm_notelet PUBLIC termscript)

add_library(diftraywm_compositor STATIC
  src/compositor/Compositor.cpp
  src/compositor/ControlServer.cpp
  src/compositor/Session.cpp
  src/compositor/WaylandRuntime.c
  src/compositor/Output.cpp
  src/compositor/Logger.cpp
  src/compositor/Signals.cpp
  src/config/Config.cpp
  src/config/ConfigProgram.cpp
  src/config/YamlConfig.c
)
diftraywm_target_defaults(diftraywm_compositor)
target_compile_definitions(diftraywm_compositor PRIVATE
  DIFTRAY_NOTELET_DEFAULT_PATH="${DIFTRAY_NOTELET_DEFAULT_PATH}"
  DIFTRAY_HELP_DEFAULT_PATH="${DIFTRAYWM_ROOT}/help:${CMAKE_INSTALL_FULL_DATADIR}/diftraywm/help")
target_link_libraries(diftraywm_compositor PUBLIC
  diftraywm_notelet
  diftray_cyaml diftray_toml
  wlroots::wlroots
  PkgConfig::WLROOTS_BUILD_DEPS
)

add_library(diftraywm_nterm STATIC
  src/nterm/NTerm.cpp
  src/nterm/NTermScript.cpp
  src/nterm/GlyphRenderer.cpp
  src/nterm/GlyphAtlas.cpp
  src/nterm/NTermRenderer.cpp
  src/nterm/ShellBridge.cpp
)
diftraywm_target_defaults(diftraywm_nterm)
# libtsm parses and renders VT; Termlib Termscript scripts operate on cells;
# libptytty owns PTYs, harfbuzz shapes, freetype rasterises, fontconfig resolves fallbacks.
target_link_libraries(diftraywm_nterm PUBLIC
  diftray_libtsm
  diftray_libshell
  diftray_ptytty
  termscript
  PkgConfig::WLROOTS_BUILD_DEPS
  PkgConfig::FREETYPE
  PkgConfig::HARFBUZZ
  PkgConfig::FONTCONFIG
  PkgConfig::NCURSESW
  util
)

add_library(diftraywm_views STATIC
  src/views/Cell.cpp
  src/views/CursorArea.cpp
  src/views/NCursorView.cpp
  src/views/GCursorView.cpp
  src/views/TCursorView.cpp
)
diftraywm_target_defaults(diftraywm_views)
target_link_libraries(diftraywm_views PUBLIC diftraywm_nterm)

add_library(diftraywm_input STATIC
  src/input/KeyboardHandler.cpp
)
diftraywm_target_defaults(diftraywm_input)

add_library(diftraywm_command STATIC
  src/command/CommandBar.cpp
  src/command/CommandHandlers.cpp
  src/command/MobilityHandler.cpp
  src/help/HelpPager.cpp
)
diftraywm_target_defaults(diftraywm_command)
target_link_libraries(diftraywm_command PUBLIC onig)

add_library(diftraywm_theme STATIC
  src/theme/ThemeEngine.cpp
)
diftraywm_target_defaults(diftraywm_theme)

add_library(diftraywm_plugin STATIC
  src/plugin/PluginManager.cpp
)
diftraywm_target_defaults(diftraywm_plugin)
target_link_libraries(diftraywm_plugin PUBLIC ${CMAKE_DL_LIBS})
target_include_directories(diftraywm_plugin SYSTEM PRIVATE third_party/dynalo/include)

add_library(diftraywm_lua STATIC
  src/lua/LuaEngine.cpp
)
diftraywm_target_defaults(diftraywm_lua)
target_include_directories(diftraywm_lua SYSTEM PRIVATE third_party/kaguya/include)
target_link_libraries(diftraywm_lua PUBLIC diftray_lua_runtime)

# The keymap model and its INI reader. Kept free of evdev and udev so the
# compositor, diftrayctl, the tests and diftrayremap all parse the same file
# without pulling in privileges they do not have.
add_library(diftraywm_keymap STATIC
  src/keymap/Keymap.cpp
)
diftraywm_target_defaults(diftraywm_keymap)

# The system-wide evdev backend. Separate from the model on purpose: it needs
# /dev/input and /dev/uinput, which only a privileged daemon has.
add_library(diftraywm_remap STATIC
  src/keymap/EvdevRemapper.cpp
)
diftraywm_target_defaults(diftraywm_remap)
target_link_libraries(diftraywm_remap PUBLIC
  diftraywm_keymap
  PkgConfig::LIBUDEV
  PkgConfig::LIBEVDEV
)

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
# The static libraries above are deliberately not linked to one another in
# dependency order here: diftraywm_link_exe lists them all and CMake resolves
# the archive order, which keeps the list readable and stops a future target
# from silently inheriting the wrong one.
function(diftraywm_link_exe target)
  target_link_libraries(${target} PRIVATE
    diftraywm_compositor
    diftraywm_command
    diftraywm_input
    diftraywm_views
    diftraywm_nterm
    diftraywm_theme
    diftraywm_plugin
    diftraywm_lua
    diftraywm_keymap
    diftraywm_notelet
    diftray_libtsm
    diftray_ptytty
    termscript
    wlroots::wlroots
    PkgConfig::WLROOTS_BUILD_DEPS
    PkgConfig::FREETYPE
    PkgConfig::HARFBUZZ
    PkgConfig::FONTCONFIG
    PkgConfig::NCURSESW
    Threads::Threads
    m
  )
endfunction()

add_executable(diftraywm src/main.cpp)
diftraywm_target_defaults(diftraywm)
diftraywm_link_exe(diftraywm)

add_executable(diftray src/main.cpp)
diftraywm_target_defaults(diftray)
diftraywm_link_exe(diftray)

add_dependencies(diftraywm diftraywm_bundled_notelets)
add_dependencies(diftray diftraywm_bundled_notelets)

# diftrayctl is a thin client: it needs the control-socket path helper and
# libc sockets, nothing from the compositor or wlroots. Keeping it standalone
# means it still links and runs when a session is down.
add_executable(diftrayctl src/ctl/diftrayctl.cpp)
target_include_directories(diftrayctl PRIVATE "${DIFTRAYWM_SRC}")
target_compile_features(diftrayctl PRIVATE cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(diftrayctl PRIVATE -Wall -Wextra -Wno-unused-parameter)
  if(DIFTRAYWM_WERROR)
    target_compile_options(diftrayctl PRIVATE -Werror)
  endif()
endif()

# diftrayremap is the system-wide half of the key remapper. It is a separate
# binary, not a mode of the compositor, because grabbing the physical keyboard
# and writing to uinput needs privileges the compositor must not have: running a
# window manager as root, or taking the keyboard away from the session this
# compositor is nested inside. It shares the keymap model with the compositor, so
# one keymap.ini defines the mapping for both halves.
add_executable(diftrayremap src/ctl/diftrayremap.cpp)
target_include_directories(diftrayremap PRIVATE "${DIFTRAYWM_SRC}")
target_compile_features(diftrayremap PRIVATE cxx_std_20)
target_link_libraries(diftrayremap PRIVATE diftraywm_remap)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(diftrayremap PRIVATE -Wall -Wextra -Wno-unused-parameter)
  if(DIFTRAYWM_WERROR)
    target_compile_options(diftrayremap PRIVATE -Werror)
  endif()
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS diftray diftraywm diftrayctl diftrayremap
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "${WLROOTS_LIBRARY}" DESTINATION "${CMAKE_INSTALL_LIBDIR}/diftraywm")
install(DIRECTORY "${DIFTRAY_VENDOR_PREFIX}/lib/"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/diftraywm"
  FILES_MATCHING PATTERN "*.so*"
  PATTERN "pkgconfig" EXCLUDE)

install(FILES ${DIFTRAY_BUNDLED_NOTELETS}
  DESTINATION "${CMAKE_INSTALL_DATADIR}/diftraywm/notelets")
install(DIRECTORY examples DESTINATION "${CMAKE_INSTALL_DATADIR}/diftraywm")
install(FILES include/DiftrayWM-Plugin.h DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES EXTENSIONS.md MONITORS.md CONFIGURATION.md DESTINATION "${CMAKE_INSTALL_DATADIR}/diftraywm")
install(FILES diftray.conf
  DESTINATION "${CMAKE_INSTALL_DATADIR}/diftraywm")
# The example keymap ships alongside the config that points at it, so a fresh
# install has working keys and a file to copy.
install(FILES keymap.ini
  DESTINATION "${CMAKE_INSTALL_DATADIR}/diftraywm")
install(DIRECTORY themes
  DESTINATION "${CMAKE_INSTALL_DATADIR}/diftraywm"
  FILES_MATCHING PATTERN "*.css")
install(DIRECTORY help
  DESTINATION "${CMAKE_INSTALL_DATADIR}/diftraywm"
  FILES_MATCHING PATTERN "*.1")
install(FILES GUIDE.md DESTINATION "${CMAKE_INSTALL_DATADIR}/diftraywm")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
