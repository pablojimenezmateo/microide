#include "platform/AsyncSubprocess.h"

#include <cerrno>
#include <cstring>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

namespace microide::platform {

#if defined(__unix__) || defined(__APPLE__)

struct AsyncSubprocess::Impl {
  pid_t pid = -1;
  int stdin_fd = -1;   // write end — parent writes here
  int stdout_fd = -1;  // read end  — parent reads here
  bool running = false;
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
};

#else

struct AsyncSubprocess::Impl {
  bool running = false;
  int pid_val = -1;
  std::optional<int> exit_code;
};

#endif

AsyncSubprocess::AsyncSubprocess() : impl_(new Impl{}) {}

AsyncSubprocess::~AsyncSubprocess() {
  if (impl_ != nullptr) {
    Shutdown(0);
    delete impl_;
  }
}

AsyncSubprocess::AsyncSubprocess(AsyncSubprocess&& other) noexcept : impl_(other.impl_) {
  other.impl_ = nullptr;
}

AsyncSubprocess& AsyncSubprocess::operator=(AsyncSubprocess&& other) noexcept {
  if (this != &other) {
    Shutdown(0);
    delete impl_;
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}

#if defined(__unix__) || defined(__APPLE__)

bool AsyncSubprocess::Start(const std::vector<std::string>& argv, const std::string& cwd) {
  if (argv.empty() || impl_->running) {
    return false;
  }

  int stdin_pipe[2] = {-1, -1};
  int stdout_pipe[2] = {-1, -1};

  if (pipe(stdin_pipe) != 0) {
    return false;
  }
  if (pipe(stdout_pipe) != 0) {
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    return false;
  }

  // Set stdout read-end non-blocking for poll-based reads.
  fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);

  const pid_t pid = fork();
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

    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);

    if (!cwd.empty()) {
      (void)chdir(cwd.c_str());
    }

    std::vector<char*> raw_argv;
    raw_argv.reserve(argv.size() + 1);
    for (const std::string& arg : argv) {
      raw_argv.push_back(const_cast<char*>(arg.c_str()));
    }
    raw_argv.push_back(nullptr);
    execvp(raw_argv[0], raw_argv.data());
    _exit(errno == ENOENT ? 127 : 126);
  }

  // Parent: close child-side ends.
  close(stdin_pipe[0]);
  close(stdout_pipe[1]);

  impl_->pid = pid;
  impl_->stdin_fd = stdin_pipe[1];
  impl_->stdout_fd = stdout_pipe[0];
  impl_->running = true;
  impl_->exit_code.reset();
  return true;
}

bool AsyncSubprocess::IsRunning() const {
  if (!impl_->running || impl_->pid < 0) {
    return false;
  }
  // Non-blocking reap — updates running flag as a side effect.
  int status = 0;
  const pid_t result = waitpid(impl_->pid, &status, WNOHANG);
  if (result == impl_->pid) {
    impl_->running = false;
    impl_->Close();
    impl_->pid = -1;
    if (WIFEXITED(status)) {
      impl_->exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      impl_->exit_code = 128 + WTERMSIG(status);
    } else {
      impl_->exit_code = std::nullopt;
    }
  }
  return impl_->running;
}

bool AsyncSubprocess::Write(std::string_view data) {
  if (!impl_->running || impl_->stdin_fd < 0) {
    return false;
  }
  const char* ptr = data.data();
  std::size_t remaining = data.size();
  while (remaining > 0) {
    const ssize_t written = write(impl_->stdin_fd, ptr, remaining);
    if (written > 0) {
      ptr += written;
      remaining -= static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    impl_->CloseStdin();
    return false;
  }
  return true;
}

std::optional<std::string> AsyncSubprocess::Read(std::size_t max_bytes, int timeout_ms) {
  if (impl_->stdout_fd < 0) {
    return std::nullopt;
  }

  pollfd pfd{.fd = impl_->stdout_fd, .events = POLLIN | POLLHUP, .revents = 0};
  const int ready = poll(&pfd, 1, timeout_ms);
  if (ready < 0 && errno != EINTR) {
    return std::nullopt;
  }
  if (ready == 0) {
    return std::string{};  // timeout — no data yet
  }

  if ((pfd.revents & POLLIN) == 0) {
    // HUP with no data
    IsRunning();  // reap
    return std::nullopt;
  }

  std::string result;
  result.resize(max_bytes);
  const ssize_t n = read(impl_->stdout_fd, result.data(), max_bytes);
  if (n > 0) {
    result.resize(static_cast<std::size_t>(n));
    return result;
  }
  if (n == 0 || (n < 0 && errno != EINTR)) {
    impl_->CloseStdout();
    IsRunning();
    return std::nullopt;
  }
  return std::string{};
}

std::optional<std::string> AsyncSubprocess::ReadExact(std::size_t n, int timeout_ms) {
  if (impl_->stdout_fd < 0) {
    return std::nullopt;
  }
  std::string result;
  result.reserve(n);

  while (result.size() < n) {
    const std::size_t remaining = n - result.size();
    pollfd pfd{.fd = impl_->stdout_fd, .events = POLLIN | POLLHUP, .revents = 0};
    const int ready = poll(&pfd, 1, timeout_ms);
    if (ready <= 0) {
      if (ready < 0 && errno == EINTR) {
        continue;
      }
      return std::nullopt;
    }
    if ((pfd.revents & POLLIN) == 0) {
      return std::nullopt;
    }
    const std::size_t old_size = result.size();
    result.resize(old_size + remaining);
    const ssize_t bytes_read =
        read(impl_->stdout_fd, result.data() + old_size, remaining);
    if (bytes_read > 0) {
      result.resize(old_size + static_cast<std::size_t>(bytes_read));
      continue;
    }
    if (bytes_read == 0 || (bytes_read < 0 && errno != EINTR)) {
      impl_->CloseStdout();
      return std::nullopt;
    }
    result.resize(old_size);
  }
  return result;
}

void AsyncSubprocess::Shutdown(int timeout_ms) {
  if (!impl_->running || impl_->pid < 0) {
    impl_->Close();
    return;
  }
  impl_->Close();  // close pipes first so child sees EOF
  kill(impl_->pid, SIGTERM);

  // Poll for exit up to timeout_ms.
  const int kSliceMs = 20;
  int elapsed = 0;
  while (elapsed < timeout_ms) {
    int status = 0;
    if (waitpid(impl_->pid, &status, WNOHANG) == impl_->pid) {
      impl_->pid = -1;
      impl_->running = false;
      if (WIFEXITED(status)) {
        impl_->exit_code = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        impl_->exit_code = 128 + WTERMSIG(status);
      } else {
        impl_->exit_code = std::nullopt;
      }
      return;
    }
    struct timespec ts{.tv_sec = 0, .tv_nsec = kSliceMs * 1'000'000L};
    nanosleep(&ts, nullptr);
    elapsed += kSliceMs;
  }

  // Still alive — SIGKILL.
  if (impl_->pid >= 0) {
    kill(impl_->pid, SIGKILL);
    int status = 0;
    while (waitpid(impl_->pid, &status, 0) < 0) {
      if (errno != EINTR) {
        break;
      }
    }
  }
  impl_->pid = -1;
  impl_->running = false;
  impl_->exit_code = 128 + SIGKILL;
}

int AsyncSubprocess::pid() const {
  return impl_ != nullptr ? static_cast<int>(impl_->pid) : -1;
}

std::optional<int> AsyncSubprocess::exit_code() const {
  return impl_ != nullptr ? impl_->exit_code : std::nullopt;
}

#else  // non-POSIX stubs

bool AsyncSubprocess::Start(const std::vector<std::string>&, const std::string&) {
  return false;
}
bool AsyncSubprocess::IsRunning() const { return false; }
bool AsyncSubprocess::Write(std::string_view) { return false; }
std::optional<std::string> AsyncSubprocess::Read(std::size_t, int) { return std::nullopt; }
std::optional<std::string> AsyncSubprocess::ReadExact(std::size_t, int) { return std::nullopt; }
void AsyncSubprocess::Shutdown(int) {}
int AsyncSubprocess::pid() const { return -1; }
std::optional<int> AsyncSubprocess::exit_code() const {
  return impl_ != nullptr ? impl_->exit_code : std::nullopt;
}

#endif

}  // namespace microide::platform
