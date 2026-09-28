#include "nterm/NTerm.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <poll.h>
#include <unistd.h>

void check(bool ok, const std::string &message) {
  if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
std::string wait_for(NTerm &term, const std::string &needle) {
  std::string output;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    term.flush_input();
    pollfd fd{term.master_fd(), POLLIN, 0};
    if (poll(&fd, 1, 20) > 0) {
      char buffer[4096];
      auto count = read(fd.fd, buffer, sizeof(buffer));
      if (count > 0) output.append(buffer, count);
    }
    if (output.find(needle) != std::string::npos) return output;
  }
  check(false, "shell did not produce " + needle + ": " + output);
  return {};
}
int main() {
  unsetenv("DIFTRAYWM_SHELL");
  NTerm term;
  check(term.shell_path() == "libshell", "LibShell is not the default");
  check(term.start(), term.last_error());
  wait_for(term, "diftray> ");
  term.feed_input("printf '%s%s\\n' embedded -shell\n");
  wait_for(term, "embedded-shell\r\n");
  term.feed_input("export DIFTRAY_TEST=exported\n");
  wait_for(term, "diftray> ");
  term.feed_input("printf '%s%s\\n' $DIFTRAY_TEST -value\n");
  wait_for(term, "exported-value\r\n");
  term.feed_input("printf '%s\\n' lowercase | tr a-z A-Z\n");
  wait_for(term, "LOWERCASE\r\n");
  term.feed_input("cd /tmp\npwd\n");
  wait_for(term, "/tmp\r\n");
  term.feed_input("/bin/sh -c 'printf \"%s%s\\n\" tty -session > /dev/tty'\n");
  wait_for(term, "tty-session\r\n");
  term.feed_input("/bin/sh -c 'printf \"%s%s\\n\" foreground -ready; sleep 5'\n");
  wait_for(term, "foreground-ready\r\n");
  term.feed_input("\003");
  wait_for(term, "diftray> ");
  term.feed_input("printf '%s%s\\n' still -alive\n");
  wait_for(term, "still-alive\r\n");
  term.stop();
  check(!term.running() && term.child_pid() <= 0, "shell did not stop");
  term.set_shell_path("/bin/sh");
  check(term.start(), term.last_error());
  term.feed_input("printf '%s%s\\n' external -override\n");
  wait_for(term, "external-override\r\n");
  term.stop();
}
