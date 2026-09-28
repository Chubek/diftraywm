#include "keymap/EvdevRemapper.hpp"

#include <fcntl.h>
#include <grp.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <sstream>

extern "C" {
#include <libevdev-1.0/libevdev/libevdev.h>
#include <libudev.h>
}

namespace {

constexpr const char *kUinputPath = "/dev/uinput";
constexpr const char *kInputGroup = "input";
// A udev property used to spot keyboards without opening anything.
constexpr const char *kIdVendor = "id/vendor";
constexpr const char *kIdProduct = "id/product";
constexpr int kInputBitsMax = KEY_MAX;

bool is_modifier_key(uint32_t code) {
  switch (code) {
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT:
    case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
    case KEY_LEFTALT: case KEY_RIGHTALT:
    case KEY_LEFTMETA: case KEY_RIGHTMETA:
      return true;
    default:
      return false;
  }
}

void enable_key_bit(std::vector<uint8_t> &bits, int code) {
  if (code < 0 || code >= static_cast<int>(bits.size() * 8)) return;
  bits[static_cast<std::size_t>(code) / 8] |=
      static_cast<uint8_t>(1u << (code % 8));
}

bool key_bit_set(const std::vector<uint8_t> &bits, int code) {
  if (code < 0 || code >= static_cast<int>(bits.size() * 8)) return false;
  return (bits[static_cast<std::size_t>(code) / 8] & (1u << (code % 8))) != 0;
}

}  // namespace

std::string RemapCapability::explain() const {
  if (can_run) {
    return "evdev remapping is available";
  }
  std::ostringstream out;
  if (!uinput_present) {
    out << "/dev/uinput does not exist";
  } else if (!uinput_writable) {
    out << "/dev/uinput is not writable by this process";
  } else {
    out << "libudev or libevdev is unavailable";
  }
  if (uinput_present && !uinput_writable) {
    out << ". Run diftrayremap as root, or grant access with a udev rule that "
           "sets GROUP=\"input\" MODE=\"0660\" on uinput and the input event "
           "nodes, and add the diftrayremap service to the input group";
  }
  return out.str();
}

EvdevRemapper::EvdevRemapper() = default;
EvdevRemapper::~EvdevRemapper() { stop(); }

RemapCapability EvdevRemapper::probe() {
  RemapCapability capability;
  capability.is_root = ::geteuid() == 0;
  if (const group *entry = ::getgrnam(kInputGroup); entry != nullptr) {
    capability.in_input_group = ::getegid() == entry->gr_gid ||
                               ::getgid() == entry->gr_gid;
  }
  capability.uinput_present = ::access(kUinputPath, F_OK) == 0;
  // access(2) does not see ACLs, so try the real open before concluding.
  if (::access(kUinputPath, W_OK) == 0) {
    capability.uinput_writable = true;
  } else if (const int fd = ::open(kUinputPath, O_WRONLY | O_CLOEXEC); fd >= 0) {
    capability.uinput_writable = true;
    ::close(fd);
  }
  if (udev *context = udev_new(); context != nullptr) {
    capability.can_run = true;
    udev_unref(context);
  }
  // libevdev is linked in unconditionally, so its presence is established by
  // the build; libudev must initialise for device discovery to work.
  capability.can_run = capability.can_run && capability.uinput_writable;
  return capability;
}

// Reads the EV_KEY bitmap of an event node and looks for the letter keys.
// Anything with a real keyboard has these; a mouse, a lid switch or a power
// button does not.
bool EvdevRemapper::is_keyboard(const std::string &path,
                                const char *id_input_keyboard) {
  // udev already worked this out where the hwdb is present, and it knows about
  // devices whose capabilities are unusual.
  if (id_input_keyboard) {
    return std::strcmp(id_input_keyboard, "1") == 0;
  }
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  const int bytes = (KEY_MAX + 8) / 8;
  std::vector<uint8_t> bits(static_cast<std::size_t>(bytes), 0);
  const int got = ::ioctl(fd, EVIOCGBIT(EV_KEY, bytes), bits.data());
  ::close(fd);
  if (got < 0) {
    return false;
  }
  const auto has = [&](unsigned int code) {
    return (bits[static_cast<std::size_t>(code) / 8] & (1u << (code % 8))) != 0;
  };
  // A and Z is the cheapest test that no pointer or button device passes, and
  // every real keyboard has both.
  return has(KEY_A) && has(KEY_Z);
}

std::string EvdevRemapper::check_access(const std::vector<RemapDevice> &out) {
  if (out.empty()) {
    return "no keyboard to check";
  }
  // Open the first one read-only. This is the operation that decides whether
  // remapping can work at all, and doing it here means `check` cannot report
  // "ready" for a process that would fail on the first event.
  for (const auto &device : out) {
    const int fd = ::open(device.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
      ::close(fd);
      return {};
    }
    const std::string reason = std::strerror(errno);
    if (reason != "Permission denied" && reason != "Operation not permitted") {
      // Something other than permissions went wrong -- the device went away,
      // or the node is not a character device. Report it rather than continuing
      // to the next one, because it is a real fault.
      return device.path + ": " + reason;
    }
  }
  return "cannot open any keyboard: permission denied. The event nodes are "
         "root:input mode 0660, so either run as root or add a udev rule "
         "granting access (GROUP=\"input\" MODE=\"0660\" on KERNEL==\"event*\", "
         "SUBSYSTEM==\"input\") and put this process in the input group";
}

// Names a single event for a diagnostic. A lone event carries no chord
// information beyond its own code, so the modifiers it is itself are the only
// thing that can be reported; run() uses held_mods for the real matching.
// Which kMod* bits an event sets or clears. A release clears, so the same
// mapping serves both directions and the caller just applies it.
uint32_t EvdevRemapper::mods_of_event(const input_event &event) {
  uint32_t mods = kModNone;
  switch (event.code) {
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT: mods |= kModShift; break;
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL: mods |= kModCtrl; break;
    case KEY_LEFTALT:
    case KEY_RIGHTALT: mods |= kModAlt; break;
    // The kernel spells the Super/Logo keys LEFTMETA and RIGHTMETA.
    case KEY_LEFTMETA:
    case KEY_RIGHTMETA: mods |= kModLogo; break;
    default: break;
  }
  return mods;
}

// A switch is armed by the press and lands on the matching release. Anything
// else -- an unbound key, or a second switch while one is already pending --
// leaves the pending switch alone, so holding the prefix down does not queue up
// a pile of profile changes.
void EvdevRemapper::ProfileState::note(const std::string &switch_to,
                                       const input_event &event) {
  applied_ = false;
  if (!switch_to.empty()) {
    // Only a press arms a switch, and only one may be pending.
    if (event.value != 0 && pending_.empty()) {
      std::string error;
      if (keymap_->profile_named(switch_to, error)) {
        pending_ = switch_to;
      }
      // An unresolvable name is impossible: the parser rejects it. Ignore it
      // rather than switching somewhere undefined.
    }
    return;
  }
  if (pending_.empty()) {
    return;
  }
  // The release of a key: the deferred switch takes effect now.
  if (event.type == EV_KEY && event.value == 0 && !is_modifier_key(event.code)) {
    std::string error;
    if (const Profile *target = keymap_->profile_named(pending_, error)) {
      active_ = target;
    }
    pending_.clear();
    applied_ = true;
  }
}

std::string EvdevRemapper::describe_event(const input_event &event) {
  KeyChord chord;
  chord.code = static_cast<uint32_t>(event.code);
  chord.mods = event.value != 0 ? mods_of_event(event) : kModNone;
  return "<" + chord.str() + ">";
}

void EvdevRemapper::reset_profile() {
  active_ = keymap_ ? keymap_->current() : nullptr;
}

void EvdevRemapper::stop() {
  // Release every grab before closing, so the physical keyboard works again
  // immediately even if we are killed straight after.
  for (auto &slot : slots_) {
    if (slot.fd >= 0) ::ioctl(slot.fd, EVIOCGRAB, 0);
    if (slot.device) libevdev_free(slot.device);
    if (slot.fd >= 0) ::close(slot.fd);
  }
  slots_.clear();
  if (uinput_fd_ >= 0) {
    ::ioctl(uinput_fd_, UI_DEV_DESTROY);
    ::close(uinput_fd_);
    uinput_fd_ = -1;
  }
  if (udev_) {
    udev_unref(udev_);
    udev_ = nullptr;
  }
  active_ = nullptr;
  keymap_ = nullptr;
}

std::string EvdevRemapper::status() const {
  std::ostringstream out;
  if (uinput_fd_ < 0) {
    out << "evdev remapper: not running";
    if (!error_.empty()) out << " (" << error_ << ")";
    return out.str();
  }
  out << "evdev remapper: running\n";
  out << "claimed devices: " << slots_.size() << "\n";
  for (const auto &slot : slots_) {
    out << "  " << slot.info.id.str() << " " << slot.info.path;
    if (!slot.info.name.empty()) out << " " << slot.info.name;
    out << "\n";
  }
  out << "profile: " << (active_ ? active_->name : std::string("(none)")) << "\n";
  return out.str();
}

std::vector<input_event> EvdevRemapper::map_event(const Profile *profile,
                                                const Keymap &keymap,
                                                uint32_t held_mods,
                                                const input_event &event,
                                                std::string &switch_to_profile,
                                                uint32_t *held_mods_next) {
  std::vector<input_event> out;
  switch_to_profile.clear();
  if (held_mods_next) {
    *held_mods_next = held_mods;
  }

  if (event.type != EV_KEY) {
    // EV_SYN and EV_MSC must pass through or consumers desynchronise.
    out.push_back(event);
    return out;
  }
  if (is_modifier_key(static_cast<uint32_t>(event.code))) {
    // Track the modifier state so the next key is matched as a real chord. The
    // event itself always passes through: a swallowed Ctrl would leave the
    // consumer stuck in a modifier.
    if (held_mods_next) {
      if (event.value != 0) {
        *held_mods_next |= mods_of_event(event);
      } else {
        *held_mods_next &= ~mods_of_event(event);
      }
    }
    out.push_back(event);
    return out;
  }

  KeyChord chord;
  chord.code = static_cast<uint32_t>(event.code);
  // A release carries no modifiers in the event, so the held state is used
  // unchanged: <C-a> and <C-b> are distinguished by which key, not by the
  // event's own code.
  chord.mods = held_mods;

  // The prefix runs its action on the press and is swallowed on both edges.
  // Swallowing the release matters: a consumer that only ever sees the press
  // dropped would hold the key forever.
  if (keymap.prefix.code == chord.code && keymap.prefix.mods == chord.mods) {
    if (event.value != 0 && keymap.prefix_action.kind == Action::Kind::trigger) {
      switch_to_profile = keymap.prefix_action.profile;
    }
    return out;
  }

  if (!profile) {
    out.push_back(event);
    return out;
  }
  const auto it = profile->bindings.find(chord);
  if (it == profile->bindings.end()) {
    out.push_back(event);
    return out;
  }
  const Action &action = it->second;
  switch (action.kind) {
    case Action::Kind::ignore:
      break;
    case Action::Kind::remap: {
      // A remap must not re-trigger itself, so the replacement is emitted once
      // and is never looked up again.
      input_event replacement = event;
      replacement.code = static_cast<__u16>(action.chord.code);
      out.push_back(replacement);
      break;
    }
    case Action::Kind::trigger:
      // Reported on the press only. The release is swallowed, and the switch
      // itself is deferred by ProfileState, so both edges of the key stay in
      // the profile it was pressed in.
      if (event.value != 0) {
        switch_to_profile = action.profile;
      }
      break;
    case Action::Kind::exec:
    case Action::Kind::typeout:
    case Action::Kind::diftray:
    case Action::Kind::none:
      // These are compositor-level actions with no evdev equivalent; the key
      // passes through so the compositor's own keymap can still handle it.
      out.push_back(event);
      break;
  }
  return out;
}

bool EvdevRemapper::discover(const Keymap &keymap, std::vector<RemapDevice> &out,
                             std::string &error) const {
  out.clear();
  error.clear();
  udev *context = udev_new();
  if (!context) {
    error = "cannot create a udev context";
    return false;
  }
  udev_enumerate *enumerator = udev_enumerate_new(context);
  if (!enumerator) {
    error = "cannot enumerate input devices";
    udev_unref(context);
    return false;
  }
  udev_enumerate_add_match_subsystem(enumerator, "input");
  udev_enumerate_scan_devices(enumerator);
  // This libudev has no udev_enumerate_list_entry_next; the list is a plain
  // udev_list_entry chain, so walk it directly.
  for (udev_list_entry *entry = udev_enumerate_get_list_entry(enumerator); entry;
       entry = udev_list_entry_get_next(entry)) {
    const char *syspath = udev_list_entry_get_name(entry);
    if (!syspath) continue;
    udev_device *device = udev_device_new_from_syspath(context, syspath);
    if (!device) continue;
    const char *devnode = udev_device_get_devnode(device);
    // Event nodes are the ones the compositor and the kernel actually read.
    if (devnode && std::strstr(devnode, "event") != nullptr) {
      // Only keyboards. Without this an empty [devices] section would claim
      // mice, lid switches and power buttons too, and intercepting a power
      // button is a very bad time to discover the problem.
      const char *is_keyboard_udev =
          udev_device_get_property_value(device, "ID_INPUT_KEYBOARD");
      if (!is_keyboard(devnode, is_keyboard_udev)) {
        udev_device_unref(device);
        continue;
      }
      // USB IDs live on the parent, one level above the input device.
      // udev_device_get_parent lazily attaches the parent to the child rather
      // than taking a reference on it, and udev_device_unref releases the
      // whole chain when the child's count reaches zero. So the parent is
      // borrowed here and must not be unref'd separately.
      udev_device *parent = udev_device_get_parent_with_subsystem_devtype(
          device, "usb", nullptr);
      if (!parent) parent = udev_device_get_parent(device);
      RemapDevice found;
      found.path = devnode;
      if (const char *name = udev_device_get_sysattr_value(device, "name")) {
        found.name = name;
      }
      // A keyboard with no USB parent (a built-in laptop keyboard on some
      // machines, or a virtual device) has no vendor:product to report, but
      // "every keyboard" should still find it.
      bool identified = false;
      if (parent) {
        const char *vendor = udev_device_get_sysattr_value(parent, kIdVendor);
        const char *product = udev_device_get_sysattr_value(parent, kIdProduct);
        if (vendor && product) {
          identified = parse_device_id(std::string(vendor) + ":" + product,
                                       found.id, error);
        }
      }
      const auto role = keymap.devices.find(found.id);
      // An empty [devices] section means every keyboard. A named one is
      // matched on the identifier, so a keyboard without a USB parent can only
      // be selected by leaving the section out.
      if (keymap.devices.empty() ||
          (identified && role != keymap.devices.end() && role->second == "keyboard")) {
        found.role = role == keymap.devices.end() ? "keyboard" : role->second;
        out.push_back(found);
      }
    }
    udev_device_unref(device);
  }
  udev_enumerate_unref(enumerator);
  udev_unref(context);
  return true;
}

bool EvdevRemapper::claim(const RemapDevice &target, const Keymap &keymap,
                          std::string &error) {
  const int fd = ::open(target.path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    error = "cannot open " + target.path + ": " + std::strerror(errno);
    return false;
  }
  // This libevdev's new_from_fd takes no flags and reports failure through its
  // out-parameter, so check the pointer rather than a return code.
  libevdev *device = nullptr;
  if (libevdev_new_from_fd(fd, &device) < 0 || !device) {
    error = "cannot wrap " + target.path + " with libevdev";
    ::close(fd);
    return false;
  }
  if (::ioctl(fd, EVIOCGRAB, 1) < 0) {
    error = "cannot grab " + target.path + ": " + std::strerror(errno);
    libevdev_free(device);
    ::close(fd);
    return false;
  }

  uinput_setup setup{};
  setup.id.bustype = BUS_USB;
  setup.id.vendor = target.id.vendor;
  setup.id.product = target.id.product;
  setup.id.version = 1;
  std::snprintf(setup.name, sizeof(setup.name), "DiftrayWM remap %s",
                target.name.empty() ? target.id.str().c_str() : target.name.c_str());

  // Clone the physical capabilities so every consumer sees the same keyboard.
  // Read the bitmap with the ioctl rather than a libevdev helper: this
  // vendored libevdev does not export one, and the ioctl is the source of truth
  // regardless.
  const int bits = kInputBitsMax + 1;
  std::vector<uint8_t> key_bits(static_cast<std::size_t>((bits + 7) / 8), 0);
  if (::ioctl(fd, EVIOCGBIT(0, bits), key_bits.data()) < 0) {
    error = "cannot read the key bitmap from " + target.path + ": " +
            std::strerror(errno);
    ::ioctl(fd, EVIOCGRAB, 0);
    libevdev_free(device);
    ::close(fd);
    return false;
  }
  // A remap may name a key the physical device does not have.
  for (const auto &[name, profile] : keymap.profiles) {
    (void)name;
    for (const auto &[chord, action] : profile.bindings) {
      if (action.kind == Action::Kind::remap) {
        enable_key_bit(key_bits, static_cast<int>(action.chord.code));
      }
    }
  }
  if (keymap.prefix.code) {
    enable_key_bit(key_bits, static_cast<int>(keymap.prefix.code));
  }

  const auto fail = [&](const std::string &message) {
    error = message;
    ::ioctl(fd, EVIOCGRAB, 0);
    libevdev_free(device);
    ::close(fd);
    return false;
  };

  if (::ioctl(uinput_fd_, UI_DEV_SETUP, &setup) < 0) {
    return fail(std::string("cannot configure the virtual device: ") + std::strerror(errno));
  }
  for (const int event_type : {EV_KEY, EV_SYN, EV_MSC}) {
    if (::ioctl(uinput_fd_, UI_SET_EVBIT, event_type) < 0) {
      return fail(std::string("cannot advertise the virtual device: ") + std::strerror(errno));
    }
  }
  for (int code = 0; code <= kInputBitsMax; ++code) {
    if (key_bit_set(key_bits, code) && ::ioctl(uinput_fd_, UI_SET_KEYBIT, code) < 0) {
      return fail(std::string("cannot advertise a key capability: ") + std::strerror(errno));
    }
  }
  if (::ioctl(uinput_fd_, UI_DEV_CREATE) < 0) {
    return fail(std::string("cannot create the virtual device: ") + std::strerror(errno));
  }
  slots_.push_back(Slot{fd, device, target});
  return true;
}

bool EvdevRemapper::start(const Keymap &keymap, std::string &error) {
  error.clear();
  stop();
  const RemapCapability capability = probe();
  if (!capability.can_run) {
    error = "system-wide remapping is unavailable: " + capability.explain();
    return false;
  }
  udev_ = udev_new();
  if (!udev_) {
    error = "cannot create a udev context";
    return false;
  }
  std::vector<RemapDevice> devices;
  if (!discover(keymap, devices, error)) {
    return false;
  }
  if (devices.empty()) {
    error = "no keyboard in [devices] is connected";
    return false;
  }
  if (keymap.devices.size() > 1) {
    error = "several keyboards are configured, but the evdev layer publishes "
            "one virtual device; keep a single keyboard in [devices]";
    return false;
  }
  if (keymap.devices.size() == 1 &&
      keymap.devices.find(devices.front().id) == keymap.devices.end()) {
    error = "the connected keyboard " + devices.front().id.str() +
            " is not the one named in [devices]";
    return false;
  }
  // Opening the device is what decides whether this can work, so check it before
  // claiming anything. Otherwise the run would grab a keyboard it then cannot
  // read, which is the worst possible time to find out.
  if (const std::string denied = check_access(devices); !denied.empty()) {
    error = denied;
    return false;
  }
  if (!keymap.system_wide) {
    error = "the keymap does not request system-wide remapping; set "
            "system_wide = true in [init] to ask for it";
    return false;
  }
  const int fd = ::open(kUinputPath, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    error = std::string("cannot open ") + kUinputPath + ": " + std::strerror(errno);
    return false;
  }
  uinput_fd_ = fd;
  keymap_ = &keymap;
  reset_profile();
  if (!active_) {
    error = "the keymap has no default profile";
    stop();
    return false;
  }
  if (!claim(devices.front(), keymap, error)) {
    stop();
    return false;
  }
  return true;
}

void EvdevRemapper::publish(const input_event &event) {
  if (uinput_fd_ < 0) return;
  ssize_t written = 0;
  do {
    written = ::write(uinput_fd_, &event, sizeof(event));
  } while (written < 0 && errno == EINTR);
}

int EvdevRemapper::run() {
  if (uinput_fd_ < 0) {
    error_ = "the remapper is not running";
    return -1;
  }
  // reset_profile() already put active_ on the default profile, which is where
  // ProfileState starts too.
  ProfileState profiles(*keymap_);
  // Which modifiers are currently down, tracked from the events themselves so
  // a chord like <C-a> is matched as the key plus Ctrl rather than as a bare
  // `a`.
  uint32_t held_mods = kModNone;
  long processed = 0;
  for (;;) {
    std::vector<pollfd> fds;
    fds.reserve(slots_.size());
    for (const auto &slot : slots_) {
      fds.push_back(pollfd{slot.fd, POLLIN, 0});
    }
    if (fds.empty()) break;
    const int ready = ::poll(fds.data(), fds.size(), -1);
    if (ready < 0) {
      if (errno == EINTR) continue;
      error_ = std::string("poll failed: ") + std::strerror(errno);
      return -1;
    }
    for (std::size_t index = 0; index < fds.size(); ++index) {
      if (!(fds[index].revents & POLLIN)) continue;
      input_event event{};
      const ssize_t count = ::read(slots_[index].fd, &event, sizeof(event));
      if (count == 0) {
        error_ = "device " + slots_[index].info.path + " went away";
        return -1;
      }
      if (count < 0) {
        if (errno == EINTR || errno == EAGAIN) continue;
        error_ = "read from " + slots_[index].info.path + " failed: " +
                 std::strerror(errno);
        return -1;
      }
      ++processed;
      std::string switch_to;
      uint32_t held = held_mods;
      const auto out = map_event(profiles.active(), *keymap_, held, event, switch_to,
                                 &held);
      held_mods = held;
      profiles.note(switch_to, event);
      if (profiles.applied()) {
        active_ = profiles.active();
      }
      for (const auto &translated : out) {
        publish(translated);
      }
    }
  }
  return static_cast<int>(processed);
}

