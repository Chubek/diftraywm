#pragma once

#include <cstddef>
#include <cstdint>

#include "memtkx/MemTKXDSL.hpp"

namespace MemTKX {

struct MemoryUnitsDSL : dsl::DSL<MemoryUnitsDSL, dsl::CustomLiterals> {
  static constexpr auto literals = dsl::literal_set(
      dsl::lit<"_B">([](long double value) {
        return static_cast<std::size_t>(value);
      }),
      dsl::lit<"_KB">([](long double value) {
        return static_cast<std::size_t>(value * 1024.0L);
      }),
      dsl::lit<"_MB">([](long double value) {
        return static_cast<std::size_t>(value * 1024.0L * 1024.0L);
      }),
      dsl::lit<"_GB">([](long double value) {
        return static_cast<std::size_t>(value * 1024.0L * 1024.0L * 1024.0L);
      }));
};

inline namespace literals {

[[nodiscard]] constexpr std::size_t operator""_B(unsigned long long value) noexcept {
  return static_cast<std::size_t>(value);
}

[[nodiscard]] constexpr std::size_t operator""_KB(unsigned long long value) noexcept {
  return static_cast<std::size_t>(value * 1024ULL);
}

[[nodiscard]] constexpr std::size_t operator""_MB(unsigned long long value) noexcept {
  return static_cast<std::size_t>(value * 1024ULL * 1024ULL);
}

[[nodiscard]] constexpr std::size_t operator""_GB(unsigned long long value) noexcept {
  return static_cast<std::size_t>(value * 1024ULL * 1024ULL * 1024ULL);
}

}  // namespace literals

[[nodiscard]] constexpr std::size_t bytes_to_kib(std::size_t bytes) noexcept {
  return bytes / 1024;
}

[[nodiscard]] constexpr std::size_t kib_to_bytes(std::size_t kib) noexcept {
  return kib * 1024;
}

[[nodiscard]] constexpr std::size_t bytes_to_mib(std::size_t bytes) noexcept {
  return bytes / (1024 * 1024);
}

[[nodiscard]] constexpr std::size_t mib_to_bytes(std::size_t mib) noexcept {
  return mib * 1024 * 1024;
}

}  // namespace MemTKX
