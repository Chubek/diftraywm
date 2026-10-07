#pragma once

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
#include "memtkx/space/immix_space.hpp"

namespace MemTKX {

class ImmixPlan : public PlanBase<ImmixPlan> {
 public:
  explicit ImmixPlan(std::size_t heap_bytes)
      : space_(heap_bytes) {}

  [[nodiscard]] constexpr PlanKind kind_value() const noexcept {
    return PlanKind::Immix;
  }

  [[nodiscard]] constexpr std::string_view name_value() const noexcept {
    return "Immix";
  }

  [[nodiscard]] AllocResult<Address> allocate(std::size_t bytes,
                                              std::size_t alignment) {
    auto result = space_.allocate(bytes, alignment);
    if (result.is_ok()) {
      objects_.emplace(result.unwrap(), bytes);
    }
    return result;
  }

  bool mark(Address object) {
    const auto it = objects_.find(object);
    if (it == objects_.end()) {
      return false;
    }
    marked_.insert(object);
    return space_.mark(object, it->second);
  }

  [[nodiscard]] GcResult<PlanStats> collect_impl(
      const std::vector<Address>& roots, const TraceFn& trace) {
    marked_.clear();
    space_.clear_marks();

    std::vector<Address> work = roots;
    while (!work.empty()) {
      const Address object = work.back();
      work.pop_back();
      if (!objects_.contains(object) || marked_.contains(object)) {
        continue;
      }
      mark(object);
      if (trace) {
        const auto children = trace(object);
        work.insert(work.end(), children.begin(), children.end());
      }
    }

    PlanStats stats;
    stats.live_objects = marked_.size();
    for (const Address object : marked_) {
      stats.live_bytes += objects_[object];
    }
    ++collection_count_;
    stats.collection_count = collection_count_;
    return GcResult<PlanStats>::from_ok(stats);
  }

  [[nodiscard]] constexpr const ImmixSpace& space() const noexcept {
    return space_;
  }

 private:
  ImmixSpace space_;
  std::unordered_map<Address, std::size_t> objects_;
  std::unordered_set<Address> marked_;
  std::size_t collection_count_{0};
};

}  // namespace MemTKX
