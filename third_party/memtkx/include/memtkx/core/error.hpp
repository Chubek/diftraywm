#pragma once

#include <cstdint>
#include <string_view>

#include "memtkx/MemTKXDSL.hpp"

namespace MemTKX {

enum class AllocError : std::uint8_t {
  OutOfMemory = 0,
  Misaligned = 1,
  LargeObjectExceeded = 2,
  AllocationFailed = 3,
};

enum class GCError : std::uint8_t {
  MutatorNotRegistered = 0,
  SafepointTimeout = 1,
  PlanUnavailable = 2,
  InvalidTrace = 3,
  CoordinatorBusy = 4,
};

[[nodiscard]] constexpr std::string_view to_string(AllocError error) noexcept {
  switch (error) {
    case AllocError::OutOfMemory:
      return "out of memory";
    case AllocError::Misaligned:
      return "invalid alignment";
    case AllocError::LargeObjectExceeded:
      return "large object size exceeded";
    case AllocError::AllocationFailed:
      return "allocation failed";
  }
  return "unknown allocation error";
}

[[nodiscard]] constexpr std::string_view to_string(GCError error) noexcept {
  switch (error) {
    case GCError::MutatorNotRegistered:
      return "mutator is not registered";
    case GCError::SafepointTimeout:
      return "safepoint timed out";
    case GCError::PlanUnavailable:
      return "GC plan is unavailable";
    case GCError::InvalidTrace:
      return "invalid object trace";
    case GCError::CoordinatorBusy:
      return "collector coordinator is busy";
  }
  return "unknown GC error";
}

template <typename T>
using AllocResult = dsl::Result<T, AllocError>;

template <typename T>
using GcResult = dsl::Result<T, GCError>;

}  // namespace MemTKX
