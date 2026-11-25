#pragma once

#include <cstdint>
#include <optional>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

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

struct HeapUsage {
  std::uint64_t in_use_bytes = 0;
  std::uint64_t free_bytes = 0;
  std::uint64_t mmapped_bytes = 0;
};

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

inline void ReleaseFreedHeap() {
#if MICROIDE_GLIBC_MALLOC_IS_THE_ALLOCATOR
  (void)::malloc_trim(0);
#endif
}

}
