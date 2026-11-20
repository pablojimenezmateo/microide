#pragma once

#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

namespace microide::platform {

// Called in the CHILD between fork() and exec(): every signal goes back to its
// default disposition and none stays blocked, the state a program expects to
// start in (what libuv and terminal emulators give their children).
//
// An ignored disposition and a blocked mask both survive exec, and a shell
// cannot reset a signal that was ignored when it started. The process ignores
// SIGPIPE itself (IgnoreBrokenPipeSignal, so a peer closing a socket or pipe is
// an EPIPE the writer handles rather than a crash): every child inherited it,
// `yes | head` in the integrated terminal left `yes` reporting "Broken pipe" and
// exiting 1 instead of dying quietly, and git and the LSP/DAP servers got EPIPE
// write errors where they expect the signal. And the editor may itself START
// with signals ignored: a background job of a non-interactive shell (`microide &`
// from a script or a launcher) begins with SIGINT and SIGQUIT ignored, so Ctrl+C
// in the integrated terminal reached no program, and the debugger's Pause --
// a SIGINT to a program gdb is still starting -- stopped nothing (the coverage
// lane, which runs the suite in one backgrounded process, failed
// DebugService/SessionPauseInterruptsAGdbStillStartingTheProgram on it). Hence
// every signal, not a list of the ones the editor changes. Both calls are
// async-signal-safe; SIGKILL and SIGSTOP reject the call harmlessly.
inline void RestoreDefaultSignalsInChild() {
  struct sigaction dfl{};
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  for (int signo = 1; signo < NSIG; ++signo) {
    sigaction(signo, &dfl, nullptr);
  }
  sigset_t unblocked;
  sigemptyset(&unblocked);
  sigprocmask(SIG_SETMASK, &unblocked, nullptr);
}

// fork() for a child that will exec: every signal is blocked across the fork, so
// the child starts with none deliverable and stays so until
// RestoreDefaultSignalsInChild resets every disposition and only THEN unblocks.
// A signal that reaches the child before that is delivered with its default
// action, as after exec.
//
// Without it the child ran the editor's own handlers until the reset. Ctrl+C in
// a terminal opened a moment earlier reached its child (the pty's foreground
// group once TIOCSCTTY ran) while the editor's SIGINT handler was still
// installed there, and that handler wrote "signal 2" into the self-pipe the
// child had inherited: the EDITOR saved its session and quit (soak
// 7ab9a5476905, `term` then ctrl+c). The parent's mask is restored at once; a
// signal it gets meanwhile stays pending and arrives then.
//
// Every child path must reach RestoreDefaultSignalsInChild before exec (or
// _exit): the child's mask is otherwise everything.
inline pid_t ForkWithSignalsBlocked() {
  sigset_t all;
  sigset_t previous;
  sigfillset(&all);
  pthread_sigmask(SIG_SETMASK, &all, &previous);
  const pid_t pid = fork();
  if (pid != 0) {
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
  }
  return pid;
}

}  // namespace microide::platform
