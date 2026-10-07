#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "memtkx/core/types.hpp"

namespace MemTKX {

class CardTable {
 public:
  CardTable() = default;

  CardTable(Address base, std::size_t region_bytes,
            std::size_t card_bytes = 512)
      : base_(base),
        region_bytes_(region_bytes),
        card_bytes_(card_bytes),
        cards_(card_count(), 0) {}

  [[nodiscard]] std::size_t card_index(Address address) const noexcept {
    if (card_bytes_ == 0 || cards_.empty()) {
      return cards_.size();
    }
    if (address < base_) {
      return cards_.size();
    }
    if (address - base_ >= region_bytes_) {
      return cards_.size();
    }
    return static_cast<std::size_t>(address - base_) / card_bytes_;
  }

  void mark(Address address) noexcept {
    const std::size_t index = card_index(address);
    if (index < cards_.size()) {
      cards_[index] = 1;
    }
  }

  [[nodiscard]] bool is_marked(Address address) const noexcept {
    const std::size_t index = card_index(address);
    return index < cards_.size() && cards_[index] != 0;
  }

  void clear(Address address) noexcept {
    const std::size_t index = card_index(address);
    if (index < cards_.size()) {
      cards_[index] = 0;
    }
  }

  void clear_all() noexcept { cards_.assign(cards_.size(), 0); }

  [[nodiscard]] std::size_t marked_cards() const noexcept {
    std::size_t total = 0;
    for (const auto card : cards_) {
      total += card != 0 ? 1 : 0;
    }
    return total;
  }

  [[nodiscard]] constexpr std::size_t card_count() const noexcept {
    if (card_bytes_ == 0) {
      return 0;
    }
    return (region_bytes_ + card_bytes_ - 1) / card_bytes_;
  }

  [[nodiscard]] constexpr std::size_t card_bytes() const noexcept {
    return card_bytes_;
  }

  [[nodiscard]] constexpr Address base() const noexcept { return base_; }

 private:
  Address base_{0};
  std::size_t region_bytes_{0};
  std::size_t card_bytes_{512};
  std::vector<std::uint8_t> cards_;
};

class CardTableBarrier {
 public:
  explicit CardTableBarrier(CardTable& table) : table_(table) {}

  void operator()(Address* slot_address, Address) {
    if (slot_address != nullptr) {
      table_.mark(reinterpret_cast<Address>(slot_address));
    }
  }

 private:
  CardTable& table_;
};

}  // namespace MemTKX
