#pragma once

// LibShell umbrella header.
//
// Layered, dependency-ordered, and platform-free:
//
//   Core.hpp     value types: diagnostics, Result, Environment, streams, status
//   Expansion.hpp  words, tilde/parameter/arithmetic expansion, glob, splitting
//   IR.hpp       typed validated command graph
//   Kernel.hpp   named in-process command extension contracts
//   Exec.hpp     Executor boundary, ExecSpec, ExecutionReport
//   Runtime.hpp  Shell: state ownership and IR dispatch
//   DSL.hpp      pure builders over the IR
//
// Each layer includes only the layers below it, so the dependency graph is a
// chain. No layer reaches upward, and no platform header appears above
// LibShell-Posix.hpp. `lsh` is the namespace; `lakposht` remains a source
// compatible alias.

#include "lsh/Core.hpp"
#include "lsh/Expansion.hpp"
#include "lsh/IR.hpp"
#include "lsh/Kernel.hpp"
#include "lsh/Exec.hpp"
#include "lsh/Runtime.hpp"
#include "lsh/DSL.hpp"

namespace lakposht = lsh;
