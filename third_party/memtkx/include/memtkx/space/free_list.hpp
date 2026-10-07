#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "memtkx/MemTKXDSL.hpp"
#include "memtkx/core/error.hpp"
#include "memtkx/core/types.hpp"
#include "memtkx/space/space.hpp"

namespace MemTKX {

class FreeListAllocator
    : public dsl::DSL<FreeListAllocator, dsl::ResultFeature> {
 public:
  struct FreeCell {
    Address address{0};
    std::size_t size{0};

    [[nodiscard]] constexpr Address end() const noexcept {
      return address + size;
    }
  };

  FreeListAllocator() = default;

  FreeListAllocator(Address start, Address end) { reset(start, end); }

  void reset(Address start, Address end) {
    start_ = start;
    limit_ = end;
    cells_.clear();
    allocations_ = 0;
    allocated_bytes_ = 0;
    if (end > start) {
      cells_.push_back(FreeCell{start, static_cast<std::size_t>(end - start)});
    }
  }

  [[nodiscard]] AllocResult<Address> allocate(std::size_t bytes,
                                              std::size_t alignment) {
    if (bytes == 0) {
      return AllocResult<Address>::from_err(AllocError::AllocationFailed);
    }
    if (!is_power_of_two(alignment)) {
      return AllocResult<Address>::from_err(AllocError::Misaligned);
    }

    for (std::size_t i = 0; i < cells_.size(); ++i) {
      const FreeCell cell = cells_[i];
      const Address object_address = align_up(cell.address, alignment);
      if (object_address < cell.address ||
          static_cast<std::size_t>(object_address - cell.address) > cell.size) {
        continue;
      }

      const std::size_t padding =
          static_cast<std::size_t>(object_address - cell.address);
      if (padding + bytes > cell.size) {
        continue;
      }

      cells_.erase(cells_.begin() + static_cast<std::ptrdiff_t>(i));
      if (padding != 0) {
        cells_.push_back(FreeCell{cell.address, padding});
      }

      const std::size_t remainder = cell.size - padding - bytes;
      if (remainder != 0) {
        cells_.push_back(
            FreeCell{object_address + bytes, remainder});
      }

      std::sort(cells_.begin(), cells_.end(),
                [](const FreeCell& lhs, const FreeCell& rhs) {
                  return lhs.address < rhs.address;
                });
      coalesce();

      ++allocations_;
      allocated_bytes_ += bytes;
      return AllocResult<Address>::from_ok(object_address);
    }

    return AllocResult<Address>::from_err(AllocError::OutOfMemory);
  }

  bool free(Address address, std::size_t bytes) {
    if (address < start_ || bytes == 0 || address >= limit_ ||
        bytes > static_cast<std::size_t>(limit_ - address)) {
      return false;
    }
    cells_.push_back(FreeCell{address, bytes});
    std::sort(cells_.begin(), cells_.end(),
              [](const FreeCell& lhs, const FreeCell& rhs) {
                return lhs.address < rhs.address;
              });
    coalesce();
    return true;
  }

  [[nodiscard]] constexpr Address start() const noexcept { return start_; }
  [[nodiscard]] constexpr Address limit() const noexcept { return limit_; }

  [[nodiscard]] constexpr bool contains(Address address) const noexcept {
    return address >= start_ && address < limit_;
  }

  [[nodiscard]] constexpr std::size_t free_cells() const noexcept {
    return cells_.size();
  }

  [[nodiscard]] constexpr std::size_t free_bytes() const noexcept {
    std::size_t total = 0;
    for (const auto& cell : cells_) {
      total += cell.size;
    }
    return total;
  }

  [[nodiscard]] constexpr std::size_t allocation_count() const noexcept {
    return allocations_;
  }

  [[nodiscard]] constexpr std::size_t allocated_bytes() const noexcept {
    return allocated_bytes_;
  }

 private:
  void coalesce() {
    if (cells_.size() < 2) {
      return;
    }

    std::vector<FreeCell> merged;
    merged.reserve(cells_.size());
    FreeCell current = cells_.front();
    for (std::size_t i = 1; i < cells_.size(); ++i) {
      const FreeCell next = cells_[i];
      if (current.end() == next.address) {
        current.size += next.size;
      } else {
        merged.push_back(current);
        current = next;
      }
    }
    merged.push_back(current);
    cells_ = std::move(merged);
  }

  Address start_{0};
  Address limit_{0};
  std::vector<FreeCell> cells_;
  std::size_t allocations_{0};
  std::size_t allocated_bytes_{0};
};

class FreeListSpace final : public SpaceBase<FreeListSpace> {
 public:
  FreeListSpace(std::string name, Address start, Address end)
      : SpaceBase<FreeListSpace>(SpaceDescriptor{
            std::move(name), SpaceKind::FreeList, AddressRange{start, end},
            alignof(std::max_align_t)}),
        allocator_(start, end) {}

  [[nodiscard]] AllocResult<Address> allocate_impl(std::size_t bytes,
                                                   std::size_t alignment) {
    auto result = allocator_.allocate(bytes, alignment);
    if (result.is_ok()) {
      record_allocation(bytes);
    }
    return result;
  }

  bool deallocate(Address address, std::size_t bytes) {
    if (!allocator_.free(address, bytes)) {
      return false;
    }
    record_deallocation(bytes);
    return true;
  }

  void reset() noexcept { allocator_.reset(region().start, region().end); }

  [[nodiscard]] constexpr const FreeListAllocator& allocator() const noexcept {
    return allocator_;
  }

 private:
  FreeListAllocator allocator_;
};

class SegregatedFreeListAllocator
    : public dsl::DSL<SegregatedFreeListAllocator, dsl::ResultFeature> {
 public:
  static constexpr std::array<std::size_t, 8> kSizeClasses{
      16, 32, 64, 128, 256, 512, 1024, 4096};

  SegregatedFreeListAllocator() = default;

  SegregatedFreeListAllocator(Address start, Address end) { reset(start, end); }

  void reset(Address start, Address end) {
    start_ = start;
    limit_ = end;
    bins_.fill(FreeListAllocator{});
    ranges_.fill(AddressRange{});

    if (end <= start) {
      return;
    }

    const std::size_t total = static_cast<std::size_t>(end - start);
    const std::size_t per_class = total / kSizeClasses.size();
    Address cursor = start;
    for (std::size_t i = 0; i < kSizeClasses.size(); ++i) {
      const std::size_t class_bytes =
          i + 1 == kSizeClasses.size() ? static_cast<std::size_t>(end - cursor)
                                       : per_class;
      const Address class_end = cursor + class_bytes;
      ranges_[i] = AddressRange{cursor, class_end};
      bins_[i].reset(cursor, class_end);
      cursor = class_end;
    }
  }

  [[nodiscard]] AllocResult<Address> allocate(std::size_t bytes,
                                              std::size_t alignment) {
    if (bytes == 0) {
      return AllocResult<Address>::from_err(AllocError::AllocationFailed);
    }
    if (!is_power_of_two(alignment)) {
      return AllocResult<Address>::from_err(AllocError::Misaligned);
    }

    for (std::size_t i = 0; i < kSizeClasses.size(); ++i) {
      if (kSizeClasses[i] < bytes) {
        continue;
      }
      auto result = bins_[i].allocate(bytes, alignment);
      if (result.is_ok()) {
        return result;
      }
    }
    return AllocResult<Address>::from_err(AllocError::OutOfMemory);
  }

  bool free(Address address, std::size_t bytes) {
    for (std::size_t i = 0; i < ranges_.size(); ++i) {
      if (ranges_[i].contains(address)) {
        return bins_[i].free(address, bytes);
      }
    }
    return false;
  }

  [[nodiscard]] constexpr Address start() const noexcept { return start_; }
  [[nodiscard]] constexpr Address limit() const noexcept { return limit_; }
  [[nodiscard]] constexpr std::size_t class_count() const noexcept {
    return kSizeClasses.size();
  }

 private:
  Address start_{0};
  Address limit_{0};
  std::array<FreeListAllocator, kSizeClasses.size()> bins_;
  std::array<AddressRange, kSizeClasses.size()> ranges_;
};

}  // namespace MemTKX
