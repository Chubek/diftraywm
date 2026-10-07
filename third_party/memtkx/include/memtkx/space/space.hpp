#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "memtkx/MemTKXDSL.hpp"
#include "memtkx/core/error.hpp"
#include "memtkx/core/types.hpp"

namespace MemTKX {

enum class SpaceKind : std::uint8_t {
  BumpPointer = 0,
  FreeList = 1,
  Immix = 2,
  LargeObject = 3,
  Nursery = 4,
  SemiSpace = 5,
};

[[nodiscard]] constexpr std::string_view to_string(SpaceKind kind) noexcept {
  switch (kind) {
    case SpaceKind::BumpPointer:
      return "bump-pointer";
    case SpaceKind::FreeList:
      return "free-list";
    case SpaceKind::Immix:
      return "immix";
    case SpaceKind::LargeObject:
      return "large-object";
    case SpaceKind::Nursery:
      return "nursery";
    case SpaceKind::SemiSpace:
      return "semi-space";
  }
  return "unknown";
}

struct SpaceMetrics {
  std::size_t allocated_bytes{0};
  std::size_t live_bytes{0};
  std::size_t total_allocations{0};
  std::size_t total_deallocations{0};
  std::size_t collection_count{0};

  void reset() noexcept {
    allocated_bytes = 0;
    live_bytes = 0;
    total_allocations = 0;
    total_deallocations = 0;
    collection_count = 0;
  }
};

struct SpaceDescriptor {
  std::string name;
  SpaceKind kind{SpaceKind::BumpPointer};
  AddressRange region;
  std::size_t default_alignment{alignof(std::max_align_t)};
};

template <typename Derived>
class SpaceBase : public dsl::DSL<Derived, dsl::ResultFeature> {
 public:
  explicit SpaceBase(SpaceDescriptor descriptor)
      : descriptor_(std::move(descriptor)) {}

  [[nodiscard]] constexpr std::string_view name() const noexcept {
    return descriptor_.name;
  }

  [[nodiscard]] constexpr SpaceKind kind() const noexcept {
    return descriptor_.kind;
  }

  [[nodiscard]] constexpr AddressRange region() const noexcept {
    return descriptor_.region;
  }

  [[nodiscard]] constexpr std::size_t default_alignment() const noexcept {
    return descriptor_.default_alignment;
  }

  [[nodiscard]] constexpr const SpaceMetrics& metrics() const noexcept {
    return metrics_;
  }

  [[nodiscard]] constexpr bool contains(Address address) const noexcept {
    return descriptor_.region.contains(address);
  }

  [[nodiscard]] AllocResult<Address> allocate(std::size_t bytes,
                                              std::size_t alignment) {
    if (!is_power_of_two(alignment)) {
      return AllocResult<Address>::from_err(AllocError::Misaligned);
    }
    return static_cast<Derived*>(this)->allocate_impl(bytes, alignment);
  }

 protected:
  void record_allocation(std::size_t bytes) noexcept {
    metrics_.allocated_bytes += bytes;
    metrics_.live_bytes += bytes;
    ++metrics_.total_allocations;
  }

  void record_deallocation(std::size_t bytes) noexcept {
    if (bytes <= metrics_.live_bytes) {
      metrics_.live_bytes -= bytes;
    }
    ++metrics_.total_deallocations;
  }

  void record_collection() noexcept { ++metrics_.collection_count; }

  SpaceDescriptor descriptor_;
  SpaceMetrics metrics_{};
};

}  // namespace MemTKX
