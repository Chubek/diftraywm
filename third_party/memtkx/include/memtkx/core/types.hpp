#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace MemTKX {

using Address = std::uintptr_t;
using ObjectReference = std::uintptr_t;
using Word = std::uintptr_t;
using Offset = std::ptrdiff_t;
using Byte = std::byte;

constexpr inline std::size_t kWordBytes = sizeof(Word);

struct AddressRange {
  Address start{0};
  Address end{0};

  [[nodiscard]] constexpr bool empty() const noexcept { return start >= end; }

  [[nodiscard]] constexpr std::size_t size() const noexcept {
    if (start >= end) {
      return 0;
    }
    return static_cast<std::size_t>(end - start);
  }

  [[nodiscard]] constexpr bool contains(Address addr) const noexcept {
    return addr >= start && addr < end;
  }

  [[nodiscard]] constexpr bool contains(AddressRange other) const noexcept {
    return other.start >= start && other.end <= end;
  }

  [[nodiscard]] constexpr bool overlaps(AddressRange other) const noexcept {
    return start < other.end && other.start < end;
  }

  [[nodiscard]] constexpr Address clamp(Address addr) const noexcept {
    if (addr < start) {
      return start;
    }
    if (addr >= end) {
      return end;
    }
    return addr;
  }

  friend constexpr bool operator==(AddressRange lhs, AddressRange rhs) noexcept {
    return lhs.start == rhs.start && lhs.end == rhs.end;
  }
};

[[nodiscard]] constexpr bool is_power_of_two(std::size_t value) noexcept {
  return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] constexpr Address align_up(Address value, std::size_t alignment) noexcept {
  if (alignment <= 1) {
    return value;
  }
  const Address mask = static_cast<Address>(alignment - 1);
  return (value + mask) & ~mask;
}

[[nodiscard]] constexpr Address align_down(Address value, std::size_t alignment) noexcept {
  if (alignment <= 1) {
    return value;
  }
  const Address mask = static_cast<Address>(alignment - 1);
  return value & ~mask;
}

template <typename T>
[[nodiscard]] constexpr std::size_t object_alignment() noexcept {
  return alignof(T);
}

[[nodiscard]] constexpr bool can_allocate(Address start, Address limit,
                                          std::size_t bytes,
                                          std::size_t alignment) noexcept {
  if (start >= limit) {
    return false;
  }
  const Address aligned = align_up(start, alignment);
  return aligned >= start && static_cast<std::size_t>(limit - aligned) >= bytes;
}

template <typename T>
[[nodiscard]] constexpr Address as_address(T* ptr) noexcept {
  return reinterpret_cast<Address>(ptr);
}

template <typename T>
[[nodiscard]] constexpr T* from_address(Address addr) noexcept {
  return reinterpret_cast<T*>(addr);
}

}  // namespace MemTKX
