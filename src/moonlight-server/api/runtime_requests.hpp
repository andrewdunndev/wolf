#pragma once

#include <algorithm>
#include <api/fake_uinput.hpp>
#include <cctype>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <rfl/json.hpp>
#include <string>
#include <string_view>

/**
 * Request handling for the per-session runtime socket (see api/runtime_socket.hpp). Everything that
 * decides whether a request from a container is acted on lives here, free of sockets, so it can be
 * unit tested (tests/testFakeUinput.cpp):
 *
 *  - the request head is parsed strictly and bounded in size;
 *  - the session is the one the socket was created for, never anything the client sends;
 *  - the sysfs name must be `input<N>` and resolve to a virtual input device (resolve_input_dir);
 *  - a device can only be plugged by one session at a time, and only unplugged by the session that
 *    plugged it.
 */
namespace wolf::api::fake_uinput {

// The shim sends a few hundred bytes. Anything larger is not a shim.
inline constexpr std::size_t MAX_HEAD_BYTES = 4096;
inline constexpr std::size_t MAX_BODY_BYTES = 256;

inline constexpr std::string_view PLUG_PATH = "/api/v1/runtime/plug-udev-device";
inline constexpr std::string_view UNPLUG_PATH = "/api/v1/runtime/unplug-udev-device";

/// The only field read from the body. Unknown fields (an old shim still sends session_id) are ignored.
struct SysfsRequest {
  std::string sysfs_name;
};

struct RequestHead {
  std::string method;
  std::string path;
  std::size_t content_length = 0;
};

inline bool iequals(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

/**
 * Parse the request line and headers (everything up to, not including, the blank line).
 * @return 0 and fills `out`, or the HTTP status to answer with.
 */
inline int parse_head(std::string_view head, RequestHead &out) {
  if (head.size() > MAX_HEAD_BYTES)
    return 431;
  auto next_line = [&head]() {
    auto end = head.find("\r\n");
    auto line = head.substr(0, end);
    head = end == std::string_view::npos ? std::string_view{} : head.substr(end + 2);
    return line;
  };

  auto request_line = next_line();
  auto sp1 = request_line.find(' ');
  auto sp2 = request_line.rfind(' ');
  if (sp1 == std::string_view::npos || sp1 == sp2 || request_line.substr(sp2 + 1).rfind("HTTP/1.", 0) != 0)
    return 400;
  out.method = std::string(request_line.substr(0, sp1));
  out.path = std::string(request_line.substr(sp1 + 1, sp2 - sp1 - 1));

  bool have_length = false;
  while (!head.empty()) {
    auto line = next_line();
    auto colon = line.find(':');
    if (colon == std::string_view::npos)
      return 400;
    auto name = line.substr(0, colon);
    auto value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ')
      value.remove_prefix(1);
    if (iequals(name, "transfer-encoding"))
      return 501;
    if (iequals(name, "content-length")) {
      if (have_length || value.empty() || value.size() > 6 ||
          !std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return 400;
      have_length = true;
      out.content_length = std::stoul(std::string(value));
    }
  }

  if (out.path != PLUG_PATH && out.path != UNPLUG_PATH)
    return 404;
  if (out.method != "POST")
    return 405;
  if (!have_length)
    return 411;
  if (out.content_length > MAX_BODY_BYTES)
    return 413;
  return 0;
}

/**
 * Which session holds which device, across every runtime socket. Lets one session refuse to plug a
 * device another session already holds, so naming someone else's device gets a container nothing.
 */
class DeviceClaims {
public:
  /// True if `sysfs_name` is free or already held by `session_id`.
  bool claim(const std::string &sysfs_name, const std::string &session_id) {
    std::lock_guard lock(mutex_);
    return owners_.try_emplace(sysfs_name, session_id).first->second == session_id;
  }

  void release(const std::string &sysfs_name, const std::string &session_id) {
    std::lock_guard lock(mutex_);
    auto it = owners_.find(sysfs_name);
    if (it != owners_.end() && it->second == session_id)
      owners_.erase(it);
  }

  void release_all(const std::string &session_id) {
    std::lock_guard lock(mutex_);
    for (auto it = owners_.begin(); it != owners_.end();)
      it = it->second == session_id ? owners_.erase(it) : std::next(it);
  }

private:
  std::mutex mutex_;
  std::map<std::string, std::string> owners_;
};

struct Outcome {
  int status = 200;
  std::string body = R"({"success":true})";
  std::optional<events::PlugDeviceEvent> plug = std::nullopt;
  std::optional<events::UnplugDeviceEvent> unplug = std::nullopt;
};

inline Outcome failure(int status, std::string_view message) {
  return {.status = status, .body = std::string(R"({"error":")") + std::string(message) + "\"}"};
}

/**
 * Decides what a request on one session's socket does. `session_id` is fixed when the socket is
 * created: it is the only session this handler will ever act for.
 */
class RuntimeHandler {
public:
  struct Paths {
    std::filesystem::path class_input = "/sys/class/input";
    std::filesystem::path virtual_root = "/sys/devices/virtual/input";
  };

  RuntimeHandler(std::string session_id, std::shared_ptr<DeviceClaims> claims, Paths paths)
      : session_id_(std::move(session_id)), claims_(std::move(claims)), paths_(std::move(paths)) {}

  /// Reads the real sysfs.
  RuntimeHandler(std::string session_id, std::shared_ptr<DeviceClaims> claims)
      : RuntimeHandler(std::move(session_id), std::move(claims), Paths{}) {}

  /// `head` must have passed parse_head; `body` is exactly head.content_length bytes.
  Outcome handle(const RequestHead &head, std::string_view body) {
    if (body.size() > MAX_BODY_BYTES)
      return failure(413, "request too large");
    auto parsed = rfl::json::read<SysfsRequest>(std::string(body));
    if (!parsed)
      return failure(400, "expected {\"sysfs_name\": \"input<N>\"}");
    const auto &name = parsed.value().sysfs_name;
    if (!is_valid_sysfs_name(name))
      return failure(400, "invalid sysfs_name");
    return head.path == PLUG_PATH ? plug(name) : unplug(name);
  }

  /// The socket is going away: forget what this session still holds.
  void release_all() {
    plugged_.clear();
    claims_->release_all(session_id_);
  }

private:
  Outcome plug(const std::string &name) {
    auto dir = resolve_input_dir(paths_.class_input, paths_.virtual_root, name);
    if (!dir)
      return failure(404, "no such device");
    if (!claims_->claim(name, session_id_))
      return failure(409, "device belongs to another session");
    auto ev = build_plug_event(session_id_, *dir);
    if (!ev) {
      claims_->release(name, session_id_);
      return failure(500, "device has no input nodes");
    }
    plugged_.insert_or_assign(name, *ev);
    return {.plug = std::move(ev)};
  }

  Outcome unplug(const std::string &name) {
    auto it = plugged_.find(name);
    if (it == plugged_.end()) // not plugged by this session: nothing to undo
      return {};
    // The device is already gone from sysfs, so replay what was recorded when it was plugged.
    Outcome out{.unplug = events::UnplugDeviceEvent{.session_id = session_id_,
                                                    .udev_events = it->second.udev_events,
                                                    .udev_hw_db_entries = it->second.udev_hw_db_entries}};
    plugged_.erase(it);
    claims_->release(name, session_id_);
    return out;
  }

  std::string session_id_;
  std::shared_ptr<DeviceClaims> claims_;
  Paths paths_;
  std::map<std::string, events::PlugDeviceEvent> plugged_;
};

} // namespace wolf::api::fake_uinput
