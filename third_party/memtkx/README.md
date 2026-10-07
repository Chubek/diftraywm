# MemTKX

**MemTKX is a header-only C++20 memory-management toolkit for building
allocators, garbage-collection experiments, and runtime integrations.** It
provides allocation spaces, four collection plans, write-barrier building
blocks, collection-phase pipelines, and cooperative mutator coordination.
Its public C++ API lives in `MemTKX`; its composable DSL utilities live in
`MemTKX::dsl`.

Start with the working example below, then follow the
[14-chapter manual in GUIDE.md](GUIDE.md). The manual includes object tracing,
thread coordination, custom space and plan plugins, and practical API
reference tables.

## At a glance

| Item | Value |
| --- | --- |
| Library identity | `MemTKX` |
| Language | C++20 or later |
| Distribution | Header-only |
| Main include | `<memtkx/MemTKX.hpp>` |
| DSL include | `<memtkx/MemTKXDSL.hpp>` |
| C++ namespace | `MemTKX` |
| DSL namespace | `MemTKX::dsl` |
| CMake package | `MemTKX` |
| CMake target | `MemTKX::MemTKX` |
| CMake minimum | 3.20 |
| Package version | 1.0 |
| Dependencies | C++ standard library and platform threads |

## What you can build

- **Short-lived arenas:** use `BumpPointerAllocator` or `BumpSpace` for fast
  linear allocation and whole-region reuse.
- **Individually reclaimable heaps:** use `FreeListAllocator`,
  `FreeListSpace`, or `SegregatedFreeListAllocator`.
- **Reachability-based collection:** use `MarkSweepPlan` with roots and an
  object-layout-aware trace callback.
- **Collector experiments:** study copying with `SemiSpacePlan`, line
  marking with `ImmixPlan`, and nursery promotion with `GenerationalPlan`.
- **Reference-write instrumentation:** compose range filters, record
  modified slots, dirty cards, or capture old references in a SATB queue.
- **Runtime collection orchestration:** register root providers, coordinate
  cooperative safepoints, and run named collection phases.
- **Application plugins:** implement CRTP space/plan adapters or supply
  callbacks and phases using the existing extension points.

Allocation returns raw addresses. Your application defines object layouts,
constructs objects, enumerates references, and chooses when collection runs.
There is no implicit replacement of `new`, automatic C++ stack scanning, or
automatic allocation-triggered collection.

## Build and run the tests

From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The test suite uses assertions, so a Debug configuration is useful when
checking behavior. Tests cover units, allocators, barriers, Immix capacity,
large objects, dispatch, scheduler behavior, and collection plans.

### CMake options

| Option | Default | Purpose |
| --- | --- | --- |
| `MemTKX_BUILD_TESTS` | `ON` | Build and register the nine tests |
| `MemTKX_BUILD_BENCHMARKS` | `OFF` | Build the two standalone benchmarks |
| `MemTKX_ENABLE_WARNINGS` | `ON` | Propagate strict GCC/Clang warnings |

The warning option adds `-Wall -Wextra -Wpedantic` through the interface
target. Set it to `OFF` if your application manages its own warning policy.

## Add MemTKX to your application

### Use a source checkout

If this repository is located at `third_party/memtkx`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyRuntime LANGUAGES CXX)

set(MemTKX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MemTKX_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/memtkx)

add_executable(my_runtime main.cpp)
target_link_libraries(my_runtime PRIVATE MemTKX::MemTKX)
```

Linking the interface target supplies include directories, the C++20
requirement, and the thread dependency. It does not link a compiled MemTKX
archive.

### Use an installed package

Install to a prefix you control:

```sh
cmake -S . -B build-install \
  -DMemTKX_BUILD_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build-install --parallel
cmake --install build-install
```

In the consuming project:

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyRuntime LANGUAGES CXX)

find_package(MemTKX 1 CONFIG REQUIRED)
add_executable(my_runtime main.cpp)
target_link_libraries(my_runtime PRIVATE MemTKX::MemTKX)
```

Configure the consumer with `-DCMAKE_PREFIX_PATH="$HOME/.local"` if needed.
The installed package resolves `Threads` automatically. Headers, CMake
package files, and both documentation files are installed using CMake's
standard installation directories.

### Compile directly

For GCC or Clang on a typical Linux system:

```sh
c++ -std=c++20 -pthread -Iinclude main.cpp -o my_runtime
./my_runtime
```

## Your first managed object

Save this complete program as `main.cpp`:

```cpp
#include <memtkx/MemTKX.hpp>

#include <iostream>
#include <memory>
#include <vector>

struct Node {
  MemTKX::Address next{0};
  int value{0};
};

int main() {
  using namespace MemTKX::literals;
  MemTKX::MarkSweepPlan heap(64_KB);

  auto allocation = heap.allocate(sizeof(Node), alignof(Node));
  if (allocation.is_err()) {
    (void)allocation.map_err([](MemTKX::AllocError error) {
      std::cerr << MemTKX::to_string(error) << '\n';
      return error;
    });
    return 1;
  }

  const auto root = allocation.unwrap();
  auto* node = std::construct_at(MemTKX::from_address<Node>(root));
  node->value = 42;

  MemTKX::TraceFn trace = [](MemTKX::Address address) {
    const auto* current = MemTKX::from_address<const Node>(address);
    std::vector<MemTKX::Address> children;
    if (current->next != 0) {
      children.push_back(current->next);
    }
    return children;
  };

  auto collected = heap.collect({root}, trace);
  if (collected.is_err()) {
    return 2;
  }

  const auto stats = collected.unwrap();
  std::cout << "value=" << node->value
            << ", live_objects=" << stats.live_objects
            << ", collections=" << stats.collection_count << '\n';

  std::destroy_at(node);
  auto emptied = heap.collect({}, trace);
  return emptied.is_ok() ? 0 : 3;
}
```

Expected first-line output:

```text
value=42, live_objects=1, collections=1
```

The root keeps the object reachable. The trace callback describes outgoing
references; an empty root set makes all objects unreachable in this example.
`MarkSweepPlan` preserves survivor addresses and returns unreachable storage
to its free-list allocator. It does not invoke object destructors; this
example performs that step explicitly before the final collection.

## Choose an allocator or space

| Component | Owns backing storage? | Reuse behavior |
| --- | --- | --- |
| `BumpPointerAllocator` | No | Whole-region `reset()` |
| `BumpSpace` | No | Whole-region reset; space metadata and metrics |
| `FreeListAllocator` | No | `free(address, bytes)` and adjacent-cell coalescing |
| `FreeListSpace` | No | `deallocate(address, bytes)` with metrics |
| `SegregatedFreeListAllocator` | No | Eight region partitions selected by request size |
| `ImmixSpace` | Yes | Block allocation, line marking, whole-block reset |
| `LargeObjectSpace` | No | Threshold-filtered bump allocation; reset for reuse |

For borrowed storage, keep the buffer alive and at a stable address for the
entire lifetime of its allocator and allocations. Supply nonzero sizes and
power-of-two alignments. Individual free-list deallocation requires the
original address and requested size.

## Choose a collection plan

These descriptions reflect the current implementation:

| Plan | Current collection behavior | Integration detail |
| --- | --- | --- |
| `MarkSweepPlan` | Traces reachable objects and frees unreachable ones | Non-moving; suitable for the introductory graph examples |
| `SemiSpacePlan` | Copies survivors between two equal-sized regions | Internal forwarding map is not exposed; roots and object fields are not rewritten |
| `ImmixPlan` | Traces objects and marks occupied lines | Collection reports reachability; it does not sweep or recycle dead lines |
| `GenerationalPlan` | Copies reachable tracked objects into mature storage and resets the nursery | No root/field rewriting, remembered-card scanning, or mature-space sweep |

The copying plans are useful algorithm-building components. Their current
address-based tracing API does not provide the relocation protocol needed
for a repeatedly usable moving object graph. See
[Chapter 8](GUIDE.md#chapter-8--collection-plans-and-statistics) for the
specific behavior and statistics of each plan.

## Plugins and extension points

MemTKX supports application-defined extensions through ordinary C++ types
and callbacks:

- `SpaceBase<Derived>` calls your `allocate_impl(bytes, alignment)`.
- `PlanBase<Derived>` calls your `collect_impl(roots, trace)` and exposes your
  `kind_value()` and `name_value()`.
- `TraceFn`, `RootProvider`, and `StackScanCallback` adapt runtime layouts
  and root discovery.
- `make_phase()` inserts application work into a `TaskPipeline`.
- `MemTKX::dsl::DSL<Derived, Features...>` composes reusable feature mixins.
- Barrier stages and wrappers implement application-specific write policies.

Plugins are compiled into the application. There is no shared-library
loader, plugin manifest, or automatic registry. The
[plugin chapter](GUIDE.md#chapter-13--plugins-and-custom-extensions) gives
complete quota-space and observed-plan examples, plus CMake packaging advice.

## Benchmarks

```sh
cmake -S . -B build-bench \
  -DCMAKE_BUILD_TYPE=Release \
  -DMemTKX_BUILD_TESTS=OFF \
  -DMemTKX_BUILD_BENCHMARKS=ON
cmake --build build-bench --parallel
./build-bench/bench_alloc
./build-bench/bench_barrier
```

Both programs print CSV with nanoseconds per operation. `bench_alloc`
compares bump, free-list, and Immix allocation loops. `bench_barrier` compares
a recording pipeline with a raw range filter. Their workloads differ; read
the benchmark sources and [Chapter 14](GUIDE.md#chapter-14--testing-performance-and-maintenance)
before using the figures to guide runtime decisions.

## Learn MemTKX in 14 chapters

1. [Identity and architecture](GUIDE.md#chapter-1--identity-and-architecture)
2. [Building and integrating](GUIDE.md#chapter-2--building-and-integrating)
3. [Addresses, units, and results](GUIDE.md#chapter-3--addresses-units-and-results)
4. [Object lifetime and bump allocation](GUIDE.md#chapter-4--object-lifetime-and-bump-allocation)
5. [Free lists and individual reclamation](GUIDE.md#chapter-5--free-lists-and-individual-reclamation)
6. [Immix and large-object spaces](GUIDE.md#chapter-6--immix-and-large-object-spaces)
7. [Roots, tracing, and object graphs](GUIDE.md#chapter-7--roots-tracing-and-object-graphs)
8. [Collection plans and statistics](GUIDE.md#chapter-8--collection-plans-and-statistics)
9. [Write barriers and remembered information](GUIDE.md#chapter-9--write-barriers-and-remembered-information)
10. [Work queues and phase pipelines](GUIDE.md#chapter-10--work-queues-and-phase-pipelines)
11. [Mutators and safepoint coordination](GUIDE.md#chapter-11--mutators-and-safepoint-coordination)
12. [The MemTKX DSL toolkit](GUIDE.md#chapter-12--the-memtkx-dsl-toolkit)
13. [Plugins and custom extensions](GUIDE.md#chapter-13--plugins-and-custom-extensions)
14. [Testing, performance, and maintenance](GUIDE.md#chapter-14--testing-performance-and-maintenance)

## Repository map

```text
include/memtkx/
  MemTKX.hpp          Main umbrella header
  MemTKXDSL.hpp       Composable DSL utilities
  core/               Addresses, units, and result aliases
  space/              Allocation mechanisms and space metadata
  plan/               Collection plans and plan statistics
  barrier/            Range filters, cards, and SATB queues
  scheduler/          Work packets, phases, and coordination
cmake/                Installed-package configuration template
tests/                Assertion-based executable tests
benchmarks/           Standalone CSV microbenchmarks
README.md             Overview and quick start
GUIDE.md              Fourteen-chapter usage and plugin manual
```

## Updating an existing integration

Use `<memtkx/MemTKX.hpp>`, qualify types with `MemTKX::`, and replace any
global DSL qualification with `MemTKX::dsl::`. Update CMake consumers to
`find_package(MemTKX CONFIG REQUIRED)` and `MemTKX::MemTKX`, and use the
`MemTKX_` option names listed above. Start with a fresh build directory after
changing package identity so cached package paths cannot select an older
installation. The API naming is case-sensitive.
