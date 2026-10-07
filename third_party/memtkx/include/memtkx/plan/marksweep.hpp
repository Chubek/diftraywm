#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "memtkx/MemTKXDSL.hpp"
#include "memtkx/core/error.hpp"
#include "memtkx/core/types.hpp"
#include "memtkx/plan/plan.hpp"
#include "memtkx/space/free_list.hpp"

namespace MemTKX {

class MarkSweepPlan : public PlanBase<MarkSweepPlan> {
 public:
  explicit MarkSweepPlan(std::size_t heap_bytes)
      : storage_(heap_bytes),
        allocator_(as_address(storage_.data()),
                   as_address(storage_.data()) + storage_.size()) {}

  [[nodiscard]] constexpr PlanKind kind_value() const noexcept {
    return PlanKind::MarkSweep;
  }

  [[nodiscard]] constexpr std::string_view name_value() const noexcept {
    return "MarkSweep";
  }

  [[nodiscard]] AllocResult<Address> allocate(std::size_t bytes,
                                              std::size_t alignment) {
    auto result = allocator_.allocate(bytes, alignment);
    if (result.is_ok()) {
      objects_.emplace(result.unwrap(), bytes);
    }
    return result;
  }

  bool mark(Address object) {
    if (!objects_.contains(object)) {
      return false;
    }
    marked_.insert(object);
    return true;
  }

  [[nodiscard]] bool is_marked(Address object) const noexcept {
    return marked_.contains(object);
  }

  [[nodiscard]] std::size_t live_objects() const noexcept {
    return objects_.size();
  }

  [[nodiscard]] std::size_t live_bytes() const noexcept {
    std::size_t total = 0;
    for (const auto& [address, bytes] : objects_) {
      (void)address;
      total += bytes;
    }
    return total;
  }

  [[nodiscard]] GcResult<PlanStats> collect_impl(
      const std::vector<Address>& roots, const TraceFn& trace) {
    marked_.clear();

    std::vector<Address> work = roots;
    while (!work.empty()) {
      const Address object = work.back();
      work.pop_back();
      if (!objects_.contains(object) || marked_.contains(object)) {
        continue;
      }
      marked_.insert(object);
      if (trace) {
        const auto children = trace(object);
        work.insert(work.end(), children.begin(), children.end());
      }
    }

    PlanStats stats;
    stats.live_objects = marked_.size();

    std::vector<Address> dead;
    dead.reserve(objects_.size());
    for (const auto& [address, bytes] : objects_) {
      if (marked_.contains(address)) {
        stats.live_bytes += bytes;
      } else {
        dead.push_back(address);
        stats.freed_objects += 1;
        stats.freed_bytes += bytes;
      }
    }

    for (const Address address : dead) {
      const std::size_t bytes = objects_[address];
      allocator_.free(address, bytes);
      objects_.erase(address);
    }

    marked_.clear();
    ++collection_count_;
    stats.collection_count = collection_count_;
    return GcResult<PlanStats>::from_ok(stats);
  }

 private:
  std::vector<std::byte> storage_;
  FreeListAllocator allocator_;
  std::unordered_map<Address, std::size_t> objects_;
  std::unordered_set<Address> marked_;
  std::size_t collection_count_{0};
};

}  // namespace MemTKX
