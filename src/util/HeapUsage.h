#pragma once

#include <cstdint>
#include <optional>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

// Whether glibc's malloc is this process's allocator. Under AddressSanitizer or
// ThreadSanitizer every malloc goes to the sanitizer's own allocator, and glibc's
// heap holds nothing of ours: trimming or measuring it answers nothing. Worse,
// glibc (2.39 on CI's runner) initializes its allocator lazily and without a lock
// on the first call into it, binding the calling thread to its main arena. With
// the sanitizer's allocator in place that first call was a `malloc_trim` from two
// mirror-sync workers at once; both were bound with the arena counting one, and
// the second to exit aborted on glibc's own `a->attached_threads > 0` assertion
// (TD-2026-11-10-438). In a plain build the main thread's first malloc, long
// before any thread, has already done that initialization.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define MICROIDE_GLIBC_MALLOC_IS_THE_ALLOCATOR 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define MICROIDE_GLIBC_MALLOC_IS_THE_ALLOCATOR 0
#endif
#endif
#if !defined(MICROIDE_GLIBC_MALLOC_IS_THE_ALLOCATOR)
#if defined(__GLIBC__)
#define MICROIDE_GLIBC_MALLOC_IS_THE_ALLOCATOR 1
#else
#define MICROIDE_GLIBC_MALLOC_IS_THE_ALLOCATOR 0
#endif
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

// nullopt where the C library has no such report (not glibc), or where glibc's
// malloc is not the allocator (a sanitizer build).
[[nodiscard]] inline std::optional<HeapUsage> CurrentHeapUsage() {
#if MICROIDE_GLIBC_MALLOC_IS_THE_ALLOCATOR && \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
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
// A no-op where glibc's malloc is not the allocator (see above).
inline void ReleaseFreedHeap() {
#if MICROIDE_GLIBC_MALLOC_IS_THE_ALLOCATOR
  (void)::malloc_trim(0);
#endif
}

}  // namespace microide::util
