#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "memtkx/MemTKXDSL.hpp"
#include "memtkx/core/error.hpp"
#include "memtkx/core/types.hpp"
#include "memtkx/space/space.hpp"

namespace MemTKX {

inline constexpr std::size_t kImmixBlockBytes = 32 * 1024;
inline constexpr std::size_t kImmixLineBytes = 256;
inline constexpr std::size_t kImmixLinesPerBlock =
    kImmixBlockBytes / kImmixLineBytes;

class ImmixBlock {
 public:
  ImmixBlock(Address start, Address end)
      : start_(start), limit_(end), cursor_(start_) {
    line_marks_.fill(false);
  }

  [[nodiscard]] std::optional<Address> allocate(std::size_t bytes,
                                                std::size_t alignment) {
    const Address aligned = align_up(cursor_, alignment);
    if (aligned < cursor_ || !can_allocate(aligned, limit_, bytes, 1)) {
      return std::nullopt;
    }

    mark_lines(aligned, bytes);
    cursor_ = aligned + bytes;
    return aligned;
  }

  void reset() noexcept {
    cursor_ = start_;
    line_marks_.fill(false);
  }

  void clear_marks() noexcept { line_marks_.fill(false); }

  void mark(Address address, std::size_t bytes) {
    if (address < start_ || address + bytes > limit_) {
      return;
    }
    mark_lines(address, bytes);
  }

  [[nodiscard]] constexpr Address start() const noexcept { return start_; }
  [[nodiscard]] constexpr Address limit() const noexcept { return limit_; }
  [[nodiscard]] constexpr Address cursor() const noexcept { return cursor_; }

  [[nodiscard]] constexpr std::size_t used_bytes() const noexcept {
    return cursor_ >= start_ ? static_cast<std::size_t>(cursor_ - start_) : 0;
  }

  [[nodiscard]] constexpr std::size_t remaining_bytes() const noexcept {
    return cursor_ >= limit_ ? 0 : static_cast<std::size_t>(limit_ - cursor_);
  }

  [[nodiscard]] std::size_t marked_lines() const noexcept {
    return static_cast<std::size_t>(
        std::count(line_marks_.begin(), line_marks_.end(), true));
  }

  [[nodiscard]] std::size_t marked_bytes() const noexcept {
    return marked_lines() * kImmixLineBytes;
  }

 private:
  void mark_lines(Address address, std::size_t bytes) {
    const std::size_t first = line_index(address);
    const std::size_t last = line_index(address + bytes - 1);
    for (std::size_t i = first; i <= last && i < line_marks_.size(); ++i) {
      line_marks_[i] = true;
    }
  }

  [[nodiscard]] std::size_t line_index(Address address) const noexcept {
    return static_cast<std::size_t>(address - start_) / kImmixLineBytes;
  }

  Address start_{0};
  Address limit_{0};
  Address cursor_{0};
  std::array<bool, kImmixLinesPerBlock> line_marks_{};
};

class ImmixSpace final : public SpaceBase<ImmixSpace> {
 public:
  explicit ImmixSpace(std::size_t bytes)
      : SpaceBase<ImmixSpace>(SpaceDescriptor{
            "immix", SpaceKind::Immix, AddressRange{}, kImmixLineBytes}),
        storage_(aligned_block_bytes(bytes + kImmixLineBytes - 1) +
                 kImmixLineBytes) {
    const Address raw_base = as_address(storage_.data());
    const Address base =
        align_up(raw_base, static_cast<std::size_t>(kImmixLineBytes));
    const std::size_t usable_bytes = storage_.size() - (base - raw_base);
    const std::size_t block_count = usable_bytes / kImmixBlockBytes;

    descriptor_.region = AddressRange{
        base, base + (block_count * kImmixBlockBytes)};

    blocks_.reserve(block_count);
    for (std::size_t i = 0; i < block_count; ++i) {
      const Address block_start = base + (i * kImmixBlockBytes);
      blocks_.emplace_back(block_start, block_start + kImmixBlockBytes);
    }
  }

  [[nodiscard]] AllocResult<Address> allocate_impl(std::size_t bytes,
                                                   std::size_t alignment) {
    if (bytes > kImmixBlockBytes) {
      return AllocResult<Address>::from_err(AllocError::LargeObjectExceeded);
    }
    for (auto& block : blocks_) {
      const auto address = block.allocate(bytes, alignment);
      if (address.has_value()) {
        record_allocation(bytes);
        return AllocResult<Address>::from_ok(*address);
      }
    }
    return AllocResult<Address>::from_err(AllocError::OutOfMemory);
  }

  bool mark(Address address, std::size_t bytes) {
    for (auto& block : blocks_) {
      if (address >= block.start() && address < block.limit()) {
        block.mark(address, bytes);
        return true;
      }
    }
    return false;
  }

  void reset_blocks() noexcept {
    for (auto& block : blocks_) {
      block.reset();
    }
  }

  void clear_marks() noexcept {
    for (auto& block : blocks_) {
      block.clear_marks();
    }
  }

  [[nodiscard]] std::size_t block_count() const noexcept {
    return blocks_.size();
  }

  [[nodiscard]] std::size_t total_bytes() const noexcept {
    return storage_.size();
  }

  [[nodiscard]] std::size_t used_bytes() const noexcept {
    std::size_t total = 0;
    for (const auto& block : blocks_) {
      total += block.used_bytes();
    }
    return total;
  }

  [[nodiscard]] std::size_t marked_bytes() const noexcept {
    std::size_t total = 0;
    for (const auto& block : blocks_) {
      total += block.marked_bytes();
    }
    return total;
  }

  [[nodiscard]] constexpr const std::vector<ImmixBlock>& blocks() const noexcept {
    return blocks_;
  }

 private:
  static std::size_t aligned_block_bytes(std::size_t bytes) {
    if (bytes <= kImmixBlockBytes) {
      return kImmixBlockBytes;
    }
    return ((bytes + kImmixBlockBytes - 1) / kImmixBlockBytes) *
           kImmixBlockBytes;
  }

  std::vector<std::byte> storage_;
  std::vector<ImmixBlock> blocks_;
};

}  // namespace MemTKX
