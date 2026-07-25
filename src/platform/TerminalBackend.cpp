#include "platform/TerminalBackend.h"

#include "platform/ShellProcess.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <util.h>
#elif defined(__unix__)
#include <pty.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util/PosixPipe.h"
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

extern "C" {
HRESULT WINAPI CreatePseudoConsole(COORD size,
                                   HANDLE hInput,
                                   HANDLE hOutput,
                                   DWORD dwFlags,
                                   HPCON* phPC);
HRESULT WINAPI ResizePseudoConsole(HPCON hPC, COORD size);
void WINAPI ClosePseudoConsole(HPCON hPC);
}
#endif

#if defined(__unix__) || defined(__APPLE__)
extern "C" char** environ;
#endif

namespace microide::platform {

namespace {

#if defined(__unix__) || defined(__APPLE__)

class PosixTerminalBackend final : public TerminalBackend {
 public:
  PosixTerminalBackend() = default;
  ~PosixTerminalBackend() override { Stop(); }

  TerminalStartResult Start(const TerminalStartRequest& request,
                            TerminalBackendCallbacks callbacks) override {
    Stop();

    int master_fd = -1;
    int slave_fd = -1;
    winsize window_size{};
    window_size.ws_row = static_cast<unsigned short>(std::min<std::size_t>(request.rows, 65535));
    window_size.ws_col = static_cast<unsigned short>(std::min<std::size_t>(request.columns, 65535));
    if (openpty(&master_fd, &slave_fd, nullptr, nullptr, &window_size) != 0) {
      return TerminalStartResult{
          .started = false,
          .running = false,
          .child_process_id = -1,
          .launch_label = request.command.empty() ? "terminal unavailable" : request.command,
          .initial_output = "failed to allocate PTY for terminal session.",
      };
    }

    // Mark both PTY ends close-on-exec. Without this, any unrelated child forked
    // by another thread (Subprocess/AsyncSubprocess on the background executor)
    // inherits copies of the master and slave: the leaked slave keeps the master
    // from ever seeing EOF/POLLHUP when the shell exits (the reader poll() hangs
    // and on_exit never fires), and the master leaks into every such subprocess,
    // pinning the PTY device open past terminal teardown. The shell child re-opens
    // its std fds via dup2(slave_fd, 0/1/2) below, which clears CLOEXEC on those
    // duplicates, so the shell keeps working. Mirrors Subprocess.cpp.
    (void)fcntl(master_fd, F_SETFD, fcntl(master_fd, F_GETFD, 0) | FD_CLOEXEC);
    (void)fcntl(slave_fd, F_SETFD, fcntl(slave_fd, F_GETFD, 0) | FD_CLOEXEC);

    const std::string shell_path = request.shell.empty() ? DefaultShellPath() : request.shell;
    const std::string shell_name = ShellProgramName(shell_path);

    // Build the child environment in the PARENT: setenv() after fork() is not
    // async-signal-safe (it allocates and takes the environ lock), so a fork racing
    // another thread — this app always has file-watcher and background-executor
    // threads live — could deadlock the child before exec, leaving the terminal
    // silently dead. The child only assigns `environ` to this array (a single
    // pointer store). `env_storage`/`env_pointers` live on this stack frame across
    // the fork so the child's copy stays valid through exec. Mirrors Subprocess.cpp.
    std::vector<std::string> env_storage;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
      const std::string_view text(*entry);
      if (text.rfind("TERM=", 0) == 0 || text.rfind("COLORTERM=", 0) == 0) {
        continue;
      }
      env_storage.emplace_back(*entry);
    }
    env_storage.emplace_back("TERM=xterm-256color");
    // Advertise 24-bit color so applications enable truecolor output.
    env_storage.emplace_back("COLORTERM=truecolor");
    std::vector<char*> env_pointers;
    env_pointers.reserve(env_storage.size() + 1);
    for (std::string& entry : env_storage) {
      env_pointers.push_back(entry.data());
    }
    env_pointers.push_back(nullptr);

    const pid_t child_pid = fork();
    if (child_pid < 0) {
      close(master_fd);
      close(slave_fd);
      return TerminalStartResult{
          .started = false,
          .running = false,
          .child_process_id = -1,
          .launch_label = request.command.empty() ? shell_name : request.command,
          .initial_output = "failed to fork terminal session.",
      };
    }

    if (child_pid == 0) {
      setsid();
      ioctl(slave_fd, TIOCSCTTY, 0);
      dup2(slave_fd, STDIN_FILENO);
      dup2(slave_fd, STDOUT_FILENO);
      dup2(slave_fd, STDERR_FILENO);
      close(master_fd);
      if (slave_fd > STDERR_FILENO) {
        close(slave_fd);
      }

      // Only chdir when a directory was requested; an empty path would make chdir
      // fail and _exit(127), silently killing the shell (matches Subprocess.cpp).
      if (!request.working_directory.empty() &&
          chdir(request.working_directory.c_str()) != 0) {
        _exit(127);
      }
      // Async-signal-safe pointer store instead of setenv(); the array was built in
      // the parent above.
      environ = env_pointers.data();
      if (request.command.empty()) {
        execl(shell_path.c_str(), shell_name.c_str(), "-i", nullptr);
      } else {
        execl(shell_path.c_str(), shell_name.c_str(), "-lc", request.command.c_str(), nullptr);
      }
      _exit(127);
    }

    close(slave_fd);

    // Make the master non-blocking so Write() never parks the calling (UI or
    // reader) thread inside write(): a child that stops draining its stdin (a
    // pager paused on a huge paste, a frozen program) would otherwise fill the
    // PTY input buffer and block whoever writes to it — historically the shell
    // thread on paste/keystroke, freezing the whole app. Reads stay gated by
    // poll(POLLIN) so a non-blocking read only yields the already-ready bytes.
    (void)fcntl(master_fd, F_SETFL, fcntl(master_fd, F_GETFL, 0) | O_NONBLOCK);

    // Self-pipe used to wake the reader's poll() on shutdown (and now to nudge
    // it to drain freshly-buffered output) without closing master_fd out from
    // under it (closing an fd another thread is polling is a data race and risks
    // fd-number reuse). O_NONBLOCK on both ends so a wake never blocks the
    // producer and the reader can drain all pending wake bytes without parking.
    // CLOEXEC both wake-pipe ends: they must not leak into subprocesses forked
    // by other threads while the terminal is live (same hazard as the PTY fds).
    int wake_pipe[2] = {-1, -1};
    if (!util::MakeCloexecPipe(wake_pipe, /*nonblocking=*/true)) {
      close(master_fd);
      RequestTerminalChildShutdown(child_pid);
      return TerminalStartResult{
          .started = false,
          .running = false,
          .child_process_id = -1,
          .launch_label = request.command.empty() ? shell_name : request.command,
          .initial_output = "failed to allocate terminal wake pipe.",
      };
    }

    callbacks_ = std::move(callbacks);
    master_fd_.store(master_fd, std::memory_order_release);
    wake_read_fd_ = wake_pipe[0];
    wake_write_fd_.store(wake_pipe[1], std::memory_order_release);
    child_pid_ = child_pid;
    running_ = true;
    stop_requested_ = false;
    reader_thread_ =
        std::thread(&PosixTerminalBackend::ReaderMain, this, master_fd, wake_pipe[0], child_pid);

    return TerminalStartResult{
        .started = true,
        .running = true,
        .child_process_id = static_cast<int>(child_pid),
        .launch_label = request.command.empty() ? shell_name : request.command,
        .initial_output = {},
    };
  }

  void Stop() override {
    const int master_fd = master_fd_.exchange(-1, std::memory_order_acq_rel);
    const int child_pid = child_pid_;
    const int wake_write_fd = wake_write_fd_.load(std::memory_order_acquire);
    const int wake_read_fd = wake_read_fd_;
    stop_requested_.store(true, std::memory_order_release);
    child_pid_ = -1;

    // Wake the reader's poll() via the self-pipe, then reap the child, then
    // join. Only once the reader has fully stopped touching master_fd is it
    // safe to close it.
    if (wake_write_fd >= 0) {
      const char byte = 1;
      ssize_t result = 0;
      do {
        result = write(wake_write_fd, &byte, 1);
      } while (result < 0 && errno == EINTR);
    }
    RequestTerminalChildShutdown(child_pid);

    if (reader_thread_.joinable()) {
      reader_thread_.join();
    }

    if (master_fd >= 0) {
      close(master_fd);
    }
    if (wake_read_fd >= 0) {
      close(wake_read_fd);
    }
    if (wake_write_fd >= 0) {
      close(wake_write_fd);
    }
    wake_read_fd_ = -1;
    wake_write_fd_.store(-1, std::memory_order_release);

    // The reader has joined and the master is closed, so no thread can touch the
    // write buffer anymore; drop any input the stopped child never drained.
    {
      std::lock_guard<std::mutex> lock(write_mutex_);
      pending_write_.clear();
      pending_write_offset_ = 0;
    }

    running_.store(false, std::memory_order_release);
    stop_requested_.store(false, std::memory_order_release);
  }

  void Resize(std::size_t rows, std::size_t columns) override {
    const int master_fd = master_fd_.load(std::memory_order_acquire);
    if (master_fd < 0) {
      return;
    }

    winsize window_size{};
    window_size.ws_row = static_cast<unsigned short>(std::min<std::size_t>(rows, 65535));
    window_size.ws_col = static_cast<unsigned short>(std::min<std::size_t>(columns, 65535));
    ioctl(master_fd, TIOCSWINSZ, &window_size);
  }

  void Write(std::string_view bytes) override {
    if (master_fd_.load(std::memory_order_acquire) < 0 || bytes.empty()) {
      return;
    }

    // Never write to the PTY here: buffer the bytes and let the reader thread
    // (the sole writer, so output bytes never interleave) drain them when the
    // master reports POLLOUT. This keeps every caller — the UI thread on
    // keystroke/paste, the reader thread on a query reply — off the blocking
    // write() path entirely. Bound the buffer so a permanently-stuck child can
    // not grow it without limit; align the cap with the session paste cap so a
    // legitimate large paste to a draining child is never truncated (only a
    // child that refuses to read past 64 MiB drops the tail).
    {
      std::lock_guard<std::mutex> lock(write_mutex_);
      const std::size_t live = pending_write_.size() - pending_write_offset_;
      const std::size_t room = live >= kMaxPendingWriteBytes ? 0 : kMaxPendingWriteBytes - live;
      if (room == 0) {
        return;
      }
      if (bytes.size() > room) {
        bytes = bytes.substr(0, room);
      }
      pending_write_.append(bytes);
    }
    WakeReader();
  }

  bool running() const override { return running_.load(std::memory_order_acquire); }

 private:
  void ReaderMain(int master_fd, int wake_fd, int child_pid) {
    std::array<char, 4096> buffer{};
    while (!stop_requested_.load(std::memory_order_acquire)) {
      bool has_pending_write = false;
      {
        std::lock_guard<std::mutex> lock(write_mutex_);
        has_pending_write = pending_write_offset_ < pending_write_.size();
      }
      std::array<pollfd, 2> pfds{
          pollfd{.fd = master_fd,
                 .events = static_cast<short>(POLLIN | POLLHUP | (has_pending_write ? POLLOUT : 0)),
                 .revents = 0},
          pollfd{.fd = wake_fd, .events = POLLIN, .revents = 0},
      };
      // No timeout: the wake pipe interrupts the poll on shutdown (and whenever
      // a producer buffers output), so the reader never busy-polls while idle.
      const int ready = poll(pfds.data(), 2, -1);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      if (ready == 0) {
        continue;
      }
      // Wake pipe readable: drain every pending wake byte (the pipe is
      // non-blocking, so the drain loop ends on EAGAIN), then re-check for
      // shutdown. A wake means either Stop() (stop_requested_ set) or a
      // producer appended output to drain via POLLOUT below.
      if ((pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        std::array<char, 256> drain{};
        while (read(wake_fd, drain.data(), drain.size()) > 0) {
        }
        if (stop_requested_.load(std::memory_order_acquire)) {
          break;
        }
      }
      if ((pfds[0].revents & (POLLERR | POLLNVAL)) != 0) {
        break;
      }
      // Flush buffered input to the child while the master accepts writes.
      if ((pfds[0].revents & POLLOUT) != 0) {
        DrainPendingWrite(master_fd);
      }
      if ((pfds[0].revents & (POLLIN | POLLHUP)) == 0) {
        continue;
      }
      const bool hangup = (pfds[0].revents & POLLHUP) != 0;
      const ssize_t count = read(master_fd, buffer.data(), buffer.size());
      if (count > 0) {
        if (callbacks_.on_output) {
          callbacks_.on_output(std::string_view(buffer.data(), static_cast<std::size_t>(count)));
        }
        continue;
      }
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        // Non-blocking master with nothing ready: a spurious POLLIN, or a
        // hangup whose buffered data is already drained. Break on hangup so we
        // do not spin re-polling a POLLHUP that never yields more bytes.
        if (hangup) {
          break;
        }
        continue;
      }
      break;
    }

    // Reap only on a natural exit. When Stop() drives shutdown it sets
    // stop_requested_ (release) before waking us and then reaps the child itself
    // via RequestTerminalChildShutdown; reaping here too would run a second reaper
    // on the same pid concurrently. On the wake-pipe path an acquire load observes
    // Stop()'s store, but the natural-EOF path is unblocked by master_fd (POLLHUP),
    // NOT the wake pipe, so there is no synchronization edge with a concurrent
    // Stop(). A *blocking* waitpid here would then race Stop()'s reap and, if the
    // pid is recycled by another of the app's forks in between, block on that
    // unrelated child until it exits — hanging Stop()'s join. Use a bounded
    // non-blocking reap instead: it collects the (already-dead) child in the
    // common case and can never latch onto a reused pid.
    if (!stop_requested_.load(std::memory_order_acquire)) {
      int status = 0;
      for (int attempt = 0; attempt < 100; ++attempt) {
        const pid_t reaped = waitpid(child_pid, &status, WNOHANG);
        if (reaped == child_pid || (reaped < 0 && errno != EINTR)) {
          break;  // reaped, or already gone (ECHILD) — either way done
        }
        if (stop_requested_.load(std::memory_order_acquire)) {
          break;  // Stop() has taken ownership of reaping
        }
        // reaped == 0: exit not yet visible; pause briefly (1ms) and retry.
        struct pollfd none {};
        poll(&none, 0, 1);
      }
    }

    running_.store(false, std::memory_order_release);
    if (!stop_requested_.load(std::memory_order_acquire) && callbacks_.on_exit) {
      callbacks_.on_exit();
    }
  }

  // Non-blocking, best-effort nudge so a producer thread's Write() wakes the
  // reader's poll() to drain the freshly-buffered output. A full wake pipe
  // (EAGAIN) already has a byte pending, so the reader will still wake.
  void WakeReader() {
    const int fd = wake_write_fd_.load(std::memory_order_acquire);
    if (fd < 0) {
      return;
    }
    const char byte = 1;
    ssize_t result = 0;
    do {
      result = write(fd, &byte, 1);
    } while (result < 0 && errno == EINTR);
  }

  // Runs only on the reader thread (the sole writer to master_fd, so buffered
  // input bytes never interleave with each other). The master is non-blocking,
  // so each write() returns immediately: a partial write leaves the remainder
  // for the next POLLOUT.
  void DrainPendingWrite(int master_fd) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    while (pending_write_offset_ < pending_write_.size()) {
      const char* data = pending_write_.data() + static_cast<std::ptrdiff_t>(pending_write_offset_);
      const std::size_t length = pending_write_.size() - pending_write_offset_;
      const ssize_t written = write(master_fd, data, length);
      if (written > 0) {
        pending_write_offset_ += static_cast<std::size_t>(written);
        continue;
      }
      if (written < 0 && errno == EINTR) {
        continue;
      }
      if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;  // master full; the next POLLOUT resumes the drain
      }
      // Hard write error (child gone / EIO): drop the buffer so the reader can
      // not spin re-arming POLLOUT on a dead master.
      pending_write_.clear();
      pending_write_offset_ = 0;
      return;
    }
    if (pending_write_offset_ >= pending_write_.size()) {
      pending_write_.clear();
      pending_write_offset_ = 0;
    } else if (pending_write_offset_ >= (std::size_t{1} << 16)) {
      // Reclaim the consumed prefix once it grows past 64 KiB so a slow-draining
      // child does not keep an ever-growing already-sent head in memory.
      pending_write_.erase(0, pending_write_offset_);
      pending_write_offset_ = 0;
    }
  }

  // Aligned with the session-level paste cap so a legitimate large paste to a
  // draining child is buffered in full; only a child that refuses to read this
  // much drops the tail.
  static constexpr std::size_t kMaxPendingWriteBytes = 64u << 20;

  TerminalBackendCallbacks callbacks_;
  std::thread reader_thread_;
  std::atomic<int> master_fd_{-1};
  int wake_read_fd_ = -1;
  std::atomic<int> wake_write_fd_{-1};
  int child_pid_ = -1;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::mutex write_mutex_;
  std::string pending_write_;
  std::size_t pending_write_offset_ = 0;
};

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
  bool Valid() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
  HANDLE Release() {
    HANDLE handle = handle_;
    handle_ = nullptr;
    return handle;
  }
  void Reset(HANDLE handle = nullptr) {
    if (Valid()) {
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

std::wstring ToWidePath(const std::filesystem::path& path) {
  return path.wstring();
}

class WindowsTerminalBackend final : public TerminalBackend {
 public:
  WindowsTerminalBackend() = default;
  ~WindowsTerminalBackend() override { Stop(); }

  TerminalStartResult Start(const TerminalStartRequest& request,
                            TerminalBackendCallbacks callbacks) override {
    Stop();
    callbacks_ = std::move(callbacks);

    SECURITY_ATTRIBUTES attributes{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = nullptr,
        .bInheritHandle = TRUE,
    };

    HANDLE input_read = nullptr;
    HANDLE input_write = nullptr;
    HANDLE output_read = nullptr;
    HANDLE output_write = nullptr;
    if (!CreatePipe(&input_read, &input_write, &attributes, 0) ||
        !CreatePipe(&output_read, &output_write, &attributes, 0)) {
      return FailureResult(request.command, "failed to create terminal pipes.");
    }

    input_read_.Reset(input_read);
    input_write_.Reset(input_write);
    output_read_.Reset(output_read);
    output_write_.Reset(output_write);
    SetHandleInformation(input_write_.Get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(output_read_.Get(), HANDLE_FLAG_INHERIT, 0);

    const COORD size{
        static_cast<SHORT>(std::min<std::size_t>(request.columns, 32767)),
        static_cast<SHORT>(std::min<std::size_t>(request.rows, 32767)),
    };
    if (FAILED(CreatePseudoConsole(size, input_read_.Get(), output_write_.Get(), 0, &pseudo_console_))) {
      return FailureResult(request.command,
                           "Windows pseudoconsole is unavailable on this host.");
    }

    SIZE_T attribute_list_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_list_size);
    attribute_list_.resize(attribute_list_size);
    auto* attribute_list =
        reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_list_.data());
    if (!InitializeProcThreadAttributeList(attribute_list, 1, 0, &attribute_list_size)) {
      return FailureResult(request.command, "failed to initialize pseudoconsole attributes.");
    }
    if (!UpdateProcThreadAttribute(attribute_list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   pseudo_console_, sizeof(pseudo_console_), nullptr, nullptr)) {
      return FailureResult(request.command, "failed to attach pseudoconsole attributes.");
    }

    const std::string shell_path = request.shell.empty() ? DefaultShellPath() : request.shell;
    std::wstring shell = ToWide(shell_path);
    const std::wstring shell_name = ToWide(ShellProgramName(shell_path));
    std::wstring command_line = shell;
    if (!request.command.empty()) {
      command_line += L" /D /C ";
      command_line += ToWide(request.command);
    }

    STARTUPINFOEXW startup_info{};
    startup_info.StartupInfo.cb = sizeof(startup_info);
    startup_info.lpAttributeList = attribute_list;

    std::wstring cwd = ToWidePath(request.working_directory);
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT, nullptr,
                        cwd.empty() ? nullptr : cwd.c_str(), &startup_info.StartupInfo,
                        &process_info_)) {
      return FailureResult(request.command, "failed to launch Windows terminal process.");
    }

    input_read_.Reset();
    output_write_.Reset();
    running_ = true;
    stop_requested_ = false;
    reader_thread_ = std::thread(&WindowsTerminalBackend::ReaderMain, this);

    return TerminalStartResult{
        .started = true,
        .running = true,
        .child_process_id = static_cast<int>(process_info_.dwProcessId),
        .launch_label = request.command.empty() ? std::string(shell_name.begin(), shell_name.end())
                                                : request.command,
        .initial_output = {},
    };
  }

  void Stop() override {
    stop_requested_ = true;
    if (input_write_.Valid()) {
      input_write_.Reset();
    }

    if (process_info_.hProcess != nullptr) {
      const DWORD wait_result = WaitForSingleObject(process_info_.hProcess, 200);
      if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(process_info_.hProcess, 1);
      }
    }

    if (reader_thread_.joinable()) {
      reader_thread_.join();
    }

    CleanupProcessHandles();
    if (!attribute_list_.empty()) {
      DeleteProcThreadAttributeList(
          reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_list_.data()));
      attribute_list_.clear();
    }
    if (pseudo_console_ != nullptr) {
      ClosePseudoConsole(pseudo_console_);
      pseudo_console_ = nullptr;
    }
    output_read_.Reset();
    output_write_.Reset();
    input_read_.Reset();
    running_ = false;
    stop_requested_ = false;
  }

  void Resize(std::size_t rows, std::size_t columns) override {
    if (pseudo_console_ == nullptr) {
      return;
    }
    const COORD size{
        static_cast<SHORT>(std::min<std::size_t>(columns, 32767)),
        static_cast<SHORT>(std::min<std::size_t>(rows, 32767)),
    };
    ResizePseudoConsole(pseudo_console_, size);
  }

  void Write(std::string_view bytes) override {
    if (!input_write_.Valid() || bytes.empty()) {
      return;
    }

    std::size_t offset = 0;
    while (offset < bytes.size()) {
      DWORD written = 0;
      if (!WriteFile(input_write_.Get(), bytes.data() + offset,
                     static_cast<DWORD>(bytes.size() - offset), &written, nullptr) ||
          written == 0) {
        break;
      }
      offset += written;
    }
  }

  bool running() const override { return running_; }

 private:
  TerminalStartResult FailureResult(std::string_view command, std::string message) {
    CleanupProcessHandles();
    if (!attribute_list_.empty()) {
      DeleteProcThreadAttributeList(
          reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_list_.data()));
      attribute_list_.clear();
    }
    if (pseudo_console_ != nullptr) {
      ClosePseudoConsole(pseudo_console_);
      pseudo_console_ = nullptr;
    }
    output_read_.Reset();
    output_write_.Reset();
    input_read_.Reset();
    input_write_.Reset();
    return TerminalStartResult{
        .started = false,
        .running = false,
        .child_process_id = -1,
        .launch_label = command.empty() ? "terminal unavailable" : std::string(command),
        .initial_output = std::move(message),
    };
  }

  void CleanupProcessHandles() {
    if (process_info_.hThread != nullptr) {
      CloseHandle(process_info_.hThread);
      process_info_.hThread = nullptr;
    }
    if (process_info_.hProcess != nullptr) {
      CloseHandle(process_info_.hProcess);
      process_info_.hProcess = nullptr;
    }
    process_info_.dwProcessId = 0;
    process_info_.dwThreadId = 0;
  }

  void ReaderMain() {
    std::array<char, 4096> buffer{};
    while (output_read_.Valid()) {
      DWORD bytes_read = 0;
      if (!ReadFile(output_read_.Get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                    &bytes_read, nullptr) ||
          bytes_read == 0) {
        break;
      }
      if (callbacks_.on_output) {
        callbacks_.on_output(
            std::string_view(buffer.data(), static_cast<std::size_t>(bytes_read)));
      }
    }

    if (process_info_.hProcess != nullptr) {
      WaitForSingleObject(process_info_.hProcess, INFINITE);
    }
    running_ = false;
    if (!stop_requested_ && callbacks_.on_exit) {
      callbacks_.on_exit();
    }
  }

  TerminalBackendCallbacks callbacks_;
  std::thread reader_thread_;
  std::vector<std::byte> attribute_list_;
  WinHandle input_read_;
  WinHandle input_write_;
  WinHandle output_read_;
  WinHandle output_write_;
  PROCESS_INFORMATION process_info_{};
  HPCON pseudo_console_ = nullptr;
  bool running_ = false;
  bool stop_requested_ = false;
};

#else

class UnsupportedTerminalBackend final : public TerminalBackend {
 public:
  TerminalStartResult Start(const TerminalStartRequest& request,
                            TerminalBackendCallbacks) override {
    return TerminalStartResult{
        .started = false,
        .running = false,
        .child_process_id = -1,
        .launch_label = request.command.empty() ? "terminal unavailable" : request.command,
        .initial_output = "terminal support is unavailable on this host.",
    };
  }

  void Stop() override {}
  void Resize(std::size_t, std::size_t) override {}
  void Write(std::string_view) override {}
  bool running() const override { return false; }
};

#endif

}  // namespace

std::unique_ptr<TerminalBackend> CreateTerminalBackend() {
#if defined(__unix__) || defined(__APPLE__)
  return std::make_unique<PosixTerminalBackend>();
#elif defined(_WIN32)
  return std::make_unique<WindowsTerminalBackend>();
#else
  return std::make_unique<UnsupportedTerminalBackend>();
#endif
}

}  // namespace microide::platform
