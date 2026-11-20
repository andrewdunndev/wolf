#include <api/runtime_requests.hpp>
#include <api/runtime_socket.hpp>
#include <boost/asio.hpp>
#include <chrono>
#include <future>
#include <helpers/logger.hpp>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace wolf::api {

using boost::asio::local::stream_protocol;
using namespace std::chrono_literals;

// A shim request is answered in a few milliseconds; a client that is slower than this, or that holds
// connections open, is not one.
constexpr auto CONNECTION_DEADLINE = 5s;
constexpr int MAX_CONNECTIONS = 8;

namespace {
/**
 * All runtime sockets are served from one thread of their own: requests are tiny and rare, and this
 * keeps them off the control API's loop. Never destroyed on purpose, so process exit does not wait
 * on it.
 */
boost::asio::io_context &runtime_io() {
  static auto *io = [] {
    auto *ctx = new boost::asio::io_context();
    std::thread([ctx]() {
      auto guard = boost::asio::make_work_guard(*ctx);
      // A handler that throws must not take Wolf (and every session) down with it: log it and serve on.
      for (;;) {
        try {
          ctx->run();
          break;
        } catch (const std::exception &e) {
          logs::log(logs::error, "[FAKE-UINPUT] runtime socket handler threw: {}", e.what());
        } catch (...) {
          logs::log(logs::error, "[FAKE-UINPUT] runtime socket handler threw");
        }
      }
    }).detach();
    return ctx;
  }();
  return *io;
}

/// Shared by every session's socket, so one session cannot plug a device another one holds.
std::shared_ptr<fake_uinput::DeviceClaims> claims() {
  static auto c = std::make_shared<fake_uinput::DeviceClaims>();
  return c;
}
} // namespace

struct SessionRuntimeSocket::Impl : std::enable_shared_from_this<Impl> {
  Impl(const std::string &session_id,
       stream_protocol::endpoint endpoint,
       std::function<void(events::PlugDeviceEvent)> plug,
       std::function<void(events::UnplugDeviceEvent)> unplug,
       fake_uinput::RuntimeHandler::Paths sysfs)
      : acceptor(runtime_io(), endpoint), retry(runtime_io()), handler(session_id, claims(), std::move(sysfs)),
        session_id(session_id), plug(std::move(plug)), unplug(std::move(unplug)) {}

  struct Connection {
    explicit Connection(boost::asio::io_context &io, std::shared_ptr<Impl> owner)
        : socket(io), deadline(io), owner(std::move(owner)) {
      ++this->owner->connections;
    }
    ~Connection() {
      --owner->connections;
    }
    stream_protocol::socket socket;
    boost::asio::steady_timer deadline;
    boost::asio::streambuf buffer{fake_uinput::MAX_HEAD_BYTES + fake_uinput::MAX_BODY_BYTES};
    fake_uinput::RequestHead head;
    std::string reply;
    std::shared_ptr<Impl> owner;
  };

  void accept() {
    auto conn = std::make_shared<Connection>(runtime_io(), shared_from_this());
    acceptor.async_accept(conn->socket, [self = shared_from_this(), conn](const boost::system::error_code &ec) {
      if (ec == boost::asio::error::operation_aborted || self->closed)
        return;
      if (ec) {
        logs::log(logs::warning, "[FAKE-UINPUT] session {}: accept failed: {}", self->session_id, ec.message());
        self->retry.expires_after(100ms);
        self->retry.async_wait([self](const boost::system::error_code &e) {
          if (!e && !self->closed)
            self->accept();
        });
        return;
      }
      {
        if (self->connections > MAX_CONNECTIONS) {
          logs::log(logs::warning,
                    "[FAKE-UINPUT] session {}: too many open connections, dropping one",
                    self->session_id);
          boost::system::error_code ignored;
          conn->socket.close(ignored);
        } else {
          self->serve(conn);
        }
      }
      self->accept();
    });
  }

  void serve(std::shared_ptr<Connection> conn) {
    conn->deadline.expires_after(CONNECTION_DEADLINE);
    conn->deadline.async_wait([conn](const boost::system::error_code &ec) {
      if (!ec) {
        boost::system::error_code ignored;
        conn->socket.close(ignored);
      }
    });
    boost::asio::async_read_until(
        conn->socket,
        conn->buffer,
        "\r\n\r\n",
        [self = shared_from_this(), conn](const boost::system::error_code &ec, std::size_t head_bytes) {
          if (ec) { // includes a head larger than the buffer allows
            self->respond(conn, ec == boost::asio::error::not_found ? 431 : 400, R"({"error":"bad request"})");
            return;
          }
          std::string head(boost::asio::buffers_begin(conn->buffer.data()),
                           boost::asio::buffers_begin(conn->buffer.data()) + head_bytes - 4);
          if (int status = fake_uinput::parse_head(head, conn->head)) {
            self->respond(conn, status, R"({"error":"rejected"})");
            return;
          }
          conn->buffer.consume(head_bytes);
          auto have = conn->buffer.size();
          if (have >= conn->head.content_length) {
            self->handle(conn);
          } else {
            boost::asio::async_read(conn->socket,
                                    conn->buffer,
                                    boost::asio::transfer_exactly(conn->head.content_length - have),
                                    [self, conn](const boost::system::error_code &ec, std::size_t) {
                                      if (ec)
                                        self->respond(conn, 400, R"({"error":"bad request"})");
                                      else
                                        self->handle(conn);
                                    });
          }
        });
  }

  void handle(std::shared_ptr<Connection> conn) {
    // The session was closed while this request was in flight: it must not claim or plug anything now
    if (closed) {
      respond(conn, 503, R"({"error":"session closed"})");
      return;
    }
    std::string body(boost::asio::buffers_begin(conn->buffer.data()),
                     boost::asio::buffers_begin(conn->buffer.data()) + conn->head.content_length);
    auto out = handler.handle(conn->head, body);
    if (out.status != 200)
      logs::log(logs::warning,
                "[FAKE-UINPUT] session {}: {} refused with {}: {}",
                session_id,
                conn->head.path,
                out.status,
                out.body);
    if (out.plug) {
      logs::log(logs::info, "[FAKE-UINPUT] session {}: plugging {} node(s)", session_id, out.plug->udev_events.size());
      plug(std::move(*out.plug));
    }
    if (out.unplug) {
      logs::log(logs::info,
                "[FAKE-UINPUT] session {}: unplugging {} node(s)",
                session_id,
                out.unplug->udev_events.size());
      unplug(std::move(*out.unplug));
    }
    respond(conn, out.status, out.body);
  }

  void respond(std::shared_ptr<Connection> conn, int status, const std::string &body) {
    conn->reply = fmt::format("HTTP/1.0 {} X\r\nContent-Type: application/json\r\nContent-Length: {}\r\n\r\n{}",
                              status,
                              body.size(),
                              body);
    boost::asio::async_write(conn->socket, boost::asio::buffer(conn->reply), [conn](const auto &, std::size_t) {
      boost::system::error_code ignored;
      conn->socket.shutdown(stream_protocol::socket::shutdown_both, ignored);
      conn->socket.close(ignored);
      conn->deadline.cancel();
    });
  }

  void close() {
    closed = true;
    boost::system::error_code ignored;
    acceptor.close(ignored);
    handler.release_all();
  }

  stream_protocol::acceptor acceptor;
  boost::asio::steady_timer retry;
  fake_uinput::RuntimeHandler handler;
  std::string session_id;
  std::function<void(events::PlugDeviceEvent)> plug;
  std::function<void(events::UnplugDeviceEvent)> unplug;
  bool closed = false;
  int connections = 0;
};

std::unique_ptr<SessionRuntimeSocket> SessionRuntimeSocket::open(const std::string &session_id,
                                                                 const std::filesystem::path &socket_path,
                                                                 std::function<void(events::PlugDeviceEvent)> plug,
                                                                 std::function<void(events::UnplugDeviceEvent)> unplug,
                                                                 uid_t owner_uid,
                                                                 gid_t owner_gid,
                                                                 fake_uinput::RuntimeHandler::Paths sysfs) {
  std::unique_ptr<SessionRuntimeSocket> sock(new SessionRuntimeSocket());
  sock->path_ = socket_path;
  try {
    ::unlink(socket_path.c_str()); // left over from a crashed Wolf
    // bind() creates the file with the process umask: take everything away from group and other for the
    // moment it exists, so there is no window between the bind and the chown/chmod below.
    const auto old_umask = ::umask(0177);
    try {
      sock->impl_ = std::make_shared<Impl>(session_id,
                                           stream_protocol::endpoint(socket_path.string()),
                                           std::move(plug),
                                           std::move(unplug),
                                           std::move(sysfs));
    } catch (...) {
      ::umask(old_umask);
      throw;
    }
    ::umask(old_umask);
  } catch (const std::exception &e) {
    logs::log(logs::error, "[FAKE-UINPUT] cannot listen on {}: {}", socket_path.string(), e.what());
    return nullptr;
  }

  // Only the user the app runs as may connect. If Wolf cannot give the socket to that user (a user
  // namespace that does not map it, say) fall back to world-writable: a socket only the owner (Wolf's
  // user) can open would be useless to the app. The socket is then reachable by every user inside the
  // one container it is mounted into, and by anything on the host that can see the state folder.
  if (::chown(socket_path.c_str(), owner_uid, owner_gid) == 0 && ::chmod(socket_path.c_str(), 0600) == 0) {
    logs::log(logs::debug, "[FAKE-UINPUT] {} is owned by {}:{}", socket_path.string(), owner_uid, owner_gid);
  } else {
    logs::log(logs::warning,
              "[FAKE-UINPUT] cannot give {} to {}:{} ({}), making it world-writable",
              socket_path.string(),
              owner_uid,
              owner_gid,
              strerror(errno));
    ::chmod(socket_path.c_str(), 0666);
  }

  boost::asio::post(runtime_io(), [impl = sock->impl_]() { impl->accept(); });
  return sock;
}

void SessionRuntimeSocket::remove_stale(const std::filesystem::path &dir) {
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    const auto name = entry.path().filename().string();
    if (name.rfind("wolf-runtime-", 0) == 0 && name.size() > 5 && name.compare(name.size() - 5, 5, ".sock") == 0) {
      std::filesystem::remove(entry.path(), ec);
    }
  }
}

SessionRuntimeSocket::~SessionRuntimeSocket() {
  if (!impl_)
    return;
  // Close on the serving thread so no request is mid-flight when the session's claims are dropped.
  auto done = std::make_shared<std::promise<void>>();
  boost::asio::post(runtime_io(), [impl = impl_, done]() {
    impl->close();
    done->set_value();
  });
  done->get_future().wait_for(2s);
  ::unlink(path_.c_str());
}

} // namespace wolf::api
