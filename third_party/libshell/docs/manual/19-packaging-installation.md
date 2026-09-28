# 19. Packaging and Installation {#manual_packaging_installation}

## Installed Files

Install rules place:

- public headers under `include`;
- pkg-config metadata under `${CMAKE_INSTALL_LIBDIR}/pkgconfig`;
- Markdown and generated documentation under `share/docs/libshell`.

## Documentation Target

`libshell-docs` is available when `docs/` is included. With Doxygen present it emits HTML; without Doxygen it remains a non-fatal placeholder target.

## Packager Options

Recommended packager configuration:

```sh
cmake -S . -B build \
  -DLIBSH_BUILD_TESTS=OFF \
  -DLIBSH_BUILD_EXAMPLES=OFF \
  -DLIBSHELL_GENERATE_DOCS=ON
```

## Prefix Contract

Documentation installs to:

```text
${CMAKE_INSTALL_PREFIX}/share/docs/libshell
```

This path is intentionally independent of platform-specific `doc` directory conventions.
