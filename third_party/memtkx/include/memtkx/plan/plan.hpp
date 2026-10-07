#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "memtkx/MemTKXDSL.hpp"
#include "memtkx/core/error.hpp"
#include "memtkx/core/types.hpp"

namespace MemTKX {

enum class PlanKind : std::uint8_t {
  MarkSweep = 0,
  SemiSpace = 1,
  Immix = 2,
  Generational = 3,
};

[[nodiscard]] constexpr std::string_view to_string(PlanKind kind) noexcept {
  switch (kind) {
    case PlanKind::MarkSweep:
      return "mark-sweep";
    case PlanKind::SemiSpace:
      return "semi-space";
    case PlanKind::Immix:
      return "immix";
    case PlanKind::Generational:
      return "generational";
  }
  return "unknown";
}

struct PlanStats {
  std::size_t live_objects{0};
  std::size_t live_bytes{0};
  std::size_t freed_objects{0};
  std::size_t freed_bytes{0};
  std::size_t collection_count{0};
};

using TraceFn = std::function<std::vector<Address>(Address)>;

template <typename Derived>
class PlanBase : public dsl::DSL<Derived, dsl::ResultFeature> {
 public:
  [[nodiscard]] constexpr PlanKind kind() const noexcept {
    return static_cast<const Derived*>(this)->kind_value();
  }

  [[nodiscard]] constexpr std::string_view name() const noexcept {
    return static_cast<const Derived*>(this)->name_value();
  }

  [[nodiscard]] GcResult<PlanStats> collect(
      const std::vector<Address>& roots, const TraceFn& trace) {
    return static_cast<Derived*>(this)->collect_impl(roots, trace);
  }
};

}  // namespace MemTKX
