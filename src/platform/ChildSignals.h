#pragma once

#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

namespace microide::platform {

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

}
