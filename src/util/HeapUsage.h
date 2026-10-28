#pragma once

#include <cstdint>
#include <optional>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace microide::util {

// The allocator's own view of this process's heap: what is handed out, and what
// it holds freed but has not given back to the system. RSS alone cannot tell a
// leak or a large live structure from a burst's freed buffers that glibc keeps in
// a thread arena -- the question every memory measurement of the remote client
// and server asked first (dev-docs/project/known-tech-debt.md TD-2026-10-28-418).
// For diagnostics (the control channel's `status`, `microide-server status`), not
// a hot path: mallinfo2 takes every arena's lock and walks its bins.
struct HeapUsage {
  std::uint64_t in_use_bytes = 0;    // allocated and not freed (arenas + mmapped chunks)
  std::uint64_t free_bytes = 0;      // freed, still held by the allocator
  std::uint64_t mmapped_bytes = 0;   // of in_use: chunks served by their own mmap
};

// nullopt where the C library has no such report (not glibc).
[[nodiscard]] inline std::optional<HeapUsage> CurrentHeapUsage() {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
  const struct mallinfo2 info = ::mallinfo2();
  return HeapUsage{.in_use_bytes = info.uordblks + info.hblkhd,
                   .free_bytes = info.fordblks,
                   .mmapped_bytes = info.hblkhd};
#else
  return std::nullopt;
#endif
}

// Hand freed heap back to the system: every arena's whole free pages, not only
// the top of each (which is all glibc returns on its own). For the END of a burst
// that freed a lot -- a first sync's object buffers, a fetch storm on the server --
// never per operation: it takes each arena's lock in turn and costs on the order
// of a millisecond. Without it those buffers stay resident in the thread arenas
// that allocated them for the life of the process (TD-2026-10-28-418: 25 MB in the
// editor after one remote project's first sync, 7.7 MB in the server).
inline void ReleaseFreedHeap() {
#if defined(__GLIBC__)
  (void)::malloc_trim(0);
#endif
}

}  // namespace microide::util
