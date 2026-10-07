#include <cassert>
#include <chrono>
#include <thread>
#include <vector>

#include "memtkx/MemTKX.hpp"

int main() {
  MemTKX::WorkQueue queue;
  queue.push({MemTKX::WorkKind::RootScan, 10, 0});
  queue.push({MemTKX::WorkKind::Mark, 20, 0});
  std::size_t processed = queue.drain(
      [](const MemTKX::WorkPacket& packet) {
        assert(packet.object != 0);
      });
  assert(processed == 2);
  assert(queue.empty());

  MemTKX::PhaseState state;
  auto pipeline =
      MemTKX::make_phase(
          "stop", [](MemTKX::PhaseState&) {
            return MemTKX::GcResult<int>::from_ok(1);
          }) |
      MemTKX::make_phase(
          "roots", [](MemTKX::PhaseState& phase) {
            phase.note_completion(2);
            return MemTKX::GcResult<int>::from_ok(2);
          }) |
      MemTKX::make_phase(
          "closure", [](MemTKX::PhaseState&) {
            return MemTKX::GcResult<int>::from_ok(3);
          });

  auto result = pipeline.run(state);
  assert(result.is_ok());
  assert(state.completed_steps == 2);
  assert(pipeline.size() == 3);

  MemTKX::CollectorCoordinator coordinator(1);
  MemTKX::ThreadRecord record;
  record.id = 1;
  record.name = "mutator";
  assert(coordinator.register_mutator(record));
  assert(coordinator.registered_mutators() == 1);

  MemTKX::MutatorContext mutator(coordinator, 1);
  MemTKX::CollectorContext collector(coordinator);

  std::thread mutator_thread([&] {
    auto polled = mutator.poll();
    assert(polled.is_ok());
  });

  auto stopped = collector.stop_the_world(std::chrono::seconds(3));
  assert(stopped.is_ok());
  collector.release_mutators();
  mutator_thread.join();

  assert(coordinator.unregister_mutator(1));
  assert(coordinator.registered_mutators() == 0);

  return 0;
}
