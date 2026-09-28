#pragma once
// System-wide evdev key remapper.
//
// This backend takes a physical keyboard away from the kernel's normal input
// path with EVIOCGRAB and republishes it as a virtual uinput device with the
// remapping applied. Events are transformed underneath every consumer, so the
// change applies to the whole machine: the compositor, another session, a TTY,
// or anything else reading evdev.
//
// That reach is why it is a separate privileged process (`diftrayremap`) rather
// than part of the compositor. The compositor deliberately runs unprivileged
// and owns its input through libinput; a compositor that grabbed the physical
// keyboard would take it away from the session it is nested inside, and would
// force every user to run their window manager as root. The two halves share one
// keymap.ini, so the mapping is defined once.
//
// `probe()` reports exactly what is missing when it cannot run, and
// `map_event()` is a pure function so the mapping can be tested with no device
// and no privileges at all.
#include <cstdint>
#include <string>
#include <vector>

#include <linux/input.h>

#include "keymap/Keymap.hpp"

struct udev;
struct libevdev;


// Why the evdev backend cannot run, if it cannot.
struct RemapCapability {
  bool uinput_present = false;   // /dev/uinput exists
  bool uinput_writable = false;  // and this process may open it
  bool is_root = false;         // euid 0
  bool in_input_group = false;   // effective gid is the "input" group
  bool can_run = false;         // uinput_writable && libraries present

  // Human-readable diagnosis, including the remedy for the common case.
  std::string explain() const;
};

// A keyboard the remapper may claim.
struct RemapDevice {
  DeviceId id;
  std::string path;  // /dev/input/eventN
  std::string name;  // udev device name
  std::string role;  // role from the [devices] section, default "keyboard"
};

class EvdevRemapper {
public:
  EvdevRemapper();
  ~EvdevRemapper();
  EvdevRemapper(const EvdevRemapper &) = delete;
  EvdevRemapper &operator=(const EvdevRemapper &) = delete;

  // Inspects the environment without claiming anything. `uinput_writable` and
  // `input_readable` are separate: the uinput node is often granted by an ACL
  // while the physical devices are not, and a probe that only checked uinput
  // would report "ready" for a process that cannot actually open a keyboard.
  static RemapCapability probe();
  // Why the devices in `out` cannot be claimed, if they cannot. Reads the node
  // without grabbing it, so it is safe to call at any time.
  static std::string check_access(const std::vector<RemapDevice> &out);

  // Lists keyboards that match the keymap's [devices] section. Read-only: an
  // empty [devices] means every keyboard. Returns false only on a udev failure;
  // an empty list is a valid, reportable answer.
  bool discover(const Keymap &keymap, std::vector<RemapDevice> &out,
                std::string &error) const;

  // True when the event node looks like a keyboard: the udev property, or, when
  // that is absent, an EV_KEY bitmap containing the letter keys. Without this,
  // "every keyboard" would also claim mice, lid switches and power buttons.
  static bool is_keyboard(const std::string &path, const char *id_input_keyboard);

  // Claims the matching keyboards and starts publishing a virtual device.
  bool start(const Keymap &keymap, std::string &error);
  // Stops remapping, releasing every grab and destroying the virtual device.
  void stop();
  bool running() const { return uinput_fd_ >= 0; }
  std::string status() const;

  // Reads from the claimed devices and writes remapped events until stopped.
  // Returns the number of events processed, or -1 on error.
  int run();

  const std::string &error() const { return error_; }
  const Profile *active_profile() const { return active_; }
  void reset_profile();

  // Pure mapping: one physical event in, the events to publish out. A result
  // with no EV_KEY means the key was swallowed. `held_mods` is the modifier
  // state already down (kMod* bits) and `switch_to_profile` is set when the
  // event triggered a profile change.
  //
  // The modifier state is a parameter rather than something derived from the
  // event, because a chord is the key plus the modifiers held at the time:
  // <C-a> is Ctrl down, then the a key down. run() maintains held_mods from the
  // modifier events it sees.
  //
  // The [init] prefix is handled the way keyd handles it: the press runs the
  // prefix action (normally Trigger into a profile) and both press and release
  // are swallowed, so the chord never reaches the consumer as a character.
  static std::vector<input_event> map_event(const Profile *profile,
                                          const Keymap &keymap,
                                          uint32_t held_mods,
                                          const input_event &event,
                                          std::string &switch_to_profile,
                                          uint32_t *held_mods_next = nullptr);

  // Which kMod* bits an event sets or clears. 0 for a release.
  static uint32_t mods_of_event(const input_event &event);

  // Holds the active profile and applies a Trigger() or the [init] prefix.
  //
  // A switch is deferred until the triggering key is released. Switching on the
  // press would look up that key's release in the *new* profile, where the
  // binding no longer exists, and the release would be published -- leaving the
  // consumer holding a key the remapper had already swallowed. Deferring keeps
  // both edges of a key in the same profile, which is also what a user expects
  // when they tap the prefix and immediately type.
  class ProfileState {
  public:
    explicit ProfileState(const Keymap &keymap) : keymap_(&keymap) {
      active_ = keymap.current();
    }
    const Profile *active() const { return active_; }
    // Call once per event, after map_event, with the profile it reported.
    void note(const std::string &switch_to, const input_event &event);
    // True when the event is a key release, the moment a deferred switch lands.
    bool applied() const { return applied_; }
    void clear_applied() { applied_ = false; }

  private:
    const Keymap *keymap_;
    const Profile *active_ = nullptr;
    std::string pending_;
    bool applied_ = false;
  };

  // Describes a chord the way the keymap spells it, for diagnostics.
  static std::string describe_event(const input_event &event);

private:
  struct Slot {
    int fd = -1;
    libevdev *device = nullptr;
    RemapDevice info;
  };

  bool claim(const RemapDevice &target, const Keymap &keymap, std::string &error);
  void publish(const input_event &event);

  udev *udev_ = nullptr;
  int uinput_fd_ = -1;
  std::vector<Slot> slots_;
  std::string error_;
  const Keymap *keymap_ = nullptr;
  const Profile *active_ = nullptr;
};

