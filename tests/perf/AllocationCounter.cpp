#include "AllocationCounter.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <execinfo.h>
#include <mutex>
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

// Aggregated allocation-site tracing, off unless MICROIDE_PERF_ALLOC_TRACE is set:
//
//   MICROIDE_PERF_ALLOC_TRACE=<min_bytes>[:<max_bytes>]
//
// Every allocation whose size falls in [min, max] has its call stack captured and
// counted, and the table is printed at the end of the run, most frequent first.
//
// This is the complement of MICROIDE_PERF_BIG_ALLOC_BYTES, which prints one
// backtrace per hit for the LARGEST allocations. That shape answers "what is
// eating memory". It cannot answer "what is doing 960 tiny allocations in this
// phase", which is the regression class TD-2026-08-06-139 turned out to be: six
// 32-byte allocations per mouse move, 160 moves, immediately freed. Printing 960
// backtraces is unreadable, and the size filter would have to be an equality
// match rather than a floor. Both are fixed here: a size BAND, and aggregation by
// stack rather than one dump per hit.
//
// Storage is a fixed open-addressed table in static memory. Nothing here may
// allocate -- it runs inside operator new.
constexpr std::size_t kTraceFrames = 24;
constexpr std::size_t kTraceSkipFrames = 1;  // operator new itself
constexpr std::size_t kTraceBuckets = 1024;  // power of two, mask-indexed

struct TraceBucket {
  std::uint64_t hash = 0;
  std::uint64_t count = 0;
  std::uint64_t bytes = 0;
  int frame_count = 0;
  void* frames[kTraceFrames] = {};
};

struct TraceBand {
  std::size_t min_bytes = 0;
  std::size_t max_bytes = 0;
  bool enabled = false;
};

const TraceBand g_trace_band = [] {
  const char* env = std::getenv("MICROIDE_PERF_ALLOC_TRACE");
  if (env == nullptr || env[0] == '\0') {
    return TraceBand{};
  }
  char* end = nullptr;
  const std::size_t min_bytes = static_cast<std::size_t>(std::strtoull(env, &end, 10));
  std::size_t max_bytes = static_cast<std::size_t>(-1);
  if (end != nullptr && *end == ':') {
    max_bytes = static_cast<std::size_t>(std::strtoull(end + 1, nullptr, 10));
  }
  return TraceBand{.min_bytes = min_bytes, .max_bytes = max_bytes, .enabled = true};
}();

// Optional phase scoping, off unless MICROIDE_PERF_ALLOC_TRACE_PHASE is set.
//
// The band filter above narrows by SIZE; this narrows by WHEN. Without it the
// table is whole-run, and a scenario's setup buries its measured phase: the
// residual 320 allocations in `editor_mouse_selection_drag` sat under twelve
// syntax-registry and plugin-reload sites with 10x the count, none of which the
// phase executes. With it, `ScenarioContext::Measure` arms recording only for
// phases whose name contains this substring, so the table is the phase.
//
// A raw `const char*` from getenv, compared by hand: this is read inside
// operator new, so it may not own a std::string.
const char* const g_trace_phase_filter = [] {
  const char* env = std::getenv("MICROIDE_PERF_ALLOC_TRACE_PHASE");
  return (env == nullptr || env[0] == '\0') ? nullptr : env;
}();

// How many sites the dump prints, MICROIDE_PERF_ALLOC_TRACE_SITES, default 12.
//
// Twelve is the right number to read by hand and the wrong number to compute a
// share from: the tail is printed as "... and N more site(s)" with no counts, so
// anything that has to attribute *all* of a phase's allocations (the scaffolding
// audit in tools/audit-perf-phase-scaffolding.py) is left with an unclassifiable
// remainder it can only guess at. Raising this to kTraceBuckets makes the table
// exhaustive.
const std::size_t g_trace_site_limit = [] {
  const char* env = std::getenv("MICROIDE_PERF_ALLOC_TRACE_SITES");
  if (env == nullptr || env[0] == '\0') {
    return std::size_t{12};
  }
  const std::size_t parsed = static_cast<std::size_t>(std::strtoull(env, nullptr, 10));
  return parsed == 0 ? std::size_t{12} : parsed;
}();

// Armed by Measure, per thread — the same reason the counters are per-thread: a
// worker allocating during the phase is not the shell-thread cost being chased.
thread_local bool t_trace_phase_active = false;
// Whether any phase ever matched the filter. A filter that matches nothing
// prints an empty table indistinguishable from "the phase allocates nothing",
// which is the vacuity trap `dev-docs/project/validation-traps.md` exists for.
std::atomic<bool> g_trace_phase_matched{false};

TraceBucket g_trace_table[kTraceBuckets];
std::atomic<std::uint64_t> g_trace_dropped{0};
std::mutex g_trace_mutex;

// backtrace() itself allocates on its first call (it dlopens the unwinder), and
// any allocation made while recording one would recurse forever. One flag per
// thread, checked before the capture, ends that.
thread_local bool t_in_trace = false;

void RecordAllocationSite(std::size_t size) {
  if (t_in_trace) {
    return;
  }
  t_in_trace = true;
  void* frames[kTraceFrames];
  const int captured = ::backtrace(frames, static_cast<int>(kTraceFrames));
  t_in_trace = false;
  if (captured <= static_cast<int>(kTraceSkipFrames)) {
    return;
  }
  const int begin = static_cast<int>(kTraceSkipFrames);
  // FNV-1a over the return addresses. Two different stacks colliding would merge
  // two sites into one row; at 1024 buckets over a handful of real sites that is
  // not a risk worth a second hash for.
  std::uint64_t hash = 1469598103934665603ULL;
  for (int i = begin; i < captured; ++i) {
    auto address = reinterpret_cast<std::uintptr_t>(frames[i]);
    for (std::size_t byte = 0; byte < sizeof(address); ++byte) {
      hash ^= (address >> (byte * 8)) & 0xFF;
      hash *= 1099511628211ULL;
    }
  }

  const std::lock_guard<std::mutex> guard(g_trace_mutex);
  std::size_t index = static_cast<std::size_t>(hash) & (kTraceBuckets - 1);
  for (std::size_t probe = 0; probe < kTraceBuckets; ++probe) {
    TraceBucket& bucket = g_trace_table[index];
    if (bucket.count == 0) {
      bucket.hash = hash;
      bucket.frame_count = captured - begin;
      for (int i = 0; i < bucket.frame_count; ++i) {
        bucket.frames[i] = frames[begin + i];
      }
    }
    if (bucket.hash == hash) {
      ++bucket.count;
      bucket.bytes += size;
      return;
    }
    index = (index + 1) & (kTraceBuckets - 1);
  }
  g_trace_dropped.fetch_add(1, std::memory_order_relaxed);
}

inline void RecordAlloc(std::size_t size) {
  ++t_allocations;
  t_bytes_allocated += static_cast<std::uint64_t>(size);
  if (g_big_alloc_threshold != 0 && size >= g_big_alloc_threshold) [[unlikely]] {
    void* frames[24];
    const int count = ::backtrace(frames, 24);
    std::fprintf(stderr, "[bigalloc] %zu bytes\n", size);
    ::backtrace_symbols_fd(frames, count, 2);
  }
  if (g_trace_band.enabled && size >= g_trace_band.min_bytes &&
      size <= g_trace_band.max_bytes &&
      (g_trace_phase_filter == nullptr || t_trace_phase_active)) [[unlikely]] {
    RecordAllocationSite(size);
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

std::string_view Allocations::PhaseTraceFilter() {
  return g_trace_phase_filter == nullptr ? std::string_view{}
                                         : std::string_view(g_trace_phase_filter);
}

void Allocations::SetPhaseTraceActive(bool active) {
  if (g_trace_phase_filter == nullptr) {
    return;
  }
  t_trace_phase_active = active;
  if (active) {
    g_trace_phase_matched.store(true, std::memory_order_relaxed);
  }
}

void Allocations::DumpTracedAllocationSites() {
  if (!g_trace_band.enabled) {
    return;
  }
  if (g_trace_phase_filter != nullptr &&
      !g_trace_phase_matched.load(std::memory_order_relaxed)) {
    // Loud, not empty: an unmatched filter and a phase that allocates nothing
    // produce the same table, and only one of them is a result.
    std::fprintf(stderr,
                 "[alloctrace] WARNING: no measured phase name contained \"%s\" — the table "
                 "below is empty because the filter never matched, not because nothing "
                 "allocated\n",
                 g_trace_phase_filter);
  }
  const std::lock_guard<std::mutex> guard(g_trace_mutex);
  // Index the non-empty buckets rather than sorting the table: TraceBucket is
  // ~220 bytes and this runs after a measured run, but a std::sort over 1024 of
  // them would move a quarter of a megabyte for no reason.
  std::size_t order[kTraceBuckets];
  std::size_t used = 0;
  std::uint64_t total = 0;
  for (std::size_t i = 0; i < kTraceBuckets; ++i) {
    if (g_trace_table[i].count != 0) {
      order[used++] = i;
      total += g_trace_table[i].count;
    }
  }
  if (used == 0) {
    std::fprintf(stderr,
                 "[alloctrace] no allocation fell in [%zu, %zu] bytes\n",
                 g_trace_band.min_bytes, g_trace_band.max_bytes);
    return;
  }
  std::sort(order, order + used, [](std::size_t a, std::size_t b) {
    return g_trace_table[a].count > g_trace_table[b].count;
  });

  std::fprintf(stderr,
               "\n[alloctrace] %llu allocations in [%zu, %zu] bytes from %zu distinct sites\n",
               static_cast<unsigned long long>(total), g_trace_band.min_bytes,
               g_trace_band.max_bytes, used);
  const std::uint64_t dropped = g_trace_dropped.load(std::memory_order_relaxed);
  if (dropped != 0) {
    // Never silently: a truncated table reads exactly like a complete one.
    std::fprintf(stderr,
                 "[alloctrace] WARNING: %llu allocations were dropped, the site table is full "
                 "(raise kTraceBuckets)\n",
                 static_cast<unsigned long long>(dropped));
  }
  std::fprintf(stderr,
               "[alloctrace] resolve with: addr2line -e <binary> -f -C -p <address>...\n");
  const std::size_t limit = std::min<std::size_t>(used, g_trace_site_limit);
  for (std::size_t i = 0; i < limit; ++i) {
    const TraceBucket& bucket = g_trace_table[order[i]];
    std::fprintf(stderr, "\n[alloctrace] #%zu: %llu allocations, %llu bytes\n", i + 1,
                 static_cast<unsigned long long>(bucket.count),
                 static_cast<unsigned long long>(bucket.bytes));
    ::backtrace_symbols_fd(const_cast<void* const*>(bucket.frames), bucket.frame_count, 2);
  }
  if (used > limit) {
    std::fprintf(stderr, "\n[alloctrace] ... and %zu more site(s)\n", used - limit);
  }
  for (std::size_t i = 0; i < kTraceBuckets; ++i) {
    g_trace_table[i] = TraceBucket{};
  }
  g_trace_dropped.store(0, std::memory_order_relaxed);
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
