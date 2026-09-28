set(LIBSHELL_MANUAL_CHAPTERS
    manual/01-architecture.md
    manual/02-installation.md
    manual/03-build-integration.md
    manual/04-typed-commands.md
    manual/05-arguments-expansion.md
    manual/06-environment.md
    manual/07-redirection-stdio.md
    manual/08-pipelines.md
    manual/09-sequencing-subshells.md
    manual/10-execution-reports.md
    manual/11-posix-executor.md
    manual/12-dry-run-validation.md
    manual/13-sinklets.md
    manual/14-kernels.md
    manual/15-standard-kernels.md
    manual/16-parser-cli.md
    manual/17-configuration-policy.md
    manual/18-testing-debugging.md
    manual/19-packaging-installation.md
    manual/20-extension-roadmap.md
)

set(LIBSHELL_DOC_SOURCES
    frontpage.md
    design/architecture.md
    design/dsl.md
    design/kernels.md
    design/puppeteer.md
    design/sinklets.md
    design/xi.md
    ${LIBSHELL_MANUAL_CHAPTERS}
)

find_package(Doxygen QUIET)

if(DOXYGEN_FOUND)
    set(LIBSHELL_DOXYGEN_INPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(LIBSHELL_DOXYGEN_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/html")
    set(LIBSHELL_DOXYGEN_PROJECT_NUMBER "${PROJECT_VERSION}")

    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in"
        "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
        @ONLY
    )

    add_custom_target(libshell-docs
        COMMAND "${DOXYGEN_EXECUTABLE}" "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        DEPENDS ${LIBSHELL_DOC_SOURCES}
        SOURCES ${LIBSHELL_DOC_SOURCES} Doxyfile.in
        COMMENT "Generating LibShell API and manual documentation"
        VERBATIM
    )

    install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/html/"
        DESTINATION "share/docs/libshell"
        OPTIONAL
    )
else()
    add_custom_target(libshell-docs
        COMMAND "${CMAKE_COMMAND}" -E echo "Doxygen not found; LibShell HTML documentation not generated"
        SOURCES ${LIBSHELL_DOC_SOURCES} Doxyfile.in
        VERBATIM
    )
endif()

install(FILES ${LIBSHELL_DOC_SOURCES}
    DESTINATION "share/docs/libshell"
)
