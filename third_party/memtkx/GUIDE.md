# MemTKX Guide

## A 14-chapter usage and plugin manual

This manual teaches MemTKX from raw allocation through object tracing,
collection orchestration, and custom extensions. It describes the APIs in
this repository, including the precise behavior of the current collection
plans. Examples use C++20 and the public MemTKX identity consistently.

Read [README.md](README.md) for the shortest path to a working application.
For a first runtime integration, work through Chapters 1–7, then Chapters
9–11. For plugins, finish Chapter 13 after learning the base types. Chapter
8 explains which collection plans fit particular experiments.

### Contents

1. [Identity and architecture](#chapter-1--identity-and-architecture)
2. [Building and integrating](#chapter-2--building-and-integrating)
3. [Addresses, units, and results](#chapter-3--addresses-units-and-results)
4. [Object lifetime and bump allocation](#chapter-4--object-lifetime-and-bump-allocation)
5. [Free lists and individual reclamation](#chapter-5--free-lists-and-individual-reclamation)
6. [Immix and large-object spaces](#chapter-6--immix-and-large-object-spaces)
7. [Roots, tracing, and object graphs](#chapter-7--roots-tracing-and-object-graphs)
8. [Collection plans and statistics](#chapter-8--collection-plans-and-statistics)
9. [Write barriers and remembered information](#chapter-9--write-barriers-and-remembered-information)
10. [Work queues and phase pipelines](#chapter-10--work-queues-and-phase-pipelines)
11. [Mutators and safepoint coordination](#chapter-11--mutators-and-safepoint-coordination)
12. [The MemTKX DSL toolkit](#chapter-12--the-memtkx-dsl-toolkit)
13. [Plugins and custom extensions](#chapter-13--plugins-and-custom-extensions)
14. [Testing, performance, and maintenance](#chapter-14--testing-performance-and-maintenance)

Each fenced C++ example is a complete translation unit. Save it as
`example.cpp`, then compile from the repository root:

```sh
c++ -std=c++20 -pthread -Iinclude example.cpp -o example
./example
```

Examples that use assertions should be compiled without `NDEBUG`. CMake
snippets show consumer configuration and belong in a `CMakeLists.txt`.

## Chapter 1 — Identity and architecture

### 1.1 The public identity

The library name is **MemTKX**. Its package, target, namespaces, and includes
have deliberately predictable spelling:

| Surface | Spelling |
| --- | --- |
| CMake project and package | `MemTKX` |
| Build target | `MemTKX` |
| Consumer target | `MemTKX::MemTKX` |
| Public namespace | `MemTKX` |
| Utility namespace | `MemTKX::dsl` |
| Umbrella header | `<memtkx/MemTKX.hpp>` |
| Utility header | `<memtkx/MemTKXDSL.hpp>` |
| Options | `MemTKX_BUILD_TESTS`, `MemTKX_BUILD_BENCHMARKS`, `MemTKX_ENABLE_WARNINGS` |

The include directory is lowercase. The namespace and CMake names use the
display-name capitalization. Both distinctions matter on case-sensitive
filesystems and in C++.

### 1.2 Five cooperating layers

MemTKX divides memory management into small mechanisms:

1. **Core types** represent addresses, regions, memory quantities, and
   success/error results.
2. **Allocators and spaces** reserve bytes within a region. Some own their
   backing memory; others borrow it from the application.
3. **Plans** track allocations and implement reachability or copying logic.
4. **Barriers** record information about reference writes.
5. **Scheduling and coordination** organize work and provide cooperative
   mutator-state tracking.

The DSL toolkit supplies CRTP composition, pipelines, dispatch tables,
results, AST helpers, and parsing utilities. Memory-management classes use
selected features from this toolkit internally.

### 1.3 What the embedding runtime supplies

A runtime must decide what an object looks like, where its references live,
which values are roots, and how threads become quiescent. MemTKX cannot
infer those facts from arbitrary C++ objects. The trace callback therefore
receives an address and returns addresses of referenced objects.

Collection is explicit. An allocator's out-of-memory result does not invoke
a collector. A plan does not scan native stacks automatically. The
coordinator calls a supplied stack scanner if one was registered.

Think of MemTKX as components for constructing a memory-management policy.
For example, an arena needs only a borrowed buffer and a bump allocator.
A mark-sweep runtime adds an object table, roots, and tracing. A multi-thread
embedding also needs a real application parking protocol around collection.

### 1.4 Header-only integration

All implementation is in headers. The CMake interface target exists to
carry include paths, language requirements, and thread linkage. CRTP base
classes dispatch to a concrete derived type at compile time. There is no
common virtual `Plan` or `Space` object that can hold every implementation.

Runtime selection can be implemented in your application with a variant or
a wrapper. Compile-time selection can simply use a concrete plan member.
Chapter 13 demonstrates an adapter that preserves the existing interfaces.

**Learning task:** locate `SpaceBase`, `PlanBase`, and `TraceFn` in the
headers and identify the method or callback your runtime would provide for
each one.

## Chapter 2 — Building and integrating

### 2.1 Requirements and a first build

Use a compiler and standard library with C++20 support. CMake integration
requires CMake 3.20 or later and discovers platform thread support with
`find_package(Threads REQUIRED)`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For multi-configuration generators, also specify `--config Debug` for the
build and `-C Debug` for CTest. Executables may be located in a configuration
subdirectory rather than directly under `build`.

### 2.2 Source-tree integration

Vendor MemTKX at `third_party/memtkx` and configure the application like this:

```cmake
cmake_minimum_required(VERSION 3.20)
project(ExampleRuntime LANGUAGES CXX)

set(MemTKX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MemTKX_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/memtkx)

add_executable(example_runtime main.cpp)
target_link_libraries(example_runtime PRIVATE MemTKX::MemTKX)
```

Set options before `add_subdirectory`. This keeps the embedding project's
build focused on its own executables. The target requirement
`cxx_std_20` lets CMake select an appropriate language mode for consumers.

### 2.3 Installation

```sh
cmake -S . -B build-install \
  -DMemTKX_BUILD_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build-install --parallel
cmake --install build-install
```

The installation uses `GNUInstallDirs`:

| Setting | Installed content |
| --- | --- |
| `CMAKE_INSTALL_INCLUDEDIR` | `memtkx/` headers |
| `CMAKE_INSTALL_LIBDIR` | `cmake/MemTKX/` package files |
| `CMAKE_INSTALL_DOCDIR` | `README.md` and `GUIDE.md` |

The library itself has no binary archive to install. The package exports
`MemTKX::MemTKX` and resolves its `Threads::Threads` dependency when loaded.
Use a normal prefix-relative include directory when creating a relocatable
installation.

### 2.4 Consuming the installed package

```cmake
cmake_minimum_required(VERSION 3.20)
project(InstalledExample LANGUAGES CXX)

find_package(MemTKX 1 CONFIG REQUIRED)
add_executable(installed_example main.cpp)
target_link_libraries(installed_example PRIVATE MemTKX::MemTKX)
```

Configure this application with:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/.local"
cmake --build build --parallel
```

Package discovery can also use `MemTKX_DIR`, pointing directly to the
directory containing `MemTKXConfig.cmake`. Version compatibility is
configured by CMake's `SameMajorVersion` policy.

### 2.5 Options and include granularity

Tests default to enabled; benchmarks default to disabled. Strict warnings
default to enabled and propagate to GCC/Clang consumers. Set
`MemTKX_ENABLE_WARNINGS=OFF` when your project's own warning configuration
should control compilation.

Use the umbrella header while learning. Later, include individual headers
such as `<memtkx/space/bump_pointer.hpp>` or
`<memtkx/barrier/card_table.hpp>` when your component needs only that API.
The DSL utilities can be included independently of the umbrella header.

**Learning task:** build a separate consumer with `find_package` after
installing MemTKX. This exercises package discovery rather than relying on
source-tree includes.

## Chapter 3 — Addresses, units, and results

### 3.1 Address vocabulary

`Address`, `ObjectReference`, and `Word` are aliases of `std::uintptr_t`.
`Byte` is `std::byte`; `Offset` is `std::ptrdiff_t`. `kWordBytes` gives the
size of a word.

`as_address(pointer)` converts a pointer to the integer address type.
`from_address<T>(address)` converts it back to `T*`. These conversions do
not allocate memory, construct an object, check its type, or extend its
lifetime. Use them only with the storage and object layout your runtime
actually owns.

### 3.2 Regions are half-open

`AddressRange{start, end}` describes `[start, end)`. The start belongs to
the region and the end does not. Empty or reversed ranges report size zero.
The API includes `contains(address)`, `contains(range)`, `overlaps(range)`,
and `clamp(address)`.

Clamping an address above the region returns `end`, which is an exclusive
boundary, not the address of a valid byte. Validate lengths and ranges before
using them to access storage.

### 3.3 Alignment and sizes

Pass power-of-two alignments such as `alignof(Node)`, 8, 16, or 256.
`is_power_of_two` rejects zero. `align_up` and `align_down` use bit masks;
the helpers themselves do not report an invalid alignment. Allocator and
space entry points perform their own checks.

Keep request sizes nonzero and within the available region. Raw address
arithmetic uses integer types; extremely large or overflowing region
calculations must be excluded by the embedding application.

### 3.4 Memory quantities

Import `MemTKX::literals` for integer literals:

| Literal | Meaning |
| --- | --- |
| `64_B` | 64 bytes |
| `4_KB` | 4 × 1024 bytes |
| `8_MB` | 8 × 1024² bytes |
| `1_GB` | 1024³ bytes |

These suffixes use binary scaling despite their short names. Conversion
helpers include `bytes_to_kib`, `kib_to_bytes`, `bytes_to_mib`, and
`mib_to_bytes`. Byte-to-unit conversions use integer division.

`MemoryUnitsDSL::parse_literal("1.5_MB")` accepts runtime strings and returns
a `long double` containing the converted byte quantity. The handlers
convert to `std::size_t`, so fractional bytes are truncated. Parsing uses
the standard numeric parser and may throw for invalid input.

### 3.5 Explicit result handling

Allocation uses `AllocResult<T>`, an alias of
`MemTKX::dsl::Result<T, AllocError>`. Collection and coordination use
`GcResult<T>`, with `GCError` as the error type.

| Operation | Purpose |
| --- | --- |
| `is_ok()` / `is_err()` | Inspect the active branch |
| `unwrap()` | Copy the success value; throw if the result is an error |
| `unwrap_or(fallback)` | Obtain a success value or a fallback |
| `map(fn)` | Transform a success value |
| `and_then(fn)` | Chain another result-producing operation |
| `map_err(fn)` | Inspect or transform an error payload |
| `from_ok(value)` / `from_err(error)` | Construct a branch explicitly |

There is no `error()` accessor. Use `map_err` to inspect the error while
preserving or changing its type. This example exercises both branches:

```cpp
#include <memtkx/MemTKX.hpp>

#include <cassert>
#include <iostream>

int main() {
  using namespace MemTKX::literals;
  static_assert(2_KB == 2048);
  static_assert(MemTKX::align_up(17, 8) == 24);

  constexpr MemTKX::AddressRange range{100, 200};
  static_assert(range.contains(100));
  static_assert(!range.contains(200));

  auto value = MemTKX::AllocResult<int>::from_ok(21);
  auto doubled = value.map([](int x) { return x * 2; });
  assert(doubled.unwrap() == 42);

  auto failure = MemTKX::AllocResult<int>::from_err(
      MemTKX::AllocError::OutOfMemory);
  bool observed = false;
  (void)failure.map_err([&](MemTKX::AllocError error) {
    observed = true;
    std::cout << MemTKX::to_string(error) << '\n';
    return error;
  });
  assert(observed && failure.unwrap_or(-1) == -1);
}
```

### 3.6 Error vocabulary

Allocation errors are `OutOfMemory`, `Misaligned`, `LargeObjectExceeded`,
and `AllocationFailed`. Collection errors are `MutatorNotRegistered`,
`SafepointTimeout`, `PlanUnavailable`, `InvalidTrace`, and `CoordinatorBusy`.
`to_string` overloads supply readable text.

The enums are shared vocabulary; declaring an error does not mean every
implementation can produce it. For example, the generational plan uses
`PlanUnavailable` for failed promotion. Trace callbacks return a vector,
so they cannot directly return a `GCError` to built-in plans.

## Chapter 4 — Object lifetime and bump allocation

### 4.1 Reserve bytes, then create objects

An allocator returns an address, not a constructed C++ object. For ordinary
C++ objects, use `std::construct_at` with properly aligned storage and call
`std::destroy_at` before manually reclaiming their storage when destruction
is required.

Collectors do not call destructors for unreachable objects. A runtime with
resource-owning objects must implement its own destruction or finalization
policy. Small trivially destructible runtime records make initial
experiments straightforward.

### 4.2 Borrowed storage has a stable lifetime

Bump allocators borrow a region. An aligned array is useful for a fixed-size
arena. A vector can also supply storage, provided it is not resized or
otherwise moved after the allocator captures its address.

Multiple independent allocators must not manage the same region at the same
time unless your application explicitly partitions it. Each allocator only
tracks its own cursor or free list.

### 4.3 A complete arena example

```cpp
#include <memtkx/MemTKX.hpp>

#include <array>
#include <cassert>
#include <memory>
#include <string>

struct Message {
  std::string text;
};

int main() {
  alignas(std::max_align_t) std::array<MemTKX::Byte, 4096> storage{};
  const auto start = MemTKX::as_address(storage.data());
  MemTKX::BumpPointerAllocator arena(start, start + storage.size());

  auto allocation = arena.allocate(sizeof(Message), alignof(Message));
  assert(allocation.is_ok());
  auto* message = std::construct_at(
      MemTKX::from_address<Message>(allocation.unwrap()), Message{"hello"});
  assert(message->text == "hello");
  assert(arena.allocation_count() == 1);
  assert(arena.used_bytes() >= sizeof(Message));

  std::destroy_at(message);
  arena.reset();
  assert(arena.used_bytes() == 0);
  assert(arena.allocation_count() == 1);
}
```

The nested `std::string` allocation is handled by the standard library, not
by MemTKX. Destroying `Message` releases that resource before arena reset.

### 4.4 Cursor behavior and accounting

The constructor aligns the beginning upward and the end downward to
`alignof(std::max_align_t)`. Each request aligns the current cursor to its
requested alignment. Alignment padding consumes usable region capacity.

| Method | Meaning |
| --- | --- |
| `start()`, `limit()`, `cursor()` | Current allocator boundaries and cursor |
| `remaining()` | Bytes after the cursor |
| `used_bytes()` | Cursor distance from aligned start, including padding |
| `allocated_bytes()` | Sum of successful requested sizes |
| `allocation_count()` | Number of successful requests |
| `reset()` | Return the cursor to its start |
| `reset(start, end)` | Replace boundaries and reset the cursor |

Reset does not clear cumulative allocation counters. It also does not
zero memory or destroy objects. Treat all previous allocations as released
by your application before using the region again.

### 4.5 BumpSpace adds a descriptor

`BumpSpace(name, start, end)` wraps the allocator with `SpaceBase` metadata:
a name, kind, region, default alignment, and `SpaceMetrics`. Call
`allocate(bytes, alignment)` exactly as with the allocator.

The descriptor's default alignment is descriptive; `allocate` still needs
an explicit alignment argument. `BumpSpace::reset` resets the underlying
cursor, while its metrics remain accumulated. Do not interpret its
`metrics().live_bytes` as a fresh post-reset heap census.

**Learning task:** allocate objects with alignments 8 and 64 and compare
`used_bytes()` with `allocated_bytes()`. Their difference illustrates
alignment overhead.

## Chapter 5 — Free lists and individual reclamation

### 5.1 Free-list allocation

`FreeListAllocator` starts with one free cell describing the borrowed
region. Allocation searches cells in address order, aligns the selected
address, and preserves any prefix padding and remaining suffix as free
cells. Adjacent cells are coalesced after allocation and deallocation.

The implementation uses a vector of cells and sorts it. This is a clear
reference mechanism for individually reclaimable storage; its cost profile
differs from constant-time bump allocation.

### 5.2 Free the exact allocation

`free(address, bytes)` requires the same object address and requested size
that were returned by allocation. The allocator checks region bounds, but
does not maintain an ownership table that detects double frees or wrong
sizes. Your runtime must associate each live allocation with its size.

```cpp
#include <memtkx/MemTKX.hpp>

#include <array>
#include <cassert>

int main() {
  alignas(std::max_align_t) std::array<MemTKX::Byte, 4096> storage{};
  const auto start = MemTKX::as_address(storage.data());
  MemTKX::FreeListSpace heap("objects", start, start + storage.size());

  auto first = heap.allocate(128, 16);
  auto second = heap.allocate(256, 16);
  assert(first.is_ok() && second.is_ok());
  assert(heap.metrics().live_bytes == 384);

  const bool freed_first = heap.deallocate(first.unwrap(), 128);
  const bool freed_second = heap.deallocate(second.unwrap(), 256);
  assert(freed_first && freed_second);
  assert(heap.metrics().live_bytes == 0);
  assert(heap.allocator().free_bytes() == storage.size());
  assert(heap.allocator().free_cells() == 1);
}
```

This example allocates byte ranges, so no object construction or destruction
is needed. Applications storing objects would add the lifetime operations
from Chapter 4.

### 5.3 Metrics and reset

`free_bytes()` sums available cells. A large total does not guarantee that
a single large request will fit: fragmentation may split it across cells.
`free_cells()` helps characterize that fragmentation.

The allocator's `allocation_count()` and `allocated_bytes()` are cumulative
between resets. `FreeListAllocator::reset(start, end)` clears its cells and
allocation counters. `FreeListSpace::reset()` resets the allocator but does
not reset the wrapper's `SpaceMetrics`.

`SpaceMetrics` records requested allocation bytes and successful
deallocation events. Padding is not charged as requested object size.
Metrics are useful observations, but allocator capacity should be queried
through the allocator when making allocation decisions.

### 5.4 Segregated free-list allocation

`SegregatedFreeListAllocator` partitions its region into eight independent
subregions. Its size-class thresholds are:

```text
16, 32, 64, 128, 256, 512, 1024, 4096 bytes
```

For a request, it tries the first class large enough, then larger classes
if needed. It allocates the requested byte count inside the chosen
subregion; it does not round the allocation to a fixed-size slab slot.

The first seven classes receive equal-sized partitions and the last
receives any remainder. Memory is not dynamically transferred between
classes. Requests larger than 4096 bytes cannot use these classes even if
the overall region is much larger.

`free` selects the partition by the allocation address and forwards the
original byte count. A small request can therefore be freed correctly even
when it originally fell back to a larger class.

### 5.5 Choosing a mechanism

Use a bump arena when all allocations have one batch lifetime. Use a free
list when object lifetimes differ and individual reuse matters. Consider
segregated partitions when your workload has a predictable distribution
of request sizes. Measure capacity per partition, rather than assuming
total free space is shared freely among classes.

**Learning task:** allocate three cells, free the first and third, and try
a request that fits their combined sizes but not either individual cell.
Then free the middle cell and repeat the request to observe coalescing.

## Chapter 6 — Immix and large-object spaces

### 6.1 Immix layout

`ImmixSpace` owns its backing vector and creates fixed-size blocks:

| Constant | Value |
| --- | --- |
| `kImmixBlockBytes` | 32 KiB |
| `kImmixLineBytes` | 256 bytes |
| `kImmixLinesPerBlock` | 128 |

Storage includes alignment padding and whole-block rounding. The actual
block count can exceed the minimum implied by the requested byte count.
`region().size()` describes usable blocks; `total_bytes()` includes the
owned storage allocation and padding.

Each block currently allocates with a forward cursor. Allocation marks
the lines touched by the request. Objects cannot span blocks through a
single request, and requests larger than one block return
`LargeObjectExceeded`.

### 6.2 Marks describe lines

`clear_marks()` clears the line marks without moving allocation cursors.
`mark(address, bytes)` marks lines occupied by a known object.
`marked_bytes()` counts marked lines times 256, not the exact sum of object
sizes. A tiny object still occupies a marked line; objects sharing a line
contribute to the same mark.

`reset_blocks()` clears cursors and marks for the whole space. It releases
all allocations from the application's perspective. It does not destroy
objects or reset the inherited space metrics.

### 6.3 A marking experiment

```cpp
#include <memtkx/MemTKX.hpp>

#include <cassert>

int main() {
  MemTKX::ImmixSpace space(MemTKX::kImmixBlockBytes * 2);
  auto object = space.allocate(300, MemTKX::kImmixLineBytes);
  assert(object.is_ok());
  assert(space.contains(object.unwrap()));
  assert(space.block_count() >= 2);

  space.clear_marks();
  assert(space.marked_bytes() == 0);
  const bool marked = space.mark(object.unwrap(), 300);
  assert(marked);
  assert(space.marked_bytes() == 2 * MemTKX::kImmixLineBytes);

  space.reset_blocks();
  assert(space.used_bytes() == 0);
}
```

The 300-byte object begins on a line boundary and touches two lines.
Use nonzero sizes and exact object extents when marking; the low-level
block methods are intended for trusted region metadata.

### 6.4 LargeObjectSpace

`LargeObjectSpace(name, start, end, threshold)` borrows a region and uses
a bump allocator. The default threshold is 4096 bytes. Requests below the
threshold or above the entire region size return `LargeObjectExceeded`.
Accepted requests may still fail with `OutOfMemory` when remaining bump
capacity is insufficient.

```cpp
#include <memtkx/MemTKX.hpp>

#include <array>
#include <cassert>

int main() {
  alignas(std::max_align_t) std::array<MemTKX::Byte, 16384> storage{};
  const auto start = MemTKX::as_address(storage.data());
  MemTKX::LargeObjectSpace large("large", start,
                               start + storage.size(), 4096);
  assert(large.allocate(128, 16).is_err());

  auto object = large.allocate(8192, 16);
  assert(object.is_ok());
  const auto capacity_after_allocation = large.allocator().remaining();
  const bool accounted = large.deallocate(object.unwrap(), 8192);
  assert(accounted);
  assert(large.metrics().live_bytes == 0);
  assert(large.allocator().remaining() == capacity_after_allocation);

  large.reset();
  assert(large.allocator().used_bytes() == 0);
}
```

Individual `deallocate` updates accounting but does not return a hole to
the bump allocator. Whole-space reset is the available reuse mechanism.
The implementation does not maintain an object ownership table, so call
deallocation only for your known live allocations.

### 6.5 Routing small and large requests

The application can route requests by size to separate spaces. For
example, a small-object region may use a free list while requests at least
4 KiB go to a large-object region. Allocate separate backing buffers or
non-overlapping subregions.

The built-in plans do not automatically route to a separate large-object
space. If you add routing, your trace and root logic must also understand
which manager owns an object. Chapter 13 shows how to wrap allocation policy
without changing the MemTKX interfaces.

## Chapter 7 — Roots, tracing, and object graphs

### 7.1 Reachability is a runtime contract

A root is an object address that must remain reachable independently of
other heap objects. Typical root sources are VM registers, globals, handles,
and references stored in paused execution frames.

`TraceFn` has this signature:

```text
std::vector<MemTKX::Address>(MemTKX::Address object)
```

It returns the addresses of the object's outgoing references. It returns
values, not writable reference slots. This distinction is important for
moving collectors, discussed in Chapter 8.

Built-in plans use their tracked allocation tables to recognize object
starts. Interior addresses are not treated as roots of the containing
object. Convert an interior pointer to its owning object start in your
runtime if that form of reference is supported.

### 7.2 A complete two-node graph

This example allocates two connected nodes and an unreachable node. It
checks that a root keeps its child alive, then removes that edge and
collects again.

```cpp
#include <memtkx/MemTKX.hpp>

#include <cassert>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <vector>

struct Node {
  MemTKX::Address next{0};
  int value{0};
};

static_assert(std::is_trivially_destructible_v<Node>);

int main() {
  MemTKX::MarkSweepPlan heap(4096);
  auto make_node = [&](int value) {
    auto allocated = heap.allocate(sizeof(Node), alignof(Node));
    if (allocated.is_err()) {
      throw std::runtime_error("node allocation failed");
    }
    const auto address = allocated.unwrap();
    std::construct_at(MemTKX::from_address<Node>(address), Node{0, value});
    return address;
  };

  const auto root = make_node(10);
  const auto child = make_node(20);
  (void)make_node(30);
  MemTKX::from_address<Node>(root)->next = child;

  MemTKX::TraceFn trace = [](MemTKX::Address address) {
    const auto* node = MemTKX::from_address<const Node>(address);
    if (node->next == 0) {
      return std::vector<MemTKX::Address>{};
    }
    return std::vector<MemTKX::Address>{node->next};
  };

  auto first = heap.collect({root}, trace);
  assert(first.is_ok());
  assert(first.unwrap().live_objects == 2);
  assert(first.unwrap().freed_objects == 1);
  assert(MemTKX::from_address<Node>(child)->value == 20);

  MemTKX::from_address<Node>(root)->next = 0;
  auto second = heap.collect({root}, trace);
  assert(second.is_ok());
  assert(second.unwrap().live_objects == 1);
  assert(second.unwrap().freed_objects == 1);

  auto last = heap.collect({}, trace);
  assert(last.is_ok());
  assert(heap.live_objects() == 0);
}
```

The `Node` fields are trivially destructible; the example has no external
resource to finalize. Raw local variables named `child` or `root` do not
keep objects alive by themselves. Only addresses supplied in the roots or
returned through reachable edges participate in collection.

### 7.3 Cycles and duplicate references

Mark-sweep maintains a visited set. Cycles terminate and duplicate edges
do not cause repeated scanning of an already marked object. A cycle with
no path from a root is unreachable and can be freed.

For a tagged runtime, the callback can inspect an object header and switch
on its layout. Return only managed references; integers and unmanaged
pointers should not be mistaken for object addresses. An empty `TraceFn`
causes plans to skip child scanning, which is appropriate only for objects
without outgoing references or for a deliberately flat experiment.

### 7.4 Allocation failure and explicit retry

A common embedding policy is:

```text
try allocation
if it fails because of capacity:
    publish current roots and pause mutation
    run collection
    retry allocation once
if it still fails:
    return the allocation failure to the application
```

Keep the newly requested object out of tracing until its header and
reference fields have been initialized. Ensure objects being constructed
are represented by roots if construction can invoke collection.

### 7.5 Trace callback discipline

Treat scanning as a read-only operation over a quiescent graph. Avoid
allocating recursively into the same plan while it is traversing or
sweeping. The callback may allocate a returned vector on the native heap,
so account for that cost when measuring collection performance.

Exceptions from trace callbacks are not translated into a `GcResult` by
the built-in plans. If your runtime uses exceptions, handle them at the
collection boundary and maintain its parking/release protocol accordingly.

**Learning task:** change the example into a two-node cycle. Run collection
first with one root, then with no roots, and compare live and freed counts.

## Chapter 8 — Collection plans and statistics

### 8.1 The common plan interface

`PlanBase<Derived>` exposes `kind()`, `name()`, and
`collect(roots, trace)`. Concrete plans also expose
`allocate(bytes, alignment)`. Plan kinds are `MarkSweep`, `SemiSpace`,
`Immix`, and `Generational`; `to_string(kind)` supplies a short label.

`collect` returns `GcResult<PlanStats>`. Statistics have these fields:

| Field | Meaning |
| --- | --- |
| `live_objects` | Reachable/surviving objects reported by this collection |
| `live_bytes` | Sum of their recorded requested sizes |
| `freed_objects` | Objects explicitly reported as freed |
| `freed_bytes` | Requested bytes explicitly reported as freed |
| `collection_count` | Successful collection count maintained by the plan |

Not every plan fills every field. The freed counters are currently populated
by mark-sweep; the other built-in plans leave them at their default zero.
Requested bytes exclude allocator padding and native-heap bookkeeping.

### 8.2 MarkSweepPlan

`MarkSweepPlan(heap_bytes)` owns a byte vector and a free-list allocator.
Successful allocations are recorded with their requested size. Collection
walks from roots, traces tracked object starts, and returns unreachable
allocations to the free list.

Survivor addresses remain stable. Repeated collection is therefore usable
with the graph model in Chapter 7. `live_objects()` and `live_bytes()`
describe currently tracked allocations; before collection, those counts
can include objects that are no longer reachable.

Public `mark(address)` and `is_marked(address)` support mark inspection,
but `collect` starts by clearing marks and clears them again after sweeping.
Pre-marking is not a way to preserve an object across `collect`.

### 8.3 SemiSpacePlan

`SemiSpacePlan(semi_space_bytes)` owns twice the supplied size: two equal
regions. Allocation initially uses the from-space bump allocator.
Collection resets to-space, copies reachable tracked objects with
`std::memcpy`, records internal forwarding, then swaps the allocators.

The trace callback scans old object addresses while copying is underway.
The plan neither rewrites root values nor rewrites pointer fields in the
copies, and it does not expose the forwarding map. Consequently, a caller
cannot obtain a usable relocated graph through this interface alone.
Do not carry the original addresses into another collection as if they
identified the copied objects.

The implementation also copies with `alignof(std::max_align_t)` rather
than preserving each object's requested alignment. It returns zero from
its internal forwarding operation for an unknown object or a failed
destination allocation, without returning a collection error for that
case. A successful result therefore does not certify that a richer moving
runtime's relocation requirements were satisfied.

The following intentionally demonstrates a single copying cycle and reads
only statistics after it:

```cpp
#include <memtkx/MemTKX.hpp>

#include <cassert>
#include <cstring>
#include <vector>

int main() {
  MemTKX::SemiSpacePlan heap(4096);
  auto allocated = heap.allocate(64, alignof(std::max_align_t));
  assert(allocated.is_ok());
  std::memset(MemTKX::from_address<void>(allocated.unwrap()), 0, 64);

  auto collected = heap.collect({allocated.unwrap()},
      [](MemTKX::Address) { return std::vector<MemTKX::Address>{}; });
  assert(collected.is_ok());
  assert(collected.unwrap().live_objects == 1);
  assert(collected.unwrap().live_bytes == 64);
  // The old allocation address is not used after copying.
}
```

### 8.4 ImmixPlan

`ImmixPlan(heap_bytes)` owns an `ImmixSpace` and tracks allocation sizes.
Collection clears prior marks, visits reachable objects, and marks their
lines. `space()` returns a const reference for observing block, usage, and
mark statistics.

This plan currently does not remove unreachable entries from its object
table, sweep unmarked lines, or reset block cursors during collection.
Collection does not restore allocation capacity. Its `live_bytes` reports
reachable requested bytes, while `space().marked_bytes()` reports occupied
line capacity and `space().used_bytes()` reflects cursor consumption.

Those three numbers answer different questions. Comparing them is useful
for studying line-level fragmentation and the effect of object sizes.

### 8.5 GenerationalPlan

`GenerationalPlan(nursery_bytes, mature_bytes)` owns two buffers. New
objects use nursery bump allocation. `minor_collect` traces reachable
tracked objects, copies them into mature free-list storage, replaces the
object-size table with copied survivors, resets the nursery, and clears
its card table. `collect` delegates to `minor_collect`.

`remember(slot_address)` marks a card, but minor collection does not scan
dirty cards as additional roots. It also does not expose forwarding or
rewrite roots and fields. There is no mature-space sweep. The copying loop
operates on reachable tracked objects, including mature survivors if they
were addressed through a future integration that recovered their new
addresses; it is not a complete age-aware promotion policy.

A failed mature allocation returns `GCError::PlanUnavailable`. Earlier
copies in that attempt are not rolled back. `nursery_live_bytes()` sums
tracked requested bytes whose current addresses lie in the nursery.

### 8.6 Plan selection in practice

| Goal | Starting point |
| --- | --- |
| Non-moving graph with reclaimed unreachable objects | `MarkSweepPlan` |
| Batch-lifetime objects | A bump allocator without a collection plan |
| Study forwarding and copying | `SemiSpacePlan` with single-cycle experiments |
| Measure reachable line occupancy | `ImmixPlan` |
| Study nursery-to-mature copying | `GenerationalPlan` |

Adding a full moving runtime requires a writable-slot trace interface or
handles, an observable relocation protocol, alignment tracking, and
defined failure handling. These are runtime design tasks beyond simply
choosing a plan name.

Owning plans and spaces contain internal addresses into their own buffers.
Keep them at a stable location and do not copy them to obtain independent
heaps. Construct an independent instance instead.

## Chapter 9 — Write barriers and remembered information

### 9.1 Why record writes?

A barrier records information that a collector's policy needs when a
reference changes. An old-to-young store may require a remembered slot;
a concurrent snapshot algorithm may need the overwritten reference.
MemTKX provides these recording mechanisms independently of the plans.

`Slot` contains `Address* slot_address` and `Address target_object`.
`ReadSlot` contains a const slot pointer and a loaded value. A slot pointer
identifies the field that is being changed; the target identifies the
referenced object. They are different addresses.

### 9.2 The range-filter pipeline

`run_write_barrier` checks whether the slot address lies in a source region
and the target value lies in a target region, then appends the slot to a
modification buffer. It records information but does not perform the
actual store.

```cpp
#include <memtkx/MemTKX.hpp>

#include <cassert>
#include <vector>

int main() {
  MemTKX::Address field = 0;
  const auto field_address = MemTKX::as_address(&field);
  const MemTKX::AddressRange source{field_address,
                                   field_address + sizeof(field)};
  const MemTKX::AddressRange target{0x1000, 0x3000};
  std::vector<MemTKX::Slot> modified;
  MemTKX::BarrierDSL barrier;

  MemTKX::run_write_barrier(barrier, {&field, 0x2000},
                            source, target, modified);
  assert(field == 0);
  field = 0x2000;
  assert(modified.size() == 1);
  assert(modified.front().slot_address == &field);
}
```

The numeric target values in this example are illustrative; they are never
dereferenced. In a runtime, use the actual source and target heap regions.

### 9.3 Stage types matter

The pipeline utility calls one stage after another; it does not implicitly
unwrap optional values. Most filter helpers accept `Slot` and return
`std::optional<Slot>`. `barrier_record` and `barrier_discard` accept an
optional slot.

| Helper | Transformation |
| --- | --- |
| `barrier_non_null()` | Slot → optional slot |
| `barrier_filter_range(region)` | Slot → optional slot, checking slot location |
| `barrier_filter_target_range(region)` | Slot → optional slot, checking target |
| `barrier_cross_region(source, target)` | Slot → optional slot, checking both |
| `barrier_saturating_filter(source, target)` | Slot → optional slot, checking both |
| `barrier_record(buffer)` | Optional slot → recording side effect |
| `barrier_discard()` | Optional slot → no recording |

Do not chain two Slot-consuming filters directly. Combine their predicates
in one stage or write an optional-aware intermediate stage. The provided
`run_write_barrier` already uses a type-correct combined filter.

### 9.4 Card tables

`CardTable(base, region_bytes, card_bytes = 512)` maps an address to a card
relative to a region base. `mark`, `clear`, `clear_all`, `is_marked`, and
`marked_cards` operate on these records. Out-of-region addresses are
ignored. A zero card size creates a table with no cards.

`CardTableBarrier(table)` is callable with a slot pointer and a new value.
It marks the slot's card and ignores the new value; it does not write the
field. Cards are bytes in a vector, not atomic synchronization objects.

### 9.5 SATB captures the old reference

`SatbBarrier::capture(slot)` reads and queues a nonzero old reference.
`write(slot, new_value)` captures before overwriting. `SatbQueue` supports
`size`, `empty`, `clear`, and `take_entries`; taking entries transfers the
vector and empties the queue.

```cpp
#include <memtkx/MemTKX.hpp>

#include <cassert>

int main() {
  MemTKX::Address field = 77;
  const auto address = MemTKX::as_address(&field);
  MemTKX::CardTable cards(address, sizeof(field), 512);
  MemTKX::CardTableBarrier card_barrier(cards);
  MemTKX::SatbQueue old_references;
  MemTKX::SatbBarrier satb(old_references);

  card_barrier(&field, 88);
  assert(field == 77 && cards.is_marked(address));
  satb.write(&field, 88);
  assert(field == 88);

  auto captured = old_references.take_entries();
  assert(captured.size() == 1 && captured.front() == 77);
  assert(old_references.empty());
}
```

### 9.6 Connecting records to a policy

Recording information is only half the mechanism. Your collector must
consume modified slots, dirty cards, or SATB entries at the right phase.
Built-in plans do not automatically connect these queues to their trace
loops. Use a collector adapter or a phase to combine the recorded
information with application roots.

Use per-mutator buffers or external synchronization for shared recorders.
The barrier buffers and SATB queues themselves are not thread-safe.

## Chapter 10 — Work queues and phase pipelines

### 10.1 Work packets

`WorkPacket` contains a `WorkKind`, an object address, and an `extra` size
field. Kinds include `RootScan`, `ProcessEdge`, `Mark`, `Sweep`, `Release`,
and `User`.

`WorkQueue` is a FIFO backed by a deque. It provides `push`, `try_pop`,
`empty`, `size`, `clear`, and `drain(fn)`. Draining keeps popping until the
queue is empty, including work added by the callback. It returns the
number of processed packets.

The queue is local work storage, not a worker pool or a work-stealing
scheduler. Sharing it between threads requires external synchronization.

### 10.2 Collection phases

`make_phase(name, function, stop_on_error = true)` wraps a callable taking
`PhaseState&` and returning `GcResult<int>`. Compose phases with `|` or
append them to a `TaskPipeline`.

`PhaseState` records the active phase name, a completed-step counter, and
the elapsed duration of the most recently executed phase. The pipeline
resets `completed_steps` at the beginning of each run; phases increment
it explicitly with `note_completion`.

The integer returned by an individual phase is not automatically added to
the counter. A successful pipeline returns the counter value converted to
`int`, not the last phase's return value.

### 10.3 A queue-processing pipeline

```cpp
#include <memtkx/MemTKX.hpp>

#include <cassert>

int main() {
  MemTKX::WorkQueue queue;
  queue.push({MemTKX::WorkKind::RootScan, 10, 0});
  queue.push({MemTKX::WorkKind::Mark, 20, 0});

  auto scan = MemTKX::make_phase("scan", [&](MemTKX::PhaseState& state) {
    const auto processed = queue.drain([](const MemTKX::WorkPacket& packet) {
      assert(packet.object != 0);
    });
    state.note_completion(processed);
    return MemTKX::GcResult<int>::from_ok(static_cast<int>(processed));
  });
  auto report = MemTKX::make_phase("report", [](MemTKX::PhaseState& state) {
    assert(state.completed_steps == 2);
    return MemTKX::GcResult<int>::from_ok(0);
  });

  auto pipeline = scan | report;
  MemTKX::PhaseState state;
  auto result = pipeline.run(state);
  assert(result.is_ok() && result.unwrap() == 2);
  assert(state.phase_name == "report");
  assert(pipeline.size() == 2 && queue.empty());
}
```

The illustrative packet addresses are not dereferenced. A real marking
phase would dispatch by kind and inspect managed objects.

### 10.4 Error and cleanup policy

A phase error stops the pipeline if that phase has `stop_on_error=true`.
When it is false, the pipeline proceeds, and a later successful completion
returns an overall success. The pipeline does not retain an automatic log
of ignored errors; capture those in your application if needed.

There is no automatic finally-phase execution. If a phase stopped the
world and a later phase fails, a subsequent release phase can be skipped.
Arrange release with an application scope guard or an explicit cleanup
path that runs independently of phase success.

### 10.5 Orchestration does not imply parallelism

`TaskPipeline` runs phases sequentially. Its elapsed field is overwritten
for each phase, so it is not a complete cycle timer. Surround the entire
`run` call with your own steady-clock measurement if you want total
collection duration.

The DSL toolkit also contains a separate task-chain API. The
memory-management scheduler described here uses `MemTKX::Phase`,
`MemTKX::TaskPipeline`, and `MemTKX::PhaseState`; use fully qualified names
when working with both systems.

## Chapter 11 — Mutators and safepoint coordination

### 11.1 Thread records and root discovery

A mutator is an application thread that can change the managed graph.
`CollectorCoordinator(expected_mutators = 1)` keeps registered
`ThreadRecord` objects under a mutex.

Each record has an application-assigned `ThreadId`, name, status, optional
`StackScanCallback`, and optional `RootProvider`. The callback signatures
are:

```text
RootProvider:       std::vector<Address>()
StackScanCallback:  std::vector<Address>(ThreadId)
```

Registration rejects duplicate IDs. Unregistration removes a record.
`registered_mutators()` reports the count. Querying an unknown ID's status
returns `Blocked`.

### 11.2 Status is cooperative metadata

Statuses are `Running`, `Polling`, `AtSafepoint`, and `Blocked`.
`update_status` changes a record and notifies waiters. The stop-the-world
predicate requires at least the configured expected number of records and
requires **every registered record** to be `AtSafepoint`.

`Blocked` does not count as a safepoint for that predicate. Merely updating
status does not physically pause a thread: your embedding must ensure it
has actually stopped touching managed memory.

### 11.3 Collector and mutator contexts

`CollectorContext` wraps `stop_the_world`, `roots`, and
`release_mutators`. The default stop timeout is two seconds.
`MutatorContext` stores a coordinator and thread ID; `poll()` calls
`poll_safepoint(id)`.

The coordinator uses a generation counter. A stop request increments the
generation before waiting. Polling marks the mutator at a safepoint, then
waits up to five seconds for a generation change. Releasing increments the
generation and sets registered statuses to `Running`.

Because a stop request also changes the generation, a poll already waiting
before that request can return due to the request rather than the later
release. Polling also blocks without checking an application-level
"collection requested" flag. A runtime needs an explicit request/parking
protocol around these primitives; `poll()` alone is not a complete
stop-the-world handshake.

### 11.4 A deterministic application-managed park

This example uses C++20 latches to enforce actual parking. The mutator
publishes its status and then cannot touch managed memory until the
application release latch opens. The collector uses coordinator metadata
and root callbacks while that gate is closed.

```cpp
#include <memtkx/MemTKX.hpp>

#include <cassert>
#include <latch>
#include <thread>
#include <vector>

int main() {
  MemTKX::CollectorCoordinator coordinator(1);
  MemTKX::ThreadRecord record;
  record.id = 1;
  record.name = "application-mutator";
  record.root_provider = [] { return std::vector<MemTKX::Address>{123}; };
  const bool registered = coordinator.register_mutator(record);
  assert(registered);

  std::latch parked(1);
  std::latch resume(1);
  std::thread mutator([&] {
    // Managed writes finish before publishing the safepoint.
    coordinator.update_status(1, MemTKX::MutatorStatus::AtSafepoint);
    parked.count_down();
    resume.wait();
    // Managed writes could resume after this application gate opens.
  });

  parked.wait();
  MemTKX::CollectorContext collector(coordinator);
  auto stopped = collector.stop_the_world();
  assert(stopped.is_ok());
  const auto roots = collector.roots();
  assert(roots.size() == 1 && roots.front() == 123);

  collector.release_mutators();
  resume.count_down();
  mutator.join();
  const bool removed = coordinator.unregister_mutator(1);
  assert(removed);
}
```

The root is an illustrative value; this program demonstrates coordination
without dereferencing it. In a runtime, invoke your plan's collection with
the gathered real roots while the mutators remain parked.

### 11.5 Root callback execution

`collect_roots()` invokes both stack and root callbacks while holding the
coordinator mutex and concatenates their results. Callbacks must not call
back into coordinator methods that acquire the same mutex. Keep them short
and read from already published, stable root data.

Root discovery does not itself verify that mutators are stopped. Call it
inside your application's established quiescent interval. If collection
fails or throws, release the actual parked threads through the same
application cleanup path used for success.

### 11.6 Thread-safety boundaries

The coordinator's record operations are mutex protected. Allocators,
plans, work queues, card tables, and SATB buffers generally are not. A
coordinator does not automatically serialize allocation or field writes.
Use per-thread allocation state or a runtime lock as appropriate, and
serialize collection against every operation that mutates plan metadata.

**Learning task:** add a second parked thread and set `expected_mutators`
to two. Give each thread a distinct root provider and observe the combined
root vector without assuming callback iteration order.

## Chapter 12 — The MemTKX DSL toolkit

### 12.1 Namespace and CRTP composition

Include `<memtkx/MemTKXDSL.hpp>` and optionally define a local alias:
`namespace dsl = MemTKX::dsl;`. The alias is a convenience for your own
translation unit; the library does not create a global `dsl` namespace.

The base `DSL<Derived, Features...>` inherits each feature's
`Mixin<Derived>`. This supplies reusable behavior without a virtual base
interface. Existing features include `Pipeline`, `Operators`, `PatternMatch`,
`AST`, `Rewrite`, `ExprTemplates`, `CustomLiterals`, and utility feature tags.

### 12.2 Pipelines and predicates

`pipe(callable)` creates a stage. The `|` operator invokes the stage on
its left-hand value. Return types must match the next stage's input type.
`predicate(callable)` supplies `&`, `|`, and `!` for composing predicates.

```cpp
#include <memtkx/MemTKXDSL.hpp>

#include <cassert>

namespace dsl = MemTKX::dsl;

struct Numbers : dsl::DSL<Numbers, dsl::Pipeline, dsl::Operators> {};

int main() {
  Numbers numbers;
  auto result = numbers.wrap(10)
      | dsl::pipe([](int value) { return value * 2; })
      | dsl::pipe([](int value) { return value + 1; });
  assert(result == 21);

  auto positive = dsl::predicate([](int value) { return value > 0; });
  auto even = dsl::predicate([](int value) { return value % 2 == 0; });
  auto accepted = positive & even;
  assert(accepted(4) && !accepted(-2) && !accepted(3));
}
```

Pipeline composition is ordinary callable invocation. It does not supply
automatic optional propagation, exception conversion, or asynchronous
execution.

### 12.3 Dispatch tables

Use `match(when<Key>(handler), ..., otherwise(handler))` for value-based
dispatch. Each handler receives the key plus any extra arguments supplied
to the table. Use compatible return types across handlers and prefer an
explicit fallback.

```cpp
#include <memtkx/MemTKXDSL.hpp>

#include <cassert>

namespace dsl = MemTKX::dsl;
enum class State { Unmarked, Marked, Other };

int main() {
  auto transition = dsl::match(
      dsl::when<State::Unmarked>([](State, int) { return State::Marked; }),
      dsl::when<State::Marked>([](State, int) { return State::Marked; }),
      dsl::otherwise([](State state, int) { return state; }));
  assert(transition(State::Unmarked, 0) == State::Marked);
  assert(transition(State::Other, 0) == State::Other);
}
```

If no clause matches and there is no fallback, the table throws a runtime
error. Handlers should return values rather than `void` for this table API.

String-pattern tags use `FixedString` non-type template arguments. Because
`when` takes an `auto` key, pass a pattern object such as
`dsl::pattern<"[0-9]+">{}` as the template argument. The matcher implements
a limited regex-like subset, not a general regular-expression engine.

### 12.4 AST construction and rewrite rules

`leaf<"tag">(value)` converts a leaf value to a string. `node<"tag">(...)`
creates an inner node with children. `ASTNode` is a value type containing
strings and a vector; copying a tree can allocate native-heap memory.
`dump()` produces an S-expression representation.

`rule<"name">(predicate, transformer)` describes one rewrite.
`rewrite_set(...)` applies rules bottom-up and repeats until no rule fires
or `max_iterations` is reached; the default limit is 100.

```cpp
#include <memtkx/MemTKXDSL.hpp>

#include <cassert>

namespace dsl = MemTKX::dsl;

int main() {
  auto tree = dsl::node<"identity">(dsl::leaf<"number">(42));
  auto rules = dsl::rewrite_set(dsl::rule<"remove-identity">(
      [](const dsl::ASTNode& node) {
        return node.tag() == "identity" && node.children().size() == 1;
      },
      [](const dsl::ASTNode& node) { return node.child(0); }));
  auto simplified = rules.apply(tree);
  assert(simplified.dump() == "(number 42)");
}
```

Ensure transformations terminate or deliberately bound iterations. A rule
whose predicate stays true even when its transformation makes no semantic
change can consume the iteration limit.

### 12.5 Results, optional values, lazy evaluation, and memoization

`Result<T, E>` supplies the result methods described in Chapter 3. Use
distinct success and error types for inspecting branches: the current
implementation uses type-based access to its variant, which is ambiguous
when `T` and `E` are the same type.

`Maybe<T>` wraps optional values and provides `map`, `flat_map`, `filter`,
and `or_else`. Free-function optional helpers are also available.
`Lazy<T>` evaluates a producer on the first `get()` or `force()`, caches
the result, and returns a reference. `is_computed()` reports whether that
has occurred.

`memoize<Key, Value>(function)` caches values by a hashable key and exposes
`clear_cache()`. These wrappers are not synchronized. Use memoization for
pure computations with a defined cache lifetime, not for heap addresses
whose meaning changes after collection.

```cpp
#include <memtkx/MemTKXDSL.hpp>

#include <cassert>

namespace dsl = MemTKX::dsl;

int main() {
  int computations = 0;
  auto square = dsl::memoize<int, int>([&](int value) {
    ++computations;
    return value * value;
  });
  assert(square(7) == 49);
  assert(square(7) == 49 && computations == 1);
  square.clear_cache();
  assert(square(7) == 49 && computations == 2);

  dsl::Lazy<int> deferred([] { return 42; });
  assert(!deferred.is_computed());
  assert(deferred.get() == 42 && deferred.is_computed());
  assert(dsl::Maybe<int>(5).map([](int value) { return value + 1; })
             .or_else(0) == 6);
}
```

### 12.6 Parser combinators

`Parser<T>` wraps a callable from `ParsecInput&` to `ExpectedResult<T>`.
`ch` matches a character, `satisfy` matches a predicate, `&` builds a
sequence, `|` builds ordered choice, and unary `*` repeats a parser.
`optional`, `try_parse`, and `labeled` add optionality, backtracking, and
readable expectations.

`run_parser(parser, source)` requires full input consumption. It returns
a `ParseOutcome<T>` with an optional value and a `ParseError` describing
the failure position, kind, and expectations.

```cpp
#include <memtkx/MemTKXDSL.hpp>

#include <cassert>

namespace dsl = MemTKX::dsl;

int main() {
  auto pair = dsl::ch('a') & dsl::ch('b');
  auto good = dsl::run_parser(pair, "ab");
  assert(good.value.has_value());
  assert(good.value->first == 'a' && good.value->second == 'b');

  auto trailing = dsl::run_parser(pair, "abc");
  assert(!trailing.value.has_value());
  assert(trailing.error.pos == 2);

  auto alternative = dsl::try_parse(pair) | (dsl::ch('a') & dsl::ch('c'));
  assert(dsl::run_parser(alternative, "ac").value.has_value());
}
```

Sequence failures after consumption can be committed; ordered choice does
not try its second branch after a committed failure unless the first is
wrapped with `try_parse`. Repeated parsers must consume input on success;
repeating a parser that succeeds without advancing can loop indefinitely.

The header also provides runtime PEG definitions, channels, matchers, and
PEG combinator adapters. Inspect that subsystem's documented API for grammar
extensions; it is separate from heap allocation and collection.

### 12.7 Expression templates and feature tags

`ExprTemplates` builds lazy arithmetic trees around a derived type's
`expr_value()`. Calling `eval()` evaluates the tree. Operands are stored
by value, so account for copies of your derived type. It does not make
arbitrary vector storage support elementwise arithmetic automatically.

Several feature tags, including `ResultFeature`, `LazyFeature`, and
`CombinatorParser`, are integration markers with empty mixins. The useful
operations are supplied by their corresponding utility classes and free
functions. Feature composition does not install a runtime plugin.

## Chapter 13 — Plugins and custom extensions

### 13.1 What a plugin means in MemTKX

A MemTKX plugin is an application-defined C++ component that conforms to
an existing extension point. It can be a space, plan adapter, trace callback,
barrier wrapper, collection phase, or DSL mixin. It is compiled into the
application and selected explicitly.

There is no runtime shared-library discovery, manifest format, plugin
registry, or stable binary ABI. A plugin normally ships as a header library
with a CMake interface target and a declared dependency on MemTKX.

| Extension point | Application responsibility |
| --- | --- |
| `SpaceBase<Derived>` | Allocation implementation, ownership, and metrics |
| `PlanBase<Derived>` | Plan identity and collection implementation |
| `TraceFn` | Object-layout scanning |
| Root/stack callbacks | Published root discovery |
| `Phase::Function` | One explicit collection phase |
| Barrier stages | Write-recording or filtering policy |
| DSL feature `Mixin<Derived>` | Compile-time reusable behavior |

### 13.2 A quota-controlled space plugin

This plugin borrows storage and delegates allocation to a bump allocator.
It limits successful requested bytes independently of alignment padding.
It records space metrics through the protected base helpers.

```cpp
#include <memtkx/MemTKX.hpp>

#include <array>
#include <cassert>
#include <string>
#include <utility>

namespace application {

class QuotaSpace final : public MemTKX::SpaceBase<QuotaSpace> {
 public:
  QuotaSpace(std::string name, MemTKX::Address start, MemTKX::Address end,
             std::size_t quota)
      : MemTKX::SpaceBase<QuotaSpace>(MemTKX::SpaceDescriptor{
            std::move(name), MemTKX::SpaceKind::BumpPointer,
            {start, end}, alignof(std::max_align_t)}),
        allocator_(start, end), quota_(quota) {}

  MemTKX::AllocResult<MemTKX::Address> allocate_impl(
      std::size_t bytes, std::size_t alignment) {
    if (bytes > quota_ - charged_) {
      return MemTKX::AllocResult<MemTKX::Address>::from_err(
          MemTKX::AllocError::OutOfMemory);
    }
    auto result = allocator_.allocate(bytes, alignment);
    if (result.is_ok()) {
      charged_ += bytes;
      record_allocation(bytes);
    }
    return result;
  }

  std::size_t charged_bytes() const noexcept { return charged_; }

  void reset() noexcept {
    allocator_.reset();
    charged_ = 0;
    metrics_.reset();
  }

 private:
  MemTKX::BumpPointerAllocator allocator_;
  std::size_t quota_;
  std::size_t charged_{0};
};

}  // namespace application

int main() {
  alignas(std::max_align_t) std::array<MemTKX::Byte, 4096> storage{};
  const auto start = MemTKX::as_address(storage.data());
  application::QuotaSpace space("quota", start, start + storage.size(), 128);
  assert(space.allocate(96, 16).is_ok());
  assert(space.allocate(64, 16).is_err());
  assert(space.charged_bytes() == 96);
  assert(space.metrics().total_allocations == 1);
  assert(space.allocate(1, 3).is_err());
  space.reset();
  assert(space.charged_bytes() == 0 && space.metrics().live_bytes == 0);
}
```

The inherited `allocate` validates alignment, then calls `allocate_impl`.
The delegated allocator handles zero requests and backing-region capacity.
`charged_ <= quota_` is maintained by the successful-allocation path, so
the subtraction avoids adding request sizes that could overflow.

The plugin's reset policy explicitly clears its metrics. This differs from
some built-in space reset methods, and documenting it is part of the
plugin's contract. As with any bump space, callers finish all object
lifetimes before resetting it.

### 13.3 A collection-observer plan plugin

`PlanBase` requires a derived `kind_value`, `name_value`, and `collect_impl`.
This adapter owns a mark-sweep plan and notifies an observer after a
successful collection. It preserves the underlying plan kind while using
a descriptive name.

```cpp
#include <memtkx/MemTKX.hpp>

#include <cassert>
#include <functional>
#include <utility>
#include <vector>

namespace application {

class ObservedPlan final : public MemTKX::PlanBase<ObservedPlan> {
 public:
  using Observer = std::function<void(const MemTKX::PlanStats&)>;

  ObservedPlan(std::size_t bytes, Observer observer)
      : inner_(bytes), observer_(std::move(observer)) {}

  constexpr MemTKX::PlanKind kind_value() const noexcept {
    return MemTKX::PlanKind::MarkSweep;
  }

  constexpr std::string_view name_value() const noexcept {
    return "ObservedMarkSweep";
  }

  MemTKX::AllocResult<MemTKX::Address> allocate(
      std::size_t bytes, std::size_t alignment) {
    return inner_.allocate(bytes, alignment);
  }

  MemTKX::GcResult<MemTKX::PlanStats> collect_impl(
      const std::vector<MemTKX::Address>& roots,
      const MemTKX::TraceFn& trace) {
    auto result = inner_.collect(roots, trace);
    if (result.is_ok() && observer_) {
      observer_(result.unwrap());
    }
    return result;
  }

 private:
  MemTKX::MarkSweepPlan inner_;
  Observer observer_;
};

}  // namespace application

int main() {
  std::size_t observations = 0;
  application::ObservedPlan heap(4096, [&](const MemTKX::PlanStats& stats) {
    ++observations;
    assert(stats.collection_count == observations);
  });
  auto allocated = heap.allocate(64, 16);
  assert(allocated.is_ok());
  auto result = heap.collect({allocated.unwrap()},
      [](MemTKX::Address) { return std::vector<MemTKX::Address>{}; });
  assert(result.is_ok() && observations == 1);
  assert(heap.kind() == MemTKX::PlanKind::MarkSweep);
}
```

This adapter's observer sees a completed cycle. It must not recursively
collect the same heap. Its captures must outlive the adapter, and its
exception behavior should match the embedding runtime's collection policy.

`PlanKind` is a closed enum of the four built-in kinds. An adapter can report
the kind it wraps. A genuinely new algorithm should define an application
identity mechanism rather than inventing an undocumented enum value.
CRTP templates still allow its concrete type to supply its own collection
behavior.

### 13.4 Callback plugins

Trace callbacks and root providers are lightweight plugins. A VM can
package its object scanner in a callable class and assign it to `TraceFn`.
A root provider can capture a handle table or a published frame list.

Capture lifetime is part of the contract. A callback storing a reference
to a destroyed vector is not made valid by being wrapped in `std::function`.
If the callback owns its state, capture that state by value or through an
appropriate shared owner.

Root providers run under the coordinator mutex. Scanners run in the
collection call. Document those execution contexts alongside the plugin,
including whether its state is read-only and which thread may invoke it.

### 13.5 Barrier and phase plugins

A barrier plugin can compose the existing range filter with an
application-specific recorder or wrap `SatbBarrier` and `CardTableBarrier`
behind one reference-store function. Define the store ordering explicitly:
capture the old value before overwriting, and record the location required
by your remembered-set policy.

A phase plugin is a `Phase` produced by `make_phase`. It can export
telemetry, drain a per-mutator record buffer, prepare roots, or run a plan.
Keep real mutator release on an unconditional cleanup path as explained
in Chapter 10.

For DSL feature plugins, provide a public template
`Mixin<Derived>` member in the feature tag. Avoid method-name collisions
with other mixins and document which members the derived class must define.

### 13.6 Packaging a header plugin

Place plugin headers in your own namespace and include tree, for example
`include/application/quota_space.hpp`. Do not place application plugin
types inside `MemTKX` merely to make them look built-in.

```cmake
cmake_minimum_required(VERSION 3.20)
project(ApplicationMemoryPlugins LANGUAGES CXX)

find_package(MemTKX 1 CONFIG REQUIRED)
add_library(ApplicationMemoryPlugins INTERFACE)
add_library(Application::MemoryPlugins ALIAS ApplicationMemoryPlugins)
target_include_directories(ApplicationMemoryPlugins INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(ApplicationMemoryPlugins INTERFACE MemTKX::MemTKX)
```

Consumers link `Application::MemoryPlugins`. If you export the plugin as
an installed package, have its package configuration resolve MemTKX using
`find_dependency(MemTKX 1 CONFIG)` before loading its exported targets.

### 13.7 Plugin contract template

Document these concrete facts for each extension:

- **Storage:** owns or borrows memory, and which owner must remain alive.
- **Allocation:** size/alignment requirements and failure results.
- **Reclamation:** individual free, whole-region reset, or collection.
- **References:** root representation, trace format, and relocation behavior.
- **Execution:** invoking thread, synchronization, and callback lifetime.
- **Metrics:** requested bytes versus consumed capacity and reset semantics.
- **Composition:** required base class, delegates, phases, or mixins.

Use this information to make the plugin usable independently of its
implementation. The quota and observer examples illustrate different
contracts while retaining ordinary MemTKX call sites.

## Chapter 14 — Testing, performance, and maintenance

### 14.1 The existing verification suite

Configure Debug and run CTest:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

| Test | Main subject |
| --- | --- |
| `test_units` | Binary memory units and address helpers |
| `test_allocator` | Bump, free-list, segregated, Immix, and large allocation |
| `test_barrier` | Recording filters, card boundaries, and SATB |
| `test_immix` | Rounded capacity and line marking |
| `test_large_object` | Large-space accounting |
| `test_scheduler_edges` | Timeout and phase error behavior |
| `test_dispatcher` | DSL dispatch |
| `test_scheduler` | Queue, pipeline, and coordinator behavior |
| `test_plans` | Collection-plan statistics |

Assertions validate the test conditions; a Release build may disable them.
Passing the suite establishes the exercised behavior, not a relocation
protocol or complete collector policy absent from the API.

### 14.2 Verify application semantics

For a real runtime, useful scenarios include a rooted chain, a rooted
cycle, an unrooted cycle, duplicate roots, an empty root set, and repeated
collection after field updates. Check object payloads as well as counts.

For a free-list plugin, verify coalescing and allocation failure under
fragmentation. For a quota plugin, verify quota and backing capacity are
independent. For coordination, use explicit handshakes so the test proves
mutators remain parked throughout the entire collection interval.

Build an external installed-package consumer as part of release
verification. This catches missing dependency discovery, wrong include
directories, and mistakes hidden by source-tree compilation.

### 14.3 Benchmark commands and interpretation

```sh
cmake -S . -B build-bench \
  -DCMAKE_BUILD_TYPE=Release \
  -DMemTKX_BUILD_TESTS=OFF \
  -DMemTKX_BUILD_BENCHMARKS=ON
cmake --build build-bench --parallel
./build-bench/bench_alloc
./build-bench/bench_barrier
```

`bench_alloc` runs 200,000 iterations. Its bump loop resets on every
iteration, its free-list loop allocates and frees, and its Immix loop
resets blocks every 256 iterations. The CSV labels report nanoseconds per
loop operation, not identical reclamation workloads.

`bench_barrier` runs 1,000,000 iterations. The pipeline appends to a vector,
whereas the raw filter only counts matches. The raw hit count is discarded,
so an optimizing compiler may eliminate much of that loop. The comparison
does not isolate an intrinsic cost of the pipe syntax.

For decisions about your runtime, create a representative workload with
observable results, real object-size distributions, meaningful lifetime
patterns, and the same compiler options for both variants. Record compiler,
CPU, heap sizes, and repetitions alongside timings.

### 14.4 Metrics that answer different questions

Separate these quantities in telemetry:

| Quantity | Suitable source |
| --- | --- |
| Successful requested bytes | Allocator counters or space metrics |
| Cursor consumption, including padding | Bump/Immix `used_bytes()` |
| Current free-list capacity | `free_bytes()` and `free_cells()` |
| Reachable object payload | `PlanStats::live_bytes` |
| Occupied Immix line capacity | `marked_bytes()` |
| Full backing allocation | Owned storage/space capacity observations |
| Collection elapsed time | Application steady-clock measurement |

Native vectors, hash tables, callbacks, and trace-result allocations use
the standard heap. They are not included in the requested managed payload
size. Include their impact when evaluating total process memory.

### 14.5 Troubleshooting by symptom

| Symptom | What to inspect |
| --- | --- |
| Header not found | Use `memtkx/MemTKX.hpp` and link the interface target |
| Namespace not found | Use `MemTKX` or `MemTKX::dsl`; check exact case |
| Package not found | Set the install prefix in `CMAKE_PREFIX_PATH` or use `MemTKX_DIR` |
| Threads target missing in a custom plugin export | Resolve dependencies before loading exported targets |
| `Misaligned` result | Supply a nonzero power-of-two alignment |
| Large-space request rejected | Check both threshold and entire region size |
| Allocation fails despite substantial total free bytes | Inspect fragmentation, partition capacity, and padding |
| Immix collection does not restore space | Its built-in plan marks but does not sweep |
| Large-space deallocation does not restore space | Individual deallocation is accounting; reuse requires reset |
| Copying-plan references are stale | Roots/fields are not rewritten or exposed through forwarding |
| Safepoint times out | Check actual parking, expected registration count, and every record's status |
| Root callback deadlocks | Avoid coordinator re-entry while callbacks hold its mutex |
| Pipeline reports zero completed steps | Phases must call `note_completion` explicitly |
| Result has no `error()` method | Inspect the error using `map_err` |
| `Result<T, T>` branch inspection does not compile | Use distinct success/error types |

### 14.6 Identity migration

Update an existing integration consistently:

1. Include `<memtkx/MemTKX.hpp>` for memory APIs and
   `<memtkx/MemTKXDSL.hpp>` for utilities.
2. Qualify memory-management types with `MemTKX::`.
3. Qualify utility types with `MemTKX::dsl::`, or define a local alias.
4. Use `find_package(MemTKX CONFIG REQUIRED)` and `MemTKX::MemTKX`.
5. Use the exact `MemTKX_` CMake option names.
6. Reconfigure in a fresh build directory and verify an installed consumer.

The public identity is a direct rename. Old include paths, namespace names,
and package targets are not compatibility aliases. Remove stale package
directories from your own installation prefix if they would otherwise be
selected by another consumer's old configuration.

### 14.7 Reading and extending the source

Start from `core/types.hpp` and `core/error.hpp`, then read the allocator
you use. `space/space.hpp` defines the space extension contract, and
`plan/plan.hpp` defines the collection contract. Concrete plan headers are
short enough to follow their worklist logic directly.

Keep application-specific object layouts and plugins outside the MemTKX
namespace. When changing a library mechanism, update documentation where
its observable behavior is described and verify the relevant tests plus
an external consumer when the public API or package changes.

You now have the pieces to reserve memory, manage object lifetimes, trace a
graph, orchestrate a collection interval, and package application plugins.
Use those contracts as the foundation of your runtime's own memory policy.
