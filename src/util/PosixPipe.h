#pragma once

#if defined(__unix__) || defined(__APPLE__)

#include <fcntl.h>
#include <unistd.h>

namespace microide::util {

// Create a pipe whose two fds are close-on-exec (and optionally non-blocking).
//
// CLOEXEC is not optional hygiene here: without it, an unrelated fork()+exec() on
// another thread (terminal shell, git, LSP/DAP adapter) inherits the fds and holds
// them open for its whole lifetime, so the reader never sees EOF — spurious git
// timeouts, a language server whose stdin never closes, a watcher control pipe that
// cannot be woken. Six sites in the tree needed exactly this and each carried its
// own copy; two of those copies called Linux-only `pipe2` from a POSIX-guarded
// block (so they did not compile on macOS) and a third set the flags with a
// non-atomic post-hoc fcntl (so a concurrent fork could still slip between the
// pipe() and the F_SETFD).
//
// `pipe2` sets both flags atomically on Linux. Elsewhere we fall back to
// pipe()+fcntl, which is the best POSIX offers.
inline bool MakeCloexecPipe(int fds[2], bool nonblocking = false) {
#if defined(__linux__)
  return ::pipe2(fds, O_CLOEXEC | (nonblocking ? O_NONBLOCK : 0)) == 0;
#else
  if (::pipe(fds) != 0) {
    return false;
  }
  for (int i = 0; i < 2; ++i) {
    (void)::fcntl(fds[i], F_SETFD, ::fcntl(fds[i], F_GETFD, 0) | FD_CLOEXEC);
    if (nonblocking) {
      (void)::fcntl(fds[i], F_SETFL, ::fcntl(fds[i], F_GETFL, 0) | O_NONBLOCK);
    }
  }
  return true;
#endif
}

}  // namespace microide::util

#endif  // __unix__ || __APPLE__
