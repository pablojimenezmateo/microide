#pragma once

#include <algorithm>
#include <cstring>
#include <string_view>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace microide::util {

// Names the calling thread for `top -H`, `perf`, gdb and /proc/<pid>/task/*/comm,
// which is how tools/qa/remote.py's idle-cost scenario attributes wakeups and CPU
// to a thread. Unnamed, every remote thread read as the process name and a
// regression could not say which loop it was. Linux keeps 15 bytes; longer is cut.
inline void NameCurrentThread(std::string_view name) {
#if defined(__linux__)
  char buffer[16] = {};
  std::memcpy(buffer, name.data(), std::min<std::size_t>(name.size(), sizeof(buffer) - 1));
  (void)::pthread_setname_np(::pthread_self(), buffer);
#else
  (void)name;
#endif
}

}  // namespace microide::util
