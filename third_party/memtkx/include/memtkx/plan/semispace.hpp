#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "memtkx/MemTKXDSL.hpp"
#include "memtkx/core/error.hpp"
#include "memtkx/core/types.hpp"
#include "memtkx/plan/plan.hpp"
#include "memtkx/space/bump_pointer.hpp"

namespace MemTKX {

class SemiSpacePlan : public PlanBase<SemiSpacePlan> {
 public:
  explicit SemiSpacePlan(std::size_t semi_space_bytes)
      : storage_(semi_space_bytes * 2),
        semi_bytes_(semi_space_bytes),
        from_(as_address(storage_.data()),
              as_address(storage_.data()) + semi_space_bytes),
        to_(as_address(storage_.data()) + semi_space_bytes,
            as_address(storage_.data()) + storage_.size()) {}

  [[nodiscard]] constexpr PlanKind kind_value() const noexcept {
    return PlanKind::SemiSpace;
  }

  [[nodiscard]] constexpr std::string_view name_value() const noexcept {
    return "SemiSpace";
  }

  [[nodiscard]] AllocResult<Address> allocate(std::size_t bytes,
                                              std::size_t alignment) {
    auto result = from_.allocate(bytes, alignment);
    if (result.is_ok()) {
      objects_.emplace(result.unwrap(), bytes);
    }
    return result;
  }

  [[nodiscard]] std::size_t live_objects() const noexcept {
    return objects_.size();
  }

  [[nodiscard]] GcResult<PlanStats> collect_impl(
      const std::vector<Address>& roots, const TraceFn& trace) {
    to_.reset();
    std::unordered_map<Address, std::size_t> survivor_objects;
    std::unordered_map<Address, Address> forwarding;
    std::vector<Address> work;

    auto forward = [&](Address old_address) -> Address {
      if (const auto it = forwarding.find(old_address);
          it != forwarding.end()) {
        return it->second;
      }
      const auto size_it = objects_.find(old_address);
      if (size_it == objects_.end()) {
        return 0;
      }

      const std::size_t bytes = size_it->second;
      const auto new_result =
          to_.allocate(bytes, alignof(std::max_align_t));
      if (new_result.is_err()) {
        return 0;
      }
      const Address new_address = new_result.unwrap();
      std::memcpy(from_address<std::byte>(new_address),
                  from_address<const std::byte>(old_address), bytes);
      forwarding.emplace(old_address, new_address);
      survivor_objects.emplace(new_address, bytes);
      work.push_back(old_address);
      return new_address;
    };

    for (const Address root : roots) {
      forward(root);
    }

    while (!work.empty()) {
      const Address old_address = work.back();
      work.pop_back();
      if (trace) {
        for (const Address child : trace(old_address)) {
          forward(child);
        }
      }
    }

    objects_ = std::move(survivor_objects);
    std::swap(from_, to_);
    to_.reset();

    PlanStats stats;
    stats.live_objects = objects_.size();
    for (const auto& [address, bytes] : objects_) {
      (void)address;
      stats.live_bytes += bytes;
    }
    ++collection_count_;
    stats.collection_count = collection_count_;
    return GcResult<PlanStats>::from_ok(stats);
  }

 private:
  std::vector<std::byte> storage_;
  std::size_t semi_bytes_;
  BumpPointerAllocator from_;
  BumpPointerAllocator to_;
  std::unordered_map<Address, std::size_t> objects_;
  std::size_t collection_count_{0};
};

}  // namespace MemTKX
