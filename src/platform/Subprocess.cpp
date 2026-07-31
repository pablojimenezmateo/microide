#include "platform/Subprocess.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>

#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"

#if defined(__unix__) || defined(__APPLE__)
#include <array>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util/PosixPipe.h"
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <array>
#include <thread>
#endif

namespace microide::platform {

namespace {

// Per-stream ceiling on captured stdout/stderr. Without it a child that emits
// gigabytes (a pathological `git show` of a huge blob, a plugin tool flooding
// stdout) grows the capture string until the host OOM-aborts. At the ceiling the
// stream is truncated and the child is torn down. 128 MiB is far above any
// legitimate git/tool output an IDE consumes.
constexpr std::size_t kMaxCaptureBytes = 128ull * 1024 * 1024;

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

#if defined(__unix__) || defined(__APPLE__)

class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : fd_(fd) {}

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept : fd_(other.Release()) {}

  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      Reset(other.Release());
    }
    return *this;
  }

  ~UniqueFd() { Reset(); }

  int Get() const { return fd_; }
  bool IsValid() const { return fd_ >= 0; }

  int Release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

  void Reset(int fd = -1) {
    if (fd_ >= 0) {
      close(fd_);
    }
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

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

void SetNonBlocking(int fd) {
  if (fd < 0) {
    return;
  }
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
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
      if (DrainReadyPipe(stdout_fd, stdout_text, kMaxCaptureBytes)) {
        if (out_truncated != nullptr) *out_truncated = true;
        return false;  // capture ceiling hit: stop and let the caller tear down
      }
    }
    if (stderr_index >= 0 &&
        (poll_fds[stderr_index].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      if (DrainReadyPipe(stderr_fd, stderr_text, kMaxCaptureBytes)) {
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

extern "C" char** environ;

// Build the child's environment (the current environ with overrides applied) in
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

#elif defined(_WIN32)

class WinHandle {
 public:
  WinHandle() = default;
  explicit WinHandle(HANDLE handle) : handle_(handle) {}
  ~WinHandle() { Reset(); }

  WinHandle(const WinHandle&) = delete;
  WinHandle& operator=(const WinHandle&) = delete;

  WinHandle(WinHandle&& other) noexcept : handle_(other.Release()) {}
  WinHandle& operator=(WinHandle&& other) noexcept {
    if (this != &other) {
      Reset(other.Release());
    }
    return *this;
  }

  HANDLE Get() const { return handle_; }
  bool IsValid() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
  HANDLE Release() {
    HANDLE handle = handle_;
    handle_ = nullptr;
    return handle;
  }
  void Reset(HANDLE handle = nullptr) {
    if (IsValid()) {
      CloseHandle(handle_);
    }
    handle_ = handle;
  }

 private:
  HANDLE handle_ = nullptr;
};

std::wstring ToWide(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                       nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

std::wstring QuoteWindowsArgument(std::string_view arg) {
  if (arg.empty()) {
    return L"\"\"";
  }

  bool needs_quotes = false;
  for (char c : arg) {
    if (c == ' ' || c == '\t' || c == '"') {
      needs_quotes = true;
      break;
    }
  }
  if (!needs_quotes) {
    return ToWide(arg);
  }

  std::wstring quoted = L"\"";
  std::size_t backslashes = 0;
  for (char c : arg) {
    if (c == '\\') {
      ++backslashes;
      continue;
    }
    if (c == '"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(L'"');
      backslashes = 0;
      continue;
    }
    if (backslashes > 0) {
      quoted.append(backslashes, L'\\');
      backslashes = 0;
    }
    quoted.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
  }
  if (backslashes > 0) {
    quoted.append(backslashes * 2, L'\\');
  }
  quoted.push_back(L'"');
  return quoted;
}

std::wstring BuildCommandLine(const std::vector<std::string>& argv) {
  std::wstring command_line;
  bool first = true;
  for (const auto& arg : argv) {
    if (!first) {
      command_line.push_back(L' ');
    }
    first = false;
    command_line += QuoteWindowsArgument(arg);
  }
  return command_line;
}

std::wstring BuildEnvironmentBlock(
    const std::vector<SubprocessEnvironmentOverride>& overrides) {
  std::vector<std::wstring> entries;
  if (LPWCH raw_environment = GetEnvironmentStringsW(); raw_environment != nullptr) {
    for (LPCWCH current = raw_environment; *current != L'\0';
         current += std::wcslen(current) + 1) {
      entries.emplace_back(current);
    }
    FreeEnvironmentStringsW(raw_environment);
  }

  auto matches_name = [](std::wstring_view entry, std::wstring_view name) {
    const std::size_t separator = entry.find(L'=');
    return separator != std::wstring_view::npos &&
           entry.substr(0, separator) == name;
  };

  for (const auto& override_entry : overrides) {
    if (!IsValidEnvironmentName(override_entry.name)) {
      continue;
    }
    const std::wstring wide_name = ToWide(override_entry.name);
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const std::wstring& entry) {
                                   return matches_name(entry, wide_name);
                                 }),
                  entries.end());
    if (override_entry.value.has_value()) {
      entries.push_back(wide_name + L"=" + ToWide(*override_entry.value));
    }
  }

  std::sort(entries.begin(), entries.end());
  std::wstring block;
  for (const auto& entry : entries) {
    block.append(entry);
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

void DrainPipeToString(HANDLE handle, std::string* output) {
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE || output == nullptr) {
    return;
  }
  std::array<char, 4096> buffer{};
  DWORD bytes_read = 0;
  while (ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr) &&
         bytes_read > 0) {
    if (output->size() >= kMaxCaptureBytes) {
      break;  // capture ceiling reached: stop growing the buffer (see kMaxCaptureBytes)
    }
    const std::size_t room = kMaxCaptureBytes - output->size();
    output->append(buffer.data(), std::min(room, static_cast<std::size_t>(bytes_read)));
  }
}

void WriteAllToHandle(HANDLE handle, const std::string& text) {
  const char* data = text.data();
  std::size_t remaining = text.size();
  while (remaining > 0) {
    DWORD written = 0;
    if (!WriteFile(handle, data, static_cast<DWORD>(remaining), &written, nullptr) ||
        written == 0) {
      break;
    }
    data += written;
    remaining -= written;
  }
}

#endif

// Split from RunSubprocess so the instrumentation wrapper below covers every one
// of this function's ~10 early returns without threading a scope guard through
// each of them.
SubprocessResult RunSubprocessUninstrumented(const std::vector<std::string>& argv,
                                             const SubprocessOptions& options) {
  SubprocessResult result;
  if (argv.empty()) {
    return result;
  }

#if defined(__unix__) || defined(__APPLE__)
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
    ApplyChildSandbox(options.sandbox);
    execvp(raw_argv[0], raw_argv.data());
    _exit(errno == ENOENT ? 127 : 126);
  }

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
  stdin_pipe[1].Reset();
  stdout_pipe[0].Reset();
  stderr_pipe[0].Reset();

  // A child can close/redirect its stdio and then keep running (or hang). PumpChildIo
  // returns once the pipes drain, NOT when the process exits, so without this the
  // blocking waitpid below would ignore timeout_ms entirely. Enforce the remaining
  // deadline with a non-blocking waitpid poll; the common case (child exits when its
  // stdio closes) is caught by the very first WNOHANG with no sleep.
  if (!timed_out && !output_truncated && options.timeout_ms > 0) {
    const auto deadline = wait_start + std::chrono::milliseconds(options.timeout_ms);
    while (true) {
      int probe_status = 0;
      const pid_t probed = waitpid(pid, &probe_status, WNOHANG);
      if (probed == pid) {
        if (WIFEXITED(probe_status)) {
          result.exit_code = WEXITSTATUS(probe_status);
        } else if (WIFSIGNALED(probe_status)) {
          result.exit_code = 128 + WTERMSIG(probe_status);
        }
        return result;
      }
      if (probed < 0 && errno != EINTR) {
        result.exit_code = -1;
        return result;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        timed_out = true;
        break;
      }
      poll(nullptr, 0, 5);  // brief sleep; only reached while the child lingers
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
#elif defined(_WIN32)
  SECURITY_ATTRIBUTES attributes{
      .nLength = sizeof(SECURITY_ATTRIBUTES),
      .lpSecurityDescriptor = nullptr,
      .bInheritHandle = TRUE,
  };

  WinHandle stdout_read;
  WinHandle stdout_write;
  WinHandle stderr_read;
  WinHandle stderr_write;
  WinHandle stdin_read;
  WinHandle stdin_write;

  auto open_pipe = [&](bool enabled, WinHandle& read_end, WinHandle& write_end) {
    if (!enabled) {
      return true;
    }
    HANDLE raw_read = nullptr;
    HANDLE raw_write = nullptr;
    if (!CreatePipe(&raw_read, &raw_write, &attributes, 0)) {
      return false;
    }
    read_end.Reset(raw_read);
    write_end.Reset(raw_write);
    return true;
  };

  const bool needs_stdin = true;
  if (!open_pipe(options.capture_stdout, stdout_read, stdout_write) ||
      !open_pipe(options.capture_stderr, stderr_read, stderr_write) ||
      !open_pipe(needs_stdin, stdin_read, stdin_write)) {
    return result;
  }

  if (stdout_read.IsValid()) {
    SetHandleInformation(stdout_read.Get(), HANDLE_FLAG_INHERIT, 0);
  }
  if (stderr_read.IsValid()) {
    SetHandleInformation(stderr_read.Get(), HANDLE_FLAG_INHERIT, 0);
  }
  if (stdin_write.IsValid()) {
    SetHandleInformation(stdin_write.Get(), HANDLE_FLAG_INHERIT, 0);
  }

  WinHandle nul_handle;
  if (!options.capture_stderr && options.silence_stderr) {
    nul_handle.Reset(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  }

  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  startup_info.hStdInput = stdin_read.Get();
  startup_info.hStdOutput =
      options.capture_stdout ? stdout_write.Get() : GetStdHandle(STD_OUTPUT_HANDLE);
  startup_info.hStdError = options.capture_stderr
                               ? stderr_write.Get()
                               : (options.silence_stderr && nul_handle.IsValid()
                                      ? nul_handle.Get()
                                      : GetStdHandle(STD_ERROR_HANDLE));

  PROCESS_INFORMATION process_info{};
  std::wstring command_line = BuildCommandLine(argv);
  std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back(L'\0');
  std::wstring cwd = options.cwd.empty() ? std::wstring{} : options.cwd.wstring();
  std::wstring environment_block = BuildEnvironmentBlock(options.environment_overrides);
  void* environment =
      options.environment_overrides.empty() ? nullptr : static_cast<void*>(environment_block.data());
  const DWORD creation_flags =
      options.environment_overrides.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT;
  if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, creation_flags,
                      environment,
                      cwd.empty() ? nullptr : cwd.c_str(), &startup_info, &process_info)) {
    result.exit_code = -1;
    result.stderr_text = "CreateProcessW failed";
    return result;
  }

  stdout_write.Reset();
  stderr_write.Reset();
  stdin_read.Reset();

  std::thread stdout_thread;
  std::thread stderr_thread;
  if (options.capture_stdout && stdout_read.IsValid()) {
    stdout_thread = std::thread([&]() { DrainPipeToString(stdout_read.Get(), &result.stdout_text); });
  }
  if (options.capture_stderr && stderr_read.IsValid()) {
    stderr_thread = std::thread([&]() { DrainPipeToString(stderr_read.Get(), &result.stderr_text); });
  }
  if (stdin_write.IsValid() && !options.stdin_text.empty()) {
    WriteAllToHandle(stdin_write.Get(), options.stdin_text);
  }
  stdin_write.Reset();

  // Honor options.timeout_ms, mirroring the POSIX deadline path: a bounded wait,
  // then TerminateProcess + a short reap so a hung/slow child cannot hold the
  // synchronous caller forever. timeout_ms <= 0 waits indefinitely as before.
  if (options.timeout_ms > 0) {
    const DWORD wait_result =
        WaitForSingleObject(process_info.hProcess, static_cast<DWORD>(options.timeout_ms));
    if (wait_result == WAIT_TIMEOUT) {
      result.timed_out = true;
      TerminateProcess(process_info.hProcess, 1);
      WaitForSingleObject(process_info.hProcess, 2000);  // reap the killed child
    }
  } else {
    WaitForSingleObject(process_info.hProcess, INFINITE);
  }
  DWORD exit_code = 0;
  if (GetExitCodeProcess(process_info.hProcess, &exit_code)) {
    result.exit_code = static_cast<int>(exit_code);
  }
  if (result.timed_out) {
    result.exit_code = -1;
  }

  if (stdout_thread.joinable()) {
    stdout_thread.join();
  }
  if (stderr_thread.joinable()) {
    stderr_thread.join();
  }

  stdout_read.Reset();
  stderr_read.Reset();
  CloseHandle(process_info.hThread);
  CloseHandle(process_info.hProcess);
  return result;
#else
  result.stderr_text = "Subprocess execution is not implemented on this platform";
  return result;
#endif
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
