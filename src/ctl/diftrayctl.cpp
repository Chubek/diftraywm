// diftrayctl -- command line control surface for a running DiftrayWM session.
//
// The compositor hosts a Unix control socket (see src/compositor/ControlServer).
// diftrayctl maps its own subcommand vocabulary onto Command Bar commands and
// sends one newline-terminated request; the compositor replies with the status
// line. Because every remote verb is a Command Bar command, the same
// operations are available interactively through `:` and diftrayctl.
//
// Exit status: 0 when the command ran, 1 when the compositor did not recognise
// it, 2 for a usage error, and 3 when no session is reachable. Handlers report
// their own failures ("plugin load failed: ...") in the printed status line, so
// scripts should read the text as well as the exit code.
#include "ctl/ControlSocketPath.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

struct Command {
  const char *name;         // diftrayctl subcommand
  const char *command;      // Command Bar prefix
  const char *arguments;    // usage suffix, or nullptr when none are taken
};

// diftrayctl subcommands mapped onto Command Bar commands. Anything not listed
// here needs no compositor and is handled locally.
constexpr Command kCommands[] = {
    // Session lifecycle.
    {"exit-session", "session exit", nullptr},
    {"session-exit", "session exit", nullptr},
    {"quit", "session exit", nullptr},
    {"restart-session", "session restart", nullptr},
    {"session-restart", "session restart", nullptr},
    {"restart", "session restart", nullptr},
    {"status", "session status", nullptr},
    {"session-status", "session status", nullptr},
    // Launcher taskbar.
    {"lock-launchbar", "launcher lock", nullptr},
    {"unlock-launchbar", "launcher unlock", nullptr},
    {"toggle-launchbar", "launcher toggle", nullptr},
    {"launchbar-status", "launcher status", nullptr},
    // Keymap INI files.
    {"keymap", "keymap", nullptr},
    {"keymap-show", "keymap show", nullptr},
    {"keymap-reload", "keymap reload", nullptr},
    {"keymap-path", "keymap path", nullptr},
    {"keymap-reset", "keymap reset", nullptr},
    {"keymap-profile", "keymap profile", "<name>"},
    {"keymap-check", "keymap check", "<path>"},
    {"keymap-chord", "keymap chord", "<chord>"},
    {"keys", "keymap show", nullptr},
    {"reload-keymap", "keymap reload", nullptr},
    // Native plugins (dynalo).
    {"plugin-load", "plugin load", "<path>"},
    {"plugin-unload", "plugin unload", "<path>"},
    {"plugin-list", "plugin list", nullptr},
    {"plugins", "plugin list", nullptr},
    // Lua extensions.
    {"extension-exec", "extension exec", "<path>"},
    {"extension-load", "extension exec", "<path>"},
    {"extension-list", "extension list", nullptr},
    {"extensions", "extension list", nullptr},
    // Configuration.
    {"config-open", "config open", nullptr},
    {"config-path", "config path", nullptr},
    {"config-reload", "config reload", nullptr},
    {"config-vars", "config vars", nullptr},
    {"config-eval", "config eval", "<expression>"},
    {"config-run", "config run", "<expression>"},
    // Termscript.
    {"source-script", "script source", "<path.tsc>"},
    {"script-source", "script source", "<path.tsc>"},
    // Cells and the multiplexer.
    {"spawn-cell", "spawn", "<above|below>"},
    {"spawn-above", "spawn above", nullptr},
    {"spawn-below", "spawn below", nullptr},
    {"kill-cell", "kill", nullptr},
    {"mux-split", "mux split", "<horizontal|vertical>"},
    {"mux-focus", "mux focus", "<next|prev|up|down|left|right>"},
    {"mux-kill", "mux kill", nullptr},
    {"mux-zoom", "mux zoom", nullptr},
    {"mux-list", "mux list", nullptr},
    // NCursors, tabs and workspaces.
    {"ncursor-list", "ncursor list", nullptr},
    {"ncursor-new", "ncursor new", nullptr},
    {"tab-next", "tab next", nullptr},
    {"tab-prev", "tab prev", nullptr},
    {"workspace", "workspace", "<1-10>"},
    {"workspace-switch", "workspace", "<1-10>"},
    // Monitors.
    {"output-list", "output list", nullptr},
    {"output-focus", "output focus", "<name|next|prev>"},
    {"output-move", "output move", "<name>"},
    {"output-rotate", "output rotate", "<name> <0|90|180|270>"},
    {"output-scale", "output scale", "<name> <0.5-4>"},
    {"output-position", "output position", "<name> <x> <y>|auto"},
    // Graphical cursors.
    {"cursor-list", "cursor list ids", nullptr},
    {"cursor-dock", "cursor dock", "<id>"},
    {"cursor-restore", "cursor restore", "<id>"},
    {"cursor-assign", "cursor assign", "<id> <F1-F4>"},
    // Notelets.
    {"notelet-list", "notelet list", nullptr},
    {"notelet-open", "notelet open", "<name>"},
    {"notelet-close", "notelet close", nullptr},
    {"notelet-refresh", "notelet refresh", nullptr},
    // Theming.
    {"theme-load", "theme load", "<file.css>"},
    {"theme-show", "theme show", nullptr},
    {"set-theme", "set theme", "<css>"},
    // Help.
    {"help", "help", "[page]"},
    {"page", "help", "[page]"},
};

void print_usage() {
  std::cout <<
      "Usage: diftrayctl <command> [arguments]\n"
      "\n"
      "Drives a running DiftrayWM session over its control socket. Every\n"
      "command is also a Command Bar command, so `diftrayctl <command>` and\n"
      "typing the same text after `:` do the same thing.\n"
      "\n"
      "Session:\n"
      "  exit-session                 leave the session for the Linux console\n"
      "  restart-session              re-exec the compositor in place\n"
      "  status                       show session, cell, plugin and config state\n"
      "\n"
      "Launcher taskbar:\n"
      "  lock-launchbar               pin the launch bar to the top of the screen\n"
      "  unlock-launchbar             let the launch bar hide when idle\n"
      "  toggle-launchbar             flip the lock\n"
      "  launchbar-status             show the lock state and bar geometry\n"
      "\n"
      "Key remapping (keymap.ini):\n"
      "  keymap-show                  print the keymap, its profiles and the active one\n"
      "  keymap-reload                re-read the keymap INI file\n"
      "  keymap-path                  print the keymap INI path\n"
      "  keymap-profile <name>        switch to another profile\n"
      "  keymap-reset                 return to the default profile\n"
      "  keymap-check <path>          validate a keymap without adopting it\n"
      "  keymap-chord <chord>         validate a chord such as <C-q>\n"
      "\n"
      "System-wide remapping is a separate program, diftrayremap, because it\n"
      "needs privileges the compositor must not have. Run `diftrayremap check`\n"
      "to see whether this machine can do it.\n"
      "\n"
      "Extensions and plugins:\n"
      "  plugin-load <path>           load a native plugin (dynalo)\n"
      "  plugin-unload <path>         unload a native plugin\n"
      "  plugin-list                  list loaded plugins\n"
      "  extension-exec <path>        load and run a Lua extension\n"
      "  extension-list               list loaded extensions\n"
      "  source-script <path.tsc>     run a Termscript file and print its output\n"
      "\n"
      "Configuration and theming:\n"
      "  config-open                  open the active config in $EDITOR\n"
      "  config-path                  print the active config path\n"
      "  config-reload                re-read the config file\n"
      "  config-vars                  list configuration variables\n"
      "  config-eval <expression>     evaluate a configuration expression\n"
      "  config-run <expression>      run a configuration expression's commands\n"
      "  theme-load <file.css>        apply a CSS theme\n"
      "  theme-show                   list active theme properties\n"
      "  set-theme <css>              apply inline CSS theme properties\n"
      "\n"
      "Cells, multiplexer, views and monitors:\n"
      "  spawn-cell <above|below>     create a terminal cell\n"
      "  spawn-above                  create a cell above the current one\n"
      "  spawn-below                  create a cell below the current one\n"
      "  kill-cell                    remove the current cell\n"
      "  mux-split <horizontal|vertical>  split the focused pane\n"
      "  mux-focus <direction>        move multiplexer focus\n"
      "  mux-kill                     close the focused pane\n"
      "  mux-zoom                     expand the pane to a TCursor and back\n"
      "  mux-list                     list panes\n"
      "  ncursor-list | ncursor-new   list or create NCursors\n"
      "  tab-next | tab-prev          cycle NCursor or GCursor tabs\n"
      "  workspace <1-10>             switch workspace\n"
      "  output-list                  list monitors\n"
      "  output-focus <name|next|prev>  focus a monitor\n"
      "  output-move <name>           move the active NCursor to a monitor\n"
      "  output-rotate <name> <deg>   rotate a monitor\n"
      "  output-scale <name> <scale>  scale a monitor\n"
      "  output-position <name> <x> <y> | auto  place a monitor\n"
      "  cursor-list                  list graphical cursor ids\n"
      "  cursor-dock <id>             hide a graphical cursor\n"
      "  cursor-restore <id>          restore a graphical cursor\n"
      "  cursor-assign <id> <F1-F4>   bind a quick-restore key\n"
      "  notelet-list                 list notelets\n"
      "  notelet-open <name>          open a notelet in a new cell\n"
      "  notelet-close                close the active notelet\n"
      "  notelet-refresh              re-render the active notelet\n"
      "  help [page]                  open the manual in a cell\n"
      "\n"
      "Local:\n"
      "  bar <command>                send a raw Command Bar command\n"
      "  version                      print the client version\n"
      "  --help                       print this text\n"
      "\n"
      "Exit status: 0 the command ran, 1 the compositor rejected it, 2 usage\n"
      "error, 3 no session is reachable. Handlers report their own failures in\n"
      "the printed text.\n"
      "\n"
      "Set DIFTRAYWM_CONTROL_SOCKET to talk to a session on a non-default socket.\n";
}

const Command *find_command(std::string_view name) {
  for (const auto &entry : kCommands) {
    if (name == entry.name) return &entry;
  }
  return nullptr;
}

constexpr int kRejected = 1;   // the compositor did not recognise the command
constexpr int kUsageError = 2;
constexpr int kNoSession = 3;

// Sends one command and returns the compositor's reply. `accepted` reports the
// verdict line so the caller can choose an exit status; the reply body is
// stripped of the framing.
bool send_command(const std::string &command, bool &accepted, std::string &reply,
                  std::string &error) {
  const std::string path = diftray::control_socket_path();
  if (path.size() >= sizeof(sockaddr_un::sun_path)) {
    error = "control socket path is too long: " + path;
    return false;
  }
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    error = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size());
  if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
    error = std::string("connect: ") + std::strerror(errno);
    ::close(fd);
    return false;
  }
  const std::string request = command + "\n";
  std::size_t sent = 0;
  while (sent < request.size()) {
    const ssize_t count = ::write(fd, request.data() + sent, request.size() - sent);
    if (count > 0) {
      sent += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    error = std::string("write: ") + std::strerror(errno);
    ::close(fd);
    return false;
  }
  // Half-close so the compositor sees the request end and can answer without
  // waiting for more input.
  ::shutdown(fd, SHUT_WR);
  std::string response;
  char buffer[4096];
  for (;;) {
    const ssize_t count = ::read(fd, buffer, sizeof(buffer));
    if (count > 0) {
      response.append(buffer, static_cast<std::size_t>(count));
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) {
      error = std::string("read: ") + std::strerror(errno);
      ::close(fd);
      return false;
    }
    break;
  }
  ::close(fd);
  // Strip the verdict line the compositor prefixes to every reply.
  const std::size_t newline = response.find('\n');
  if (newline == std::string::npos || response.empty()) {
    error = "malformed reply from the compositor";
    return false;
  }
  accepted = response[0] == '+';
  if (response[0] != '+' && response[0] != '-') {
    error = "malformed reply from the compositor";
    return false;
  }
  response.erase(0, newline + 1);
  while (!response.empty() && (response.back() == '\n' || response.back() == '\r')) {
    response.pop_back();
  }
  reply = std::move(response);
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    print_usage();
    return kUsageError;
  }
  const std::string name = argv[1];
  if (name == "--help" || name == "-h" || name == "help-local") {
    print_usage();
    return 0;
  }
  if (name == "--version" || name == "-V" || name == "version") {
    std::cout << "diftrayctl 0.1\n";
    return 0;
  }

  const Command *entry = find_command(name);
  std::string command;
  if (!entry) {
    if (name != "bar") {
      std::cerr << "diftrayctl: unknown command: " << name
                << "\nRun `diftrayctl --help` for the command list.\n";
      return kUsageError;
    }
    // `bar` forwards the rest of the line unchanged, so any Command Bar command
    // -- including ones registered later by a plugin or extension -- stays
    // reachable from the shell without a diftrayctl release.
    if (argc < 3) {
      std::cerr << "diftrayctl bar requires a Command Bar command\n";
      return kUsageError;
    }
    command = argv[2];
    for (int index = 3; index < argc; ++index) {
      command += ' ';
      command += argv[index];
    }
  } else {
    command = entry->command;
    for (int index = 2; index < argc; ++index) {
      command += ' ';
      command += argv[index];
    }
    if (entry->arguments != nullptr && argc == 2) {
      std::cerr << "diftrayctl " << name << " requires " << entry->arguments << '\n';
      return kUsageError;
    }
  }

  std::string reply, error;
  bool accepted = false;
  if (!send_command(command, accepted, reply, error)) {
    // Distinguish "no compositor" from a broken socket so scripts can branch.
    std::cerr << "diftrayctl: no DiftrayWM session at " << diftray::control_socket_path()
              << " (" << error << ")\n"
              << "Start the compositor, or set DIFTRAYWM_CONTROL_SOCKET.\n";
    return kNoSession;
  }
  if (!reply.empty()) {
    std::cout << reply << '\n';
  }
  // The command reached the compositor and was recognised. Handlers report
  // their own failures through the status line (for example "plugin load
  // failed"), so a rejection here means the command itself was unknown.
  return accepted ? 0 : kRejected;
}
