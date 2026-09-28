#include "compositor/Compositor.hpp"
#include <wayland-server-core.h>
#include "views/Cell.hpp"
#include "nterm/NTerm.hpp"
#include <algorithm>
#include <csignal>
#include <sys/wait.h>

bool Compositor::install_signals() {
  auto *loop = wl_display_get_event_loop(display_.get());
  signal_sources_[0] = wl_event_loop_add_signal(loop, SIGINT, shutdown_signal, this);
  signal_sources_[1] = wl_event_loop_add_signal(loop, SIGTERM, shutdown_signal, this);
  signal_sources_[2] = wl_event_loop_add_signal(loop, SIGCHLD, child_signal, this);
  return signal_sources_[0] && signal_sources_[1] && signal_sources_[2];
}
int Compositor::shutdown_signal(int, void *userdata) {
  static_cast<Compositor *>(userdata)->stop();
  return 0;
}
int Compositor::child_signal(int, void *userdata) {
  auto *self = static_cast<Compositor *>(userdata);
  for (auto &cell : self->cells_) if (cell->nterm()) cell->nterm()->reap_child();
  // Notelet children are reaped by the worker transport.
  auto &pids = self->launched_pids_;
  pids.erase(std::remove_if(pids.begin(), pids.end(), [](int pid) {
    return waitpid(pid, nullptr, WNOHANG) == pid;
  }), pids.end());
  return 0;
}
