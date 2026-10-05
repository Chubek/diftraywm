#include "compositor/ControlServer.hpp"
#include <wayland-server-core.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
int connect_to(const std::string &path) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  require(fd >= 0, "socket creation failed");
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size());
  require(connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0,
          "connection failed");
  return fd;
}
void send_line(int fd, const std::string &line) {
  require(send(fd, line.data(), line.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(line.size()),
          "request send failed");
}
std::string reply(wl_event_loop *loop, int fd) {
  std::string response;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    wl_event_loop_dispatch(loop, 0);
    char buffer[4096];
    const ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count > 0) response.append(buffer, count);
    else if (count == 0) return response;
    else require(errno == EAGAIN || errno == EWOULDBLOCK, "reply read failed");
  }
  throw std::runtime_error("reply timed out");
}
}

int main() {
  char directory[] = "/tmp/diftray-control-XXXXXX";
  if (!mkdtemp(directory)) return EXIT_FAILURE;
  wl_display *display = wl_display_create();
  if (!display) return EXIT_FAILURE;
  int result = EXIT_SUCCESS;
  try {
    auto *loop = wl_display_get_event_loop(display);
    const std::string path = std::string(directory) + "/ctl.sock";
    ControlServer server;
    auto dispatch = [](const std::string &line) {
      return std::make_pair(true, line == "large" ? std::string(2 * 1024 * 1024, 'x') : line);
    };
    require(server.start(display, path, dispatch), "server start failed");
    require(server.start(display, server.path(), dispatch), "restart with own path failed");
    {
      ControlServer second;
      require(!second.start(display, path, dispatch), "second server stole live socket");
      require(std::filesystem::exists(path), "failed start removed live socket");
    }
    int normal = connect_to(path);
    send_line(normal, "hello\r\n");
    shutdown(normal, SHUT_WR);
    require(reply(loop, normal) == "+\nhello\n", "half-closed request framing failed");
    close(normal);

    int incomplete = connect_to(path);
    send_line(incomplete, "partial");
    shutdown(incomplete, SHUT_WR);
    require(reply(loop, incomplete) == "-\ndiftraywm: incomplete control request\n",
            "incomplete request was not rejected");
    close(incomplete);

    // A peer that never reads a large reply must not stop other commands.
    int blocked = connect_to(path);
    int receive_size = 1024;
    setsockopt(blocked, SOL_SOCKET, SO_RCVBUF, &receive_size, sizeof(receive_size));
    send_line(blocked, "large\n");
    auto before = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) wl_event_loop_dispatch(loop, 0);
    normal = connect_to(path);
    send_line(normal, "responsive\n");
    require(reply(loop, normal) == "+\nresponsive\n", "blocked client stopped other requests");
    require(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(500),
            "control reply blocked the event loop");
    close(normal);
    close(blocked);
    for (int i = 0; i < 4; ++i) wl_event_loop_dispatch(loop, 0);

    int large = connect_to(path);
    send_line(large, "large\n");
    const auto large_reply = reply(loop, large);
    require(large_reply.size() == 2 * 1024 * 1024 + 3 &&
            large_reply.substr(0, 2) == "+\n" && large_reply.back() == '\n',
            "partial writes truncated the response");
    close(large);

    // Silent clients are reclaimed, even when they never generate an fd event.
    int silent = connect_to(path);
    wl_event_loop_dispatch(loop, 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(6200);
    bool expired = false;
    while (std::chrono::steady_clock::now() < deadline) {
      wl_event_loop_dispatch(loop, 25);
      char byte;
      if (read(silent, &byte, 1) == 0) { expired = true; break; }
    }
    require(expired, "silent connection did not expire");
    close(silent);
    server.stop();
    require(!std::filesystem::exists(path), "owned socket was not cleaned up");

    // A stale socket is recovered; an unrelated file is preserved.
    int stale = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size());
    require(bind(stale, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0,
            "stale socket creation failed");
    close(stale);
    require(server.start(display, path, dispatch), "stale socket was not recovered");
    server.stop();
    { std::ofstream file(path); file << "keep"; }
    require(!server.start(display, path, dispatch), "server replaced an unrelated file");
    require(std::filesystem::is_regular_file(path), "failed server removed unrelated file");
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    result = EXIT_FAILURE;
  }
  wl_display_destroy(display);
  std::filesystem::remove_all(directory);
  return result;
}
