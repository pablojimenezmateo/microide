#include "terminal/TerminalProcessControl.h"

#include <chrono>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace microide::terminal {

namespace {

#if defined(__unix__) || defined(__APPLE__)
constexpr auto kTerminalHangupGrace = std::chrono::milliseconds(75);
constexpr auto kTerminalTerminateGrace = std::chrono::milliseconds(150);
constexpr auto kTerminalKillGrace = std::chrono::milliseconds(100);
constexpr auto kTerminalWaitPollInterval = std::chrono::milliseconds(10);

bool SendSignalToTerminalProcessGroup(int child_pid, int signal_number) {
  if (child_pid <= 0) {
    return true;
  }
  if (kill(-child_pid, signal_number) == 0) {
    return true;
  }
  if (kill(child_pid, signal_number) == 0) {
    return true;
  }
  return errno == ESRCH;
}

bool ReapTerminalChildNoHang(int child_pid) {
  if (child_pid <= 0) {
    return true;
  }

  int status = 0;
  while (true) {
    const pid_t result = waitpid(child_pid, &status, WNOHANG);
    if (result == child_pid) {
      return true;
    }
    if (result == 0) {
      return false;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return result < 0 && errno == ECHILD;
  }
}

bool WaitForTerminalChildExit(int child_pid, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (ReapTerminalChildNoHang(child_pid)) {
      return true;
    }
    std::this_thread::sleep_for(kTerminalWaitPollInterval);
  }
  return ReapTerminalChildNoHang(child_pid);
}
#endif

}  // namespace

void RequestTerminalChildShutdown(int child_pid) {
#if defined(__unix__) || defined(__APPLE__)
  if (child_pid <= 0 || ReapTerminalChildNoHang(child_pid)) {
    return;
  }

  SendSignalToTerminalProcessGroup(child_pid, SIGHUP);
  if (WaitForTerminalChildExit(child_pid, kTerminalHangupGrace)) {
    return;
  }

  SendSignalToTerminalProcessGroup(child_pid, SIGTERM);
  if (WaitForTerminalChildExit(child_pid, kTerminalTerminateGrace)) {
    return;
  }

  SendSignalToTerminalProcessGroup(child_pid, SIGKILL);
  WaitForTerminalChildExit(child_pid, kTerminalKillGrace);
#else
  (void)child_pid;
#endif
}

}  // namespace microide::terminal
