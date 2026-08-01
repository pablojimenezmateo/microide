#include "AllocationCounter.h"

#include <cstdlib>
#include <new>

namespace {

// PER-THREAD, not process-global.
//
// Every consumer of these counters snapshots and deltas on one thread and asks a
// question about that thread's work: "did this frame allocate", "how many
// allocations does this scenario's measured phase do". A process-global counter
// answers a different question, and answers it nondeterministically: the editor
// runs file-index builds, tree walks, git, and syntax prefetch on workers, so a
// worker's allocations were charged to whichever measured iteration the scheduler
// happened to run them in. That made whole scenarios unusable as gates --
// `cold_startup_large_project` measured a p50 of 399 allocations on one run and
// 1749 on the next, `scroll_large_file` 4410 and 10580, from identical binaries.
// No percentage tolerance covers a 4x swing without being meaningless.
//
// Counting per thread makes the number deterministic AND makes it the number that
// matters: allocation on the shell thread is what costs the user a frame.
// Background allocation is still visible where it belongs -- in the ranked trace
// summary's self-time column and in the RSS budget scenario.
//
// A side benefit: a plain thread-local increment replaces an atomic
// read-modify-write on the hottest possible path.
thread_local std::uint64_t t_allocations = 0;
thread_local std::uint64_t t_frees = 0;
thread_local std::uint64_t t_bytes_allocated = 0;
thread_local std::uint64_t t_bytes_freed = 0;

inline void RecordAlloc(std::size_t size) {
  ++t_allocations;
  t_bytes_allocated += static_cast<std::uint64_t>(size);
}

inline void RecordFree(std::size_t size) {
  ++t_frees;
  t_bytes_freed += static_cast<std::uint64_t>(size);
}

}  // namespace

namespace microide::tests::perf {

AllocationSnapshot Allocations::Snapshot() {
  return AllocationSnapshot{
      .allocations = t_allocations,
      .frees = t_frees,
      .bytes_allocated = t_bytes_allocated,
      .bytes_freed = t_bytes_freed,
  };
}

AllocationDelta Allocations::DeltaSince(const AllocationSnapshot& before) {
  const AllocationSnapshot now = Snapshot();
  return AllocationDelta{
      .allocations = static_cast<std::int64_t>(now.allocations) -
                     static_cast<std::int64_t>(before.allocations),
      .frees = static_cast<std::int64_t>(now.frees) - static_cast<std::int64_t>(before.frees),
      .bytes_allocated = static_cast<std::int64_t>(now.bytes_allocated) -
                         static_cast<std::int64_t>(before.bytes_allocated),
      .bytes_freed = static_cast<std::int64_t>(now.bytes_freed) -
                     static_cast<std::int64_t>(before.bytes_freed),
  };
}

}  // namespace microide::tests::perf

#if MICROIDE_PERF_HARNESS_BUILD
void* operator new(std::size_t size) {
  if (void* ptr = std::malloc(size)) {
    RecordAlloc(size);
    return ptr;
  }
  throw std::bad_alloc();
}

void operator delete(void* ptr) noexcept {
  if (ptr != nullptr) {
    // Sized-deallocation is unavailable on this overload, so only the count moves.
    RecordFree(0);
  }
  std::free(ptr);
}

void operator delete(void* ptr, std::size_t size) noexcept {
  if (ptr != nullptr) {
    RecordFree(size);
  }
  std::free(ptr);
}

void* operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete[](void* ptr) noexcept {
  ::operator delete(ptr);
}

void operator delete[](void* ptr, std::size_t size) noexcept {
  ::operator delete(ptr, size);
}
#endif
