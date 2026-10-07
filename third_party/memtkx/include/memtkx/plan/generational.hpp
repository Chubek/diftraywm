#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "memtkx/MemTKXDSL.hpp"
#include "memtkx/barrier/card_table.hpp"
#include "memtkx/core/error.hpp"
#include "memtkx/core/types.hpp"
#include "memtkx/plan/plan.hpp"
#include "memtkx/space/bump_pointer.hpp"
#include "memtkx/space/free_list.hpp"

namespace MemTKX {

class GenerationalPlan : public PlanBase<GenerationalPlan> {
 public:
  GenerationalPlan(std::size_t nursery_bytes, std::size_t mature_bytes)
      : nursery_storage_(nursery_bytes),
        mature_storage_(mature_bytes),
        nursery_(as_address(nursery_storage_.data()),
                 as_address(nursery_storage_.data()) +
                     nursery_storage_.size()),
        mature_(as_address(mature_storage_.data()),
                as_address(mature_storage_.data()) +
                    mature_storage_.size()),
        card_table_(as_address(mature_storage_.data()),
                    mature_storage_.size()) {}

  [[nodiscard]] constexpr PlanKind kind_value() const noexcept {
    return PlanKind::Generational;
  }

  [[nodiscard]] constexpr std::string_view name_value() const noexcept {
    return "Generational";
  }

  [[nodiscard]] AllocResult<Address> allocate(std::size_t bytes,
                                              std::size_t alignment) {
    auto result = nursery_.allocate(bytes, alignment);
    if (result.is_ok()) {
      object_sizes_.emplace(result.unwrap(), bytes);
    }
    return result;
  }

  void remember(Address slot_address) { card_table_.mark(slot_address); }

  [[nodiscard]] std::size_t nursery_live_bytes() const noexcept {
    std::size_t total = 0;
    for (const auto& [address, bytes] : object_sizes_) {
      if (nursery_.limit() != 0 && address >= nursery_.start() &&
          address < nursery_.limit()) {
        total += bytes;
      }
    }
    return total;
  }

  [[nodiscard]] GcResult<PlanStats> minor_collect(
      const std::vector<Address>& roots, const TraceFn& trace) {
    std::vector<Address> work = roots;
    std::unordered_set<Address> seen;
    PlanStats stats;

    while (!work.empty()) {
      const Address object = work.back();
      work.pop_back();
      if (!object_sizes_.contains(object) || seen.contains(object)) {
        continue;
      }
      seen.insert(object);
      if (trace) {
        const auto children = trace(object);
        work.insert(work.end(), children.begin(), children.end());
      }
    }

    std::unordered_map<Address, std::size_t> survivors;
    for (const Address object : seen) {
      const std::size_t bytes = object_sizes_[object];
      const auto promoted = mature_.allocate(bytes, alignof(std::max_align_t));
      if (promoted.is_err()) {
        return GcResult<PlanStats>::from_err(GCError::PlanUnavailable);
      }
      std::memcpy(from_address<std::byte>(promoted.unwrap()),
                  from_address<const std::byte>(object), bytes);
      survivors.emplace(promoted.unwrap(), bytes);
      stats.live_objects += 1;
      stats.live_bytes += bytes;
    }

    object_sizes_ = std::move(survivors);
    nursery_.reset();
    card_table_.clear_all();
    ++collection_count_;
    stats.collection_count = collection_count_;
    return GcResult<PlanStats>::from_ok(stats);
  }

  [[nodiscard]] GcResult<PlanStats> collect_impl(
      const std::vector<Address>& roots, const TraceFn& trace) {
    return minor_collect(roots, trace);
  }

 private:
  std::vector<std::byte> nursery_storage_;
  std::vector<std::byte> mature_storage_;
  BumpPointerAllocator nursery_;
  FreeListAllocator mature_;
  CardTable card_table_;
  std::unordered_map<Address, std::size_t> object_sizes_;
  std::size_t collection_count_{0};
};

}  // namespace MemTKX
