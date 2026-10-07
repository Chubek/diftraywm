#include <cassert>
#include <cstdint>

#include "memtkx/MemTKX.hpp"

namespace {

enum class GCState : std::uint8_t {
  Unmarked = 0,
  Marked = 1,
  Forwarded = 2,
  Pinned = 3,
};

struct TraceDispatcher
    : MemTKX::dsl::DSL<TraceDispatcher, MemTKX::dsl::PatternMatch> {
  static constexpr auto trace_object = MemTKX::dsl::match(
      MemTKX::dsl::when<GCState::Unmarked>([](GCState, MemTKX::Address) {
        return GCState::Marked;
      }),
      MemTKX::dsl::when<GCState::Marked>([](GCState, MemTKX::Address) {
        return GCState::Marked;
      }),
      MemTKX::dsl::when<GCState::Forwarded>([](GCState, MemTKX::Address) {
        return GCState::Forwarded;
      }),
      MemTKX::dsl::otherwise([](GCState, MemTKX::Address) {
        return GCState::Pinned;
      }));
};

}  // namespace

int main() {
  assert(TraceDispatcher::trace_object(GCState::Unmarked, 0) ==
         GCState::Marked);
  assert(TraceDispatcher::trace_object(GCState::Forwarded, 0) ==
         GCState::Forwarded);
  assert(TraceDispatcher::trace_object(GCState::Pinned, 0) ==
         GCState::Pinned);

  auto positive = MemTKX::dsl::predicate([](int value) { return value > 0; });
  int even_checks = 0;
  auto even = MemTKX::dsl::predicate([&](int value) {
    ++even_checks;
    return value % 2 == 0;
  });
  auto both = positive & even;
  auto either = positive | even;
  auto non_positive = !positive;
  assert(!both(-2) && even_checks == 0);
  assert(either(3) && even_checks == 0);
  assert(both(4) && even_checks == 1);
  assert(!both(3) && even_checks == 2);
  assert(either(-2) && even_checks == 3);
  assert(!either(-3) && even_checks == 4);
  assert(non_positive(0) && !non_positive(1));
  return 0;
}
