#include "platform/Subprocess.h"

#include <cerrno>
#include <cstdlib>

#if defined(__unix__) || defined(__APPLE__)
#include <array>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace microide::platform {

namespace {

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

  int raw_pipe[2] = {-1, -1};
  if (pipe(raw_pipe) != 0) {
    return false;
  }
  (*pipe_fds)[0].Reset(raw_pipe[0]);
  (*pipe_fds)[1].Reset(raw_pipe[1]);
  return true;
}

void DrainReadyPipe(UniqueFd* fd, std::string* output) {
  if (fd == nullptr || !fd->IsValid() || output == nullptr) {
    return;
  }

  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t bytes_read = read(fd->Get(), buffer.data(), buffer.size());
    if (bytes_read > 0) {
      output->append(buffer.data(), static_cast<std::size_t>(bytes_read));
      if (bytes_read == static_cast<ssize_t>(buffer.size())) {
        continue;
      }
      return;
    }
    if (bytes_read == 0) {
      fd->Reset();
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    fd->Reset();
    return;
  }
}

void DrainCapturedPipes(UniqueFd* stdout_fd,
                        std::string* stdout_text,
                        UniqueFd* stderr_fd,
                        std::string* stderr_text) {
  while ((stdout_fd != nullptr && stdout_fd->IsValid()) ||
         (stderr_fd != nullptr && stderr_fd->IsValid())) {
    std::array<pollfd, 2> poll_fds{};
    nfds_t count = 0;
    if (stdout_fd != nullptr && stdout_fd->IsValid()) {
      poll_fds[count++] =
          pollfd{.fd = stdout_fd->Get(), .events = POLLIN | POLLHUP, .revents = 0};
    }
    if (stderr_fd != nullptr && stderr_fd->IsValid()) {
      poll_fds[count++] =
          pollfd{.fd = stderr_fd->Get(), .events = POLLIN | POLLHUP, .revents = 0};
    }

    if (count == 0) {
      break;
    }

    const int poll_result = poll(poll_fds.data(), count, -1);
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    for (nfds_t index = 0; index < count; ++index) {
      if ((poll_fds[index].revents & (POLLIN | POLLHUP)) == 0) {
        continue;
      }
      if (stdout_fd != nullptr && stdout_fd->IsValid() &&
          poll_fds[index].fd == stdout_fd->Get()) {
        DrainReadyPipe(stdout_fd, stdout_text);
        continue;
      }
      if (stderr_fd != nullptr && stderr_fd->IsValid() &&
          poll_fds[index].fd == stderr_fd->Get()) {
        DrainReadyPipe(stderr_fd, stderr_text);
      }
    }
  }
}

void WriteAllToPipe(int fd, const std::string& text) {
  const char* data = text.data();
  std::size_t remaining = text.size();
  while (remaining > 0) {
    const ssize_t written = write(fd, data, remaining);
    if (written > 0) {
      data += written;
      remaining -= static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
}

void ApplyEnvironmentOverrides(const std::vector<SubprocessEnvironmentOverride>& overrides) {
  for (const auto& override_entry : overrides) {
    if (override_entry.name.empty()) {
      continue;
    }

    if (override_entry.value.has_value()) {
      (void)setenv(override_entry.name.c_str(), override_entry.value->c_str(), 1);
      continue;
    }

    (void)unsetenv(override_entry.name.c_str());
  }
}

#endif

}  // namespace

SubprocessResult RunSubprocess(const std::vector<std::string>& argv, const SubprocessOptions& options) {
  SubprocessResult result;
  if (argv.empty()) {
    return result;
  }

#if !(defined(__unix__) || defined(__APPLE__))
  result.stderr_text = "Subprocess execution is not implemented on this platform";
  return result;
#else
  std::array<UniqueFd, 2> stdout_pipe;
  std::array<UniqueFd, 2> stderr_pipe;
  std::array<UniqueFd, 2> stdin_pipe;

  const bool needs_stdin = !options.stdin_text.empty();
  if (!OpenPipe(options.capture_stdout, &stdout_pipe) ||
      !OpenPipe(options.capture_stderr, &stderr_pipe) || !OpenPipe(needs_stdin, &stdin_pipe)) {
    return result;
  }

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
      UniqueFd devnull(open("/dev/null", O_WRONLY));
      if (devnull.IsValid()) {
        dup2(devnull.Get(), STDERR_FILENO);
      }
    }
    if (needs_stdin) {
      dup2(stdin_pipe[0].Get(), STDIN_FILENO);
    }

    stdout_pipe[0].Reset();
    stdout_pipe[1].Reset();
    stderr_pipe[0].Reset();
    stderr_pipe[1].Reset();
    stdin_pipe[0].Reset();
    stdin_pipe[1].Reset();

    if (!options.cwd.empty()) {
      (void)chdir(options.cwd.string().c_str());
    }
    ApplyEnvironmentOverrides(options.environment_overrides);

    std::vector<char*> raw_argv;
    raw_argv.reserve(argv.size() + 1);
    for (const std::string& arg : argv) {
      raw_argv.push_back(const_cast<char*>(arg.c_str()));
    }
    raw_argv.push_back(nullptr);
    execvp(raw_argv[0], raw_argv.data());
    _exit(errno == ENOENT ? 127 : 126);
  }

  stdout_pipe[1].Reset();
  stderr_pipe[1].Reset();
  stdin_pipe[0].Reset();
  if (needs_stdin) {
    WriteAllToPipe(stdin_pipe[1].Get(), options.stdin_text);
    stdin_pipe[1].Reset();
  }

  DrainCapturedPipes(options.capture_stdout ? &stdout_pipe[0] : nullptr, &result.stdout_text,
                     options.capture_stderr ? &stderr_pipe[0] : nullptr, &result.stderr_text);
  stdout_pipe[0].Reset();
  stderr_pipe[0].Reset();

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
#endif
}

}  // namespace microide::platform
