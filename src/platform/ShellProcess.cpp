#include "platform/ShellProcess.h"

#include <chrono>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <vector>

#include <cerrno>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace microide::platform {

std::string DefaultShellPath() {
  if (const char* shell = std::getenv("SHELL"); shell != nullptr && shell[0] != '\0') {
    return shell;
  }
  return "/bin/sh";
}

std::string ShellProgramName(const std::string& shell_path) {
  const std::size_t slash = shell_path.find_last_of("/\\");
  return slash == std::string::npos ? shell_path : shell_path.substr(slash + 1);
}

namespace {

bool TakesPosixCommandFlag(const std::string& program) {
  const std::string name = ShellProgramName(program);
  static constexpr std::string_view kPosixShellNames[] = {
      "sh", "bash", "rbash", "zsh", "dash", "ash", "ksh", "mksh", "fish", "csh", "tcsh",
  };
  for (const std::string_view candidate : kPosixShellNames) {
    if (name == candidate) {
      return true;
    }
  }
  return false;
}

}

std::vector<std::string> BuildTerminalArgv(const std::vector<std::string>& shell,
                                           const std::string& command) {
  std::vector<std::string> argv = shell.empty() ? std::vector<std::string>{DefaultShellPath()}
                                                : shell;
  if (argv.size() == 1) {
    argv.front() = ShellProgramName(argv.front());
    if (command.empty()) {
      argv.push_back("-i");
    } else {
      argv.push_back("-lc");
      argv.push_back(command);
    }
    return argv;
  }
  if (command.empty()) {
    return argv;
  }
  if (TakesPosixCommandFlag(argv.front())) {
    argv.push_back("-c");
  }
  argv.push_back(command);
  return argv;
}

namespace {

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

}

void RequestTerminalChildShutdown(int child_pid) {
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
}

}
