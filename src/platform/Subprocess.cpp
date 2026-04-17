#include "platform/Subprocess.h"

#include <cerrno>

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

void CloseIfValid(int fd) {
  if (fd >= 0) {
    close(fd);
  }
}

bool OpenPipe(bool enabled, int (&pipe_fds)[2]) {
  pipe_fds[0] = -1;
  pipe_fds[1] = -1;
  if (!enabled) {
    return true;
  }
  return pipe(pipe_fds) == 0;
}

void DrainReadyPipe(int* fd, std::string* output) {
  if (fd == nullptr || *fd < 0 || output == nullptr) {
    return;
  }

  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t bytes_read = read(*fd, buffer.data(), buffer.size());
    if (bytes_read > 0) {
      output->append(buffer.data(), static_cast<std::size_t>(bytes_read));
      if (bytes_read == static_cast<ssize_t>(buffer.size())) {
        continue;
      }
      return;
    }
    if (bytes_read == 0) {
      CloseIfValid(*fd);
      *fd = -1;
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    CloseIfValid(*fd);
    *fd = -1;
    return;
  }
}

void DrainCapturedPipes(int* stdout_fd,
                        std::string* stdout_text,
                        int* stderr_fd,
                        std::string* stderr_text) {
  while ((stdout_fd != nullptr && *stdout_fd >= 0) || (stderr_fd != nullptr && *stderr_fd >= 0)) {
    std::array<pollfd, 2> poll_fds{};
    nfds_t count = 0;
    if (stdout_fd != nullptr && *stdout_fd >= 0) {
      poll_fds[count++] = pollfd{.fd = *stdout_fd, .events = POLLIN | POLLHUP, .revents = 0};
    }
    if (stderr_fd != nullptr && *stderr_fd >= 0) {
      poll_fds[count++] = pollfd{.fd = *stderr_fd, .events = POLLIN | POLLHUP, .revents = 0};
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
      if (stdout_fd != nullptr && poll_fds[index].fd == *stdout_fd) {
        DrainReadyPipe(stdout_fd, stdout_text);
        continue;
      }
      if (stderr_fd != nullptr && poll_fds[index].fd == *stderr_fd) {
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
  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  int stdin_pipe[2] = {-1, -1};

  const bool needs_stdin = !options.stdin_text.empty();
  if (!OpenPipe(options.capture_stdout, stdout_pipe) || !OpenPipe(options.capture_stderr, stderr_pipe) ||
      !OpenPipe(needs_stdin, stdin_pipe)) {
    CloseIfValid(stdout_pipe[0]);
    CloseIfValid(stdout_pipe[1]);
    CloseIfValid(stderr_pipe[0]);
    CloseIfValid(stderr_pipe[1]);
    CloseIfValid(stdin_pipe[0]);
    CloseIfValid(stdin_pipe[1]);
    return result;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    CloseIfValid(stdout_pipe[0]);
    CloseIfValid(stdout_pipe[1]);
    CloseIfValid(stderr_pipe[0]);
    CloseIfValid(stderr_pipe[1]);
    CloseIfValid(stdin_pipe[0]);
    CloseIfValid(stdin_pipe[1]);
    return result;
  }

  if (pid == 0) {
    if (options.capture_stdout) {
      dup2(stdout_pipe[1], STDOUT_FILENO);
    }
    if (options.capture_stderr) {
      dup2(stderr_pipe[1], STDERR_FILENO);
    } else if (options.silence_stderr) {
      const int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
        close(devnull);
      }
    }
    if (needs_stdin) {
      dup2(stdin_pipe[0], STDIN_FILENO);
    }

    CloseIfValid(stdout_pipe[0]);
    CloseIfValid(stdout_pipe[1]);
    CloseIfValid(stderr_pipe[0]);
    CloseIfValid(stderr_pipe[1]);
    CloseIfValid(stdin_pipe[0]);
    CloseIfValid(stdin_pipe[1]);

    if (!options.cwd.empty()) {
      (void)chdir(options.cwd.string().c_str());
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

  CloseIfValid(stdout_pipe[1]);
  CloseIfValid(stderr_pipe[1]);
  CloseIfValid(stdin_pipe[0]);
  if (needs_stdin) {
    WriteAllToPipe(stdin_pipe[1], options.stdin_text);
    CloseIfValid(stdin_pipe[1]);
  }

  DrainCapturedPipes(options.capture_stdout ? &stdout_pipe[0] : nullptr, &result.stdout_text,
                     options.capture_stderr ? &stderr_pipe[0] : nullptr, &result.stderr_text);
  CloseIfValid(stdout_pipe[0]);
  CloseIfValid(stderr_pipe[0]);

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
