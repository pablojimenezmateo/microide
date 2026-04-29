#include "AllocationCounter.h"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {

std::atomic<std::uint64_t> g_allocations{0};
std::atomic<std::uint64_t> g_frees{0};
std::atomic<std::uint64_t> g_bytes_allocated{0};
std::atomic<std::uint64_t> g_bytes_freed{0};

inline void RecordAlloc(std::size_t size) {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  g_bytes_allocated.fetch_add(static_cast<std::uint64_t>(size), std::memory_order_relaxed);
}

inline void RecordFree(std::size_t size) {
  g_frees.fetch_add(1, std::memory_order_relaxed);
  g_bytes_freed.fetch_add(static_cast<std::uint64_t>(size), std::memory_order_relaxed);
}

}  // namespace

namespace microide::tests::perf {

AllocationSnapshot Allocations::Snapshot() {
  return AllocationSnapshot{
      .allocations = g_allocations.load(std::memory_order_relaxed),
      .frees = g_frees.load(std::memory_order_relaxed),
      .bytes_allocated = g_bytes_allocated.load(std::memory_order_relaxed),
      .bytes_freed = g_bytes_freed.load(std::memory_order_relaxed),
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
    g_frees.fetch_add(1, std::memory_order_relaxed);
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
