#include <cassert>
#include <cstddef>
#include <vector>

#include "memtkx/MemTKX.hpp"

int main() {
  MemTKX::MarkSweepPlan mark_sweep(4096);
  auto live = mark_sweep.allocate(64, 8);
  auto dead = mark_sweep.allocate(64, 8);
  assert(live.is_ok());
  assert(dead.is_ok());
  assert(mark_sweep.live_objects() == 2);

  auto mark_stats = mark_sweep.collect(
      {live.unwrap()}, [](MemTKX::Address) {
        return std::vector<MemTKX::Address>{};
      });
  assert(mark_stats.is_ok());
  assert(mark_stats.unwrap().live_objects == 1);
  assert(mark_stats.unwrap().freed_objects == 1);

  MemTKX::SemiSpacePlan semi_space(2048);
  auto semi_live = semi_space.allocate(128, 8);
  auto semi_dead = semi_space.allocate(64, 8);
  assert(semi_live.is_ok());
  assert(semi_dead.is_ok());
  auto semi_stats = semi_space.collect(
      {semi_live.unwrap()}, [](MemTKX::Address) {
        return std::vector<MemTKX::Address>{};
      });
  assert(semi_stats.is_ok());
  assert(semi_stats.unwrap().live_objects == 1);
  assert(semi_stats.unwrap().live_bytes == 128);

  MemTKX::ImmixPlan immix(MemTKX::kImmixBlockBytes * 2);
  auto immix_live = immix.allocate(512, MemTKX::kImmixLineBytes);
  auto immix_dead = immix.allocate(256, MemTKX::kImmixLineBytes);
  assert(immix_live.is_ok());
  assert(immix_dead.is_ok());
  auto immix_stats = immix.collect(
      {immix_live.unwrap()}, [](MemTKX::Address) {
        return std::vector<MemTKX::Address>{};
      });
  assert(immix_stats.is_ok());
  assert(immix_stats.unwrap().live_objects == 1);
  assert(immix.space().marked_bytes() >= 512);

  MemTKX::GenerationalPlan generational(1024, 4096);
  auto young_live = generational.allocate(64, 8);
  auto young_dead = generational.allocate(64, 8);
  assert(young_live.is_ok());
  assert(young_dead.is_ok());
  auto minor_stats = generational.minor_collect(
      {young_live.unwrap()}, [](MemTKX::Address) {
        return std::vector<MemTKX::Address>{};
      });
  assert(minor_stats.is_ok());
  assert(minor_stats.unwrap().live_objects == 1);
  assert(generational.nursery_live_bytes() == 0);

  return 0;
}
