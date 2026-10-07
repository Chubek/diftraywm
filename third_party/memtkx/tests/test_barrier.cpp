#include <cassert>
#include <cstddef>
#include <vector>

#include "memtkx/MemTKX.hpp"

int main() {
  MemTKX::Address object = 0;
  MemTKX::Slot slot{&object, 0x2000};
  MemTKX::AddressRange source{MemTKX::as_address(&object),
                             MemTKX::as_address(&object) + 1};
  MemTKX::AddressRange target{0x1000, 0x3000};
  std::vector<MemTKX::Slot> buffer;

  MemTKX::BarrierDSL barrier;
  MemTKX::run_write_barrier(barrier, slot, source, target, buffer);
  assert(buffer.size() == 1);
  assert(buffer.front().target_object == slot.target_object);

  buffer.clear();
  MemTKX::Slot outside{nullptr, 0x2000};
  MemTKX::run_write_barrier(barrier, outside, source, target, buffer);
  assert(buffer.empty());

  MemTKX::CardTable cards(0, 4096, 512);
  assert(cards.card_count() == 8);
  cards.mark(513);
  assert(cards.is_marked(513));
  assert(cards.marked_cards() == 1);
  cards.clear(513);
  assert(!cards.is_marked(513));

  MemTKX::SatbQueue queue;
  MemTKX::SatbBarrier satb(queue);
  MemTKX::Address old_value = 77;
  MemTKX::Address slot_value = old_value;
  assert(satb.capture(&slot_value) == old_value);
  assert(queue.size() == 1);
  satb.write(&slot_value, 99);
  assert(slot_value == 99);
  assert(queue.size() == 2);

  MemTKX::CardTable near_zero(0, 4096, 512);
  near_zero.mark(1024);
  assert(near_zero.is_marked(1024));
  near_zero.mark(4096 + 1);
  assert(!near_zero.is_marked(4096 + 1));
  near_zero.clear(1024);
  assert(!near_zero.is_marked(1024));
  near_zero.mark(near_zero.base() + near_zero.card_bytes());
  assert(near_zero.is_marked(near_zero.base() + near_zero.card_bytes()));
  near_zero.clear(near_zero.base() + near_zero.card_bytes());
  assert(!near_zero.is_marked(near_zero.base() + near_zero.card_bytes()));

  MemTKX::CardTable zero_card(0, 4096, 0);
  assert(zero_card.card_count() == 0);
  zero_card.mark(100);
  assert(!zero_card.is_marked(100));

  return 0;
}
