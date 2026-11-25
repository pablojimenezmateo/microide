#pragma once

#include <algorithm>
#include <cstring>
#include <string_view>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace microide::util {

inline void NameCurrentThread(std::string_view name) {
#if defined(__linux__)
  char buffer[16] = {};
  std::memcpy(buffer, name.data(), std::min<std::size_t>(name.size(), sizeof(buffer) - 1));
  (void)::pthread_setname_np(::pthread_self(), buffer);
#else
  (void)name;
#endif
}

}
