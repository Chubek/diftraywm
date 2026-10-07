#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "memtkx/MemTKXDSL.hpp"
#include "memtkx/core/error.hpp"
#include "memtkx/core/types.hpp"
#include "memtkx/space/space.hpp"

namespace MemTKX {

struct BumpPointerPolicy {
  static constexpr SpaceKind kind = SpaceKind::BumpPointer;
};

class BumpPointerAllocator
    : public dsl::DSL<BumpPointerAllocator, dsl::ResultFeature> {
 public:
  BumpPointerAllocator() = default;

  BumpPointerAllocator(Address start, Address end)
      : start_(align_up(start, alignof(std::max_align_t))),
        limit_(align_down(end, alignof(std::max_align_t))),
        cursor_(start_) {}

  [[nodiscard]] AllocResult<Address> allocate(std::size_t bytes,
                                              std::size_t alignment) {
    if (bytes == 0) {
      return AllocResult<Address>::from_err(AllocError::AllocationFailed);
    }
    if (!is_power_of_two(alignment)) {
      return AllocResult<Address>::from_err(AllocError::Misaligned);
    }

    const Address aligned = align_up(cursor_, alignment);
    if (aligned < cursor_ || !can_allocate(aligned, limit_, bytes, 1)) {
      return AllocResult<Address>::from_err(AllocError::OutOfMemory);
    }

    cursor_ = aligned + bytes;
    if (cursor_ < aligned) {
      return AllocResult<Address>::from_err(AllocError::OutOfMemory);
    }

    allocations_ += 1;
    allocated_bytes_ += bytes;
    return AllocResult<Address>::from_ok(aligned);
  }

  void reset() noexcept { cursor_ = start_; }

  void reset(Address start, Address end) noexcept {
    start_ = align_up(start, alignof(std::max_align_t));
    limit_ = align_down(end, alignof(std::max_align_t));
    cursor_ = start_;
  }

  [[nodiscard]] constexpr Address start() const noexcept { return start_; }
  [[nodiscard]] constexpr Address limit() const noexcept { return limit_; }
  [[nodiscard]] constexpr Address cursor() const noexcept { return cursor_; }

  [[nodiscard]] constexpr std::size_t remaining() const noexcept {
    return cursor_ >= limit_ ? 0 : static_cast<std::size_t>(limit_ - cursor_);
  }

  [[nodiscard]] constexpr std::size_t used_bytes() const noexcept {
    return cursor_ >= start_ ? static_cast<std::size_t>(cursor_ - start_) : 0;
  }

  [[nodiscard]] constexpr std::size_t allocation_count() const noexcept {
    return allocations_;
  }

  [[nodiscard]] constexpr std::size_t allocated_bytes() const noexcept {
    return allocated_bytes_;
  }

 private:
  Address start_{0};
  Address limit_{0};
  Address cursor_{0};
  std::size_t allocations_{0};
  std::size_t allocated_bytes_{0};
};

class BumpSpace final : public SpaceBase<BumpSpace> {
 public:
  BumpSpace(std::string name, Address start, Address end)
      : SpaceBase<BumpSpace>(SpaceDescriptor{
            std::move(name), SpaceKind::BumpPointer,
            AddressRange{start, end}, alignof(std::max_align_t)}),
        allocator_(start, end) {}

  [[nodiscard]] AllocResult<Address> allocate_impl(std::size_t bytes,
                                                   std::size_t alignment) {
    auto result = allocator_.allocate(bytes, alignment);
    if (result.is_ok()) {
      record_allocation(bytes);
    }
    return result;
  }

  void reset() noexcept { allocator_.reset(); }

  [[nodiscard]] constexpr const BumpPointerAllocator& allocator() const noexcept {
    return allocator_;
  }

 private:
  BumpPointerAllocator allocator_;
};

}  // namespace MemTKX
