#pragma once

#include <cstddef>
#include <cstdint>

namespace microide::tests::perf {

struct AllocationSnapshot {
  std::uint64_t allocations = 0;
  std::uint64_t frees = 0;
  std::uint64_t bytes_allocated = 0;
  std::uint64_t bytes_freed = 0;
};

struct AllocationDelta {
  std::int64_t allocations = 0;
  std::int64_t frees = 0;
  std::int64_t bytes_allocated = 0;
  std::int64_t bytes_freed = 0;
};

// Allocation counters for the counting `operator new`/`delete` that a
// MICROIDE_PERF_HARNESS_BUILD arms.
//
// The counts are PER CALLING THREAD. Snapshot on the thread whose work you are
// measuring and delta on that same thread; a snapshot taken on one thread says
// nothing about another. This is deliberate -- see the note in the .cpp -- and it
// is what makes the numbers deterministic for scenarios that drive background
// workers, and what makes "this frame must not allocate" mean the frame rather
// than the whole process.
class Allocations {
 public:
  static AllocationSnapshot Snapshot();
  static AllocationDelta DeltaSince(const AllocationSnapshot& before);
};

}  // namespace microide::tests::perf
