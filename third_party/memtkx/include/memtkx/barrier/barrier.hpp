#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "memtkx/MemTKXDSL.hpp"
#include "memtkx/core/types.hpp"

namespace MemTKX {

struct Slot {
  Address* slot_address{nullptr};
  Address target_object{0};
};

struct ReadSlot {
  const Address* slot_address{nullptr};
  Address loaded_object{0};
};

struct BarrierDSL : dsl::DSL<BarrierDSL, dsl::Pipeline> {};

[[nodiscard]] inline auto barrier_non_null() {
  return dsl::pipe([](Slot slot) -> std::optional<Slot> {
    if (slot.slot_address != nullptr && slot.target_object != 0) {
      return slot;
    }
    return std::nullopt;
  });
}

[[nodiscard]] inline auto barrier_filter_range(AddressRange range) {
  return dsl::pipe([range](Slot slot) -> std::optional<Slot> {
    if (range.contains(reinterpret_cast<Address>(slot.slot_address))) {
      return slot;
    }
    return std::nullopt;
  });
}

[[nodiscard]] inline auto barrier_filter_target_range(AddressRange range) {
  return dsl::pipe([range](Slot slot) -> std::optional<Slot> {
    if (range.contains(slot.target_object)) {
      return slot;
    }
    return std::nullopt;
  });
}

[[nodiscard]] inline auto barrier_cross_region(AddressRange source_region,
                                               AddressRange target_region) {
  return dsl::pipe([=](Slot slot) -> std::optional<Slot> {
    if (source_region.contains(
            reinterpret_cast<Address>(slot.slot_address)) &&
        target_region.contains(slot.target_object)) {
      return slot;
    }
    return std::nullopt;
  });
}

[[nodiscard]] inline auto barrier_record(std::vector<Slot>& buffer) {
  return dsl::pipe([&buffer](std::optional<Slot> slot) {
    if (slot.has_value()) {
      buffer.push_back(*slot);
    }
  });
}

[[nodiscard]] inline auto barrier_discard() {
  return dsl::pipe([](std::optional<Slot>) {});
}

[[nodiscard]] inline auto barrier_saturating_filter(AddressRange source_region,
                                                    AddressRange target_region) {
  return dsl::pipe([=](Slot slot) -> std::optional<Slot> {
    if (!source_region.contains(
            reinterpret_cast<Address>(slot.slot_address))) {
      return std::nullopt;
    }
    if (!target_region.contains(slot.target_object)) {
      return std::nullopt;
    }
    return slot;
  });
}

inline void run_write_barrier(BarrierDSL& barrier, Slot slot,
                              AddressRange source_region,
                              AddressRange target_region,
                              std::vector<Slot>& mod_buffer) {
  barrier.wrap(slot) |
      barrier_saturating_filter(source_region, target_region) |
      barrier_record(mod_buffer);
}

}  // namespace MemTKX
