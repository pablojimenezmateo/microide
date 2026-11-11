#pragma once

#include <sys/resource.h>

namespace microide::platform {

// The editor's own descriptor budget. A process starts with the soft
// RLIMIT_NOFILE it inherited -- 1024 under systemd and most desktop sessions --
// a number kept low only for programs that still use select(), whose fd_set
// cannot name a descriptor above 1023. The editor never does (poll/epoll
// throughout), and every integrated terminal holds a pty and two pipes, every
// language server, debug adapter and git job its pipes, every open document's
// directory an inotify watch: a heavy session spent the 1024 and the first
// thing to fail was the session save -- "persistence: could not write", with
// the unsaved buffers it exists to keep (soak c47c6a1f54cf, 330 terminals). The
// hard limit is what the system grants; the soft one is for the process to
// raise, which is what systemd's own guidance asks of a program that needs more.
//
// Header-only so every curated target that compiles a spawn site gets it; the
// remembered value is one inline variable across them.
namespace open_file_limit_detail {
// The soft limit the process inherited, or RLIM_INFINITY when it was never
// raised (nothing to put back). Written once, at startup, before any thread.
inline rlim_t inherited_soft = RLIM_INFINITY;
}  // namespace open_file_limit_detail

// Raises the soft RLIMIT_NOFILE to the hard one and remembers the inherited
// value for RestoreInheritedOpenFileLimitInChild. Call first thing in main,
// before any thread. A hard limit of "unlimited" cannot be the soft one (the
// kernel caps it at fs.nr_open), so that case asks for the common nr_open
// default instead. Best effort: a failure leaves the inherited limit in place.
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

// Called in the CHILD between fork() and exec(), beside
// RestoreDefaultSignalsInChild: a shell, git or a language server starts with
// the soft limit it would have had without the editor in between, so a child
// that does use select() is not handed descriptors its fd_set cannot hold.
// Async-signal-safe (getrlimit/setrlimit are system calls).
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

}  // namespace microide::platform
