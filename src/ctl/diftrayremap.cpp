// diftrayremap -- the system-wide half of DiftrayWM's key remapper.
//
// The compositor applies keymap.ini to the keys it already reads, unprivileged
// and scoped to its own session. This tool does the other half: it takes the
// physical keyboard with EVIOCGRAB and republishes it through uinput, so the
// same profile applies to every consumer on the machine -- the compositor, a
// TTY, another session, or a program reading evdev directly.
//
// It is a separate binary on purpose. The compositor must not need root, and a
// compositor that grabbed the keyboard would take it away from whatever session
// it is nested inside. Run this as root, or grant the narrow udev rule printed
// by `diftrayremap check`.
//
// Usage:
//   diftrayremap check [KEYMAP]   report whether it can run, and what it found
//   diftrayremap list  [KEYMAP]   list the keyboards [devices] resolves to
//   diftrayremap run   [KEYMAP]   remap until interrupted (the usual mode)
//   diftrayremap --help
//
// Exit codes: 0 success, 1 refused or failed, 2 usage.

#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "keymap/EvdevRemapper.hpp"
#include "keymap/Keymap.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

constexpr const char *kDefaultKeymap = "keymap.ini";
constexpr int kUsage = 2;

void usage(std::FILE *out) {
  std::fprintf(out,
               "diftrayremap -- system-wide key remapping for DiftrayWM\n"
               "\n"
               "Usage:\n"
               "  diftrayremap check [KEYMAP]   report whether remapping can run\n"
               "  diftrayremap list  [KEYMAP]   list keyboards [devices] resolves to\n"
               "  diftrayremap run   [KEYMAP]   remap until interrupted\n"
               "  diftrayremap --help\n"
               "\n"
               "KEYMAP defaults to %s and may be overridden with the\n"
               "DIFTRAYWM_KEYMAP environment variable. The file is the same one the\n"
               "compositor reads; see help/keymap.1 for its format.\n"
               "\n"
               "Remapping is system-wide and needs access to /dev/input/event* and\n"
               "/dev/uinput. Run `check` first: it prints the exact reason when it\n"
               "cannot start rather than failing halfway.\n"
               "\n"
               "Exit codes: 0 success, 1 refused or failed, %d usage.\n",
               kDefaultKeymap, kUsage);
}

std::string keymap_path(const std::vector<std::string> &args) {
  if (args.size() > 1) return args[1];
  if (const char *env = std::getenv("DIFTRAYWM_KEYMAP")) return env;
  return kDefaultKeymap;
}

int fail(const std::string &message) {
  std::fprintf(stderr, "diftrayremap: %s\n", message.c_str());
  return 1;
}

int do_check(const Keymap &keymap) {
  const RemapCapability capability = EvdevRemapper::probe();
  std::printf("uinput present : %s\n", capability.uinput_present ? "yes" : "no");
  std::printf("uinput writable: %s\n", capability.uinput_writable ? "yes" : "no");
  std::printf("euid 0         : %s\n", capability.is_root ? "yes" : "no");
  std::printf("in input group : %s\n", capability.in_input_group ? "yes" : "no");
  std::printf("%s", keymap.summary().c_str());
  const EvdevRemapper remapper;
  std::vector<RemapDevice> devices;
  std::string error;
  if (!remapper.discover(keymap, devices, error)) {
    return fail(error);
  }
  if (devices.empty()) {
    std::printf("matched keyboards: none\n");
  } else {
    std::printf("matched keyboards: %zu\n", devices.size());
    for (const auto &device : devices) {
      std::printf("  %s %s %s\n", device.id.str().c_str(), device.path.c_str(),
                  device.name.c_str());
    }
  }
  if (!capability.can_run) {
    std::printf("verdict: unavailable -- %s\n", capability.explain().c_str());
    return 1;
  }
  if (devices.empty()) {
    std::printf("verdict: nothing to remap\n");
    return 1;
  }
  // Opening a keyboard is the operation that decides whether this can work, so
  // check it rather than reporting "ready" and failing on the first keypress.
  const std::string denied = EvdevRemapper::check_access(devices);
  if (!denied.empty()) {
    std::printf("verdict: unavailable -- %s\n", denied.c_str());
    return 1;
  }
  if (!keymap.system_wide) {
    std::printf("verdict: ready, but the keymap does not request it -- set "
                "system_wide = true in [init] to use diftrayremap run\n");
    return 0;
  }
  std::printf("verdict: ready\n");
  return 0;
}

int do_list(const Keymap &keymap) {
  const EvdevRemapper remapper;
  std::vector<RemapDevice> devices;
  std::string error;
  if (!remapper.discover(keymap, devices, error)) {
    return fail(error);
  }
  if (devices.empty()) {
    std::printf("no keyboard matches [devices]\n");
    return 1;
  }
  for (const auto &device : devices) {
    std::printf("%s %s %s\n", device.id.str().c_str(), device.path.c_str(),
                device.name.c_str());
  }
  return 0;
}

int do_run(const Keymap &keymap) {
  // SIGINT and SIGTERM stop the loop between events, so the grabs are released
  // and the physical keyboard is handed back to the kernel on the way out.
  struct sigaction action {};
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
  sigaction(SIGHUP, &action, nullptr);

  EvdevRemapper remapper;
  std::string error;
  if (!remapper.start(keymap, error)) {
    return fail(error);
  }
  std::printf("%s", remapper.status().c_str());
  std::printf("remapping; press Ctrl-C to stop\n");
  std::fflush(stdout);
  // run() only returns on a device error or a signal; there is no reason for
  // the daemon to keep going after a grab is lost.
  const int processed = remapper.run();
  remapper.stop();
  if (g_stop) {
    std::printf("stopped\n");
    return 0;
  }
  if (processed < 0) {
    return fail(remapper.error());
  }
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty()) {
    usage(stderr);
    return kUsage;
  }
  const std::string command = args.front();
  if (command == "--help" || command == "-h" || command == "help") {
    usage(stdout);
    return 0;
  }
  if (command != "check" && command != "list" && command != "run") {
    usage(stderr);
    return kUsage;
  }
  if (args.size() > 2) {
    std::fprintf(stderr, "diftrayremap: unexpected argument: %s\n", args[2].c_str());
    return kUsage;
  }

  const std::string path = keymap_path(args);
  Keymap keymap;
  std::string error;
  if (!load_keymap(path, keymap, error)) {
    return fail(error);
  }
  if (command == "check") return do_check(keymap);
  if (command == "list") return do_list(keymap);
  return do_run(keymap);
}
