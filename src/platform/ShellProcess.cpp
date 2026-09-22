#include "platform/ShellProcess.h"

#include <chrono>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace microide::platform {

std::string DefaultShellPath() {
#if defined(_WIN32)
  if (const char* shell = std::getenv("COMSPEC"); shell != nullptr && shell[0] != '\0') {
    return shell;
  }
  return "C:\\Windows\\System32\\cmd.exe";
#else
  if (const char* shell = std::getenv("SHELL"); shell != nullptr && shell[0] != '\0') {
    return shell;
  }
  return "/bin/sh";
#endif
}

std::string ShellProgramName(const std::string& shell_path) {
  const std::size_t slash = shell_path.find_last_of("/\\");
  return slash == std::string::npos ? shell_path : shell_path.substr(slash + 1);
}

namespace {

// Programs that take a command the POSIX way: a `-c` flag with the command as one
// following word. Everything else configured as a `terminal.shell` — `ssh host`,
// `docker exec -it box`, `toolbox run` — takes it as a trailing argument instead,
// and would read `-c` as one of its own flags.
//
// A list of names is a heuristic, but it is the same one the one-word form already
// makes implicitly (a lone word is assumed to be a shell, and gets `-i`), and it
// errs toward the trailing-argument form, which fails loudly when it is wrong
// rather than running the command somewhere unintended.
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

}  // namespace

std::vector<std::string> BuildTerminalArgv(const std::vector<std::string>& shell,
                                           const std::string& command) {
  std::vector<std::string> argv = shell.empty() ? std::vector<std::string>{DefaultShellPath()}
                                                : shell;
  if (argv.size() == 1) {
    // A bare shell name or path. argv[0] becomes the base name — the login-shell
    // convention, so `$0` reads `bash` rather than `/bin/bash` — and with no command
    // to run it starts interactively. A lone word is a shell by definition here:
    // that is what the setting has always meant.
    argv.front() = ShellProgramName(argv.front());
    if (command.empty()) {
      argv.push_back("-i");
    } else {
      argv.push_back("-lc");
      argv.push_back(command);
    }
    return argv;
  }
  // A multi-word value is exec'd exactly as written; the user already said what to
  // run, so nothing is appended when there is no command.
  if (command.empty()) {
    return argv;
  }
  if (TakesPosixCommandFlag(argv.front())) {
    // A shell the user has configured themselves. Add only `-c`, not `-lc`: their
    // own flags (`--norc`, `--login`) are the ones that decide the rest.
    argv.push_back("-c");
  }
  argv.push_back(command);
  return argv;
}

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

}  // namespace microide::platform
