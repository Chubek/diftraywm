#pragma once

#include <cstdint>
#include <vector>

#include "memtkx/core/types.hpp"

namespace MemTKX {

class SatbQueue {
 public:
  void push(Address object) { entries_.push_back(object); }

  [[nodiscard]] std::vector<Address> take_entries() {
    std::vector<Address> result;
    result.swap(entries_);
    return result;
  }

  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

  void clear() noexcept { entries_.clear(); }

 private:
  std::vector<Address> entries_;
};

class SatbBarrier {
 public:
  explicit SatbBarrier(SatbQueue& queue) : queue_(queue) {}

  Address capture(Address* slot_address) {
    if (slot_address == nullptr) {
      return 0;
    }
    const Address old_value = *slot_address;
    if (old_value != 0) {
      queue_.push(old_value);
    }
    return old_value;
  }

  void write(Address* slot_address, Address new_value) {
    if (slot_address == nullptr) {
      return;
    }
    capture(slot_address);
    *slot_address = new_value;
  }

 private:
  SatbQueue& queue_;
};

}  // namespace MemTKX
