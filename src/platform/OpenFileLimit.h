#pragma once

#include <sys/resource.h>

namespace microide::platform {

namespace open_file_limit_detail {
inline rlim_t inherited_soft = RLIM_INFINITY;
}

inline void RaiseOpenFileLimit() {
  rlimit limit{};
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_cur >= limit.rlim_max) {
    return;
  }
  const rlim_t inherited = limit.rlim_cur;
  limit.rlim_cur = limit.rlim_max;
  if (limit.rlim_max == RLIM_INFINITY) {
    limit.rlim_cur = rlim_t{1} << 20;
  }
  if (setrlimit(RLIMIT_NOFILE, &limit) == 0) {
    open_file_limit_detail::inherited_soft = inherited;
  }
}

inline void RestoreInheritedOpenFileLimitInChild() {
  const rlim_t inherited = open_file_limit_detail::inherited_soft;
  if (inherited == RLIM_INFINITY) {
    return;
  }
  rlimit limit{};
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
    return;
  }
  limit.rlim_cur = inherited < limit.rlim_max ? inherited : limit.rlim_max;
  (void)setrlimit(RLIMIT_NOFILE, &limit);
}

}
