#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

#include "memtkx/core/types.hpp"

namespace MemTKX {

enum class WorkKind : std::uint8_t {
  RootScan = 0,
  ProcessEdge = 1,
  Mark = 2,
  Sweep = 3,
  Release = 4,
  User = 5,
};

struct WorkPacket {
  WorkKind kind{WorkKind::User};
  Address object{0};
  std::size_t extra{0};
};

class WorkQueue {
 public:
  void push(WorkPacket packet) { packets_.push_back(packet); }

  [[nodiscard]] bool try_pop(WorkPacket& packet) {
    if (packets_.empty()) {
      return false;
    }
    packet = packets_.front();
    packets_.pop_front();
    return true;
  }

  [[nodiscard]] bool empty() const noexcept { return packets_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return packets_.size(); }

  void clear() noexcept { packets_.clear(); }

  template <typename Fn>
  std::size_t drain(Fn&& fn) {
    std::size_t processed = 0;
    WorkPacket packet;
    while (try_pop(packet)) {
      fn(packet);
      ++processed;
    }
    return processed;
  }

 private:
  std::deque<WorkPacket> packets_;
};

}  // namespace MemTKX
