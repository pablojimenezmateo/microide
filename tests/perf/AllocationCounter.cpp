#include "AllocationCounter.h"

#include <cstdio>
#include <cstdlib>
#include <execinfo.h>
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

// Diagnostic only, off unless MICROIDE_PERF_BIG_ALLOC_BYTES is set: print a
// backtrace for any single allocation at or above that many bytes, then resolve
// them with `addr2line -e <binary> -f -C <offset>`.
//
// This exists because balanced allocation counts with growing RSS -- the shape
// TD-2026-08-04-130 was filed as -- is invisible to every other counter here. A
// container doubling itself is ONE allocation and ONE free each time, so the
// counts stay balanced while the bytes climb; if the block is large enough glibc
// serves it from mmap, so the arena and uordblks stay flat too. The debt entry's
// hypothesis was heap fragmentation. One run of this named the actual line
// (PieceTree's append-only add buffer, 17 -> 35 -> 70 MB) in about a minute.
//
// Namespace-scope const, not a function-local static: a function-local would put
// a thread-safe-init guard load on the hottest path in the process.
const std::size_t g_big_alloc_threshold = [] {
  const char* env = std::getenv("MICROIDE_PERF_BIG_ALLOC_BYTES");
  return env == nullptr ? std::size_t{0} : static_cast<std::size_t>(std::strtoull(env, nullptr, 10));
}();

inline void RecordAlloc(std::size_t size) {
  ++t_allocations;
  t_bytes_allocated += static_cast<std::uint64_t>(size);
  if (g_big_alloc_threshold != 0 && size >= g_big_alloc_threshold) [[unlikely]] {
    void* frames[24];
    const int count = ::backtrace(frames, 24);
    std::fprintf(stderr, "[bigalloc] %zu bytes\n", size);
    ::backtrace_symbols_fd(frames, count, 2);
  }
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
