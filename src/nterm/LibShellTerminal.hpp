#pragma once
#include <unistd.h>

// LibShell's generic local executor creates detached sessions for external
// commands. NTerm already owns a controlling PTY session: detaching here loses
// /dev/tty and prevents terminal-generated SIGINT from reaching applications.
// Adapt only that executor header's session creation; leave its parsing,
// expansion, redirection, environment and execution machinery intact.
inline pid_t diftray_shell_inherit_session() { return ::getsid(0); }
#define setsid diftray_shell_inherit_session
#include <LibShell-Posix.hpp>
#undef setsid
