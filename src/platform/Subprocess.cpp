#include "platform/Subprocess.h"

#include "platform/ChildSignals.h"
#include "platform/OpenFileLimit.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"

#include <array>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util/PosixFd.h"
#include "util/PosixPipe.h"

extern "C" char** environ;

namespace microide::platform {

namespace {

bool IsValidEnvironmentName(const std::string& name) {
  return !name.empty() && name.find('=') == std::string::npos &&
         name.find('\0') == std::string::npos;
}

using util::SetNonBlocking;
using util::UniqueFd;

int OpenPidFd(pid_t pid) {
#if defined(SYS_pidfd_open)
  const long fd = syscall(SYS_pidfd_open, pid, 0u);
  return fd < 0 ? -1 : static_cast<int>(fd);
#else
  (void)pid;
  return -1;
#endif
}

bool OpenPipe(bool enabled, std::array<UniqueFd, 2>* pipe_fds) {
  if (pipe_fds == nullptr) {
    return false;
  }
  (*pipe_fds)[0].Reset();
  (*pipe_fds)[1].Reset();
  if (!enabled) {
    return true;
  }

  int raw_pipe[2] = {-1, -1};
  if (!microide::util::MakeCloexecPipe(raw_pipe)) {
    return false;
  }
  (*pipe_fds)[0].Reset(raw_pipe[0]);
  (*pipe_fds)[1].Reset(raw_pipe[1]);
  return true;
}

bool DrainReadyPipe(UniqueFd* fd, std::string* output, std::size_t max_bytes) {
  if (fd == nullptr || !fd->IsValid() || output == nullptr) {
    return false;
  }

  std::array<char, 4096> buffer{};
  while (true) {
    if (output->size() >= max_bytes) {
      return true;
    }
    const ssize_t bytes_read = read(fd->Get(), buffer.data(), buffer.size());
    if (bytes_read > 0) {
      const std::size_t room = max_bytes - output->size();
      const std::size_t take = std::min(room, static_cast<std::size_t>(bytes_read));
      output->append(buffer.data(), take);
      if (take < static_cast<std::size_t>(bytes_read)) {
        return true;
      }
      continue;
    }
    if (bytes_read == 0) {
      fd->Reset();
      return false;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return false;
    }
    fd->Reset();
    return false;
  }
}

bool PumpChildIo(UniqueFd* stdin_fd,
                 const std::string& stdin_text,
                 UniqueFd* stdout_fd,
                 std::string* stdout_text,
                 UniqueFd* stderr_fd,
                 std::string* stderr_text,
                 int timeout_ms,
                 bool* out_truncated) {
  using Clock = std::chrono::steady_clock;
  const bool bounded = timeout_ms > 0;
  const Clock::time_point deadline =
      bounded ? Clock::now() + std::chrono::milliseconds(timeout_ms) : Clock::time_point{};
  if (stdin_fd != nullptr && stdin_fd->IsValid()) {
    SetNonBlocking(stdin_fd->Get());
    if (stdin_text.empty()) {
      stdin_fd->Reset();
    }
  }
  if (stdout_fd != nullptr && stdout_fd->IsValid()) {
    SetNonBlocking(stdout_fd->Get());
  }
  if (stderr_fd != nullptr && stderr_fd->IsValid()) {
    SetNonBlocking(stderr_fd->Get());
  }

  std::size_t stdin_offset = 0;
  while (true) {
    std::array<pollfd, 3> poll_fds{};
    nfds_t count = 0;
    int stdout_index = -1;
    int stderr_index = -1;
    int stdin_index = -1;

    if (stdout_fd != nullptr && stdout_fd->IsValid()) {
      stdout_index = static_cast<int>(count);
      poll_fds[count++] = pollfd{.fd = stdout_fd->Get(), .events = POLLIN | POLLHUP, .revents = 0};
    }
    if (stderr_fd != nullptr && stderr_fd->IsValid()) {
      stderr_index = static_cast<int>(count);
      poll_fds[count++] = pollfd{.fd = stderr_fd->Get(), .events = POLLIN | POLLHUP, .revents = 0};
    }
    if (stdin_fd != nullptr && stdin_fd->IsValid()) {
      stdin_index = static_cast<int>(count);
      poll_fds[count++] = pollfd{.fd = stdin_fd->Get(), .events = POLLOUT, .revents = 0};
    }

    if (count == 0) {
      break;
    }

    int poll_timeout = -1;
    if (bounded) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - Clock::now());
      if (remaining.count() <= 0) {
        return true;
      }
      poll_timeout = static_cast<int>(remaining.count());
    }

    const int poll_result = poll(poll_fds.data(), count, poll_timeout);
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (poll_result == 0) {
      return true;
    }

    if (stdout_index >= 0 &&
        (poll_fds[stdout_index].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      if (DrainReadyPipe(stdout_fd, stdout_text, kMaxSubprocessCaptureBytes)) {
        if (out_truncated != nullptr) *out_truncated = true;
        return false;
      }
    }
    if (stderr_index >= 0 &&
        (poll_fds[stderr_index].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      if (DrainReadyPipe(stderr_fd, stderr_text, kMaxSubprocessCaptureBytes)) {
        if (out_truncated != nullptr) *out_truncated = true;
        return false;
      }
    }
    if (stdin_index >= 0 &&
        (poll_fds[stdin_index].revents & (POLLOUT | POLLERR | POLLHUP)) != 0) {
      const ssize_t written =
          write(stdin_fd->Get(), stdin_text.data() + stdin_offset, stdin_text.size() - stdin_offset);
      if (written > 0) {
        stdin_offset += static_cast<std::size_t>(written);
        if (stdin_offset >= stdin_text.size()) {
          stdin_fd->Reset();
        }
      } else if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      } else {
        stdin_fd->Reset();
      }
    }
  }
  return false;
}

void RecordSpawnPhaseNs(const char* phase, std::chrono::steady_clock::time_point start) {
  if (!util::PerformanceTrace::SummaryEnabled()) {
    return;
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  char label[48];
  std::snprintf(label, sizeof(label), "platform::RunSubprocess::%s", phase);
  util::PerformanceTrace::RecordSampleNs(
      label,
      static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
}

template <typename Fn>
class ScopeExit {
 public:
  explicit ScopeExit(Fn fn) : fn_(std::move(fn)) {}
  ~ScopeExit() { fn_(); }
  ScopeExit(const ScopeExit&) = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;

 private:
  Fn fn_;
};

void BuildChildEnvironment(const std::vector<SubprocessEnvironmentOverride>& overrides,
                           std::vector<std::string>& storage, std::vector<char*>& pointers) {
  storage.clear();
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    storage.emplace_back(*entry);
  }
  for (const auto& override_entry : overrides) {
    if (!IsValidEnvironmentName(override_entry.name)) {
      continue;
    }
    const std::string prefix = override_entry.name + "=";
    storage.erase(std::remove_if(storage.begin(), storage.end(),
                                 [&](const std::string& env_entry) {
                                   return env_entry.size() >= prefix.size() &&
                                          env_entry.compare(0, prefix.size(), prefix) == 0;
                                 }),
                  storage.end());
    if (override_entry.value.has_value()) {
      storage.push_back(prefix + *override_entry.value);
    }
  }
  pointers.clear();
  pointers.reserve(storage.size() + 1);
  for (std::string& env_entry : storage) {
    pointers.push_back(env_entry.data());
  }
  pointers.push_back(nullptr);
}

SubprocessResult RunSubprocessUninstrumented(const std::vector<std::string>& argv,
                                             const SubprocessOptions& options) {
  SubprocessResult result;
  if (argv.empty()) {
    return result;
  }

  std::array<UniqueFd, 2> stdout_pipe;
  std::array<UniqueFd, 2> stderr_pipe;
  std::array<UniqueFd, 2> stdin_pipe;

  constexpr bool needs_stdin = true;
  if (!OpenPipe(options.capture_stdout, &stdout_pipe) ||
      !OpenPipe(options.capture_stderr, &stderr_pipe) || !OpenPipe(needs_stdin, &stdin_pipe)) {
    return result;
  }

  const bool has_env_overrides = !options.environment_overrides.empty();
  std::vector<std::string> child_env_storage;
  std::vector<char*> child_envp;
  if (has_env_overrides) {
    BuildChildEnvironment(options.environment_overrides, child_env_storage, child_envp);
  }
  std::vector<char*> raw_argv;
  raw_argv.reserve(argv.size() + 1);
  for (const std::string& arg : argv) {
    raw_argv.push_back(const_cast<char*>(arg.c_str()));
  }
  raw_argv.push_back(nullptr);
  const std::string cwd_string = options.cwd.empty() ? std::string() : options.cwd.string();

  const auto fork_start = std::chrono::steady_clock::now();
  const pid_t pid = ForkWithSignalsBlocked();
  if (pid < 0) {
    return result;
  }

  if (pid == 0) {
    if (options.capture_stdout) {
      dup2(stdout_pipe[1].Get(), STDOUT_FILENO);
    }
    if (options.capture_stderr) {
      dup2(stderr_pipe[1].Get(), STDERR_FILENO);
    } else if (options.silence_stderr) {
      UniqueFd devnull(open("/dev/null", O_WRONLY | O_CLOEXEC));
      if (devnull.IsValid()) {
        dup2(devnull.Get(), STDERR_FILENO);
      }
    }
    dup2(stdin_pipe[0].Get(), STDIN_FILENO);

    stdout_pipe[0].Reset();
    stdout_pipe[1].Reset();
    stderr_pipe[0].Reset();
    stderr_pipe[1].Reset();
    stdin_pipe[0].Reset();
    stdin_pipe[1].Reset();

    if (!cwd_string.empty()) {
      if (chdir(cwd_string.c_str()) != 0) {
        _exit(127);
      }
    }
    if (has_env_overrides) {
      environ = child_envp.data();
    }
    RestoreDefaultSignalsInChild();
    RestoreInheritedOpenFileLimitInChild();
    ApplyChildSandbox(options.sandbox);
    execvp(raw_argv[0], raw_argv.data());
    _exit(errno == ENOENT ? 127 : 126);
  }

  RecordSpawnPhaseNs("Fork", fork_start);

  stdout_pipe[1].Reset();
  stderr_pipe[1].Reset();
  stdin_pipe[0].Reset();

  const auto wait_start = std::chrono::steady_clock::now();
  bool output_truncated = false;
  bool timed_out =
      PumpChildIo(&stdin_pipe[1], options.stdin_text,
                  options.capture_stdout ? &stdout_pipe[0] : nullptr, &result.stdout_text,
                  options.capture_stderr ? &stderr_pipe[0] : nullptr, &result.stderr_text,
                  options.timeout_ms, &output_truncated);
  RecordSpawnPhaseNs("Pump", wait_start);
  const auto reap_start = std::chrono::steady_clock::now();
  const ScopeExit reap_scope([&reap_start]() { RecordSpawnPhaseNs("Reap", reap_start); });

  stdin_pipe[1].Reset();
  stdout_pipe[0].Reset();
  stderr_pipe[0].Reset();

  if (!timed_out && !output_truncated && options.timeout_ms > 0) {
    const auto deadline = wait_start + std::chrono::milliseconds(options.timeout_ms);
    const auto reap_finished = [&](int probe_status) {
      if (WIFEXITED(probe_status)) {
        result.exit_code = WEXITSTATUS(probe_status);
      } else if (WIFSIGNALED(probe_status)) {
        result.exit_code = 128 + WTERMSIG(probe_status);
      }
    };
    UniqueFd child_pidfd(OpenPidFd(pid));
    while (true) {
      int probe_status = 0;
      const pid_t probed = waitpid(pid, &probe_status, WNOHANG);
      if (probed == pid) {
        reap_finished(probe_status);
        return result;
      }
      if (probed < 0 && errno != EINTR) {
        result.exit_code = -1;
        return result;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        timed_out = true;
        break;
      }
      const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    deadline - now)
                                    .count();
      if (child_pidfd.IsValid()) {
        pollfd child_poll{.fd = child_pidfd.Get(), .events = POLLIN, .revents = 0};
        poll(&child_poll, 1, static_cast<int>(std::min<long long>(remaining_ms + 1, 1000)));
        continue;
      }
      poll(nullptr, 0, 1);
    }
  }

  if (timed_out || output_truncated) {
    result.timed_out = timed_out;
    result.truncated = output_truncated;
    kill(pid, SIGKILL);
  }

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      result.exit_code = -1;
      return result;
    }
  }

  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
  }
  return result;
}

}

SubprocessResult RunSubprocess(const std::vector<std::string>& argv,
                               const SubprocessOptions& options) {
  util::PerformanceTrace::ScopeLabel label("platform::RunSubprocess");
  if (!argv.empty()) {
    std::string_view program = argv.front();
    const std::size_t slash = program.find_last_of('/');
    if (slash != std::string_view::npos) {
      program.remove_prefix(slash + 1);
    }
    label.Field("program", program);
    bool skip_next_value = false;
    for (std::size_t i = 1; i < argv.size(); ++i) {
      const std::string_view argument = argv[i];
      if (skip_next_value) {
        skip_next_value = false;
        continue;
      }
      if (!argument.empty() && argument.front() == '-') {
        skip_next_value = argument == "-C" || argument == "-c";
        continue;
      }
      constexpr std::size_t kMaxSubcommandLength = 24;
      const bool looks_like_subcommand =
          !argument.empty() && argument.size() <= kMaxSubcommandLength &&
          std::all_of(argument.begin(), argument.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
          });
      if (looks_like_subcommand) {
        label.Field("sub", argument);
      }
      break;
    }
  }
  util::PerformanceTrace::Scope scope(label.View());

  const auto start = std::chrono::steady_clock::now();
  SubprocessResult result = RunSubprocessUninstrumented(argv, options);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  util::AddPerformanceCounter(util::PerfCounterId::SubprocessSpawns);
  util::AddPerformanceCounter(
      util::PerfCounterId::SubprocessWaitMs,
      static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
  util::AddPerformanceCounter(util::PerfCounterId::SubprocessOutputBytes,
                              result.stdout_text.size() + result.stderr_text.size());
  if (result.timed_out) {
    util::AddPerformanceCounter(util::PerfCounterId::SubprocessTimeouts);
  }
  return result;
}

}
