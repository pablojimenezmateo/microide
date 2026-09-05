#pragma once

#if !defined(_WIN32)
#include <signal.h>
#endif

namespace microide::platform {

// Called in the CHILD between fork() and exec(): every disposition the editor
// changed for itself goes back to the default the child expects.
//
// The process ignores SIGPIPE (IgnoreBrokenPipeSignal, so a peer closing a
// socket or pipe is an EPIPE the writer handles rather than a crash), and an
// ignored disposition survives exec. Every child inherited it: `yes | head` in
// the integrated terminal left `yes` reporting "Broken pipe" and exiting 1
// instead of dying quietly with SIGPIPE, git and the LSP/DAP servers got EPIPE
// write errors where they expect the signal, and a shell cannot reset a signal
// that was ignored when it started. A blocked signal mask survives exec too, so
// it is cleared as well. Both calls are async-signal-safe.
inline void RestoreDefaultSignalsInChild() {
#if !defined(_WIN32)
  signal(SIGPIPE, SIG_DFL);
  sigset_t unblocked;
  sigemptyset(&unblocked);
  sigprocmask(SIG_SETMASK, &unblocked, nullptr);
#endif
}

}  // namespace microide::platform
