#pragma once
#include <unistd.h>

// The new LibShell POSIX executor already stays in the shell's session
// (no setsid), so no adaptation is needed. Include the POSIX executor
// umbrella for LocalExecutor and the builtin registry.
#include <LibShell-Posix.hpp>
