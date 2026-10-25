#pragma once

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
// own copy; one of them set the flags with a non-atomic post-hoc fcntl (so a
// concurrent fork could still slip between the pipe() and the F_SETFD).
// `pipe2` sets both flags atomically.
inline bool MakeCloexecPipe(int fds[2], bool nonblocking = false) {
  return ::pipe2(fds, O_CLOEXEC | (nonblocking ? O_NONBLOCK : 0)) == 0;
}

}  // namespace microide::util

