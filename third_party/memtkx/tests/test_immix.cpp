#include <cassert>
#include <cstddef>

#include "memtkx/MemTKX.hpp"

int main() {
  constexpr std::size_t requested = MemTKX::kImmixBlockBytes + 1;
  MemTKX::ImmixSpace space(requested);

  auto head = space.allocate(MemTKX::kImmixBlockBytes, MemTKX::kImmixLineBytes);
  auto tail = space.allocate(1, MemTKX::kImmixLineBytes);
  assert(head.is_ok());
  assert(tail.is_ok());
  assert(space.block_count() >= 2);
  assert(space.total_bytes() >= requested);

  const MemTKX::AddressRange base_range{head.unwrap(), head.unwrap() + 1};
  assert(space.mark(base_range.start, 1));
  assert(space.marked_bytes() >= MemTKX::kImmixLineBytes);
  space.reset_blocks();
  assert(space.used_bytes() == 0);

  return 0;
}
