#include "platform/DetachedProcess.h"

#include <cstdint>
#include <system_error>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace microide::platform {

std::filesystem::path CurrentExecutablePath() {
#if defined(__linux__)
  std::error_code ec;
  std::filesystem::path resolved = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec && !resolved.empty()) {
    return resolved;
  }
  return {};
#elif defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size + 1, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
  std::error_code ec;
  std::filesystem::path resolved = std::filesystem::weakly_canonical(buffer.data(), ec);
  return ec ? std::filesystem::path(buffer.data()) : resolved;
#elif defined(_WIN32)
  wchar_t buffer[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  if (length == 0 || length == MAX_PATH) {
    return {};
  }
  return std::filesystem::path(std::wstring(buffer, length));
#else
  return {};
#endif
}

#if defined(__unix__) || defined(__APPLE__)

namespace {

// Async-signal-safe re-exec in the forked child: redirect stdio to /dev/null,
// detach from the controlling terminal, then exec. Never returns on success.
[[noreturn]] void ExecDetachedChild(const std::vector<std::string>& argv,
                                    const std::filesystem::path& cwd) {
  setsid();  // New session so the grandchild is not tied to our terminal/pgroup.

  const int devnull = ::open("/dev/null", O_RDWR);
  if (devnull >= 0) {
    ::dup2(devnull, STDIN_FILENO);
    ::dup2(devnull, STDOUT_FILENO);
    ::dup2(devnull, STDERR_FILENO);
    if (devnull > STDERR_FILENO) {
      ::close(devnull);
    }
  }

  if (!cwd.empty()) {
    // Best-effort; a failed chdir still execs (inherits the parent cwd).
    (void)::chdir(cwd.c_str());
  }

  std::vector<char*> raw_argv;
  raw_argv.reserve(argv.size() + 1);
  for (const std::string& arg : argv) {
    raw_argv.push_back(const_cast<char*>(arg.c_str()));
  }
  raw_argv.push_back(nullptr);

  ::execv(raw_argv[0], raw_argv.data());
  _exit(127);  // exec failed.
}

}  // namespace

bool SpawnDetached(const std::vector<std::string>& argv, const std::filesystem::path& cwd) {
  if (argv.empty() || argv[0].empty()) {
    return false;
  }

  const pid_t first = ::fork();
  if (first < 0) {
    return false;
  }
  if (first == 0) {
    // Intermediate child: fork again so the grandchild is reparented to init and
    // never becomes a zombie (the parent only reaps `first`, which exits now).
    const pid_t second = ::fork();
    if (second < 0) {
      _exit(127);
    }
    if (second == 0) {
      ExecDetachedChild(argv, cwd);
    }
    _exit(0);  // Intermediate child exits immediately.
  }

  // Parent: reap the intermediate child so it does not linger as a zombie. The
  // grandchild (the actual detached window) is now owned by init.
  int status = 0;
  ::waitpid(first, &status, 0);
  return true;
}

#elif defined(_WIN32)

bool SpawnDetached(const std::vector<std::string>& argv, const std::filesystem::path& cwd) {
  if (argv.empty() || argv[0].empty()) {
    return false;
  }
  std::wstring command_line;
  for (const std::string& arg : argv) {
    if (!command_line.empty()) {
      command_line.push_back(L' ');
    }
    command_line.push_back(L'"');
    command_line.append(std::filesystem::path(arg).wstring());
    command_line.push_back(L'"');
  }

  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  PROCESS_INFORMATION process_info{};
  const std::wstring cwd_wide = cwd.empty() ? std::wstring() : cwd.wstring();
  const BOOL ok = CreateProcessW(
      nullptr, command_line.data(), nullptr, nullptr, FALSE,
      DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr,
      cwd_wide.empty() ? nullptr : cwd_wide.c_str(), &startup_info, &process_info);
  if (!ok) {
    return false;
  }
  CloseHandle(process_info.hThread);
  CloseHandle(process_info.hProcess);
  return true;
}

#else

bool SpawnDetached(const std::vector<std::string>&, const std::filesystem::path&) {
  return false;
}

#endif

}  // namespace microide::platform
