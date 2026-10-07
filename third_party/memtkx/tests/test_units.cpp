#include <cassert>
#include <cstddef>

#include "memtkx/MemTKX.hpp"

int main() {
  using namespace MemTKX::literals;

  static_assert(1_KB == 1024);
  static_assert(1_MB == 1024 * 1024);
  static_assert(1_GB == 1024ULL * 1024ULL * 1024ULL);

  assert(MemTKX::is_power_of_two(8));
  assert(!MemTKX::is_power_of_two(6));

  assert(MemTKX::align_up(17, 8) == 24);
  assert(MemTKX::align_down(17, 8) == 16);

  MemTKX::AddressRange range{100, 200};
  assert(range.contains(100));
  assert(!range.contains(200));
  assert(range.size() == 100);

  assert(MemTKX::bytes_to_kib(2048) == 2);
  assert(MemTKX::mib_to_bytes(2) == 2 * 1024 * 1024);

  return 0;
}
