#include <api/fake_uinput.hpp>
#include <api/runtime_requests.hpp>
#include <api/runtime_socket.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace wolf::api::fake_uinput;
namespace fs = std::filesystem;

// Build a throwaway sysfs-style input dir with capabilities/{key,rel} files.
static fs::path make_input_dir(const std::string &key_caps, const std::string &rel_caps = "") {
  static int n = 0;
  auto dir = fs::temp_directory_path() / ("fu-test-" + std::to_string(::getpid()) + "-" + std::to_string(n++));
  fs::create_directories(dir / "capabilities");
  std::ofstream(dir / "capabilities" / "key") << key_caps;
  if (!rel_caps.empty())
    std::ofstream(dir / "capabilities" / "rel") << rel_caps;
  return dir;
}

TEST_CASE("fake-uinput read_caps / has_bit parse hex bitmasks", "[fake-uinput]") {
  // Space-separated 64-bit hex words, high group first. "1<<48" in the 5th word == bit 0x130.
  auto dir = make_input_dir("1000000000000 0 0 0 0");
  auto caps = read_caps(dir / "capabilities" / "key");
  REQUIRE(has_bit(caps, 0x130)); // BTN_GAMEPAD
  REQUIRE_FALSE(has_bit(caps, 0x131));
  REQUIRE_FALSE(has_bit(caps, 9999)); // out of range must not read OOB
  fs::remove_all(dir);
}

TEST_CASE("fake-uinput classify from sysfs capabilities", "[fake-uinput]") {
  SECTION("js* nodes are always joysticks (joydev, no evdev caps)") {
    auto dir = make_input_dir("0");
    const char *c = classify(dir, "js0");
    REQUIRE(c != nullptr);
    REQUIRE(std::string(c) == "ID_INPUT_JOYSTICK");
    fs::remove_all(dir);
  }
  SECTION("BTN_GAMEPAD -> joystick") {
    auto dir = make_input_dir("1000000000000 0 0 0 0");
    const char *c = classify(dir, "event5");
    REQUIRE(c != nullptr);
    REQUIRE(std::string(c) == "ID_INPUT_JOYSTICK");
    fs::remove_all(dir);
  }
  SECTION("KEY_A + KEY_Z -> keyboard") {
    auto dir = make_input_dir("100040000000"); // bits 30 (KEY_A) and 44 (KEY_Z)
    const char *c = classify(dir, "event6");
    REQUIRE(c != nullptr);
    REQUIRE(std::string(c) == "ID_INPUT_KEYBOARD");
    fs::remove_all(dir);
  }
  SECTION("no recognized capabilities -> unclassified") {
    auto dir = make_input_dir("0");
    REQUIRE(classify(dir, "event7") == nullptr);
    fs::remove_all(dir);
  }
}

// ---------------------------------------------------------------------------------------------
// Runtime socket validation. A request comes from an app in a container, so every check here is one
// that stops a hostile or buggy client.
// ---------------------------------------------------------------------------------------------

// A sysfs lookalike: /sys/class/input/<name> symlinks into /sys/devices/..., as on a real host.
struct FakeSysfs {
  fs::path root;
  FakeSysfs() {
    static int n = 0;
    root = fs::temp_directory_path() / ("fu-sys-" + std::to_string(::getpid()) + "-" + std::to_string(n++));
    fs::create_directories(root / "class/input");
    fs::create_directories(root / "devices/virtual/input");
    fs::create_directories(root / "devices/pci0");
  }
  ~FakeSysfs() {
    fs::remove_all(root);
  }
  // A uinput pad: one evdev node with BTN_GAMEPAD, created through the shim unless phys says otherwise.
  void virtual_device(const std::string &name, const std::string &phys = "wolf-fake-uinput/0", int minor = 70) {
    auto dir = root / "devices/virtual/input" / name;
    fs::create_directories(dir / "capabilities");
    fs::create_directories(dir / ("event" + std::to_string(minor)));
    std::ofstream(dir / "phys") << phys << "\n";
    std::ofstream(dir / "capabilities/key") << "1000000000000 0 0 0 0";
    std::ofstream(dir / ("event" + std::to_string(minor)) / "dev") << "13:" << minor << "\n";
    fs::create_directory_symlink("../../devices/virtual/input/" + name, root / "class/input" / name);
  }
  // Real hardware that even carries the marker: only its place in sysfs should keep it out.
  void hardware_device(const std::string &name) {
    auto dir = root / "devices/pci0" / name;
    fs::create_directories(dir / "event3");
    std::ofstream(dir / "phys") << "wolf-fake-uinput/0\n";
    std::ofstream(dir / "event3/dev") << "13:67\n";
    fs::create_directory_symlink("../../devices/pci0/" + name, root / "class/input" / name);
  }
  RuntimeHandler::Paths paths() const {
    return {.class_input = root / "class/input", .virtual_root = root / "devices/virtual/input"};
  }
};

static RequestHead post(std::string_view path) {
  return {.method = "POST", .path = std::string(path), .content_length = 0};
}

static std::string body_for(const std::string &sysfs_name, const std::string &extra = "") {
  return "{" + extra + "\"sysfs_name\":\"" + sysfs_name + "\"}";
}

TEST_CASE("fake-uinput accepts only input<N> sysfs names", "[fake-uinput]") {
  REQUIRE(is_valid_sysfs_name("input0"));
  REQUIRE(is_valid_sysfs_name("input82"));
  for (std::string bad : {"",
                          "input",
                          "..",
                          "../input5",
                          "input5/..",
                          "input5/",
                          "/input5",
                          "input5/../input6",
                          "input5\n",
                          "input 5",
                          " input5",
                          "INPUT5",
                          "inputx",
                          "input-1",
                          "event5",
                          "js0",
                          "input1234567890"}) {
    INFO("name: " << bad);
    REQUIRE_FALSE(is_valid_sysfs_name(bad));
  }
  REQUIRE_FALSE(is_valid_sysfs_name(std::string("input5\0", 7)));
}

TEST_CASE("fake-uinput resolves only shim-made virtual devices", "[fake-uinput]") {
  FakeSysfs sys;
  sys.virtual_device("input5");
  sys.virtual_device("input7", "something else");
  sys.hardware_device("input6");
  auto p = sys.paths();

  SECTION("a good name resolves to the device") {
    auto dir = resolve_input_dir(p.class_input, p.virtual_root, "input5");
    REQUIRE(dir);
    REQUIRE(*dir == fs::canonical(sys.root / "devices/virtual/input/input5"));
  }
  SECTION("a path-shaped name never reaches the filesystem") {
    // The target exists: only the name check keeps these out.
    REQUIRE_FALSE(resolve_input_dir(p.class_input, p.virtual_root, "../../devices/virtual/input/input5"));
    REQUIRE_FALSE(resolve_input_dir(p.class_input, p.virtual_root, "input5/../input5"));
    REQUIRE_FALSE(resolve_input_dir(p.class_input, p.virtual_root, ".."));
  }
  SECTION("a device outside the virtual tree is refused even when it carries the marker") {
    REQUIRE_FALSE(resolve_input_dir(p.class_input, p.virtual_root, "input6"));
  }
  SECTION("a device the shim did not create is refused") {
    REQUIRE_FALSE(resolve_input_dir(p.class_input, p.virtual_root, "input7"));
  }
  SECTION("a device that does not exist is refused") {
    REQUIRE_FALSE(resolve_input_dir(p.class_input, p.virtual_root, "input99"));
  }
}

TEST_CASE("fake-uinput plugs a good device into the socket's session", "[fake-uinput]") {
  FakeSysfs sys;
  sys.virtual_device("input5");
  RuntimeHandler h("100", std::make_shared<DeviceClaims>(), sys.paths());

  auto out = h.handle(post(PLUG_PATH), body_for("input5"));
  REQUIRE(out.status == 200);
  REQUIRE(out.plug);
  REQUIRE(out.plug->session_id == "100");
  REQUIRE(out.plug->udev_events.size() == 1);
  REQUIRE(out.plug->udev_events[0].at("DEVNAME") == "/dev/input/event70");
  REQUIRE(out.plug->udev_events[0].at("ID_INPUT_JOYSTICK") == "1");
}

TEST_CASE("fake-uinput refuses a traversal name and plugs nothing", "[fake-uinput]") {
  FakeSysfs sys;
  sys.virtual_device("input5");
  RuntimeHandler h("100", std::make_shared<DeviceClaims>(), sys.paths());

  for (std::string bad : {"../../devices/virtual/input/input5", "input5/..", "..", ""}) {
    INFO("name: " << bad);
    auto out = h.handle(post(PLUG_PATH), body_for(bad));
    REQUIRE(out.status == 400);
    REQUIRE_FALSE(out.plug);
  }
}

TEST_CASE("fake-uinput takes the session from the socket, not the body", "[fake-uinput]") {
  FakeSysfs sys;
  sys.virtual_device("input5");
  auto claims = std::make_shared<DeviceClaims>();
  RuntimeHandler mine("100", claims, sys.paths());
  RuntimeHandler theirs("200", claims, sys.paths());

  SECTION("a session_id in the body is ignored") {
    auto out = mine.handle(post(PLUG_PATH), body_for("input5", R"("session_id":"200",)"));
    REQUIRE(out.status == 200);
    REQUIRE(out.plug);
    REQUIRE(out.plug->session_id == "100");
  }
  SECTION("a device held by another session cannot be plugged") {
    REQUIRE(mine.handle(post(PLUG_PATH), body_for("input5")).status == 200);
    auto out = theirs.handle(post(PLUG_PATH), body_for("input5"));
    REQUIRE(out.status == 409);
    REQUIRE_FALSE(out.plug);
  }
  SECTION("a device held by another session cannot be unplugged") {
    REQUIRE(mine.handle(post(PLUG_PATH), body_for("input5")).status == 200);
    auto out = theirs.handle(post(UNPLUG_PATH), body_for("input5"));
    REQUIRE_FALSE(out.unplug);
    // ...and it is still held, so it can be unplugged by its owner and then taken by the other.
    REQUIRE(theirs.handle(post(PLUG_PATH), body_for("input5")).status == 409);
    auto owner_out = mine.handle(post(UNPLUG_PATH), body_for("input5"));
    REQUIRE(owner_out.unplug);
    REQUIRE(owner_out.unplug->session_id == "100");
    REQUIRE(theirs.handle(post(PLUG_PATH), body_for("input5")).status == 200);
  }
  SECTION("what a closed socket held is free again") {
    REQUIRE(mine.handle(post(PLUG_PATH), body_for("input5")).status == 200);
    mine.release_all();
    REQUIRE(theirs.handle(post(PLUG_PATH), body_for("input5")).status == 200);
  }
}

TEST_CASE("fake-uinput bounds the request size", "[fake-uinput]") {
  RequestHead head;
  auto request = [](const std::string &extra) {
    return std::string("POST ") + std::string(PLUG_PATH) + " HTTP/1.1\r\nHost: localhost\r\n" + extra;
  };

  SECTION("a normal shim request is accepted") {
    REQUIRE(parse_head(request("Content-Length: 31"), head) == 0);
    REQUIRE(head.content_length == 31);
  }
  SECTION("a declared body over the limit is refused before it is read") {
    REQUIRE(parse_head(request("Content-Length: " + std::to_string(MAX_BODY_BYTES + 1)), head) == 413);
    REQUIRE(parse_head(request("Content-Length: 99999999999999999999"), head) == 400);
  }
  SECTION("a head over the limit is refused") {
    REQUIRE(parse_head(request("X-Pad: " + std::string(MAX_HEAD_BYTES, 'a') + "\r\nContent-Length: 1"), head) == 431);
  }
  SECTION("a body over the limit is refused by the handler too") {
    FakeSysfs sys;
    sys.virtual_device("input5");
    RuntimeHandler h("100", std::make_shared<DeviceClaims>(), sys.paths());
    auto out = h.handle(post(PLUG_PATH), body_for("input5", "\"pad\":\"" + std::string(MAX_BODY_BYTES, 'a') + "\","));
    REQUIRE(out.status == 413);
    REQUIRE_FALSE(out.plug);
  }
  SECTION("only the two shim requests are served") {
    REQUIRE(parse_head("POST /api/v1/events HTTP/1.1\r\nContent-Length: 1", head) == 404);
    REQUIRE(parse_head("GET " + std::string(PLUG_PATH) + " HTTP/1.1\r\n", head) == 405);
    REQUIRE(parse_head(request("Transfer-Encoding: chunked"), head) == 501);
    REQUIRE(parse_head(request(""), head) == 411);
  }
}

// ---- the transport: SessionRuntimeSocket over a real unix socket in a temp directory ----

namespace {
// A raw client: the point is to misbehave in ways the shim never does.
int connect_to(const fs::path &path, int recv_timeout_ms = 7000) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  timeval tv{recv_timeout_ms / 1000, (recv_timeout_ms % 1000) * 1000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
  return fd;
}

void send_all(int fd, const std::string &data) {
  REQUIRE(::send(fd, data.data(), data.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(data.size()));
}

/// Reads until the peer closes or the receive timeout passes; `closed` says which.
std::string read_reply(int fd, bool &closed) {
  std::string out;
  char buf[512];
  for (;;) {
    auto n = ::recv(fd, buf, sizeof(buf), 0);
    if (n > 0) {
      out.append(buf, static_cast<std::size_t>(n));
      continue;
    }
    closed = n == 0;
    return out;
  }
}

std::string status_line(const std::string &reply) {
  return reply.substr(0, reply.find("\r\n"));
}

std::string plug_request(const std::string &name) {
  auto body = body_for(name);
  return std::string("POST ") + std::string(PLUG_PATH) + " HTTP/1.1\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\n\r\n" + body;
}

struct RuntimeSocketFixture {
  fs::path path;
  FakeSysfs sys;
  std::atomic<int> plugged{0};
  std::unique_ptr<wolf::api::SessionRuntimeSocket> sock;

  explicit RuntimeSocketFixture(std::function<void(wolf::core::events::PlugDeviceEvent)> plug = nullptr) {
    static int n = 0;
    path = fs::temp_directory_path() / ("fu-rt-" + std::to_string(::getpid()) + "-" + std::to_string(n++) + ".sock");
    sys.virtual_device("input5");
    sock = wolf::api::SessionRuntimeSocket::open(
        "rt-" + std::to_string(n),
        path,
        [this, plug](wolf::core::events::PlugDeviceEvent ev) {
          ++plugged;
          if (plug)
            plug(std::move(ev));
        },
        [](wolf::core::events::UnplugDeviceEvent) {},
        ::getuid(),
        ::getgid(),
        sys.paths());
    REQUIRE(sock);
  }
  ~RuntimeSocketFixture() {
    sock.reset();
    fs::remove(path);
  }
};
} // namespace

TEST_CASE("fake-uinput runtime socket: a valid request is served and the socket is private", "[fake-uinput][socket]") {
  RuntimeSocketFixture f;
  REQUIRE((fs::status(f.path).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
  int fd = connect_to(f.path);
  send_all(fd, plug_request("input5"));
  bool closed = false;
  REQUIRE(status_line(read_reply(fd, closed)) == "HTTP/1.0 200 X");
  REQUIRE(f.plugged == 1);
  ::close(fd);
}

TEST_CASE("fake-uinput runtime socket: an oversize head gets 431", "[fake-uinput][socket]") {
  RuntimeSocketFixture f;
  int fd = connect_to(f.path);
  send_all(fd, "POST / HTTP/1.1\r\nX-Pad: " + std::string(MAX_HEAD_BYTES + MAX_BODY_BYTES, 'a'));
  bool closed = false;
  REQUIRE(status_line(read_reply(fd, closed)) == "HTTP/1.0 431 X");
  REQUIRE(f.plugged == 0);
  ::close(fd);
}

TEST_CASE("fake-uinput runtime socket: a client that stalls is cut off after the deadline", "[fake-uinput][socket]") {
  RuntimeSocketFixture f;
  int fd = connect_to(f.path, 8000);
  send_all(fd, "POST /api/v1/runtime/plug-udev-device HTTP/1.1\r\nContent-Len"); // never finishes the head
  auto start = std::chrono::steady_clock::now();
  bool closed = false;
  read_reply(fd, closed);
  auto took = std::chrono::steady_clock::now() - start;
  REQUIRE(closed);
  REQUIRE(took > std::chrono::seconds(4));
  REQUIRE(took < std::chrono::seconds(7));
  ::close(fd);
}

TEST_CASE("fake-uinput runtime socket: connections past the cap are dropped", "[fake-uinput][socket]") {
  RuntimeSocketFixture f;
  std::vector<int> fds;
  for (int i = 0; i < 9; ++i)
    fds.push_back(connect_to(f.path, 500));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  int dropped = 0;
  for (int fd : fds) {
    bool closed = false;
    read_reply(fd, closed); // idle connections time out here; a dropped one is closed already
    dropped += closed ? 1 : 0;
  }
  REQUIRE(dropped == 1);
  // and the socket still serves once they are gone
  for (int fd : fds)
    ::close(fd);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  int fd = connect_to(f.path);
  send_all(fd, plug_request("input5"));
  bool closed = false;
  REQUIRE(status_line(read_reply(fd, closed)) == "HTTP/1.0 200 X");
  ::close(fd);
}

TEST_CASE("fake-uinput runtime socket: a request in flight when the session closes is refused",
          "[fake-uinput][socket]") {
  RuntimeSocketFixture f;
  auto request = plug_request("input5");
  int fd = connect_to(f.path);
  send_all(fd, request.substr(0, request.size() - 5)); // head and most of the body
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  f.sock.reset(); // the session ends
  send_all(fd, request.substr(request.size() - 5));
  bool closed = false;
  REQUIRE(status_line(read_reply(fd, closed)) == "HTTP/1.0 503 X");
  REQUIRE(f.plugged == 0);
  ::close(fd);
}

TEST_CASE("fake-uinput runtime socket: a handler that throws does not take the process down", "[fake-uinput][socket]") {
  RuntimeSocketFixture f([](wolf::core::events::PlugDeviceEvent) { throw std::runtime_error("subscriber failed"); });
  int fd = connect_to(f.path, 1000);
  send_all(fd, plug_request("input5")); // the plug callback throws on the serving thread
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  REQUIRE(f.plugged == 1);
  // still alive and serving
  int fd2 = connect_to(f.path);
  send_all(fd2, "POST /nothing HTTP/1.1\r\nContent-Length: 1\r\n\r\nx");
  bool closed = false;
  REQUIRE(status_line(read_reply(fd2, closed)) == "HTTP/1.0 404 X");
  ::close(fd2);
  ::close(fd);
}
