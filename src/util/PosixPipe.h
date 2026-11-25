#pragma once

#include <fcntl.h>
#include <unistd.h>

namespace microide::util {

inline bool MakeCloexecPipe(int fds[2], bool nonblocking = false) {
  return ::pipe2(fds, O_CLOEXEC | (nonblocking ? O_NONBLOCK : 0)) == 0;
}

}

