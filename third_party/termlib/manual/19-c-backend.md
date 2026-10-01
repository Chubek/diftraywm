# Chapter 19: Termscript C Backend

Compilation parses and validates the source, emits ordinary C statements, and stores the generated unit until the next compile or VM destruction.

## Scope

This chapter documents the stable Termlib contract and the decisions applications must make when using it.

## Recommended pattern

Initialize option structures with their `*_options_init` function, check every returned status, and release each owned handle with its matching cleanup function. Keep borrowed pointers and file descriptors valid for the lifetime promised by the API.

## Reference

See `termlib.h` for exact signatures and `termlib.hpp` for the header-only C++ facade.
