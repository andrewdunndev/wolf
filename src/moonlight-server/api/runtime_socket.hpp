#pragma once

#include <api/runtime_requests.hpp>
#include <events/events.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <sys/types.h>

namespace wolf::api {

using namespace wolf::core;

/**
 * A unix socket that serves one session's container and nothing else.
 *
 * Wolf creates it when it starts the session's runner and binds it into that container alone, so the
 * session a request belongs to follows from which socket it arrived on: nothing in the request says
 * who the caller is, and a container that is not given the socket cannot reach it. It accepts only
 * the two device plug/unplug requests of the fake-uinput shim (api/runtime_requests.hpp).
 *
 * Destroying the object closes the socket and removes its file.
 */
class SessionRuntimeSocket {
public:
  /**
   * @param plug  receives the device the session's own app created, to be mknod'd into its container
   * @param unplug receives the removal of a device that session plugged earlier
   * @param owner_uid/gid the socket is made accessible to this user only (the one the app runs as)
   * @return nullptr (logged) if the socket cannot be created
   */
  static std::unique_ptr<SessionRuntimeSocket> open(const std::string &session_id,
                                                    const std::filesystem::path &socket_path,
                                                    std::function<void(events::PlugDeviceEvent)> plug,
                                                    std::function<void(events::UnplugDeviceEvent)> unplug,
                                                    uid_t owner_uid,
                                                    gid_t owner_gid,
                                                    fake_uinput::RuntimeHandler::Paths sysfs = {});

  /// Removes the wolf-runtime-*.sock files in `dir` that a Wolf which did not shut down cleanly left behind.
  /// Meant for Wolf's start, before any session has one.
  static void remove_stale(const std::filesystem::path &dir);

  ~SessionRuntimeSocket();
  SessionRuntimeSocket(const SessionRuntimeSocket &) = delete;
  SessionRuntimeSocket &operator=(const SessionRuntimeSocket &) = delete;

  const std::filesystem::path &path() const {
    return path_;
  }

  struct Impl;

private:
  SessionRuntimeSocket() = default;
  std::shared_ptr<Impl> impl_;
  std::filesystem::path path_;
};

} // namespace wolf::api
