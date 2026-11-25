#include "platform/AsyncSubprocess.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <thread>
#include <cstring>
#include <mutex>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "platform/ChildSignals.h"
#include "platform/OpenFileLimit.h"
#include "util/PosixFd.h"
#include "util/PosixPipe.h"

namespace microide::platform {

using microide::util::MakeCloexecPipe;

struct AsyncSubprocess::Impl {
  std::mutex state_mutex;
  std::atomic<pid_t> pid{-1};
  int stdin_fd = -1;
  int stdout_fd = -1;
  std::shared_ptr<AsyncProcessController> controller;
  std::atomic<bool> running{false};
  std::optional<int> exit_code;

  void CloseStdin() {
    if (stdin_fd >= 0) {
      close(stdin_fd);
      stdin_fd = -1;
    }
  }

  void CloseStdout() {
    if (stdout_fd >= 0) {
      close(stdout_fd);
      stdout_fd = -1;
    }
  }

  void Close() {
    CloseStdin();
    CloseStdout();
  }

  bool ReapIfExited(pid_t expected_pid) {
    if (expected_pid < 0 || pid.load(std::memory_order_acquire) != expected_pid) {
      return false;
    }
    int status = 0;
    if (waitpid(expected_pid, &status, WNOHANG) != expected_pid) {
      return false;
    }
    running.store(false, std::memory_order_release);
    Close();
    pid.store(-1, std::memory_order_release);
    if (WIFEXITED(status)) {
      exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      exit_code = 128 + WTERMSIG(status);
    } else {
      exit_code = std::nullopt;
    }
    return true;
  }
};

AsyncSubprocess::AsyncSubprocess() : impl_(std::make_unique<Impl>()) {}

AsyncSubprocess::~AsyncSubprocess() {
  if (impl_ != nullptr) {
    Shutdown(0);
  }
}

AsyncSubprocess::AsyncSubprocess(AsyncSubprocess&& other) noexcept = default;

AsyncSubprocess& AsyncSubprocess::operator=(AsyncSubprocess&& other) noexcept {
  if (this != &other) {
    Shutdown(0);
    impl_ = std::move(other.impl_);
  }
  return *this;
}

bool AsyncSubprocess::Start(const std::vector<std::string>& argv, const std::string& cwd,
                            const SubprocessSandbox& sandbox, int stderr_fd) {
  if (impl_ == nullptr) {
    return false;
  }
  std::lock_guard lock(impl_->state_mutex);
  if (argv.empty() || impl_->running.load(std::memory_order_acquire)) {
    return false;
  }

  int stdin_pipe[2] = {-1, -1};
  int stdout_pipe[2] = {-1, -1};

  if (!MakeCloexecPipe(stdin_pipe)) {
    return false;
  }
  if (!MakeCloexecPipe(stdout_pipe)) {
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    return false;
  }

  util::SetNonBlocking(stdin_pipe[1]);
  util::SetNonBlocking(stdout_pipe[0]);

  std::vector<char*> raw_argv;
  raw_argv.reserve(argv.size() + 1);
  for (const std::string& arg : argv) {
    raw_argv.push_back(const_cast<char*>(arg.c_str()));
  }
  raw_argv.push_back(nullptr);

  const pid_t pid = ForkWithSignalsBlocked();
  if (pid < 0) {
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    return false;
  }

  if (pid == 0) {
    dup2(stdin_pipe[0], STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    if (stderr_fd >= 0) {
      dup2(stderr_fd, STDERR_FILENO);
    }

    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);

    if (!cwd.empty()) {
      if (chdir(cwd.c_str()) != 0) {
        _exit(127);
      }
    }
    RestoreDefaultSignalsInChild();
    RestoreInheritedOpenFileLimitInChild();
    ApplyChildSandbox(sandbox);
    execvp(raw_argv[0], raw_argv.data());
    _exit(errno == ENOENT ? 127 : 126);
  }

  close(stdin_pipe[0]);
  close(stdout_pipe[1]);

  impl_->pid.store(pid, std::memory_order_release);
  impl_->stdin_fd = stdin_pipe[1];
  impl_->stdout_fd = stdout_pipe[0];
  impl_->running.store(true, std::memory_order_release);
  impl_->exit_code.reset();
  return true;
}

bool AsyncSubprocess::Adopt(int stdin_fd, int stdout_fd,
                            std::shared_ptr<AsyncProcessController> controller) {
  if (impl_ == nullptr || controller == nullptr || stdin_fd < 0 || stdout_fd < 0) {
    return false;
  }
  std::lock_guard lock(impl_->state_mutex);
  if (impl_->running.load(std::memory_order_acquire)) {
    return false;
  }
  util::SetNonBlocking(stdin_fd);
  util::SetNonBlocking(stdout_fd);
  impl_->stdin_fd = stdin_fd;
  impl_->stdout_fd = stdout_fd;
  impl_->controller = std::move(controller);
  impl_->exit_code.reset();
  impl_->running.store(true, std::memory_order_release);
  return true;
}

bool AsyncSubprocess::IsRunning() const {
  if (impl_ == nullptr) {
    return false;
  }
  std::lock_guard lock(impl_->state_mutex);
  if (impl_->controller != nullptr) {
    if (!impl_->controller->IsRunning()) {
      impl_->running.store(false, std::memory_order_release);
      impl_->exit_code = impl_->controller->exit_code();
    }
    return impl_->running.load(std::memory_order_acquire);
  }
  const pid_t current_pid = impl_->pid.load(std::memory_order_acquire);
  if (!impl_->running.load(std::memory_order_acquire) || current_pid < 0) {
    return false;
  }
  impl_->ReapIfExited(current_pid);
  return impl_->running.load(std::memory_order_acquire);
}

bool AsyncSubprocess::Write(std::string_view data) {
  if (impl_ == nullptr) {
    return false;
  }
  const char* ptr = data.data();
  std::size_t remaining = data.size();
  constexpr auto kWriteStallTimeout = std::chrono::seconds(30);
  auto stall_deadline = std::chrono::steady_clock::now() + kWriteStallTimeout;
  while (remaining > 0) {
    int stdin_fd = -1;
    {
      std::lock_guard lock(impl_->state_mutex);
      if (!impl_->running.load(std::memory_order_acquire) || impl_->stdin_fd < 0) {
        return false;
      }
      stdin_fd = impl_->stdin_fd;
    }

    pollfd pfd{.fd = stdin_fd, .events = POLLOUT | POLLHUP, .revents = 0};
    const int ready = poll(&pfd, 1, 100);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      CloseStdin();
      return false;
    }
    if (ready == 0) {
      if (std::chrono::steady_clock::now() >= stall_deadline) {
        CloseStdin();
        return false;
      }
      continue;
    }
    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      CloseStdin();
      return false;
    }

    ssize_t written = -1;
    {
      std::lock_guard lock(impl_->state_mutex);
      if (!impl_->running.load(std::memory_order_acquire) || impl_->stdin_fd < 0) {
        return false;
      }
      written = write(impl_->stdin_fd, ptr, remaining);
    }
    if (written > 0) {
      ptr += written;
      remaining -= static_cast<std::size_t>(written);
      stall_deadline = std::chrono::steady_clock::now() + kWriteStallTimeout;
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
    }
    CloseStdin();
    return false;
  }
  return true;
}

std::optional<std::string> AsyncSubprocess::Read(std::size_t max_bytes, int timeout_ms) {
  if (impl_ == nullptr) {
    return std::nullopt;
  }
  if (max_bytes == 0) {
    return std::string{};
  }
  int stdout_fd = -1;
  pid_t current_pid = -1;
  {
    std::lock_guard lock(impl_->state_mutex);
    if (impl_->stdout_fd < 0) {
      return std::nullopt;
    }
    stdout_fd = impl_->stdout_fd;
    current_pid = impl_->pid.load(std::memory_order_acquire);
  }

  while (true) {
    pollfd pfd{.fd = stdout_fd, .events = POLLIN | POLLHUP, .revents = 0};
    const int ready = poll(&pfd, 1, timeout_ms);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::nullopt;
    }
    if (ready == 0) {
      return std::string{};
    }

    if ((pfd.revents & POLLIN) == 0) {
      std::lock_guard lock(impl_->state_mutex);
      if (impl_->stdout_fd == stdout_fd) {
        impl_->CloseStdout();
      }
      impl_->ReapIfExited(current_pid);
      return std::nullopt;
    }

    std::string result;
    result.resize(max_bytes);
    ssize_t n = -1;
    {
      std::lock_guard lock(impl_->state_mutex);
      if (impl_->stdout_fd != stdout_fd) {
        return std::nullopt;
      }
      n = read(stdout_fd, result.data(), max_bytes);
    }
    if (n > 0) {
      result.resize(static_cast<std::size_t>(n));
      return result;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
      std::lock_guard lock(impl_->state_mutex);
      if (impl_->stdout_fd == stdout_fd) {
        impl_->CloseStdout();
      }
      impl_->ReapIfExited(current_pid);
      return std::nullopt;
    }
    return std::string{};
  }
}

void AsyncSubprocess::CloseStdin() {
  if (impl_ == nullptr) {
    return;
  }
  std::lock_guard lock(impl_->state_mutex);
  impl_->CloseStdin();
}

void AsyncSubprocess::Finish(int grace_ms, int kill_timeout_ms) {
  CloseStdin();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(grace_ms);
  while (IsRunning() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (IsRunning()) {
    Shutdown(kill_timeout_ms);
  }
}

void AsyncSubprocess::Shutdown(int timeout_ms) {
  if (impl_ == nullptr) {
    return;
  }
  std::lock_guard lock(impl_->state_mutex);
  if (impl_->controller != nullptr) {
    impl_->Close();
    impl_->controller->Terminate(timeout_ms);
    impl_->exit_code = impl_->controller->exit_code();
    impl_->running.store(false, std::memory_order_release);
    return;
  }
  pid_t pid = impl_->pid.load(std::memory_order_acquire);
  if (!impl_->running.load(std::memory_order_acquire) || pid < 0) {
    impl_->Close();
    return;
  }
  impl_->Close();
  kill(pid, SIGTERM);

  const int kSliceMs = 20;
  int elapsed = 0;
  while (elapsed < timeout_ms) {
    if (impl_->ReapIfExited(pid)) {
      return;
    }
    struct timespec ts{.tv_sec = 0, .tv_nsec = kSliceMs * 1'000'000L};
    nanosleep(&ts, nullptr);
    elapsed += kSliceMs;
  }

  pid = impl_->pid.load(std::memory_order_acquire);
  if (pid >= 0) {
    kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
      if (errno != EINTR) {
        break;
      }
    }
  }
  impl_->pid.store(-1, std::memory_order_release);
  impl_->running.store(false, std::memory_order_release);
  impl_->exit_code = 128 + SIGKILL;
}

int AsyncSubprocess::pid() const {
  if (impl_ == nullptr) {
    return -1;
  }
  std::lock_guard lock(impl_->state_mutex);
  if (impl_->controller != nullptr) {
    return impl_->controller->pid();
  }
  return static_cast<int>(impl_->pid.load(std::memory_order_acquire));
}

std::optional<int> AsyncSubprocess::exit_code() const {
  if (impl_ == nullptr) {
    return std::nullopt;
  }
  std::lock_guard lock(impl_->state_mutex);
  if (impl_->controller != nullptr && !impl_->exit_code.has_value()) {
    return impl_->controller->exit_code();
  }
  return impl_->exit_code;
}

int AsyncSubprocess::stdout_fd() const {
  if (impl_ == nullptr) {
    return -1;
  }
  std::lock_guard lock(impl_->state_mutex);
  return impl_->stdout_fd;
}

int AsyncSubprocess::stdin_fd() const {
  if (impl_ == nullptr) {
    return -1;
  }
  std::lock_guard lock(impl_->state_mutex);
  return impl_->stdin_fd;
}

}
