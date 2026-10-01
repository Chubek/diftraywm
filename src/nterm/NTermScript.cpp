#include "nterm/NTerm.hpp"

#include <libtsm.h>
#include <termlib.h>
#include <termscript/termscript.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace {
constexpr std::size_t kMaxScreenBytes = 256 * 1024;
constexpr std::size_t kMaxScriptBytes = 1024 * 1024;
constexpr std::size_t kMaxOutputBytes = 64 * 1024;

void append_utf8(std::string &out, uint32_t ch) {
  if (ch == 0 || ch > 0x10ffff || (ch >= 0xd800 && ch <= 0xdfff)) return;
  if (ch < 0x80) out += static_cast<char>(ch);
  else if (ch < 0x800) {
    out += static_cast<char>(0xc0 | (ch >> 6));
    out += static_cast<char>(0x80 | (ch & 0x3f));
  } else if (ch < 0x10000) {
    out += static_cast<char>(0xe0 | (ch >> 12));
    out += static_cast<char>(0x80 | ((ch >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (ch & 0x3f));
  } else {
    out += static_cast<char>(0xf0 | (ch >> 18));
    out += static_cast<char>(0x80 | ((ch >> 12) & 0x3f));
    out += static_cast<char>(0x80 | ((ch >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (ch & 0x3f));
  }
}

struct Capture {
  std::string text;
  unsigned row = 0;
  bool first = true;
};

int capture_cell(tsm_screen *, uint64_t, const uint32_t *ch, size_t len,
                 unsigned width, unsigned x, unsigned y,
                 const tsm_screen_attr *, tsm_age_t, void *data) {
  auto &capture = *static_cast<Capture *>(data);
  if (capture.text.size() >= kMaxScreenBytes) return 0;
  if (capture.first) { capture.row = y; capture.first = false; }
  while (capture.row < y && capture.text.size() < kMaxScreenBytes) {
    capture.text += '\n';
    ++capture.row;
  }
  // libtsm calls this in grid order, including blank cells. A continuation
  // cell of a wide character has width zero, so it must not add a space.
  if (!width) return 0;
  if (ch && len) {
    for (size_t i = 0; i < len && capture.text.size() < kMaxScreenBytes; ++i)
      append_utf8(capture.text, ch[i] ? ch[i] : ' ');
  } else if (capture.text.size() < kMaxScreenBytes) {
    capture.text += ' ';
  }
  (void)x;
  return 0;
}

TS_Status terminal_call(TS_VM *, void *userdata, const TS_Value *argv,
                        size_t argc, TS_Value *ret, TS_Error *error,
                        const char *operation) {
  auto &term = *static_cast<NTerm *>(userdata);
  const bool needs_text = std::string_view(operation) == "display" ||
                          std::string_view(operation) == "send";
  if (argc != (needs_text ? 1U : 0U) ||
      (needs_text && (argv[0].type != TS_STRING || !argv[0].as.string))) {
    ts_error_set(error, TS_ERR_INVAL, 0, 0, "%s: expected %s", operation,
                 needs_text ? "one string" : "no arguments");
    return TS_ERR_INVAL;
  }
  if (needs_text) {
    const std::string_view bytes(argv[0].as.string);
    if (bytes.size() > kMaxScriptBytes) {
      ts_error_set(error, TS_ERR_LIMIT, 0, 0, "%s: text exceeds 1 MiB", operation);
      return TS_ERR_LIMIT;
    }
    const bool accepted = std::string_view(operation) == "display" || term.master_fd() >= 0;
    if (std::string_view(operation) == "display") term.display(bytes);
    else if (accepted) term.feed_input(bytes);
    ts_value_make_bool(ret, accepted);
    return TS_OK;
  }
  if (std::string_view(operation) == "running") {
    ts_value_make_bool(ret, term.running());
    return TS_OK;
  }
  std::string value;
  if (std::string_view(operation) == "screen") value = term.screen_text();
  else if (std::string_view(operation) == "cursor") {
    value = term.screen() ? std::to_string(tsm_screen_get_cursor_y(term.screen())) +
          "," + std::to_string(tsm_screen_get_cursor_x(term.screen())) : "0,0";
  } else value = std::to_string(term.rows()) + "," + std::to_string(term.columns());
  const TS_Status status = ts_value_make_string(ret, value.c_str());
  if (status != TS_OK) ts_error_set(error, status, 0, 0, "terminal: out of memory");
  return status;
}

#define TERMINAL_FUNCTION(name) \
  TS_Status terminal_##name(TS_VM *vm, void *ud, const TS_Value *args, \
                            size_t count, TS_Value *ret, TS_Error *error) { \
    return terminal_call(vm, ud, args, count, ret, error, #name); \
  }
TERMINAL_FUNCTION(screen)
TERMINAL_FUNCTION(cursor)
TERMINAL_FUNCTION(size)
TERMINAL_FUNCTION(display)
TERMINAL_FUNCTION(send)
TERMINAL_FUNCTION(running)
#undef TERMINAL_FUNCTION
} // namespace

std::string NTerm::screen_text() const {
  if (!screen_) return {};
  Capture capture;
  tsm_screen_draw(screen_, capture_cell, &capture);
  return capture.text;
}

std::string NTerm::run_script(const std::string &path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec))
    return ec ? "cannot read terminal script: " + ec.message() :
                "terminal script requires a regular file";
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size > kMaxScriptBytes)
    return ec ? "cannot read terminal script: " + ec.message() :
                "terminal script exceeds 1 MiB";

  DT_Error dt_error{};
  DT_TermVM *vm = dt_termscript_create(&dt_error);
  if (!vm) return "cannot create terminal script VM: " + std::string(dt_error.message);
  TS_FuncDef funcs[] = {
    {"screen", terminal_screen, this}, {"cursor", terminal_cursor, this},
    {"size", terminal_size, this}, {"display", terminal_display, this},
    {"send", terminal_send, this}, {"running", terminal_running, this},
    {nullptr, nullptr, nullptr}
  };
  const TS_Module module{"diftray.terminal", funcs};
  TS_Error ts_error{};
  if (ts_vm_register_module(dt_termscript_inner_vm(vm), &module, &ts_error) != TS_OK) {
    const std::string message = "terminal script failed: " + std::string(ts_error.message);
    dt_termscript_free(vm);
    return message;
  }
  char *output = nullptr;
  const DT_Status status = dt_termscript_run_file(vm, path.c_str(), &output, &dt_error);
  std::string result = status != DT_OK ?
      "terminal script failed: " + std::string(dt_error.message) :
      output && std::char_traits<char>::length(output) <= kMaxOutputBytes ?
      std::string(output) : output ? "terminal script output exceeds 64 KiB" :
      "sourced terminal script " + path;
  std::free(output);
  dt_termscript_free(vm);
  return result;
}
