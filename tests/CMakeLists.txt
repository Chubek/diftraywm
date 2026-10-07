add_executable(diftraywm_smoke_test smoke.cpp)
target_compile_features(diftraywm_smoke_test PRIVATE cxx_std_20)
target_include_directories(diftraywm_smoke_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
  ${DIFTRAY_THIRD_PARTY}/wlroots/include
)
target_link_libraries(diftraywm_smoke_test PRIVATE diftraywm_views)
add_test(NAME diftraywm_smoke_test COMMAND diftraywm_smoke_test)

add_executable(diftraywm_navigation_test navigation.cpp)
diftraywm_target_defaults(diftraywm_navigation_test)
diftraywm_link_exe(diftraywm_navigation_test)
add_test(NAME diftraywm_navigation_test COMMAND diftraywm_navigation_test)

add_executable(diftraywm_notelet_test notelet.cpp)
diftraywm_target_defaults(diftraywm_notelet_test)
target_link_libraries(diftraywm_notelet_test PRIVATE diftraywm_notelet)
target_compile_definitions(diftraywm_notelet_test PRIVATE
  DIFTRAY_BUNDLED_NOTELETS_DIR="${DIFTRAY_BUNDLED_NOTELETS_DIR}")
add_dependencies(diftraywm_notelet_test diftraywm_bundled_notelets)
add_test(NAME diftraywm_notelet_test COMMAND diftraywm_notelet_test)

add_executable(diftraywm_help_pager_test help_pager.cpp)
diftraywm_target_defaults(diftraywm_help_pager_test)
target_link_libraries(diftraywm_help_pager_test PRIVATE diftraywm_command)
target_compile_definitions(diftraywm_help_pager_test PRIVATE
  DIFTRAY_HELP_TEST_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../help")
add_test(NAME diftraywm_help_pager_test COMMAND diftraywm_help_pager_test)

add_executable(diftraywm_nterm_display_test nterm_display.cpp)
diftraywm_target_defaults(diftraywm_nterm_display_test)
target_link_libraries(diftraywm_nterm_display_test PRIVATE diftraywm_nterm)
add_test(NAME diftraywm_nterm_display_test COMMAND diftraywm_nterm_display_test)

add_executable(diftraywm_outputs_test outputs.cpp)
diftraywm_target_defaults(diftraywm_outputs_test)
diftraywm_link_exe(diftraywm_outputs_test)
add_test(NAME diftraywm_outputs_test COMMAND diftraywm_outputs_test)

add_executable(diftraywm_headless_test headless.cpp)
diftraywm_target_defaults(diftraywm_headless_test)
diftraywm_link_exe(diftraywm_headless_test)
add_dependencies(diftraywm_headless_test diftraywm_bundled_notelets)
add_test(NAME diftraywm_headless_test COMMAND diftraywm_headless_test)
set_tests_properties(diftraywm_headless_test PROPERTIES TIMEOUT 15)

add_executable(diftraywm_nterm_input_test nterm_input.cpp)
diftraywm_target_defaults(diftraywm_nterm_input_test)
target_link_libraries(diftraywm_nterm_input_test PRIVATE diftraywm_nterm)
add_test(NAME diftraywm_nterm_input_test COMMAND diftraywm_nterm_input_test)

# The keymap subsystem. It links only the model and the evdev backend, neither
# of which needs a seat, a device or root: map_event() is a pure function, so
# the whole decision table is covered without privileges.
add_executable(diftraywm_keymap_test keymap.cpp)
diftraywm_target_defaults(diftraywm_keymap_test)
target_link_libraries(diftraywm_keymap_test PRIVATE diftraywm_remap)
add_test(NAME diftraywm_keymap_test COMMAND diftraywm_keymap_test)

# The compositor's half of the keymap: a profile applied to real key events.
add_executable(diftraywm_keymap_compositor_test keymap_compositor.cpp)
diftraywm_target_defaults(diftraywm_keymap_compositor_test)
diftraywm_link_exe(diftraywm_keymap_compositor_test)
add_test(NAME diftraywm_keymap_compositor_test COMMAND diftraywm_keymap_compositor_test)

add_executable(diftraywm_config_test config.cpp ../src/config/ConfigProgram.cpp ../src/config/Config.cpp ../src/config/YamlConfig.c)
target_include_directories(diftraywm_config_test PRIVATE ../src ${DIFTRAY_THIRD_PARTY}/PEGTL/include)
target_link_libraries(diftraywm_config_test PRIVATE diftray_cyaml diftray_toml PkgConfig::WLROOTS_BUILD_DEPS)
add_test(NAME diftraywm_config_test COMMAND diftraywm_config_test)

add_library(diftraywm_test_plugin MODULE extension_plugin.c)
target_include_directories(diftraywm_test_plugin PRIVATE ../include)
add_library(diftraywm_failed_plugin MODULE extension_plugin.c)
target_include_directories(diftraywm_failed_plugin PRIVATE ../include)
target_compile_definitions(diftraywm_failed_plugin PRIVATE FAIL_INIT)
add_executable(diftraywm_extensions_test extensions.cpp)
diftraywm_target_defaults(diftraywm_extensions_test)
diftraywm_link_exe(diftraywm_extensions_test)
target_compile_definitions(diftraywm_extensions_test PRIVATE
  TEST_PLUGIN="$<TARGET_FILE:diftraywm_test_plugin>"
  FAILED_PLUGIN="$<TARGET_FILE:diftraywm_failed_plugin>")
add_dependencies(diftraywm_extensions_test diftraywm_test_plugin diftraywm_failed_plugin)
add_test(NAME diftraywm_extensions_test COMMAND diftraywm_extensions_test)
set_tests_properties(diftraywm_extensions_test PROPERTIES TIMEOUT 10)

# Every Lua extension shipped in extensions/, loaded into the real engine and
# exercised through the Command Bar. A syntax error, a call to a function the
# sandbox removed or a runaway loop fails here rather than on a desktop.
add_executable(diftraywm_extension_scripts_test extension_scripts.cpp)
diftraywm_target_defaults(diftraywm_extension_scripts_test)
diftraywm_link_exe(diftraywm_extension_scripts_test)
target_compile_definitions(diftraywm_extension_scripts_test PRIVATE
  EXTENSION_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../extensions")
add_dependencies(diftraywm_extension_scripts_test diftraywm_bundled_notelets)
add_test(NAME diftraywm_extension_scripts_test
  COMMAND diftraywm_extension_scripts_test)
set_tests_properties(diftraywm_extension_scripts_test PROPERTIES TIMEOUT 30)

add_executable(diftraywm_shell_test shell.cpp)
diftraywm_target_defaults(diftraywm_shell_test)
target_link_libraries(diftraywm_shell_test PRIVATE diftraywm_nterm)
add_test(NAME diftraywm_shell_test COMMAND diftraywm_shell_test)
set_tests_properties(diftraywm_shell_test PROPERTIES TIMEOUT 15)

add_test(NAME diftraywm_extension_cli_test COMMAND ${CMAKE_COMMAND} -E env
  DIFTRAYWM_LOGIC_ONLY=1
  DIFTRAYWM_CONFIG=${DIFTRAYWM_ROOT}/examples/diftray.yaml
  DIFTRAYWM_EXTENSION_PATH=${DIFTRAYWM_ROOT}/examples
  DIFTRAYWM_PLUGIN_PATH=/nonexistent/diftray-plugins
  $<TARGET_FILE:diftray> --command scratchpad)
set_tests_properties(diftraywm_extension_cli_test PROPERTIES
  PASS_REGULAR_EXPRESSION "opened notelet scratchpad" TIMEOUT 10)

add_executable(diftraywm_config_program_test config_program.cpp)
diftraywm_target_defaults(diftraywm_config_program_test)
diftraywm_link_exe(diftraywm_config_program_test)
add_test(NAME diftraywm_config_program_test COMMAND diftraywm_config_program_test)
set_tests_properties(diftraywm_config_program_test PROPERTIES TIMEOUT 10)

add_executable(diftraywm_mux_test mux.cpp)
diftraywm_target_defaults(diftraywm_mux_test)
diftraywm_link_exe(diftraywm_mux_test)
add_test(NAME diftraywm_mux_test COMMAND diftraywm_mux_test)
set_tests_properties(diftraywm_mux_test PROPERTIES TIMEOUT 15)

# Drives the installed binaries end to end: a real headless compositor plus the
# real diftrayctl, over the control socket.
add_executable(diftraywm_diftrayctl_test diftrayctl.cpp)
diftraywm_target_defaults(diftraywm_diftrayctl_test)
target_compile_definitions(diftraywm_diftrayctl_test PRIVATE
  DIFTRAYCTL_BINARY="$<TARGET_FILE:diftrayctl>"
  DIFTRAYWM_BINARY="$<TARGET_FILE:diftraywm>")
add_dependencies(diftraywm_diftrayctl_test diftrayctl diftraywm diftraywm_test_plugin
  diftraywm_bundled_notelets)
add_test(NAME diftraywm_diftrayctl_test COMMAND diftraywm_diftrayctl_test)
set_tests_properties(diftraywm_diftrayctl_test PROPERTIES TIMEOUT 120)

add_executable(diftraywm_launcher_test launcher.cpp)
diftraywm_target_defaults(diftraywm_launcher_test)
diftraywm_link_exe(diftraywm_launcher_test)
add_dependencies(diftraywm_launcher_test diftraywm_bundled_notelets)
add_test(NAME diftraywm_launcher_test COMMAND diftraywm_launcher_test)
set_tests_properties(diftraywm_launcher_test PROPERTIES TIMEOUT 15)

add_executable(diftraywm_control_server_test control_server.cpp)
diftraywm_target_defaults(diftraywm_control_server_test)
diftraywm_link_exe(diftraywm_control_server_test)
add_test(NAME diftraywm_control_server_test COMMAND diftraywm_control_server_test)
set_tests_properties(diftraywm_control_server_test PROPERTIES TIMEOUT 15)

add_executable(diftraywm_rendering_test rendering.cpp)
diftraywm_target_defaults(diftraywm_rendering_test)
target_link_libraries(diftraywm_rendering_test PRIVATE diftraywm_nterm)
add_test(NAME diftraywm_rendering_test COMMAND diftraywm_rendering_test)

add_executable(diftraywm_theme_test theme.cpp)
diftraywm_target_defaults(diftraywm_theme_test)
diftraywm_link_exe(diftraywm_theme_test)
add_test(NAME diftraywm_theme_test COMMAND diftraywm_theme_test)

add_executable(diftraywm_animation_test animation.cpp)
diftraywm_target_defaults(diftraywm_animation_test)
target_link_libraries(diftraywm_animation_test PRIVATE diftraywm_theme)
add_test(NAME diftraywm_animation_test COMMAND diftraywm_animation_test)

add_executable(diftraywm_gcursor_launch_test gcursor_launch.cpp)
diftraywm_target_defaults(diftraywm_gcursor_launch_test)
diftraywm_link_exe(diftraywm_gcursor_launch_test)
add_test(NAME diftraywm_gcursor_launch_test COMMAND diftraywm_gcursor_launch_test)
set_tests_properties(diftraywm_gcursor_launch_test PROPERTIES TIMEOUT 10)

add_executable(diftraywm_keyboard_input_test keyboard_input.cpp)
diftraywm_target_defaults(diftraywm_keyboard_input_test)
diftraywm_link_exe(diftraywm_keyboard_input_test)
add_test(NAME diftraywm_keyboard_input_test COMMAND diftraywm_keyboard_input_test)
set_tests_properties(diftraywm_keyboard_input_test PROPERTIES TIMEOUT 10)
