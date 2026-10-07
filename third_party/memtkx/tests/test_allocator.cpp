#include <cassert>
#include <cstddef>
#include <vector>

#include "memtkx/MemTKX.hpp"

int main() {
  std::vector<MemTKX::Byte> storage(8192, MemTKX::Byte{0});
  const MemTKX::Address base = MemTKX::as_address(storage.data());
  const MemTKX::Address end = base + storage.size();

  MemTKX::BumpPointerAllocator bump(base, end);
  auto first = bump.allocate(128, 32);
  assert(first.is_ok());
  assert(MemTKX::as_address(reinterpret_cast<void*>(first.unwrap())) %
             32 ==
         0);
  auto second = bump.allocate(64, 8);
  assert(second.is_ok());
  assert(bump.used_bytes() == 192);
  auto too_big = bump.allocate(storage.size(), 1);
  assert(too_big.is_err());
  bump.reset();
  assert(bump.remaining() == storage.size());

  MemTKX::FreeListAllocator free_list(base, end);
  auto a = free_list.allocate(1024, 8);
  auto b = free_list.allocate(512, 16);
  assert(a.is_ok());
  assert(b.is_ok());
  assert(free_list.free(a.unwrap(), 1024));
  assert(free_list.free(b.unwrap(), 512));
  auto c = free_list.allocate(1500, 8);
  assert(c.is_ok());

  MemTKX::SegregatedFreeListAllocator segregated(base, end);
  auto small = segregated.allocate(16, 8);
  auto medium = segregated.allocate(1024, 8);
  assert(small.is_ok());
  assert(medium.is_ok());
  assert(segregated.free(small.unwrap(), 16));
  assert(segregated.free(medium.unwrap(), 1024));

  MemTKX::ImmixSpace immix(MemTKX::kImmixBlockBytes * 2);
  auto immix_object = immix.allocate(128, MemTKX::kImmixLineBytes);
  assert(immix_object.is_ok());
  assert(immix.mark(immix_object.unwrap(), 128));
  assert(immix.block_count() >= 2);
  assert(immix.used_bytes() >= 128);

  std::vector<MemTKX::Byte> los_storage(8192, MemTKX::Byte{0});
  MemTKX::LargeObjectSpace los(
      "los", MemTKX::as_address(los_storage.data()),
      MemTKX::as_address(los_storage.data()) + los_storage.size(), 256);
  auto large = los.allocate(512, 64);
  assert(large.is_ok());
  auto too_small = los.allocate(128, 8);
  assert(too_small.is_err());

  return 0;
}
