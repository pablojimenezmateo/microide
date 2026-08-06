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

  // Print the aggregated allocation-site table collected under
  // MICROIDE_PERF_ALLOC_TRACE, most-frequent site first, and clear it.
  //
  // Does nothing when tracing is off. Safe to call unconditionally; PerfMain
  // calls it once at the end of a run.
  //
  // Why this exists: the counters say a phase allocated 960 times and nothing
  // says WHERE. TD-2026-08-06-139 sat unexplained for exactly that reason -- five
  // gates had drifted up by a flat number of small allocations, and attributing
  // them meant a git bisect over 90 commits because the harness could measure the
  // regression but not point at it. `MICROIDE_PERF_BIG_ALLOC_BYTES` could not
  // help: it traces the LARGEST allocations, and this class of regression is
  // hundreds of 32-byte ones.
  static void DumpTracedAllocationSites();
};

}  // namespace microide::tests::perf
