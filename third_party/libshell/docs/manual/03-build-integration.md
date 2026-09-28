# 03. Build Integration {#manual_build_integration}

## CMake Embedding

Embed LibShell as a subdirectory and link consumers to the target exported by the local build layout.

```cmake
add_subdirectory(libshell)
target_include_directories(app PRIVATE libshell/include)
```

## Documentation Integration

`docs/CMakeLists.txt` is guarded by the root option `LIBSHELL_GENERATE_DOCS`. When enabled, it:

- configures `docs/Doxyfile.in`;
- adds the `libshell-docs` target;
- generates HTML from Markdown and docstrings when Doxygen is available;
- installs Markdown sources and generated HTML to `share/docs/libshell`.

## Doxygen Input Set

- `include/`
- `stdkern/`
- `docs/frontpage.md`
- `docs/manual/`
- `docs/design/`

## Failure Policy

Doxygen absence is non-fatal. The `libshell-docs` target remains present and emits a diagnostic message. Raw Markdown installation remains available.
