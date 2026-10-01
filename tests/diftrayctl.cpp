// Integration test for diftrayctl against a real headless compositor.
//
// This drives the shipped binaries the way a user would: it starts diftraywm
// with a private control socket, then runs diftrayctl for each command and
// checks the printed status line and the exit status. Unit-testing the Command
// Bar handlers instead would miss the socket framing, the reply verdict and
// the process-level session handling, which is exactly what this covers.
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cerrno>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef DIFTRAYCTL_BINARY
#error "DIFTRAYCTL_BINARY must name the diftrayctl binary"
#endif
#ifndef DIFTRAYWM_BINARY
#error "DIFTRAYWM_BINARY must name the compositor binary"
#endif

namespace {

int failures = 0;

void check(bool ok, const std::string &message) {
  if (!ok) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

struct Reply {
  int status = -1;
  std::string output;
};

std::string join(const std::vector<std::string> &arguments) {
  std::string out;
  for (const auto &argument : arguments) {
    if (!out.empty()) out += ' ';
    out += argument;
  }
  return out;
}

// Runs diftrayctl directly (no shell) so glob characters, braces and spaces in
// arguments reach the client exactly as a caller would pass them.
Reply ctl(const std::vector<std::string> &arguments) {
  Reply reply;
  int channel[2];
  if (::pipe(channel) != 0) {
    reply.output = "pipe failed";
    return reply;
  }
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::dup2(channel[1], STDOUT_FILENO);
    ::dup2(channel[1], STDERR_FILENO);
    ::close(channel[0]);
    ::close(channel[1]);
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(DIFTRAYCTL_BINARY));
    for (const auto &argument : arguments) {
      argv.push_back(const_cast<char *>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(DIFTRAYCTL_BINARY, argv.data());
    std::_Exit(127);
  }
  ::close(channel[1]);
  std::array<char, 1024> buffer{};
  for (;;) {
    const ssize_t count = ::read(channel[0], buffer.data(), buffer.size());
    if (count > 0) {
      reply.output.append(buffer.data(), static_cast<std::size_t>(count));
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    break;
  }
  ::close(channel[0]);
  int status = 0;
  ::waitpid(pid, &status, 0);
  reply.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return reply;
}

bool contains(const Reply &reply, const std::string &needle) {
  return reply.output.find(needle) != std::string::npos;
}

void expect(const std::vector<std::string> &arguments, const std::string &needle,
            int status = 0) {
  const Reply reply = ctl(arguments);
  check(reply.status == status && contains(reply, needle),
        "`diftrayctl " + join(arguments) + "` expected status " +
            std::to_string(status) + " containing \"" + needle + "\", got " +
            std::to_string(reply.status) + " and \"" + reply.output + "\"");
}

bool wait_for_socket(const std::string &path, pid_t session) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    struct stat info{};
    if (::stat(path.c_str(), &info) == 0 && S_ISSOCK(info.st_mode)) return true;
    int status = 0;
    if (::waitpid(session, &status, WNOHANG) == session) return false;
    ::usleep(50 * 1000);
  }
  return false;
}

}  // namespace

int main() {
  char directory[] = "/tmp/diftrayctl-test-XXXXXX";
  if (!::mkdtemp(directory)) {
    std::cerr << "mkdtemp failed\n";
    return EXIT_FAILURE;
  }
  const std::filesystem::path root(directory);
  const auto socket = (root / "ctl.sock").string();
  const auto config = root / "diftray.conf";
  {
    std::ofstream out(config);
    out << "general {\n  font_size = 13\n}\nterminal {\n  shell = libshell\n}\n";
  }
  // A real keymap INI beside the config, so the `keymap` commands run against an
  // actually-loaded file rather than reporting a missing path.
  {
    std::ofstream out(root / "keymap.ini");
    out << "[init]\n"
           "prefix = <C-q>\n"
           "action = Trigger(alt)\n"
           "\n"
           "[default]\n"
           "<C-t> = Diftray(workspace 2)\n"
           "\n"
           "[alt]\n"
           "<M-esc> = Trigger(default)\n"
           "<C-w> = Ignore()\n";
  }
  // A Termscript file and a Lua extension for the two execution paths.
  const auto script = root / "hello.tsc";
  {
    std::ofstream out(script);
    out << "G:puts \"termscript ran\";\n";
  }
  const auto terminal_script = root / "terminal.tsc";
  {
    std::ofstream out(terminal_script);
    out << "const T = G:load \"diftray.terminal\";\n"
           "const dimensions = T:size;\n"
           "G:puts dimensions;\n"
           "G:puts \"cell script ran\";\n";
  }
  const auto extension = root / "hello.lua";
  {
    std::ofstream out(extension);
    out << "diftray.status(\"extension loaded\")\n"
           "diftray.register_command(\"ctlping\", function(tokens, scope)\n"
           "  diftray.status(\"pong \" .. scope)\n"
           "end)\n";
  }

  ::setenv("XDG_RUNTIME_DIR", directory, 1);
  ::setenv("DIFTRAYWM_CONTROL_SOCKET", socket.c_str(), 1);
  ::setenv("DIFTRAYWM_CONFIG", config.c_str(), 1);
  ::setenv("WLR_BACKENDS", "headless", 1);
  ::setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
  ::setenv("WLR_RENDERER", "pixman", 1);
  ::unsetenv("DIFTRAYWM_LOGIC_ONLY");

  const pid_t session = ::fork();
  if (session == 0) {
    ::execl(DIFTRAYWM_BINARY, "diftraywm", static_cast<char *>(nullptr));
    std::_Exit(127);
  }
  if (session < 0) {
    std::cerr << "fork failed\n";
    return EXIT_FAILURE;
  }
  if (!wait_for_socket(socket, session)) {
    std::cerr << "compositor did not publish " << socket << '\n';
    ::kill(session, SIGKILL);
    ::waitpid(session, nullptr, 0);
    std::filesystem::remove_all(root);
    return EXIT_FAILURE;
  }
  // The socket grants session control and code loading, so it must not be
  // reachable by other users.
  {
    struct stat info{};
    ::stat(socket.c_str(), &info);
    check((info.st_mode & 077) == 0, "control socket is not owner-only");
  }

  // Session state.
  expect({"status"}, "workspace: 1");
  expect({"status"}, "control socket: " + socket);
  expect({"config-path"}, config.string());
  expect({"plugin-list"}, "no plugins loaded");
  expect({"extension-list"}, "no extensions loaded");
  expect({"mux-list"}, "* ");
  expect({"output-list"}, "HEADLESS-1");
  expect({"cursor-list"}, "no cursors");
  expect({"notelet-list"}, "notelets:");

  // Multiplexer.
  expect({"mux-split", "horizontal"}, "split cell ");
  expect({"mux-list"}, "\n* ");
  expect({"mux-focus", "next"}, "focused cell ");
  expect({"mux-zoom"}, "promoted cell ");
  expect({"mux-zoom"}, "restored tcursor");
  expect({"mux-kill"}, "removed cell");
  expect({"mux-split", "horizontal", "sideways"}, "requires horizontal or vertical");

  // Termscript and Lua extension execution.
  expect({"source-script", script.string()}, "termscript ran");
  expect({"bar", "terminal", "script", terminal_script.string()}, "cell script ran");
  expect({"source-script", (root / "missing.tsc").string()}, "cannot read script");
  expect({"extension-exec", extension.string()}, "loaded extension");
  expect({"extension-list"}, extension.string());
  expect({"bar", "ctlping"}, "pong global");

  // Native plugins (dynalo).
  {
    const std::filesystem::path plugin =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "build" /
        "tests" / "libdiftraywm_test_plugin.so";
    if (std::filesystem::exists(plugin)) {
      expect({"plugin-load", plugin.string()}, "loaded plugin");
      expect({"plugin-list"}, "libdiftraywm_test_plugin.so");
      expect({"bar", "fixture", "hello"}, "hello");
      expect({"plugin-unload", plugin.string()}, "unloaded plugin");
      expect({"plugin-list"}, "no plugins loaded");
      // The command the plugin registered is gone with it.
      expect({"bar", "fixture", "hello"}, "unknown command: fixture", 1);
    }
  }
  expect({"plugin-load", "/nonexistent/plugin.so"}, "plugin load failed");

  // Configuration.
  expect({"config-eval", "6 * 7"}, "42");
  expect({"config-reload"}, "reloaded " + config.string());
  {
    // A broken config must be rejected atomically, leaving the session usable.
    std::ofstream out(config);
    out << "general {\n  font_size = 2\n}\n";
  }
  expect({"config-reload"}, "reload failed");
  expect({"status"}, "workspace: 1");
  {
    std::ofstream out(config);
    out << "general {\n  font_size = 13\n}\nterminal {\n  shell = libshell\n}\n";
  }
  expect({"config-reload"}, "reloaded " + config.string());

  // Theming.
  {
    const std::filesystem::path theme =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "themes" / "light.css";
    expect({"theme-load", theme.string()}, "theme applied");
    expect({"theme-show"}, "border-color: #2456a6");
    expect({"set-theme", ":root { border-size: 7px; }"}, "theme applied");
    expect({"theme-show"}, "border-size: 7px");
  }

  // Usage errors and unknown commands.
  expect({"definitely-not-a-command"}, "unknown command", 2);
  expect({"mux-split"}, "requires <horizontal|vertical>", 2);
  expect({"bar", "definitely-not-a-command"}, "unknown command", 1);
  {
    const Reply version = ctl({"version"});
    check(version.status == 0 && contains(version, "diftrayctl"),
          "version is local and reports the client version");
    const Reply usage = ctl({"--help"});
    check(usage.status == 0 && contains(usage, "exit-session"),
          "--help lists the session commands");
  }
  // diftrayctl works without a session too, and says so clearly.
  {
    ::setenv("DIFTRAYWM_CONTROL_SOCKET", (root / "absent.sock").c_str(), 1);
    const Reply orphan = ctl({"status"});
    check(orphan.status == 3 && contains(orphan, "no DiftrayWM session"),
          "a missing session reports status 3, got " +
              std::to_string(orphan.status) + " and \"" + orphan.output + "\"");
    const Reply local = ctl({"version"});
    check(local.status == 0, "version still works with no session");
    ::setenv("DIFTRAYWM_CONTROL_SOCKET", socket.c_str(), 1);
  }

  // Launcher taskbar.
  expect({"launchbar-status"}, "launch bar: unlocked");
  expect({"lock-launchbar"}, "launch bar locked on top");
  expect({"launchbar-status"}, "launch bar: locked");
  expect({"launchbar-status"}, "visible: yes");
  expect({"launchbar-status"}, "anchor: top");
  expect({"lock-launchbar"}, "launch bar already locked");
  expect({"unlock-launchbar"}, "launch bar unlocked");
  expect({"launchbar-status"}, "visible: no");
  expect({"toggle-launchbar"}, "launch bar locked on top");
  expect({"toggle-launchbar"}, "launch bar unlocked");
  expect({"bar", "launcher", "lock"}, "launch bar locked on top");
  expect({"bar", "launcher", "unlock"}, "launch bar unlocked");
  expect({"bar", "launcher", "status"}, "anchor: top");
  expect({"bar", "launcher"}, "launcher supports: lock, unlock, toggle, status");
  expect({"bar", "launcher", "lock", "extra"}, "takes no arguments");

  // Keymap INI. The compositor reads keymap.ini from the config directory, so
  // these run against a real file rather than a stubbed load.
  expect({"keymap-path"}, "keymap.ini");
  expect({"keymap-show"}, "layer prefix: <C-q> -> alt");
  expect({"keymap-show"}, "active profile: default");
  expect({"keymap-show"}, "<C-t> = Diftray(workspace 2)");
  expect({"keymap-chord", "<C-q>"}, "chord <C-q> is code");
  expect({"keymap-chord", "<C-S-z>"}, "chord <C-S-z> is code");
  // An unknown chord is a hard error with the reason, never a wrong key.
  expect({"keymap-chord", "<C-nope>"}, "unknown key");
  expect({"keymap-chord", "<C-C-q>"}, "duplicate modifier");
  // A missing required argument is caught client-side, so the exit status is
  // the usage code and the command never reaches the compositor.
  expect({"keymap-chord"}, "requires <chord>", 2);
  expect({"keymap-check", "/nonexistent/keymap.ini"}, "keymap check failed");
  expect({"keymap-reload"}, "reloaded ");
  expect({"keymap-reset"}, "keymap profile: default");
  expect({"keymap-profile", "alt"}, "keymap profile: alt");
  expect({"keymap-profile", "default"}, "keymap profile: default");
  // A profile that does not exist is refused rather than silently accepted.
  expect({"keymap-profile", "nowhere"}, "unknown profile");
  expect({"keymap"}, "keymap supports: show, reload, profile <name>");
  // Every remote verb must be a real Command Bar command, so the same text
  // typed after `:` does the same thing.
  expect({"bar", "keymap", "show"}, "layer prefix: <C-q> -> alt");
  expect({"bar", "keymap", "chord", "<G-q>"}, "chord <G-q> is code");

  // restart-session re-execs in place, so the PID is unchanged and the socket
  // is republished by the new image.
  expect({"restart-session"}, "restarting session");
  check(wait_for_socket(socket, session),
        "compositor did not come back after restart-session");
  expect({"status"}, "workspace: 1");

  // Tear down.
  expect({"exit-session"}, "exiting session");
  int status = 0;
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (::waitpid(session, &status, WNOHANG) == session) break;
    ::usleep(50 * 1000);
  }
  check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "compositor exited cleanly after exit-session");
  check(!std::filesystem::exists(socket), "control socket was unlinked on exit");

  std::filesystem::remove_all(root);
  if (failures == 0) {
    std::cout << "diftrayctl drives a live session\n";
    return EXIT_SUCCESS;
  }
  std::cerr << failures << " diftrayctl check(s) failed\n";
  return EXIT_FAILURE;
}
