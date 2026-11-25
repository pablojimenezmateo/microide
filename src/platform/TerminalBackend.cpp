#include "platform/TerminalBackend.h"

#include "platform/ChildSignals.h"
#include "platform/OpenFileLimit.h"
#include "platform/ShellProcess.h"
#include "platform/TerminalLifeTrace.h"

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

#include <cerrno>
#include <fcntl.h>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util/PosixFd.h"
#include "util/PosixPipe.h"
#include "util/ThreadName.h"

extern "C" char** environ;

namespace microide::platform {

namespace {

#ifndef MICROIDE_VERSION
#define MICROIDE_VERSION "dev"
#endif
constexpr const char* kTerminalProgramVersion = MICROIDE_VERSION;

bool IsTerminalIdentityEntry(std::string_view entry) {
  static constexpr std::string_view kKeys[] = {
      "TERM=", "COLORTERM=", "TERM_PROGRAM=", "TERM_PROGRAM_VERSION=", "VTE_VERSION=",
      "WT_SESSION=", "KITTY_WINDOW_ID=", "KONSOLE_VERSION=", "ITERM_SESSION_ID=",
      "TERMINAL_EMULATOR=", "WEZTERM_PANE=", "FORCE_HYPERLINK=",
  };
  for (const std::string_view key : kKeys) {
    if (entry.starts_with(key)) {
      return true;
    }
  }
  return false;
}

bool OverridesEntry(const std::vector<std::string>& overrides, std::string_view entry) {
  const std::size_t equals = entry.find('=');
  if (equals == std::string_view::npos) {
    return false;
  }
  const std::string_view key = entry.substr(0, equals + 1);
  for (const std::string& override_entry : overrides) {
    if (std::string_view(override_entry).starts_with(key)) {
      return true;
    }
  }
  return false;
}


class PosixTerminalBackend final : public TerminalBackend {
 public:
  PosixTerminalBackend() = default;
  ~PosixTerminalBackend() override { Stop(); }

  TerminalStartResult Start(const TerminalStartRequest& request,
                            TerminalBackendCallbacks callbacks) override {
    Stop();

    winsize window_size{};
    window_size.ws_row = static_cast<unsigned short>(std::min<std::size_t>(request.rows, 65535));
    window_size.ws_col = static_cast<unsigned short>(std::min<std::size_t>(request.columns, 65535));
    int master_fd = -1;
    int slave_fd = -1;
    const auto pty_failure = [&]() {
      if (master_fd >= 0) {
        close(master_fd);
      }
      return TerminalStartResult{
          .started = false,
          .running = false,
          .child_process_id = -1,
          .launch_label = request.command.empty() ? "terminal unavailable" : request.command,
          .initial_output = "failed to allocate PTY for terminal session.",
      };
    };
    master_fd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master_fd < 0 || grantpt(master_fd) != 0 || unlockpt(master_fd) != 0) {
      return pty_failure();
    }
    char slave_name[PATH_MAX];
    if (ptsname_r(master_fd, slave_name, sizeof(slave_name)) != 0) {
      return pty_failure();
    }
    slave_fd = open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave_fd < 0) {
      return pty_failure();
    }
    (void)ioctl(master_fd, TIOCSWINSZ, &window_size);

    const std::string program =
        request.shell.empty() ? DefaultShellPath() : request.shell.front();
    const std::string shell_name = ShellProgramName(program);
    std::vector<std::string> argv_storage = BuildTerminalArgv(request.shell, request.command);
    std::vector<char*> argv_pointers;
    argv_pointers.reserve(argv_storage.size() + 1);
    for (std::string& word : argv_storage) {
      argv_pointers.push_back(word.data());
    }
    argv_pointers.push_back(nullptr);

    std::vector<std::string> env_storage;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
      const std::string_view text(*entry);
      if (IsTerminalIdentityEntry(text) || OverridesEntry(request.environment, text)) {
        continue;
      }
      env_storage.emplace_back(*entry);
    }
    env_storage.emplace_back("TERM=xterm-256color");
    env_storage.emplace_back("COLORTERM=truecolor");
    env_storage.emplace_back("TERM_PROGRAM=microide");
    env_storage.emplace_back(std::string("TERM_PROGRAM_VERSION=") + kTerminalProgramVersion);
    env_storage.emplace_back("FORCE_HYPERLINK=1");
    for (const std::string& entry : request.environment) {
      env_storage.push_back(entry);
    }
    std::vector<char*> env_pointers;
    env_pointers.reserve(env_storage.size() + 1);
    for (std::string& entry : env_storage) {
      env_pointers.push_back(entry.data());
    }
    env_pointers.push_back(nullptr);

    const pid_t child_pid = ForkWithSignalsBlocked();
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

      if (!request.working_directory.empty() &&
          chdir(request.working_directory.c_str()) != 0) {
        _exit(127);
      }
      environ = env_pointers.data();
      RestoreDefaultSignalsInChild();
      RestoreInheritedOpenFileLimitInChild();
      execvp(program.c_str(), argv_pointers.data());
      _exit(127);
    }

    close(slave_fd);

    util::SetNonBlocking(master_fd);

    int wake_pipe[2] = {-1, -1};
    if (!util::MakeCloexecPipe(wake_pipe, true)) {
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
    TraceTerminalLife("start", static_cast<int>(child_pid), true);

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
    if (child_pid > 0) {
      TraceTerminalLife("stop", child_pid, false);
    }

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

  bool HasUnreadOutput() const override {
    if (delivering_.load(std::memory_order_acquire)) {
      return true;
    }
    const int master_fd = master_fd_.load(std::memory_order_acquire);
    int unread = 0;
    return master_fd >= 0 && ioctl(master_fd, FIONREAD, &unread) == 0 && unread > 0;
  }

 private:
  void ReaderMain(int master_fd, int wake_fd, int child_pid) {
    util::NameCurrentThread("term-reader");
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
      if ((pfds[0].revents & POLLOUT) != 0) {
        DrainPendingWrite(master_fd);
      }
      if ((pfds[0].revents & (POLLIN | POLLHUP)) == 0) {
        continue;
      }
      const bool hangup = (pfds[0].revents & POLLHUP) != 0;
      delivering_.store(true, std::memory_order_release);
      const ssize_t count = read(master_fd, buffer.data(), buffer.size());
      if (count > 0 && callbacks_.on_output) {
        callbacks_.on_output(std::string_view(buffer.data(), static_cast<std::size_t>(count)));
      }
      delivering_.store(false, std::memory_order_release);
      if (count > 0) {
        continue;
      }
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (hangup) {
          break;
        }
        continue;
      }
      break;
    }

    if (!stop_requested_.load(std::memory_order_acquire)) {
      int status = 0;
      for (int attempt = 0; attempt < 100; ++attempt) {
        const pid_t reaped = waitpid(child_pid, &status, WNOHANG);
        if (reaped == child_pid || (reaped < 0 && errno != EINTR)) {
          break;
        }
        if (stop_requested_.load(std::memory_order_acquire)) {
          break;
        }
        struct pollfd none {};
        poll(&none, 0, 1);
      }
    }

    running_.store(false, std::memory_order_release);
    if (!stop_requested_.load(std::memory_order_acquire) && callbacks_.on_exit) {
      callbacks_.on_exit();
    }
  }

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
        break;
      }
      pending_write_.clear();
      pending_write_offset_ = 0;
      return;
    }
    if (pending_write_offset_ >= pending_write_.size()) {
      pending_write_.clear();
      pending_write_offset_ = 0;
    } else if (pending_write_offset_ >= (std::size_t{1} << 16)) {
      pending_write_.erase(0, pending_write_offset_);
      pending_write_offset_ = 0;
    }
  }

  static constexpr std::size_t kMaxPendingWriteBytes = 64u << 20;

  TerminalBackendCallbacks callbacks_;
  std::thread reader_thread_;
  std::atomic<int> master_fd_{-1};
  int wake_read_fd_ = -1;
  std::atomic<int> wake_write_fd_{-1};
  int child_pid_ = -1;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> delivering_{false};
  std::mutex write_mutex_;
  std::string pending_write_;
  std::size_t pending_write_offset_ = 0;
};

}

std::unique_ptr<TerminalBackend> CreateTerminalBackend() {
  return std::make_unique<PosixTerminalBackend>();
}

}
