#include "nterm/NTerm.hpp"

#include <algorithm>
#include <utility>

NTerm::NTerm(std::string shell_override) : shell_path_(std::move(shell_override)) {
  if (shell_path_.empty()) {
    shell_path_ = "/bin/sh";
  }
}

NTerm::~NTerm() = default;

void NTerm::on_readable() {
  if (!input_buffer_.empty()) {
    scrollback_.push_back(input_buffer_);
    input_buffer_.clear();
  }
}

void NTerm::resize(std::size_t columns, std::size_t rows) {
  columns_ = std::max<std::size_t>(1, columns);
  rows_ = std::max<std::size_t>(1, rows);
}

void NTerm::feed_input(std::string_view bytes) { input_buffer_.append(bytes); }

void NTerm::append_output(std::string_view bytes) { scrollback_.emplace_back(bytes); }

void NTerm::set_shell_path(std::string shell_path) {
  if (!shell_path.empty()) {
    shell_path_ = std::move(shell_path);
  }
}
int NTerm::master_fd() const { return master_fd_; }
pid_t NTerm::child_pid() const { return child_pid_; }
const std::string &NTerm::shell_path() const { return shell_path_; }
std::size_t NTerm::columns() const { return columns_; }
std::size_t NTerm::rows() const { return rows_; }
const std::deque<std::string> &NTerm::scrollback() const { return scrollback_; }
const std::string &NTerm::input_buffer() const { return input_buffer_; }
