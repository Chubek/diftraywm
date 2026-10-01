# Termlib -- PTY/TTY management, Terminfo, session recording, connections,
# terminal orchestration and the Termscript automation language.
cmake_minimum_required(VERSION 3.15)
project(Termlib LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)

# Perl drives scripts/aurocks.pl, which compiles Termscript.g into the
# Termscript parser.  There is no fallback: the grammar is the source of
# truth for the language front end.
find_program(TERMLIB_PERL perl REQUIRED)
set(TERMLIB_AUROCKS ${CMAKE_CURRENT_SOURCE_DIR}/scripts/aurocks.pl)

# ── generated Termscript parser ──────────────────────────────────────
# One translation unit, shared by the standalone runtime below.  The
# custom entry point avoids clashing with any other aurocks output; the
# ts_mk_* actions it calls live in ts_runtime.c.
set(TERMLIB_PARSER_C ${CMAKE_CURRENT_BINARY_DIR}/termscript_parser.c)
add_custom_command(
  OUTPUT ${TERMLIB_PARSER_C}
  COMMAND ${TERMLIB_PERL} ${TERMLIB_AUROCKS} --entrypoint ts_gram_parse
          ${CMAKE_CURRENT_SOURCE_DIR}/termscript/Termscript.g
          > ${TERMLIB_PARSER_C}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/termscript/Termscript.g
          ${TERMLIB_AUROCKS}
  COMMENT "Generating Termscript parser (aurocks)"
  VERBATIM
)

# ── libtermlib (core: no Termscript) ─────────────────────────────────
add_library(termlib STATIC
  termlib.c
  src/dt_util.c
  src/dt_terminfo.c
  src/dt_tiexpand.c
  src/dt_tty.c
  src/dt_pty.c
  src/dt_record.c
  src/dt_conn.c
  src/dt_puppeteer.c
)

target_include_directories(termlib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/termlib>
)
target_include_directories(termlib PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(termlib PUBLIC Threads::Threads)

# ── libtermscript_standalone (embedding + extension API, no Termlib) ─
add_library(termscript_standalone STATIC
  termscript/ts_runtime.c
  ${TERMLIB_PARSER_C}
)
target_include_directories(termscript_standalone PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(termscript_standalone PRIVATE Threads::Threads)

# ── libtermscript (Termlib Termscript bindings; links standalone) ─────
add_library(termscript STATIC
  src/dt_termscript.c
)
target_include_directories(termscript PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/termlib>
)
target_include_directories(termscript PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(termscript PUBLIC termlib termscript_standalone)

# ── libtermscript_stdlib (hybrid C + Termscript companion modules) ─
add_subdirectory(termscript/stdlib)
if(TARGET termscript_stdlib)
  # Every DT_TermVM carries the stdlib natively (registered in
  # dt_termscript_create), so the driver and transpiled programs get
  # G:import out of the box.
  target_link_libraries(termscript PUBLIC termscript_stdlib)
endif()

# The generated parser carries benign unused-variable warnings from the
# aurocks runtime scaffolding; keep hand-written sources warning-clean
# without drowning the build log.
set_source_files_properties(${TERMLIB_PARSER_C} PROPERTIES COMPILE_OPTIONS "-w")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(termlib PRIVATE -Wall -Wextra)
  target_compile_options(termscript PRIVATE -Wall -Wextra)
  target_compile_options(termscript_standalone PRIVATE -Wall -Wextra)
endif()

# ── termscript driver ────────────────────────────────────────────────
# NOTE: the binary cannot land next to the target (domlibs/termlib/)
# because OUTPUT_NAME "termscript" would collide with the termscript/
# source directory; it goes to the build-tree bin/ instead.
add_executable(termscript-driver termscript/driver.c)
set_target_properties(termscript-driver PROPERTIES
  OUTPUT_NAME termscript
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
target_link_libraries(termscript-driver PRIVATE termscript)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(termscript-driver PRIVATE -Wall -Wextra)
endif()

# ── install ──────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(TARGETS termlib termscript termscript_standalone
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(TARGETS termscript-driver
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES termlib.h termlib.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/termlib
)
install(FILES termscript/termscript.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/termscript
)
install(FILES termscript/Termscript.g
  DESTINATION ${CMAKE_INSTALL_DATADIR}/termlib
)

# Project documentation and the 22-chapter manual.
install(FILES README.md GUIDE.md INSTALL.md
  DESTINATION ${CMAKE_INSTALL_DOCDIR}
)
install(DIRECTORY manual/
  DESTINATION ${CMAKE_INSTALL_DOCDIR}/manual
  FILES_MATCHING PATTERN "*.md"
)
add_custom_target(termlib-manual
  SOURCES README.md GUIDE.md INSTALL.md
          ${CMAKE_CURRENT_SOURCE_DIR}/manual/01-overview.md
          ${CMAKE_CURRENT_SOURCE_DIR}/manual/22-portability-testing.md
)
