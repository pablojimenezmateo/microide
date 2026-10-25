#include "platform/Subprocess.h"

#include "platform/ChildSignals.h"

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

// At global scope: declared inside the anonymous namespace, GCC 15 gives it
// internal linkage and the link fails on an undefined
// `(anonymous namespace)::environ`.
extern "C" char** environ;

namespace microide::platform {

namespace {

// A usable environment-variable name: non-empty and free of '=' and NUL. A name
// containing '=' would be split by the child (or the environ scan) at the first '=',
// turning e.g. "LD_PRELOAD=x" into an effective LD_PRELOAD entry; a NUL truncates the
// C-string. Callers validate untrusted overrides at their own boundary
// (TD-2026-07-17A-126); this is the platform-level backstop so no env builder emits a
// malformed entry regardless of source.
bool IsValidEnvironmentName(const std::string& name) {
  return !name.empty() && name.find('=') == std::string::npos &&
         name.find('\0') == std::string::npos;
}

using util::SetNonBlocking;
using util::UniqueFd;

// A pollable handle on a child, so waiting for it to become reapable is an event
// wait rather than a sleep-and-probe spin. Returns -1 when the kernel has no
// pidfd_open (pre-5.3 Linux, other Unices); callers fall back to probing.
//
// The descriptor-creation close-on-exec rule applies here as it does to
// socket/open/pipe2: pidfd_open sets close-on-exec on the returned descriptor
// itself, so there is no follow-up fcntl and no window for another thread's
// fork/exec to inherit it.
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

  // Create the pipe close-on-exec. Without this, a pipe fd created here can be
  // inherited by an UNRELATED child that another thread fork()s during the window
  // before this spawn's child closes its copies — and because the fd is not
  // close-on-exec, that other child holds the write/read end open for its whole
  // lifetime, so our reader never sees EOF (spurious git timeouts; a language
  // server whose stdin never closes). This spawn's own child re-establishes stdio
  // via dup2 onto fds 0/1/2, which clears CLOEXEC on those duplicates, so the child
  // keeps working while every stray inherited copy auto-closes at exec.
  int raw_pipe[2] = {-1, -1};
  if (!microide::util::MakeCloexecPipe(raw_pipe)) {
    return false;
  }
  (*pipe_fds)[0].Reset(raw_pipe[0]);
  (*pipe_fds)[1].Reset(raw_pipe[1]);
  return true;
}

// Drains a (non-blocking) readable pipe into `output`. Stops on EOF (closing
// the fd) or once the kernel buffer is exhausted (EAGAIN); never blocks. Returns
// true if `output` reached `max_bytes` (the caller stops pumping and reaps the
// child); when capped it appends only up to the ceiling.
bool DrainReadyPipe(UniqueFd* fd, std::string* output, std::size_t max_bytes) {
  if (fd == nullptr || !fd->IsValid() || output == nullptr) {
    return false;
  }

  std::array<char, 4096> buffer{};
  while (true) {
    if (output->size() >= max_bytes) {
      return true;  // capture ceiling reached before this read
    }
    const ssize_t bytes_read = read(fd->Get(), buffer.data(), buffer.size());
    if (bytes_read > 0) {
      const std::size_t room = max_bytes - output->size();
      const std::size_t take = std::min(room, static_cast<std::size_t>(bytes_read));
      output->append(buffer.data(), take);
      if (take < static_cast<std::size_t>(bytes_read)) {
        return true;  // capture ceiling reached mid-read; discard the rest
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

// Pumps the child's stdio in a single non-blocking poll loop: feeds `stdin_text`
// to `stdin_fd` while concurrently draining captured stdout/stderr. Writing all
// of stdin up front (as the old code did) deadlocks when the child fills its
// stdout pipe before consuming stdin — the child blocks on write(stdout) and we
// block on write(stdin). Interleaving the two directions cannot deadlock.
// Pumps stdio until both directions are done, or until `timeout_ms` (> 0)
// elapses. Returns true if the deadline expired with the child still active --
// the caller then kills and reaps it. timeout_ms <= 0 means wait indefinitely
// (poll(-1)), preserving the historical behavior for every caller that does not
// opt in.
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
      stdin_fd->Reset();  // No payload: close immediately so the child sees EOF.
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
        return true;  // Deadline already passed; report timeout to the caller.
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
      // poll() can only return 0 when we passed a finite (bounded) timeout.
      return true;
    }

    if (stdout_index >= 0 &&
        (poll_fds[stdout_index].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      if (DrainReadyPipe(stdout_fd, stdout_text, kMaxSubprocessCaptureBytes)) {
        if (out_truncated != nullptr) *out_truncated = true;
        return false;  // capture ceiling hit: stop and let the caller tear down
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
          stdin_fd->Reset();  // Fully written: close to signal EOF to the child.
        }
      } else if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        // Transient: retry on the next poll.
      } else {
        stdin_fd->Reset();  // EPIPE or fatal error: child is gone, stop feeding it.
      }
    }
  }
  return false;  // stdio fully drained (or poll error) before any deadline.
}

// Build the child's environment (the current environ with overrides applied) in
// Fold one spawn phase into the ranked summary, labelled under the RunSubprocess
// row so the three phases sit together. Reads the clock only when the summary is
// on: an untraced run must not pay a steady_clock read per phase.
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

// Minimal scope guard: the reap block below returns from several places and the
// phase must be recorded on each.
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

// the PARENT, before fork. The child then only assigns `environ` to point at it —
// a single async-signal-safe pointer store. This replaces calling setenv/unsetenv
// in the child, which are NOT async-signal-safe: `RunSubprocess` runs on
// ProjectBackgroundExecutor worker threads, and the environ lock is not covered by
// pthread_atfork, so a fork racing another thread's setenv/getenv could deadlock
// the child before exec — hanging the parent's pipe read forever. `storage` owns
// the strings; `pointers` (built last) is the null-terminated char* array; both
// must outlive the exec (they live on the caller's stack across the fork).
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
    // setenv(name, value, 1) / unsetenv(name) semantics: drop any existing entries
    // for this name, then re-add "name=value" when a value is present.
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

// Split from RunSubprocess so the instrumentation wrapper below covers every one
// of this function's ~10 early returns without threading a scope guard through
// each of them.
SubprocessResult RunSubprocessUninstrumented(const std::vector<std::string>& argv,
                                             const SubprocessOptions& options) {
  SubprocessResult result;
  if (argv.empty()) {
    return result;
  }

  std::array<UniqueFd, 2> stdout_pipe;
  std::array<UniqueFd, 2> stderr_pipe;
  std::array<UniqueFd, 2> stdin_pipe;

  // Always provide a host-owned stdin pipe so background helpers never inherit
  // the controlling terminal. Callers that do not send stdin should observe EOF.
  constexpr bool needs_stdin = true;
  if (!OpenPipe(options.capture_stdout, &stdout_pipe) ||
      !OpenPipe(options.capture_stderr, &stderr_pipe) || !OpenPipe(needs_stdin, &stdin_pipe)) {
    return result;
  }

  // Everything the child needs is built HERE, in the parent, before fork(): the
  // child must call only async-signal-safe functions (no malloc / no libc global
  // mutation), because it is forked off a background worker thread. See
  // BuildChildEnvironment for the deadlock this avoids.
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

  // Split the spawn into its three phases. `RunSubprocess(program=..)` ranks
  // "git costs 6 ms here" but not WHY, and the three answers want opposite fixes:
  // fork() copying this process's page tables is the app's own weight (fix: spawn
  // fewer children, or spawn them from a smaller address space), the pump is the
  // child's real work plus our capture, and the reap is pure latency after the
  // work is done. Measured, not sampled: these are recorded from the PARENT only,
  // because the forked child must not touch the trace channel's mutex/allocator.
  const auto fork_start = std::chrono::steady_clock::now();
  const pid_t pid = fork();
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
    // Async-signal-safe pointer store, replacing the child-side setenv/unsetenv.
    if (has_env_overrides) {
      environ = child_envp.data();
    }
    RestoreDefaultSignalsInChild();
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
  // Records the reap phase on every return path out of the block below.
  const ScopeExit reap_scope([&reap_start]() { RecordSpawnPhaseNs("Reap", reap_start); });

  stdin_pipe[1].Reset();
  stdout_pipe[0].Reset();
  stderr_pipe[0].Reset();

  // A child can close/redirect its stdio and then keep running (or hang). PumpChildIo
  // returns once the pipes drain, NOT when the process exits, so without this the
  // blocking waitpid below would ignore timeout_ms entirely. Enforce the remaining
  // deadline here.
  //
  // This used to be a WNOHANG spin with a flat `poll(nullptr, 0, 5)` between probes,
  // on the theory that "the child exits when its stdio closes, so the first WNOHANG
  // catches it with no sleep". That is a race, and it loses far more often than it
  // wins: the child's descriptors are closed by the kernel *during* exit, so the
  // parent routinely observes EOF a few microseconds before the process is
  // reapable — and then slept a full 5 ms. Measured on the compare/git scenarios,
  // that put ~5 ms of pure sleep on the majority of git spawns, several of which run
  // synchronously on the shell thread.
  //
  // Wait on the child as an event instead: a pidfd polls readable exactly when the
  // process becomes reapable, so the common case costs one syscall and no sleep, and
  // the timeout stays exact. pidfd_open sets close-on-exec itself. Where the syscall
  // is unavailable (pre-5.3 Linux, other Unices) fall back to the probe loop with a
  // sub-millisecond backoff rather than a flat 5 ms.
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
        // Sleep until the child is actually reapable (or the deadline lands),
        // rounding the remaining window up so a sub-millisecond tail still waits.
        pollfd child_poll{.fd = child_pidfd.Get(), .events = POLLIN, .revents = 0};
        poll(&child_poll, 1, static_cast<int>(std::min<long long>(remaining_ms + 1, 1000)));
        continue;
      }
      // Only reached without pidfd support: probe at 1 ms instead of 5 ms, so an
      // already-exiting child is reaped within a millisecond of becoming reapable.
      poll(nullptr, 0, 1);
    }
  }

  if (timed_out || output_truncated) {
    // Deadline exceeded or capture ceiling hit: kill the child so the
    // (synchronous) caller is not held hostage by a hung, pathologically slow, or
    // firehose process, then reap it below to avoid a zombie.
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

}  // namespace

SubprocessResult RunSubprocess(const std::vector<std::string>& argv,
                               const SubprocessOptions& options) {
  // Every git invocation, external tool, and one-shot helper the editor runs
  // funnels through here, and each one is a fork+exec plus a blocking wait on a
  // background thread. This was the single largest unmeasured cost in the app:
  // a session could spend seconds in child processes with nothing in any trace
  // to say so.
  util::PerformanceTrace::ScopeLabel label("platform::RunSubprocess");
  if (!argv.empty()) {
    // Program basename only. The full argv would mint a distinct label per
    // invocation (paths, revs, sha1s), which blows the label cap and buries the
    // aggregate rather than ranking it.
    std::string_view program = argv.front();
    const std::size_t slash = program.find_last_of('/');
    if (slash != std::string_view::npos) {
      program.remove_prefix(slash + 1);
    }
    label.Field("program", program);
    // Plus the subcommand, when there is an argument that looks like one: the
    // program basename alone ranks "git is slow", which is never the actionable
    // statement -- `git status` on a big worktree and `git rev-parse` are four
    // orders of magnitude apart, and a row that mixes them cannot say which of
    // the two is on the shell thread.
    //
    // Bounded on purpose: only a short, lowercase, [a-z0-9-] argument qualifies,
    // which admits every git/tool subcommand and rejects the paths, revisions and
    // object ids that would mint a distinct label per invocation and blow the
    // label cap.
    bool skip_next_value = false;
    for (std::size_t i = 1; i < argv.size(); ++i) {
      const std::string_view argument = argv[i];
      if (skip_next_value) {
        skip_next_value = false;
        continue;  // the value of a flag that takes one (e.g. `git -C <dir>`)
      }
      if (!argument.empty() && argument.front() == '-') {
        // git's global flags that consume the following argument.
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

}  // namespace microide::platform
