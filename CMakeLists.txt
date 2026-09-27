# DiftrayWM -- top-level build.
#
# Dependency policy
# -----------------
# Vendored *sources* that this build compiles itself:
#   libtsm, libvterm, libptytty, domterm (Termscript + stdlib), PEGTL.
#
# wlroots is vendored but is a meson project, so scripts/build-wlroots.sh
# builds it out of tree and the result is consumed as an imported library.
#
# Everything else -- wayland, libinput, libxkbcommon, pixman, libdrm, freetype,
# harfbuzz, fontconfig, ncursesw -- is discovered with pkg-config and linked
# from the system.  The copies under third_party/ are not part of this build,
# so their headers have to stay off the include path: the vendored copies lag
# the installed libraries (libxkbcommon 1.14.0-beta1 vs 1.13.2, freetype 2.14.3
# vs 26.6.20, harfbuzz 14.2.1 vs 14.4.0) and shadowing the real headers makes
# wlr/types/wlr_keyboard.h and the NTerm renderer compile against declarations
# the linked shared object does not implement.

cmake_minimum_required(VERSION 3.16)

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

set(_diftraywm_vendored_required wlroots domterm libvterm libtsm libptytty PEGTL)
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

# The vendored domterm generates the Termscript parser with this script and
# hard-codes its path, so a missing copy only shows up much later as an opaque
# "No rule to make target" from the generator.  Check it while we can still
# explain it.
set(DIFTRAY_AUROCKS "${DIFTRAYWM_ROOT}/scripts/aurocks.pl")
if(NOT EXISTS "${DIFTRAY_AUROCKS}")
  message(FATAL_ERROR
    "${DIFTRAY_AUROCKS} is missing. third_party/domterm compiles "
    "termscript/Termscript.g with it to produce termscript_parser.c, and "
    "refuses to build without it.")
endif()

# ---------------------------------------------------------------------------
# System libraries
# ---------------------------------------------------------------------------
pkg_check_modules(WLROOTS_BUILD_DEPS REQUIRED IMPORTED_TARGET
  pixman-1 libdrm wayland-server xkbcommon libinput)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(NCURSESW REQUIRED IMPORTED_TARGET ncursesw)

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

set(WLROOTS_LIBRARY "${WLROOTS_BUILD_DIR}/libwlroots-${WLROOTS_ABI_VERSION}.so")

# This is a custom target rather than an OUTPUT-driven custom command on
# purpose: meson/ninja own wlroots' own incrementality, and a custom target
# re-invokes the script on every build so edits to the vendored tree (or a
# deleted .so) are picked up without re-configuring CMake.
add_custom_target(wlroots_ep
  COMMAND "${CMAKE_COMMAND}" -E env
          "MESON_BIN=${MESON_EXECUTABLE}"
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
# domterm: Termscript plus its standard library
# ---------------------------------------------------------------------------
# Provides the `termscript`, `termscript_standalone`, `termscript_stdlib`,
# `domterm` and `termscript-driver` targets, and installs the stdlib .tsc
# companions that notelets import at run time.
add_subdirectory("${DIFTRAY_THIRD_PARTY}/domterm"
                 "${CMAKE_CURRENT_BINARY_DIR}/domterm")

# ---------------------------------------------------------------------------
# libvterm
# ---------------------------------------------------------------------------
# NTerm only needs the state machine, never libvterm's curses demo UI, and
# DEFINE_CURSES has to be set before the subproject reads it.
set(DEFINE_CURSES OFF CACHE BOOL "Build libvterm with curses UI" FORCE)
add_subdirectory("${DIFTRAY_THIRD_PARTY}/libvterm"
                 "${CMAKE_CURRENT_BINARY_DIR}/libvterm" EXCLUDE_FROM_ALL)

# libvterm's nc_wrapper.c includes <ncursesw/curses.h> unconditionally on Linux,
# and that path only exists when ncurses installs it.  Point it at the real
# header instead of requiring a particular ncurses layout.
set(DIFTRAYWM_NCURSES_SHIM "${CMAKE_CURRENT_BINARY_DIR}/ncurses-shim")
file(MAKE_DIRECTORY "${DIFTRAYWM_NCURSES_SHIM}/ncursesw")
file(WRITE "${DIFTRAYWM_NCURSES_SHIM}/ncursesw/curses.h"
     "#include <curses.h>\n")
file(WRITE "${DIFTRAYWM_NCURSES_SHIM}/ncursesw/ncurses.h"
     "#include <ncurses.h>\n")
if(TARGET vterm-static)
  target_include_directories(vterm-static PUBLIC "${DIFTRAYWM_NCURSES_SHIM}")
endif()
foreach(_vterm_target vterm.o vterm-shared)
  if(TARGET ${_vterm_target})
    target_include_directories(${_vterm_target} PRIVATE
      "${DIFTRAYWM_NCURSES_SHIM}")
  endif()
endforeach()

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
      "${DIFTRAY_THIRD_PARTY}/libvterm"
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

add_custom_command(
  OUTPUT "${DIFTRAY_BUNDLED_NOTELETS_DIR}/hello.notelet"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "${DIFTRAY_BUNDLED_NOTELETS_DIR}"
  COMMAND "${Python3_EXECUTABLE}" "${DIFTRAYWM_ROOT}/scripts/bundle-notelet.py"
          "${DIFTRAYWM_ROOT}/notelets/examples/hello"
          "${DIFTRAY_BUNDLED_NOTELETS_DIR}/hello.notelet"
  DEPENDS "${DIFTRAYWM_ROOT}/scripts/bundle-notelet.py"
          "${DIFTRAYWM_ROOT}/notelets/notelet.tsc"
          "${DIFTRAYWM_ROOT}/notelets/examples/hello/main.tsc"
          "${DIFTRAYWM_ROOT}/notelets/examples/hello/assets/message.txt"
  COMMENT "Bundling notelets"
  VERBATIM)
add_custom_target(diftraywm_bundled_notelets ALL
  DEPENDS "${DIFTRAY_BUNDLED_NOTELETS_DIR}/hello.notelet")

# ---------------------------------------------------------------------------
# DiftrayWM libraries
# ---------------------------------------------------------------------------
add_library(diftraywm_notelet STATIC
  src/notelet/Notelet.cpp
)
diftraywm_target_defaults(diftraywm_notelet)
target_link_libraries(diftraywm_notelet PUBLIC termscript)

add_library(diftraywm_compositor STATIC
  src/compositor/Compositor.cpp
  src/compositor/WaylandRuntime.c
  src/compositor/DamageTracker.cpp
  src/compositor/Output.cpp
  src/compositor/Protocols.cpp
  src/compositor/Logger.cpp
  src/compositor/Signals.cpp
  src/config/Config.cpp
)
diftraywm_target_defaults(diftraywm_compositor)
target_compile_definitions(diftraywm_compositor PRIVATE
  DIFTRAY_NOTELET_DEFAULT_PATH="${DIFTRAY_NOTELET_DEFAULT_PATH}")
target_link_libraries(diftraywm_compositor PUBLIC
  diftraywm_notelet
  wlroots::wlroots
  PkgConfig::WLROOTS_BUILD_DEPS
)

add_library(diftraywm_nterm STATIC
  src/nterm/NTerm.cpp
  src/nterm/GlyphRenderer.cpp
  src/nterm/GlyphAtlas.cpp
  src/nterm/NTermRenderer.cpp
  src/nterm/ShellBridge.cpp
)
diftraywm_target_defaults(diftraywm_nterm)
# AGENTS.md section 6: libtsm parses VT, libvterm holds the grid, libptytty owns
# the PTY, harfbuzz shapes, freetype rasterises, fontconfig resolves fallbacks.
target_link_libraries(diftraywm_nterm PUBLIC
  diftray_libtsm
  diftray_ptytty
  vterm-static
  PkgConfig::WLROOTS_BUILD_DEPS
  PkgConfig::FREETYPE
  PkgConfig::HARFBUZZ
  PkgConfig::FONTCONFIG
  PkgConfig::NCURSESW
  util
)

add_library(diftraywm_views STATIC
  src/views/View.cpp
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
)
diftraywm_target_defaults(diftraywm_command)

add_library(diftraywm_theme STATIC
  src/theme/ThemeEngine.cpp
)
diftraywm_target_defaults(diftraywm_theme)

add_library(diftraywm_plugin STATIC
  src/plugin/PluginManager.cpp
)
diftraywm_target_defaults(diftraywm_plugin)
target_link_libraries(diftraywm_plugin PUBLIC ${CMAKE_DL_LIBS})

add_library(diftraywm_lua STATIC
  src/lua/LuaEngine.cpp
)
diftraywm_target_defaults(diftraywm_lua)

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
    diftraywm_notelet
    diftray_libtsm
    diftray_ptytty
    vterm-static
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

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS diftray diftraywm RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "${DIFTRAY_BUNDLED_NOTELETS_DIR}/hello.notelet"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/diftraywm/notelets")
install(FILES diftray.conf
  DESTINATION "${CMAKE_INSTALL_DATADIR}/diftraywm")
install(DIRECTORY themes
  DESTINATION "${CMAKE_INSTALL_DATADIR}/diftraywm"
  FILES_MATCHING PATTERN "*.css")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
