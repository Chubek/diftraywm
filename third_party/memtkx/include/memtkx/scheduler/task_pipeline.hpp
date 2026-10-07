#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "memtkx/MemTKXDSL.hpp"
#include "memtkx/core/error.hpp"

namespace MemTKX {

struct PhaseState {
  std::string phase_name;
  std::size_t completed_steps{0};
  std::chrono::steady_clock::duration elapsed{};

  void note_completion(std::size_t steps = 1) noexcept {
    completed_steps += steps;
  }
};

class Phase {
 public:
  using Function = std::function<GcResult<int>(PhaseState&)>;

  Phase(std::string name, Function function, bool stop_on_error = true)
      : name_(std::move(name)),
        function_(std::move(function)),
        stop_on_error_(stop_on_error) {}

  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  [[nodiscard]] bool stop_on_error() const noexcept { return stop_on_error_; }

  [[nodiscard]] GcResult<int> run(PhaseState& state) {
    return function_ ? function_(state) : GcResult<int>::from_ok(0);
  }

 private:
  std::string name_;
  Function function_;
  bool stop_on_error_;
};

class TaskPipeline {
 public:
  TaskPipeline() = default;

  explicit TaskPipeline(Phase phase) { phases_.push_back(std::move(phase)); }

  TaskPipeline& append(Phase phase) {
    phases_.push_back(std::move(phase));
    return *this;
  }

  TaskPipeline& append(TaskPipeline other) {
    for (auto& phase : other.phases_) {
      phases_.push_back(std::move(phase));
    }
    return *this;
  }

  [[nodiscard]] GcResult<int> run(PhaseState& state) {
    state.completed_steps = 0;
    for (auto& phase : phases_) {
      state.phase_name = phase.name();
      const auto start = std::chrono::steady_clock::now();
      auto result = phase.run(state);
      state.elapsed = std::chrono::steady_clock::now() - start;
      if (result.is_err() && phase.stop_on_error()) {
        return result;
      }
    }
    return GcResult<int>::from_ok(static_cast<int>(state.completed_steps));
  }

  [[nodiscard]] bool empty() const noexcept { return phases_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return phases_.size(); }

 private:
  std::vector<Phase> phases_;
};

[[nodiscard]] inline TaskPipeline operator|(TaskPipeline lhs, Phase rhs) {
  lhs.append(std::move(rhs));
  return lhs;
}

[[nodiscard]] inline TaskPipeline operator|(TaskPipeline lhs, TaskPipeline rhs) {
  lhs.append(std::move(rhs));
  return lhs;
}

[[nodiscard]] inline TaskPipeline operator|(Phase lhs, Phase rhs) {
  TaskPipeline pipeline(std::move(lhs));
  pipeline.append(std::move(rhs));
  return pipeline;
}

[[nodiscard]] inline TaskPipeline operator|(Phase lhs, TaskPipeline rhs) {
  TaskPipeline pipeline(std::move(lhs));
  pipeline.append(std::move(rhs));
  return pipeline;
}

[[nodiscard]] inline Phase make_phase(
    std::string name, Phase::Function function,
    bool stop_on_error = true) {
  return Phase(std::move(name), std::move(function), stop_on_error);
}

}  // namespace MemTKX
