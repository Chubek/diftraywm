#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "memtkx/MemTKXDSL.hpp"
#include "memtkx/core/error.hpp"
#include "memtkx/core/types.hpp"
#include "memtkx/space/bump_pointer.hpp"
#include "memtkx/space/space.hpp"

namespace MemTKX {

inline constexpr std::size_t kDefaultLargeObjectThreshold = 4 * 1024;

class LargeObjectSpace final : public SpaceBase<LargeObjectSpace> {
 public:
  LargeObjectSpace(std::string name, Address start, Address end,
                   std::size_t threshold = kDefaultLargeObjectThreshold)
      : SpaceBase<LargeObjectSpace>(SpaceDescriptor{
            std::move(name), SpaceKind::LargeObject, AddressRange{start, end},
            alignof(std::max_align_t)}),
        allocator_(start, end),
        threshold_(threshold) {}

  [[nodiscard]] AllocResult<Address> allocate_impl(std::size_t bytes,
                                                   std::size_t alignment) {
    if (bytes < threshold_) {
      return AllocResult<Address>::from_err(AllocError::LargeObjectExceeded);
    }
    if (bytes > region().size()) {
      return AllocResult<Address>::from_err(AllocError::LargeObjectExceeded);
    }
    auto result = allocator_.allocate(bytes, alignment);
    if (result.is_ok()) {
      record_allocation(bytes);
    }
    return result;
  }

  bool deallocate(Address address, std::size_t bytes) noexcept {
    if (address < region().start || address >= region().end || bytes == 0 ||
        !region().contains(address)) {
      return false;
    }
    if (bytes > region().size()) {
      return false;
    }
    record_deallocation(bytes);
    return true;
  }

  void reset() noexcept { allocator_.reset(); }

  [[nodiscard]] constexpr std::size_t threshold() const noexcept {
    return threshold_;
  }

  [[nodiscard]] constexpr const BumpPointerAllocator& allocator() const noexcept {
    return allocator_;
  }

 private:
  BumpPointerAllocator allocator_;
  std::size_t threshold_;
};

}  // namespace MemTKX
