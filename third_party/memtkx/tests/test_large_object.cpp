#include <cassert>
#include <vector>

#include "memtkx/MemTKX.hpp"

int main() {
  std::vector<MemTKX::Byte> storage(2048);
  MemTKX::LargeObjectSpace los(
      "los", MemTKX::as_address(storage.data()),
      MemTKX::as_address(storage.data()) + storage.size(), 256);

  auto allocation = los.allocate(512, 64);
  assert(allocation.is_ok());
  const MemTKX::Address object = allocation.unwrap();

  assert(los.deallocate(object, 512));
  assert(los.metrics().live_bytes == 0);
  assert(!los.deallocate(0, 128));
  assert(!los.deallocate(object, 0));

  auto too_small = los.allocate(128, 64);
  assert(too_small.is_err());

  return 0;
}
