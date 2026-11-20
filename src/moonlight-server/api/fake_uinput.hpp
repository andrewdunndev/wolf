#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <events/events.hpp>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * Helpers for the fake-uinput runtime device injection (see api/runtime_socket.cpp
 * and src/fake-uinput). Turns a uinput device that an app created inside a container -- known to Wolf
 * only by its sysfs name -- into the PlugDeviceEvent that the docker runner consumes to mknod the
 * node and broadcast the udev event.
 *
 * Extracted into a header so the pure parsing/classification logic can be unit-tested
 * (tests/testFakeUinput.cpp).
 */
namespace wolf::api::fake_uinput {

using namespace wolf::core; // for events::

// Read an evdev capabilities bitmask sysfs file (space-separated 64-bit hex words, high group
// first). Returns the words little-end first, i.e. index 0 holds bits 0..63.
inline std::vector<uint64_t> read_caps(const std::filesystem::path &p) {
  std::ifstream f(p);
  std::vector<uint64_t> words;
  std::string tok;
  while (f >> tok)
    words.push_back(std::strtoull(tok.c_str(), nullptr, 16));
  std::reverse(words.begin(), words.end());
  return words;
}

inline bool has_bit(const std::vector<uint64_t> &words, unsigned bit) {
  return (bit / 64) < words.size() && ((words[bit / 64] >> (bit % 64)) & 1ULL);
}

// Classify a device from its sysfs capabilities: the /dev node doesn't exist yet when we run, so we
// can't use EVIOCGBIT. This is a small subset of udev's input_id builtin, enough to set the
// ID_INPUT_* property SDL/Steam look for. Returns nullptr for a plain, unclassified input device.
inline const char *classify(const std::filesystem::path &input_dir, const std::string &node_name) {
  constexpr unsigned BTN_JOYSTICK = 0x120, BTN_GAMEPAD = 0x130, KEY_A = 30, KEY_Z = 44, REL_X = 0x00, BTN_LEFT = 0x110;
  if (node_name.rfind("js", 0) == 0)
    return "ID_INPUT_JOYSTICK"; // js* is joydev -> a joystick by definition
  auto key = read_caps(input_dir / "capabilities" / "key");
  if (has_bit(key, BTN_GAMEPAD) || has_bit(key, BTN_JOYSTICK))
    return "ID_INPUT_JOYSTICK";
  if (has_bit(key, KEY_A) && has_bit(key, KEY_Z))
    return "ID_INPUT_KEYBOARD";
  if (has_bit(read_caps(input_dir / "capabilities" / "rel"), REL_X) && has_bit(key, BTN_LEFT))
    return "ID_INPUT_MOUSE";
  return nullptr;
}

// A sysfs input name as the kernel assigns it to a uinput device: "input" and a number, nothing else.
// The name arrives from a container, so it is matched strictly before it touches a path.
inline bool is_valid_sysfs_name(std::string_view name) {
  constexpr std::string_view prefix = "input";
  if (name.size() <= prefix.size() || name.size() > prefix.size() + 9 || name.substr(0, prefix.size()) != prefix)
    return false;
  return std::all_of(name.begin() + prefix.size(), name.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// What the shim stamps into phys (see src/fake-uinput). Devices without it were not created through the
// shim; Wolf refuses to plug them. A hostile app that holds /dev/uinput can still forge it, so this is
// a guard against mistakes and against naming someone else's device, not a security boundary.
inline constexpr std::string_view SHIM_PHYS_PREFIX = "wolf-fake-uinput/";

// Resolve a client-supplied sysfs name to the device directory, or nullopt. Only a direct child of
// `virtual_root` qualifies, reached through `class_input` (/sys/class/input): the symlink is followed
// and its target must still be a virtual input device, so a name can neither climb out of the tree nor
// select real hardware, and the device must carry the shim's phys marker.
inline std::optional<std::filesystem::path> resolve_input_dir(const std::filesystem::path &class_input,
                                                              const std::filesystem::path &virtual_root,
                                                              std::string_view name) {
  if (!is_valid_sysfs_name(name))
    return std::nullopt;
  std::error_code ec;
  const auto root = std::filesystem::canonical(virtual_root, ec);
  if (ec)
    return std::nullopt;
  const auto dir = std::filesystem::canonical(class_input / std::string(name), ec);
  if (ec || dir.parent_path() != root)
    return std::nullopt;
  std::ifstream phys(dir / "phys");
  std::string marker;
  if (!std::getline(phys, marker) || marker.compare(0, SHIM_PHYS_PREFIX.size(), SHIM_PHYS_PREFIX) != 0)
    return std::nullopt;
  return dir;
}

// Build a PlugDeviceEvent for every event*/js* node of the device directory `input_dir` (from
// resolve_input_dir). The field set mirrors gen_udev_base_event (uinput.hpp);
// we can't reuse it directly because it stat()s the /dev node for major:minor and that node doesn't
// exist yet, so we read major:minor from sysfs instead. Returns nullopt if nothing is found.
inline std::optional<events::PlugDeviceEvent> build_plug_event(const std::string &session_id,
                                                               const std::filesystem::path &input_dir) {
  std::error_code ec;
  const auto now = std::chrono::system_clock::now();
  const auto usec = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());

  events::PlugDeviceEvent ev{.session_id = session_id};
  for (const auto &child : std::filesystem::directory_iterator(input_dir, ec)) {
    if (!child.is_directory())
      continue;
    const auto name = child.path().filename().string();
    if (name.rfind("event", 0) != 0 && name.rfind("js", 0) != 0)
      continue;
    std::ifstream df(child.path() / "dev");
    unsigned int major = 0, minor = 0;
    char sep = ':';
    df >> major >> sep >> minor;
    if (!major)
      continue;
    const std::string devnode = "/dev/input/" + name;
    std::string devpath = child.path().string();
    if (devpath.rfind("/sys", 0) == 0)
      devpath.erase(0, 4); // udev DEVPATH is the sysfs path without the /sys prefix
    const char *cls = classify(input_dir, name);

    std::map<std::string, std::string> uev = {{"ACTION", "add"},
                                              {"SEQNUM", "7"},
                                              {"USEC_INITIALIZED", usec},
                                              {"SUBSYSTEM", "input"},
                                              {"ID_INPUT", "1"},
                                              {"ID_SERIAL", "noserial"},
                                              {"TAGS", ":seat:uaccess:"},
                                              {"CURRENT_TAGS", ":seat:uaccess:"},
                                              {"DEVNAME", devnode},
                                              {"DEVPATH", devpath},
                                              {"MAJOR", std::to_string(major)},
                                              {"MINOR", std::to_string(minor)}};
    if (cls)
      uev[cls] = "1";
    ev.udev_events.push_back(uev);

    std::vector<std::string> hw = {"E:ID_INPUT=1"};
    if (cls)
      hw.push_back(std::string("E:") + cls + "=1");
    hw.push_back("G:seat");
    hw.push_back("G:uaccess");
    ev.udev_hw_db_entries.push_back({"c" + std::to_string(major) + ":" + std::to_string(minor), hw});
  }
  if (ev.udev_events.empty())
    return std::nullopt;
  return ev;
}

} // namespace wolf::api::fake_uinput
