#include <chrono>
#include <cassert>

#include "memtkx/MemTKX.hpp"

int main() {
  MemTKX::CollectorCoordinator coordinator(2);

  auto timed_out = coordinator.request_stop_the_world(std::chrono::milliseconds(10));
  assert(timed_out.is_err());
  (void)timed_out.map_err([](MemTKX::GCError error) {
    assert(error == MemTKX::GCError::SafepointTimeout);
    return error;
  });

  MemTKX::ThreadRecord record;
  record.id = 1;
  record.name = "mutator";
  assert(coordinator.register_mutator(record));
  assert(coordinator.registered_mutators() == 1);
  assert(coordinator.status(1) == MemTKX::MutatorStatus::Running);
  assert(coordinator.unregister_mutator(1));

  MemTKX::PhaseState state;
  auto phase1 = MemTKX::make_phase(
      "warning", [](MemTKX::PhaseState&) {
        return MemTKX::GcResult<int>::from_err(MemTKX::GCError::InvalidTrace);
      },
      /*stop_on_error=*/false);
  auto phase2 = MemTKX::make_phase(
      "continue", [](MemTKX::PhaseState& phase_state) {
        phase_state.note_completion();
        return MemTKX::GcResult<int>::from_ok(1);
      });

  auto pipeline = phase1 | phase2;
  auto result = pipeline.run(state);
  assert(result.is_ok());
  assert(state.completed_steps == 1);

  return 0;
}
