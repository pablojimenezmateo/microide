#include "AllocationCounter.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <execinfo.h>
#include <mutex>
#include <vector>
#include <new>

namespace {

thread_local std::uint64_t t_allocations = 0;
thread_local std::uint64_t t_frees = 0;
thread_local std::uint64_t t_bytes_allocated = 0;
thread_local std::uint64_t t_bytes_freed = 0;

const std::size_t g_big_alloc_threshold = [] {
  const char* env = std::getenv("MICROIDE_PERF_BIG_ALLOC_BYTES");
  return env == nullptr ? std::size_t{0} : static_cast<std::size_t>(std::strtoull(env, nullptr, 10));
}();

constexpr std::size_t kTraceFrames = 24;
constexpr std::size_t kTraceSkipFrames = 1;
constexpr std::size_t kTraceBuckets = 65536;

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

const char* const g_trace_phase_filter = [] {
  const char* env = std::getenv("MICROIDE_PERF_ALLOC_TRACE_PHASE");
  return (env == nullptr || env[0] == '\0') ? nullptr : env;
}();

const std::size_t g_trace_site_limit = [] {
  const char* env = std::getenv("MICROIDE_PERF_ALLOC_TRACE_SITES");
  if (env == nullptr || env[0] == '\0') {
    return std::size_t{12};
  }
  const std::size_t parsed = static_cast<std::size_t>(std::strtoull(env, nullptr, 10));
  return parsed == 0 ? std::size_t{12} : parsed;
}();

thread_local bool t_trace_phase_active = false;
std::atomic<bool> g_trace_phase_matched{false};

TraceBucket g_trace_table[kTraceBuckets];
std::atomic<std::uint64_t> g_trace_dropped{0};
std::mutex g_trace_mutex;

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

}

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
    std::fprintf(stderr,
                 "[alloctrace] WARNING: no measured phase name contained \"%s\" — the table "
                 "below is empty because the filter never matched, not because nothing "
                 "allocated\n",
                 g_trace_phase_filter);
  }
  const bool outer_in_trace = t_in_trace;
  t_in_trace = true;
  const struct TraceGuard {
    bool restore;
    ~TraceGuard() { t_in_trace = restore; }
  } trace_guard{outer_in_trace};

  const std::lock_guard<std::mutex> guard(g_trace_mutex);
  static std::vector<std::size_t> order;
  order.assign(kTraceBuckets, 0);
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
  std::sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(used),
            [](std::size_t a, std::size_t b) {
              return g_trace_table[a].count > g_trace_table[b].count;
            });

  std::fprintf(stderr,
               "\n[alloctrace] %llu allocations in [%zu, %zu] bytes from %zu distinct sites\n",
               static_cast<unsigned long long>(total), g_trace_band.min_bytes,
               g_trace_band.max_bytes, used);
  const std::uint64_t dropped = g_trace_dropped.load(std::memory_order_relaxed);
  if (dropped != 0) {
    std::fprintf(stderr,
                 "[alloctrace] WARNING: %llu of %llu allocations (%.0f%%) were DROPPED -- the "
                 "site table filled at %zu sites.\n"
                 "[alloctrace] The ranking below is NOT a ranking: it covers only the sites seen "
                 "before the table filled, so the phase's largest site may be missing entirely. "
                 "Raise kTraceBuckets and re-run before drawing any conclusion.\n",
                 static_cast<unsigned long long>(dropped),
                 static_cast<unsigned long long>(dropped + total),
                 100.0 * static_cast<double>(dropped) / static_cast<double>(dropped + total),
                 kTraceBuckets);
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

}

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
    RecordFree(::malloc_usable_size(ptr));
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
