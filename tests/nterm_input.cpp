#include "nterm/NTerm.hpp"
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

struct NTermTestAccess {
  static void writer(NTerm &term, int fd) { term.master_fd_ = fd; }
};
int main() {
  int pipes[2];
  if (pipe2(pipes, O_NONBLOCK | O_CLOEXEC)) return EXIT_FAILURE;
  NTerm term;
  NTermTestAccess::writer(term, pipes[1]);
  const std::string data(512 * 1024, 'x');
  term.feed_input(data);
  if (!term.input_pending()) return EXIT_FAILURE;
  std::string received;
  char bytes[8192];
  for (int tries = 0; tries < 10000 && received.size() < data.size(); ++tries) {
    ssize_t count = read(pipes[0], bytes, sizeof(bytes));
    if (count > 0) received.append(bytes, static_cast<size_t>(count));
    term.flush_input();
  }
  close(pipes[0]); close(pipes[1]);
  if (term.input_pending() || received != data) {
    std::cerr << "input lost after PTY backpressure\n"; return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
