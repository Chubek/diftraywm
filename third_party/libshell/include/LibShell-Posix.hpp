#pragma once

// POSIX executor umbrella.
//
// Platform-specific shell execution: process primitives, in-process builtins,
// and the LocalExecutor. The core runtime (LibShell.hpp) stays free of platform
// headers; this is the only header a POSIX build needs to add.

#include "lsh/posix/Process.hpp"
#include "lsh/posix/Builtins.hpp"
#include "lsh/posix/Executor.hpp"
