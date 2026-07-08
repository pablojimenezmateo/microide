#include "platform/TerminalBackend.h"

#include "platform/ShellProcess.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>

#if defined(__APPLE__)
#include <util.h>
#elif defined(__unix__)
#include <pty.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
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

    const std::string shell_path = request.shell.empty() ? DefaultShellPath() : request.shell;
    const std::string shell_name = ShellProgramName(shell_path);
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

      if (chdir(request.working_directory.c_str()) != 0) {
        _exit(127);
      }
      setenv("TERM", "xterm-256color", 1);
      // Advertise 24-bit color so applications enable truecolor output.
      setenv("COLORTERM", "truecolor", 1);
      if (request.command.empty()) {
        execl(shell_path.c_str(), shell_name.c_str(), "-i", nullptr);
      } else {
        execl(shell_path.c_str(), shell_name.c_str(), "-lc", request.command.c_str(), nullptr);
      }
      _exit(127);
    }

    close(slave_fd);

    // Self-pipe used to wake the reader's poll() on shutdown without closing
    // master_fd out from under it (closing an fd another thread is polling is
    // a data race and risks fd-number reuse).
    int wake_pipe[2] = {-1, -1};
    if (pipe(wake_pipe) != 0) {
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
    wake_write_fd_ = wake_pipe[1];
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
    const int wake_write_fd = wake_write_fd_;
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
    wake_write_fd_ = -1;

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
    const int master_fd = master_fd_.load(std::memory_order_acquire);
    if (master_fd < 0 || bytes.empty()) {
      return;
    }

    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const ssize_t written =
          write(master_fd, bytes.data() + static_cast<std::ptrdiff_t>(offset), bytes.size() - offset);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      offset += static_cast<std::size_t>(written);
    }
  }

  bool running() const override { return running_.load(std::memory_order_acquire); }

 private:
  void ReaderMain(int master_fd, int wake_fd, int child_pid) {
    std::array<char, 4096> buffer{};
    while (!stop_requested_.load(std::memory_order_acquire)) {
      std::array<pollfd, 2> pfds{
          pollfd{.fd = master_fd, .events = POLLIN | POLLHUP, .revents = 0},
          pollfd{.fd = wake_fd, .events = POLLIN, .revents = 0},
      };
      // No timeout: the wake pipe interrupts the poll on shutdown, so the
      // reader never busy-polls while idle.
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
      // Shutdown signalled via the self-pipe: stop before touching master_fd.
      if ((pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        break;
      }
      if ((pfds[0].revents & (POLLERR | POLLNVAL)) != 0) {
        break;
      }
      if ((pfds[0].revents & (POLLIN | POLLHUP)) == 0) {
        continue;
      }
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
      break;
    }

    int status = 0;
    while (waitpid(child_pid, &status, 0) < 0 && errno == EINTR) {
    }

    running_.store(false, std::memory_order_release);
    if (!stop_requested_.load(std::memory_order_acquire) && callbacks_.on_exit) {
      callbacks_.on_exit();
    }
  }

  TerminalBackendCallbacks callbacks_;
  std::thread reader_thread_;
  std::atomic<int> master_fd_{-1};
  int wake_read_fd_ = -1;
  int wake_write_fd_ = -1;
  int child_pid_ = -1;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
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
