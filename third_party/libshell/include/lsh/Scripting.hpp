#pragma once

// Embedded scripting adapters.
//
// The core runtime knows only the ScriptingBackend contract; it has no
// dependency on any embedded language. This header declares the adapters that
// link against one, so the dependency lives in exactly one translation unit.

#include "lsh/Runtime.hpp"

#include <string>
#include <string_view>

namespace lsh::scripting {

// QaMRpp Lua evaluator. Each call builds a fresh context: the host environment
// is reachable through env(NAME), and identifier-safe names are exposed as
// read-only globals. No standard libraries are loaded, so an inline expansion
// acquires no filesystem or process capability by default.
//
// Defined in src/LibShell-Qamrpp.cpp; the header-only core does not link it.
[[nodiscard]] Result<std::string> eval_lua_qamrpp(std::string_view script, const Environment& environment);

} // namespace lsh::scripting
