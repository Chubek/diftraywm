#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "memtkx/core/error.hpp"
#include "memtkx/core/types.hpp"

namespace MemTKX {

using ThreadId = std::uint64_t;
using RootProvider = std::function<std::vector<Address>()>;
using StackScanCallback = std::function<std::vector<Address>(ThreadId)>;

enum class MutatorStatus : std::uint8_t {
  Running = 0,
  Polling = 1,
  AtSafepoint = 2,
  Blocked = 3,
};

struct ThreadRecord {
  ThreadId id{0};
  std::string name;
  MutatorStatus status{MutatorStatus::Running};
  StackScanCallback stack_scan;
  RootProvider root_provider;
};

class CollectorCoordinator {
 public:
  explicit CollectorCoordinator(std::size_t expected_mutators = 1)
      : expected_mutators_(expected_mutators) {}

  bool register_mutator(ThreadRecord record) {
    std::lock_guard lock(mutex_);
    if (records_.contains(record.id)) {
      return false;
    }
    records_.emplace(record.id, std::move(record));
    return true;
  }

  bool unregister_mutator(ThreadId id) {
    std::lock_guard lock(mutex_);
    return records_.erase(id) != 0;
  }

  void update_status(ThreadId id, MutatorStatus status) {
    std::lock_guard lock(mutex_);
    const auto it = records_.find(id);
    if (it != records_.end()) {
      it->second.status = status;
    }
    cv_.notify_all();
  }

  [[nodiscard]] MutatorStatus status(ThreadId id) const {
    std::lock_guard lock(mutex_);
    const auto it = records_.find(id);
    return it == records_.end() ? MutatorStatus::Blocked : it->second.status;
  }

  [[nodiscard]] GcResult<int> request_stop_the_world(
      std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    std::unique_lock lock(mutex_);
    ++generation_;
    const bool ready = cv_.wait_for(lock, timeout, [this] {
      return all_at_safepoint();
    });
    if (!ready) {
      return GcResult<int>::from_err(GCError::SafepointTimeout);
    }
    return GcResult<int>::from_ok(static_cast<int>(generation_));
  }

  void release_mutators() {
    {
      std::lock_guard lock(mutex_);
      ++generation_;
      for (auto& [id, record] : records_) {
        record.status = MutatorStatus::Running;
      }
    }
    cv_.notify_all();
  }

  [[nodiscard]] GcResult<int> poll_safepoint(ThreadId id) {
    std::unique_lock lock(mutex_);
    const auto it = records_.find(id);
    if (it == records_.end()) {
      return GcResult<int>::from_err(GCError::MutatorNotRegistered);
    }

    it->second.status = MutatorStatus::AtSafepoint;
    const std::uint64_t generation = generation_;
    cv_.notify_all();

    const bool released = cv_.wait_for(
        lock, std::chrono::seconds(5),
        [&] { return generation_ != generation; });
    if (!released) {
      return GcResult<int>::from_err(GCError::SafepointTimeout);
    }
    return GcResult<int>::from_ok(static_cast<int>(generation_));
  }

  [[nodiscard]] std::vector<Address> collect_roots() {
    std::vector<Address> roots;
    std::lock_guard lock(mutex_);
    for (auto& [id, record] : records_) {
      if (record.stack_scan) {
        auto stack_roots = record.stack_scan(id);
        roots.insert(roots.end(), stack_roots.begin(), stack_roots.end());
      }
      if (record.root_provider) {
        auto global_roots = record.root_provider();
        roots.insert(roots.end(), global_roots.begin(), global_roots.end());
      }
    }
    return roots;
  }

  [[nodiscard]] std::size_t registered_mutators() const {
    std::lock_guard lock(mutex_);
    return records_.size();
  }

 private:
  [[nodiscard]] bool all_at_safepoint() const {
    if (records_.size() < expected_mutators_) {
      return false;
    }
    for (const auto& [id, record] : records_) {
      (void)id;
      if (record.status != MutatorStatus::AtSafepoint) {
        return false;
      }
    }
    return true;
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::unordered_map<ThreadId, ThreadRecord> records_;
  std::size_t expected_mutators_{1};
  std::uint64_t generation_{0};
};

class MutatorContext {
 public:
  MutatorContext(CollectorCoordinator& coordinator, ThreadId id)
      : coordinator_(coordinator), id_(id) {}

  [[nodiscard]] GcResult<int> poll() {
    return coordinator_.poll_safepoint(id_);
  }

  [[nodiscard]] constexpr ThreadId id() const noexcept { return id_; }

 private:
  CollectorCoordinator& coordinator_;
  ThreadId id_;
};

class CollectorContext {
 public:
  explicit CollectorContext(CollectorCoordinator& coordinator)
      : coordinator_(coordinator) {}

  [[nodiscard]] GcResult<int> stop_the_world(
      std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    return coordinator_.request_stop_the_world(timeout);
  }

  void release_mutators() { coordinator_.release_mutators(); }

  [[nodiscard]] std::vector<Address> roots() {
    return coordinator_.collect_roots();
  }

 private:
  CollectorCoordinator& coordinator_;
};

}  // namespace MemTKX
