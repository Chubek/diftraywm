cmake_minimum_required(VERSION 3.25)

project(DiftrayWM LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(ExternalProject)
include(CTest)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WLROOTS_BUILD_DEPS REQUIRED IMPORTED_TARGET
  pixman-1 libdrm wayland-server xkbcommon libinput)

set(DIFTRAYWM_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(DIFTRAYWM_SRC ${DIFTRAYWM_ROOT}/src)
set(DIFTRAYWM_INCLUDE ${DIFTRAYWM_ROOT}/include)

function(diftraywm_target_defaults target)
  target_include_directories(${target}
    PRIVATE
      ${DIFTRAYWM_SRC}
      ${DIFTRAYWM_INCLUDE}
      ${DIFTRAYWM_ROOT}/third_party/wayland/src
      ${DIFTRAYWM_ROOT}/third_party/wlroots/include
      ${DIFTRAYWM_ROOT}/third_party/libinput/include
      ${DIFTRAYWM_ROOT}/third_party/libxkbcommon/include
      ${DIFTRAYWM_ROOT}/third_party/libtsm/src
      ${DIFTRAYWM_ROOT}/third_party/libvterm
      ${DIFTRAYWM_ROOT}/third_party/freetype/include
      ${DIFTRAYWM_ROOT}/third_party/harfbuzz/src
      ${DIFTRAYWM_ROOT}/third_party/fontconfig
      ${DIFTRAYWM_ROOT}/third_party/PEGTL/include
  )
  target_compile_options(${target}
    PRIVATE -Wall -Wextra -Wno-unused-parameter
            $<$<CONFIG:Debug>:-Werror>)
endfunction()

add_library(wayland_headers INTERFACE)
target_include_directories(wayland_headers INTERFACE ${DIFTRAYWM_ROOT}/third_party/wayland/src)
ExternalProject_Add(wayland_ep SOURCE_DIR ${DIFTRAYWM_ROOT}/third_party/wayland
  CONFIGURE_COMMAND "" BUILD_COMMAND "" INSTALL_COMMAND "" TEST_COMMAND "")

add_library(wlroots_headers INTERFACE)
target_include_directories(wlroots_headers INTERFACE
  ${DIFTRAYWM_ROOT}/third_party/wlroots/include
  ${DIFTRAYWM_ROOT}/build-wlroots/include)
ExternalProject_Add(wlroots_ep SOURCE_DIR ${DIFTRAYWM_ROOT}/third_party/wlroots
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ${DIFTRAYWM_ROOT}/scripts/build-wlroots.sh
  INSTALL_COMMAND ""
  TEST_COMMAND ""
  BUILD_ALWAYS TRUE)
add_library(wlroots_runtime SHARED IMPORTED GLOBAL)
set_target_properties(wlroots_runtime PROPERTIES
  IMPORTED_LOCATION ${DIFTRAYWM_ROOT}/build-wlroots/libwlroots-0.21.so)
add_dependencies(wlroots_runtime wlroots_ep)

add_library(libinput_headers INTERFACE)
target_include_directories(libinput_headers INTERFACE ${DIFTRAYWM_ROOT}/third_party/libinput/include)
ExternalProject_Add(libinput_ep SOURCE_DIR ${DIFTRAYWM_ROOT}/third_party/libinput
  CONFIGURE_COMMAND "" BUILD_COMMAND "" INSTALL_COMMAND "" TEST_COMMAND "")

add_library(libxkbcommon_headers INTERFACE)
target_include_directories(libxkbcommon_headers INTERFACE ${DIFTRAYWM_ROOT}/third_party/libxkbcommon/include)
ExternalProject_Add(libxkbcommon_ep SOURCE_DIR ${DIFTRAYWM_ROOT}/third_party/libxkbcommon
  CONFIGURE_COMMAND "" BUILD_COMMAND "" INSTALL_COMMAND "" TEST_COMMAND "")

add_library(libtsm_headers INTERFACE)
target_include_directories(libtsm_headers INTERFACE ${DIFTRAYWM_ROOT}/third_party/libtsm/src)
ExternalProject_Add(libtsm_ep SOURCE_DIR ${DIFTRAYWM_ROOT}/third_party/libtsm
  CONFIGURE_COMMAND "" BUILD_COMMAND "" INSTALL_COMMAND "" TEST_COMMAND "")

add_library(fontconfig_headers INTERFACE)
target_include_directories(fontconfig_headers INTERFACE ${DIFTRAYWM_ROOT}/third_party/fontconfig)
ExternalProject_Add(fontconfig_ep SOURCE_DIR ${DIFTRAYWM_ROOT}/third_party/fontconfig
  CONFIGURE_COMMAND "" BUILD_COMMAND "" INSTALL_COMMAND "" TEST_COMMAND "")

add_library(harfbuzz INTERFACE)
add_library(freetype INTERFACE)
add_library(ptytty INTERFACE)
add_library(vterm-static INTERFACE)
add_library(pegtl INTERFACE)
add_library(taocpp::pegtl ALIAS pegtl)

ExternalProject_Add(harfbuzz_ep SOURCE_DIR ${DIFTRAYWM_ROOT}/third_party/harfbuzz
  CONFIGURE_COMMAND "" BUILD_COMMAND "" INSTALL_COMMAND "" TEST_COMMAND "")
ExternalProject_Add(freetype_ep SOURCE_DIR ${DIFTRAYWM_ROOT}/third_party/freetype
  CONFIGURE_COMMAND "" BUILD_COMMAND "" INSTALL_COMMAND "" TEST_COMMAND "")
ExternalProject_Add(libptytty_ep SOURCE_DIR ${DIFTRAYWM_ROOT}/third_party/libptytty
  CONFIGURE_COMMAND "" BUILD_COMMAND "" INSTALL_COMMAND "" TEST_COMMAND "")
ExternalProject_Add(libvterm_ep SOURCE_DIR ${DIFTRAYWM_ROOT}/third_party/libvterm
  CONFIGURE_COMMAND "" BUILD_COMMAND "" INSTALL_COMMAND "" TEST_COMMAND "")
ExternalProject_Add(pegtl_ep SOURCE_DIR ${DIFTRAYWM_ROOT}/third_party/PEGTL
  CONFIGURE_COMMAND "" BUILD_COMMAND "" INSTALL_COMMAND "" TEST_COMMAND "")

add_library(diftraywm_compositor STATIC
  src/compositor/Compositor.cpp
  src/compositor/WaylandRuntime.c
  src/config/Config.cpp
  src/compositor/Output.cpp
  src/compositor/DamageTracker.cpp
  src/compositor/Protocols.cpp
  src/compositor/Logger.cpp
  src/compositor/Signals.cpp
)
diftraywm_target_defaults(diftraywm_compositor)
target_compile_definitions(diftraywm_compositor PRIVATE WLR_USE_UNSTABLE)
target_link_libraries(diftraywm_compositor PRIVATE
  wayland_headers wlroots_headers libinput_headers libxkbcommon_headers
  libtsm_headers fontconfig_headers harfbuzz freetype ptytty vterm-static
  taocpp::pegtl wlroots_runtime PkgConfig::WLROOTS_BUILD_DEPS)

add_library(diftraywm_views STATIC
  src/views/View.cpp
  src/views/Cell.cpp
  src/views/CursorArea.cpp
  src/views/NCursorView.cpp
  src/views/GCursorView.cpp
  src/views/TCursorView.cpp
)
diftraywm_target_defaults(diftraywm_views)
target_link_libraries(diftraywm_views PRIVATE wlroots_headers)

add_library(diftraywm_nterm STATIC
  src/nterm/NTerm.cpp
  src/nterm/GlyphRenderer.cpp
  src/nterm/GlyphAtlas.cpp
  src/nterm/NTermRenderer.cpp
  src/nterm/ShellBridge.cpp
)
diftraywm_target_defaults(diftraywm_nterm)
target_link_libraries(diftraywm_nterm PRIVATE
  wayland_headers wlroots_headers libtsm_headers fontconfig_headers
  harfbuzz freetype ptytty vterm-static util)

add_library(diftraywm_input STATIC
  src/input/KeyboardHandler.cpp
)
diftraywm_target_defaults(diftraywm_input)
target_link_libraries(diftraywm_input PRIVATE wlroots_headers libxkbcommon_headers)

add_library(diftraywm_command STATIC
  src/command/CommandBar.cpp
  src/command/CommandHandlers.cpp
  src/command/MobilityHandler.cpp
)
diftraywm_target_defaults(diftraywm_command)
target_link_libraries(diftraywm_command PRIVATE wlroots_headers)

add_library(diftraywm_theme STATIC
  src/theme/ThemeEngine.cpp
)
diftraywm_target_defaults(diftraywm_theme)

add_library(diftraywm_plugin STATIC
  src/plugin/PluginManager.cpp
)
diftraywm_target_defaults(diftraywm_plugin)
target_link_libraries(diftraywm_plugin PRIVATE ${CMAKE_DL_LIBS})

add_library(diftraywm_lua STATIC
  src/lua/LuaEngine.cpp
)
diftraywm_target_defaults(diftraywm_lua)

add_executable(diftraywm src/main.cpp)
diftraywm_target_defaults(diftraywm)
target_link_libraries(diftraywm PRIVATE
  diftraywm_compositor diftraywm_views diftraywm_nterm diftraywm_input
  diftraywm_command diftraywm_theme diftraywm_plugin diftraywm_lua)

add_executable(diftray src/main.cpp)
diftraywm_target_defaults(diftray)
target_link_libraries(diftray PRIVATE
  diftraywm_compositor diftraywm_views diftraywm_nterm diftraywm_input
  diftraywm_command diftraywm_theme diftraywm_plugin diftraywm_lua)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
