#pragma once
// Control-socket server backing diftrayctl.
//
// The socket carries one newline-terminated command per connection. The
// dispatcher runs the command through the Command Bar and returns whether the
// command was accepted plus the status line. The reply is written before the
// connection closes, so `exit-session` and `restart-session` can report
// success before the event loop unwinds.
//
// Reply framing: a one-character verdict line ('+' accepted, '-' rejected)
// followed by the status line, which may itself span several lines.
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

struct wl_display;
struct wl_event_source;

class ControlServer {
public:
  using Dispatcher = std::function<std::pair<bool, std::string>(const std::string &)>;

  ~ControlServer();
  // Binds and listens on `path`, registering with the display event loop.
  // Returns false with a message in error() when the socket is unavailable;
  // the compositor stays usable without it.
  bool start(wl_display *display, const std::string &path, Dispatcher dispatcher);
  void stop();
  bool active() const { return listen_fd_ >= 0; }
  const std::string &path() const { return path_; }
  const std::string &error() const { return error_; }

private:
  struct Client {
    wl_event_source *source = nullptr;
    std::string buffer;
  };

  static int on_accept(int fd, uint32_t mask, void *data);
  static int on_client(int fd, uint32_t mask, void *data);
  void handle_line(int fd, const std::string &line);
  void close_client(int fd);
  static void write_all(int fd, const std::string &payload);

  wl_display *display_ = nullptr;
  int listen_fd_ = -1;
  wl_event_source *listen_source_ = nullptr;
  Dispatcher dispatcher_;
  std::string path_;
  std::string error_;
  std::unordered_map<int, Client> clients_;
};
