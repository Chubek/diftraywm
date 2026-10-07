#include <chrono>
#include <cstddef>
#include <iostream>
#include <vector>

#include "memtkx/MemTKX.hpp"

namespace {

using Clock = std::chrono::steady_clock;

double run_bump(std::size_t iterations) {
  std::vector<MemTKX::Byte> storage(16 * 1024 * 1024);
  MemTKX::BumpPointerAllocator allocator(
      MemTKX::as_address(storage.data()),
      MemTKX::as_address(storage.data()) + storage.size());

  const auto start = Clock::now();
  for (std::size_t i = 0; i < iterations; ++i) {
    allocator.reset();
    const auto result = allocator.allocate(32, 8);
    if (result.is_err()) {
      return -1.0;
    }
  }
  const auto stop = Clock::now();
  return std::chrono::duration<double, std::nano>(stop - start).count() /
         static_cast<double>(iterations);
}

double run_free_list(std::size_t iterations) {
  std::vector<MemTKX::Byte> storage(16 * 1024 * 1024);
  MemTKX::FreeListAllocator allocator(
      MemTKX::as_address(storage.data()),
      MemTKX::as_address(storage.data()) + storage.size());

  const auto start = Clock::now();
  for (std::size_t i = 0; i < iterations; ++i) {
    const auto result = allocator.allocate(32, 8);
    if (result.is_err()) {
      return -1.0;
    }
    allocator.free(result.unwrap(), 32);
  }
  const auto stop = Clock::now();
  return std::chrono::duration<double, std::nano>(stop - start).count() /
         static_cast<double>(iterations);
}

double run_immix(std::size_t iterations) {
  MemTKX::ImmixSpace space(MemTKX::kImmixBlockBytes * 4);
  const auto start = Clock::now();
  for (std::size_t i = 0; i < iterations; ++i) {
    if (i % 256 == 0) {
      space.reset_blocks();
    }
    const auto result = space.allocate(32, 8);
    if (result.is_err()) {
      return -1.0;
    }
  }
  const auto stop = Clock::now();
  return std::chrono::duration<double, std::nano>(stop - start).count() /
         static_cast<double>(iterations);
}

}  // namespace

int main() {
  constexpr std::size_t iterations = 200000;

  const double bump_ns = run_bump(iterations);
  const double free_list_ns = run_free_list(iterations);
  const double immix_ns = run_immix(iterations);

  std::cout << "allocator,ns_per_operation\n";
  std::cout << "bump-pointer," << bump_ns << '\n';
  std::cout << "free-list," << free_list_ns << '\n';
  std::cout << "immix," << immix_ns << '\n';

  return bump_ns < 0.0 || free_list_ns < 0.0 || immix_ns < 0.0;
}
