#include "nterm/LibShellTerminal.hpp"
#include <iostream>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <termios.h>
#include <vector>

namespace lsh::cli {
Result<ir::Program> parse_line(std::string_view line);
}

bool diftraywm_shell_available(const char *path) {
  return path && *path && (std::string_view(path) == "libshell" || ::access(path, X_OK) == 0);
}

namespace {

// --- UTF-8 helpers: the line buffer stores bytes, the cursor is a byte index
// that always lands on a character boundary. Display width matters for cursor
// placement: most code points are 1 cell, combining marks are 0, East Asian
// wide code points are 2.

std::size_t utf8_sequence_length(unsigned char lead) {
  if (lead < 0x80) return 1;
  if ((lead & 0xe0) == 0xc0) return 2;
  if ((lead & 0xf0) == 0xe0) return 3;
  if ((lead & 0xf8) == 0xf0) return 4;
  return 1;
}

bool is_continuation(unsigned char byte) { return (byte & 0xc0) == 0x80; }

// Decode one code point at byte offset pos; on invalid input yields the raw
// byte with length 1 so editing never gets stuck.
void decode_one(const std::string &text, std::size_t pos, uint32_t &code,
                std::size_t &length) {
  const std::size_t size = text.size();
  if (pos >= size) {
    code = 0;
    length = 0;
    return;
  }
  const auto lead = static_cast<unsigned char>(text[pos]);
  if (lead < 0x80) {
    code = lead;
    length = 1;
    return;
  }
  const std::size_t want = utf8_sequence_length(lead);
  if (pos + want > size) {
    code = lead;
    length = 1;
    return;
  }
  for (std::size_t i = 1; i < want; ++i) {
    if (!is_continuation(static_cast<unsigned char>(text[pos + i]))) {
      code = lead;
      length = 1;
      return;
    }
  }
  if (want == 2) {
    code = (static_cast<uint32_t>(lead & 0x1f) << 6) |
           static_cast<uint32_t>(static_cast<unsigned char>(text[pos + 1]) & 0x3f);
    if (code < 0x80) {  // overlong
      code = lead;
      length = 1;
      return;
    }
    length = 2;
    return;
  }
  if (want == 3) {
    code = (static_cast<uint32_t>(lead & 0x0f) << 12) |
           (static_cast<uint32_t>(static_cast<unsigned char>(text[pos + 1]) & 0x3f) << 6) |
           static_cast<uint32_t>(static_cast<unsigned char>(text[pos + 2]) & 0x3f);
    if (code < 0x800 || (code >= 0xd800 && code <= 0xdfff)) {
      code = lead;
      length = 1;
      return;
    }
    length = 3;
    return;
  }
  code = (static_cast<uint32_t>(lead & 0x07) << 18) |
         (static_cast<uint32_t>(static_cast<unsigned char>(text[pos + 1]) & 0x3f) << 12) |
         (static_cast<uint32_t>(static_cast<unsigned char>(text[pos + 2]) & 0x3f) << 6) |
         static_cast<uint32_t>(static_cast<unsigned char>(text[pos + 3]) & 0x3f);
  if (code < 0x10000 || code > 0x10ffff) {
    code = lead;
    length = 1;
    return;
  }
  length = 4;
}

int code_width(uint32_t code) {
  if (code < 0x20 || (code >= 0x7f && code < 0xa0)) return 1;  // controls (not inserted)
  if ((code >= 0x0300 && code <= 0x036f) || (code >= 0x1ab0 && code <= 0x1aff) ||
      (code >= 0x1dc0 && code <= 0x1dff) || (code >= 0x20d0 && code <= 0x20ff) ||
      (code >= 0xfe20 && code <= 0xfe2f)) {
    return 0;  // combining marks
  }
  if ((code >= 0x1100 && code <= 0x115f) || (code >= 0x2e80 && code <= 0xa4cf) ||
      (code >= 0xac00 && code <= 0xd7a3) || (code >= 0xf900 && code <= 0xfaff) ||
      (code >= 0xfe30 && code <= 0xfe4f) || (code >= 0xff00 && code <= 0xff60) ||
      (code >= 0xffe0 && code <= 0xffe6) || (code >= 0x20000 && code <= 0x3fffd)) {
    return 2;
  }
  return 1;
}

std::size_t prev_char_start(const std::string &text, std::size_t pos) {
  if (pos == 0 || pos > text.size()) return 0;
  std::size_t start = pos - 1;
  while (start > 0 && is_continuation(static_cast<unsigned char>(text[start]))) --start;
  // Validate that start really begins the char ending at pos; otherwise step
  // back a single byte so invalid sequences still make progress.
  uint32_t code = 0;
  std::size_t length = 0;
  decode_one(text, start, code, length);
  if (start + length != pos) return pos - 1;
  return start;
}

std::size_t next_char_end(const std::string &text, std::size_t pos) {
  if (pos >= text.size()) return text.size();
  uint32_t code = 0;
  std::size_t length = 0;
  decode_one(text, pos, code, length);
  return pos + length;
}

int suffix_width(const std::string &text, std::size_t cursor) {
  int width = 0;
  std::size_t pos = cursor;
  while (pos < text.size()) {
    uint32_t code = 0;
    std::size_t length = 0;
    decode_one(text, pos, code, length);
    width += code_width(code);
    pos += length;
  }
  return width;
}

void write_all(int fd, const char *data, std::size_t length) {
  std::size_t done = 0;
  while (done < length) {
    const ssize_t count = ::write(fd, data + done, length - done);
    if (count > 0) {
      done += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    break;
  }
}

void write_all(int fd, const std::string &text) {
  if (!text.empty()) write_all(fd, text.data(), text.size());
}

// Blocking read of one byte; returns false on EOF/error.
bool read_byte(int fd, unsigned char &out) {
  for (;;) {
    unsigned char byte = 0;
    const ssize_t count = ::read(fd, &byte, 1);
    if (count == 1) {
      out = byte;
      return true;
    }
    if (count < 0 && errno == EINTR) continue;
    return false;
  }
}

// After ESC, wait briefly for the rest of a sequence so a lone Escape key
// does not hang the editor waiting for bytes that never arrive.
bool read_byte_timeout(int fd, unsigned char &out, int timeout_ms) {
  pollfd pfd{fd, POLLIN, 0};
  const int ready = ::poll(&pfd, 1, timeout_ms);
  if (ready <= 0) return false;
  return read_byte(fd, out);
}

bool is_word_char(unsigned char byte) {
  return std::isalnum(byte) || byte == '_';
}

std::size_t word_left(const std::string &text, std::size_t cursor) {
  std::size_t pos = cursor;
  while (pos > 0 && !is_word_char(static_cast<unsigned char>(text[pos - 1]))) {
    pos = prev_char_start(text, pos);
  }
  while (pos > 0 && is_word_char(static_cast<unsigned char>(text[pos - 1]))) {
    pos = prev_char_start(text, pos);
  }
  // For non-word (e.g. punctuation) runs, move over at least one char so
  // Ctrl+Left always makes progress even on symbols.
  if (pos == cursor && pos > 0) pos = prev_char_start(text, pos);
  return pos;
}

std::size_t word_right(const std::string &text, std::size_t cursor) {
  std::size_t pos = cursor;
  const std::size_t size = text.size();
  // If on a word, skip to its end first, then skip separators.
  bool moved = false;
  while (pos < size && is_word_char(static_cast<unsigned char>(text[pos]))) {
    pos = next_char_end(text, pos);
    moved = true;
  }
  while (pos < size && !is_word_char(static_cast<unsigned char>(text[pos]))) {
    pos = next_char_end(text, pos);
    moved = true;
  }
  if (!moved && pos < size) pos = next_char_end(text, pos);
  return pos;
}

struct LineEditor {
  std::string prompt;
  std::string line;
  std::size_t cursor = 0;
  std::vector<std::string> *history = nullptr;
  std::size_t history_index = 0;
  std::string stashed;

  void redraw() {
    std::string out;
    out.push_back('\r');
    out += prompt;
    out += line;
    out += "\x1b[K";
    const int back = suffix_width(line, cursor);
    for (int i = 0; i < back; ++i) out += "\x1b[D";
    write_all(STDOUT_FILENO, out);
  }

  void set_line(std::string next) {
    line = std::move(next);
    cursor = line.size();
    redraw();
  }

  void history_up() {
    if (!history || history->empty() || history_index == 0) return;
    if (history_index == history->size()) stashed = line;
    --history_index;
    set_line((*history)[history_index]);
  }

  void history_down() {
    if (!history || history->empty() || history_index >= history->size()) return;
    ++history_index;
    if (history_index == history->size()) set_line(stashed);
    else set_line((*history)[history_index]);
  }

  void move_left() {
    if (cursor > 0) {
      cursor = prev_char_start(line, cursor);
      redraw();
    }
  }

  void move_right() {
    if (cursor < line.size()) {
      cursor = next_char_end(line, cursor);
      redraw();
    }
  }

  void insert_bytes(const char *data, std::size_t length) {
    line.insert(cursor, data, length);
    cursor += length;
    redraw();
  }

  void backspace() {
    if (cursor == 0) return;
    const std::size_t start = prev_char_start(line, cursor);
    line.erase(start, cursor - start);
    cursor = start;
    redraw();
  }

  void delete_under() {
    if (cursor >= line.size()) return;
    line.erase(cursor, next_char_end(line, cursor) - cursor);
    redraw();
  }

  void kill_to_end() {
    if (cursor < line.size()) {
      line.erase(cursor);
      redraw();
    }
  }

  void kill_to_start() {
    if (cursor > 0) {
      line.erase(0, cursor);
      cursor = 0;
      redraw();
    }
  }

  void delete_word_left() {
    if (cursor == 0) return;
    const std::size_t start = word_left(line, cursor);
    line.erase(start, cursor - start);
    cursor = start;
    redraw();
  }
};

// Reads one logical line with local echo. Returns nullopt on EOF (Ctrl+D on
// an empty line or read error); otherwise the submitted line, which may be
// empty for a bare Enter or an aborted (Ctrl+C) line.
std::optional<std::string> read_line_edited(const std::string &prompt,
                                            std::vector<std::string> &history) {
  LineEditor editor;
  editor.prompt = prompt;
  editor.history = &history;
  editor.history_index = history.size();
  write_all(STDOUT_FILENO, prompt);

  for (;;) {
    unsigned char byte = 0;
    if (!read_byte(STDIN_FILENO, byte)) {
      // EOF: mimic canonical semantics -- non-empty line is submitted with a
      // newline, empty line ends the shell.
      if (!editor.line.empty()) {
        write_all(STDOUT_FILENO, "\n");
        return editor.line;
      }
      return std::nullopt;
    }

    if (byte == '\r' || byte == '\n') {
      write_all(STDOUT_FILENO, "\n");
      return editor.line;
    }
    if (byte == 0x03) {  // Ctrl+C: abort line, print ^C like canonical ECHOCTL.
      editor.line.clear();
      editor.cursor = 0;
      write_all(STDOUT_FILENO, "^C\n");
      return std::string{};
    }
    if (byte == 0x04) {  // Ctrl+D
      if (editor.line.empty()) return std::nullopt;
      editor.delete_under();
      continue;
    }
    if (byte == 0x01) {  // Ctrl+A
      editor.cursor = 0;
      editor.redraw();
      continue;
    }
    if (byte == 0x05) {  // Ctrl+E
      editor.cursor = editor.line.size();
      editor.redraw();
      continue;
    }
    if (byte == 0x02) {  // Ctrl+B
      editor.move_left();
      continue;
    }
    if (byte == 0x06) {  // Ctrl+F
      editor.move_right();
      continue;
    }
    if (byte == 0x0b) {  // Ctrl+K
      editor.kill_to_end();
      continue;
    }
    if (byte == 0x15) {  // Ctrl+U
      editor.line.clear();
      editor.cursor = 0;
      editor.redraw();
      continue;
    }
    if (byte == 0x17) {  // Ctrl+W
      editor.delete_word_left();
      continue;
    }
    if (byte == 0x08 || byte == 0x7f) {  // BackSpace (either code)
      editor.backspace();
      continue;
    }
    if (byte == 0x09) continue;  // Tab: no completion; swallow to avoid artifacts.
    if (byte == 0x1b) {          // Escape sequences (arrows, Home/End, Delete...)
      unsigned char next = 0;
      if (!read_byte_timeout(STDIN_FILENO, next, 100)) continue;  // lone ESC
      if (next == '[') {
        std::string params;
        unsigned char final = 0;
        for (int i = 0; i < 16; ++i) {
          if (!read_byte(STDIN_FILENO, final)) break;
          if (final >= 0x40 && final <= 0x7e) break;
          if (params.size() < 15) params.push_back(static_cast<char>(final));
          final = 0;
        }
        if (final == 0) continue;
        const bool ctrl = params == "1;5" || params == "5";
        // const bool shift = params == "1;2";
        switch (final) {
          case 'A': editor.history_up(); break;
          case 'B': editor.history_down(); break;
          case 'C':
            if (ctrl) editor.cursor = word_right(editor.line, editor.cursor), editor.redraw();
            else editor.move_right();
            break;
          case 'D':
            if (ctrl) editor.cursor = word_left(editor.line, editor.cursor), editor.redraw();
            else editor.move_left();
            break;
          case 'H': editor.cursor = 0; editor.redraw(); break;
          case 'F': editor.cursor = editor.line.size(); editor.redraw(); break;
          case 'Z': break;  // Shift+Tab
          case '~':
            if (params == "3") editor.delete_under();
            else if (params == "1" || params == "7") { editor.cursor = 0; editor.redraw(); }
            else if (params == "4" || params == "8") { editor.cursor = editor.line.size(); editor.redraw(); }
            break;
          default: break;  // function keys, page keys: swallow
        }
        continue;
      }
      if (next == 'O') {  // application-cursor SS3 sequences
        unsigned char final = 0;
        if (!read_byte(STDIN_FILENO, final)) continue;
        switch (final) {
          case 'A': editor.history_up(); break;
          case 'B': editor.history_down(); break;
          case 'C': editor.move_right(); break;
          case 'D': editor.move_left(); break;
          case 'H': editor.cursor = 0; editor.redraw(); break;
          case 'F': editor.cursor = editor.line.size(); editor.redraw(); break;
          default: break;
        }
        continue;
      }
      // Alt+letter (libtsm prepends ESC for Alt): support word motion and
      // word kill, swallow anything else so Meta combos never echo garbage.
      if (next == 'b' || next == 'B') {
        editor.cursor = word_left(editor.line, editor.cursor);
        editor.redraw();
      } else if (next == 'f' || next == 'F') {
        editor.cursor = word_right(editor.line, editor.cursor);
        editor.redraw();
      } else if (next == 'd' || next == 'D') {
        const std::size_t end = word_right(editor.line, editor.cursor);
        editor.line.erase(editor.cursor, end - editor.cursor);
        editor.redraw();
      }
      continue;
    }
    if (byte < 0x20) continue;  // other controls: swallow (no artifacts)

    if (byte < 0x80) {
      const char ch = static_cast<char>(byte);
      editor.insert_bytes(&ch, 1);
      continue;
    }
    // UTF-8 multi-byte character: collect continuation bytes first so a
    // partial sequence is never echoed (which would flicker or artifact).
    const std::size_t want = utf8_sequence_length(byte);
    char sequence[4] = {static_cast<char>(byte), 0, 0, 0};
    std::size_t have = 1;
    bool ok = true;
    while (have < want) {
      unsigned char cont = 0;
      if (!read_byte(STDIN_FILENO, cont)) {
        ok = false;
        break;
      }
      if (!is_continuation(cont)) {
        // Unexpected byte: insert what we have and reprocess this byte next
        // iteration is complex; simplest is to insert the valid prefix and
        // treat the stray byte as a fresh keypress by handling it now.
        // To keep the loop simple, insert prefix and fall through to handle
        // cont via a small recursive step: insert prefix, then process cont
        // as if just read. Implement inline by inserting prefix and then
        // treating cont with the same logic (only ASCII/UTF-8 lead matter).
        editor.insert_bytes(sequence, have);
        // Reprocess cont: emulate one loop iteration for the common cases.
        if (cont == 0x08 || cont == 0x7f) editor.backspace();
        else if (cont >= 0x20 && cont < 0x7f) {
          const char ch = static_cast<char>(cont);
          editor.insert_bytes(&ch, 1);
        } else if (cont >= 0x80) {
          // Start of another sequence; push back is unavailable, so insert
          // the single byte and let subsequent reads complete it. This keeps
          // bytes in order without loss.
          const char ch = static_cast<char>(cont);
          editor.insert_bytes(&ch, 1);
        }
        ok = false;  // prefix already inserted
        break;
      }
      sequence[have++] = static_cast<char>(cont);
    }
    if (ok) editor.insert_bytes(sequence, have);
  }
}

}  // namespace

// Runs only in NTerm's PTY child; command execution never blocks the compositor.
int diftraywm_run_embedded_shell() {
  try {
    // Keep the interactive shell alive on Ctrl+C. Caught handlers reset to
    // default at exec, so foreground external commands still receive SIGINT.
    struct sigaction action {};
    action.sa_handler = [](int) {};
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGQUIT, &action, nullptr);
    lsh::Shell shell{std::make_shared<lsh::posix::LocalExecutor>()};
    shell.set_command_substitution_parser(lsh::cli::parse_line);
    int status = 0;

    const bool interactive =
        ::isatty(STDIN_FILENO) && ::isatty(STDOUT_FILENO);
    struct termios original {};
    const bool have_termios =
        interactive && ::tcgetattr(STDIN_FILENO, &original) == 0;
    if (!have_termios) {
      std::string line;
      while (std::cout << "diftray> " << std::flush, std::getline(std::cin, line)) {
        if (line == "exit" || line == "quit") return status;
        if (line.empty()) continue;
        auto program = lsh::cli::parse_line(line);
        if (!program) {
          std::cerr << "libshell: " << program.error().message << '\n';
          status = 2;
          continue;
        }
        auto result = shell.run(program.value());
        if (!result) {
          std::cerr << "libshell: " << result.error().message << '\n';
          status = 1;
          continue;
        }
        status = result.value().status.code;
        while (::waitpid(-1, nullptr, WNOHANG) > 0) {}
        for (const auto &diagnostic : result.value().diagnostics)
          std::cerr << "libshell: " << diagnostic.message << '\n';
      }
      return status;
    }

    // Raw-ish editing mode: the kernel must not canonicalise or echo -- the
    // editor below owns erase, arrows, and redraw, so BS/DEL and ESC [ D no
    // longer echo as ^H / ^[[D artifacts. ISIG stays off while editing (Ctrl+C
    // aborts the line); it is re-enabled around command execution so Ctrl+C
    // still interrupts foreground children like `sleep`.
    struct termios editing = original;
    editing.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    editing.c_oflag |= (OPOST | ONLCR);
    editing.c_cflag |= CS8;
    editing.c_lflag &= ~(ECHO | ECHONL | ICANON | IEXTEN | ISIG);
    editing.c_cc[VMIN] = 1;
    editing.c_cc[VTIME] = 0;

    std::vector<std::string> history;
    ::tcsetattr(STDIN_FILENO, TCSANOW, &editing);
    while (true) {
      std::cout.flush();
      std::cerr.flush();
      auto maybe_line = read_line_edited("diftray> ", history);
      if (!maybe_line) break;
      std::string line = std::move(*maybe_line);
      if (line == "exit" || line == "quit") break;
      if (line.empty()) continue;
      history.push_back(line);
      // Let the child see a normal terminal while it runs.
      ::tcsetattr(STDIN_FILENO, TCSANOW, &original);
      std::cout.flush();
      std::cerr.flush();
      auto program = lsh::cli::parse_line(line);
      if (!program) {
        std::cerr << "libshell: " << program.error().message << '\n';
        status = 2;
      } else {
        auto result = shell.run(program.value());
        if (!result) {
          std::cerr << "libshell: " << result.error().message << '\n';
          status = 1;
        } else {
          status = result.value().status.code;
          for (const auto &diagnostic : result.value().diagnostics)
            std::cerr << "libshell: " << diagnostic.message << '\n';
        }
      }
      std::cout.flush();
      std::cerr.flush();
      while (::waitpid(-1, nullptr, WNOHANG) > 0) {}
      ::tcsetattr(STDIN_FILENO, TCSANOW, &editing);
    }
    ::tcsetattr(STDIN_FILENO, TCSANOW, &original);
    return status;
  } catch (const std::exception &error) {
    std::cerr << "libshell: " << error.what() << '\n';
    return 1;
  }
}
