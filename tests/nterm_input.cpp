#include "nterm/NTerm.hpp"
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

struct NTermTestAccess {
  static void writer(NTerm &term, int fd) { term.master_fd_ = fd; term.output_eof_ = false; }
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
  // Read callbacks must yield even after a shell exits with buffered output.
  char path[] = "/tmp/diftray-output-XXXXXX";
  const int fd = mkstemp(path);
  if (fd < 0) return EXIT_FAILURE;
  unlink(path);
  const std::string output(128 * 1024, 'a');
  if (write(fd, output.data(), output.size()) != static_cast<ssize_t>(output.size())) return EXIT_FAILURE;
  lseek(fd, 0, SEEK_SET);
  NTerm reader;
  reader.resize(80, 3);
  NTermTestAccess::writer(reader, fd);
  reader.on_readable();
  if (lseek(fd, 0, SEEK_CUR) != 64 * 1024 || !reader.output_open()) {
    std::cerr << "terminal read monopolised the event loop or dropped buffered output\n";
    return EXIT_FAILURE;
  }
  reader.on_readable();
  reader.on_readable();
  if (reader.output_open()) return EXIT_FAILURE;
  close(fd);
  NTermTestAccess::writer(reader, -1);
  NTermTestAccess::writer(term, -1);
  return EXIT_SUCCESS;
}
