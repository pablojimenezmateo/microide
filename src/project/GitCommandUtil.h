#pragma once

#include <array>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace microide::project::internal {

struct CommandResult {
  int exit_code = -1;
  std::string output;

  bool success() const { return exit_code == 0; }
};

inline bool HasGitMarker(const std::filesystem::path& root) {
  return !root.empty() && std::filesystem::exists(root / ".git");
}

inline std::optional<std::filesystem::path> AbsoluteToRelativePath(
    const std::filesystem::path& root,
    const std::filesystem::path& absolute_path) {
  if (root.empty() || absolute_path.empty()) {
    return std::nullopt;
  }

  std::error_code error;
  const std::filesystem::path relative = std::filesystem::relative(
      absolute_path.lexically_normal(), root.lexically_normal(), error);
  if (error || relative.empty() || relative.native().rfind("..", 0) == 0) {
    return std::nullopt;
  }
  return relative.lexically_normal();
}

inline CommandResult ReadCommandOutput(const std::vector<std::string>& command,
                                       bool silence_stderr = true) {
  CommandResult result;
  if (command.empty()) {
    return result;
  }

  int stdout_pipe[2] = {-1, -1};
  if (pipe(stdout_pipe) != 0) {
    return result;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    return result;
  }

  if (pid == 0) {
    dup2(stdout_pipe[1], STDOUT_FILENO);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);

    if (silence_stderr) {
      const int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
        close(devnull);
      }
    }

    std::vector<char*> argv;
    argv.reserve(command.size() + 1);
    for (const std::string& arg : command) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(errno == ENOENT ? 127 : 126);
  }

  close(stdout_pipe[1]);
  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t bytes_read = read(stdout_pipe[0], buffer.data(), buffer.size());
    if (bytes_read > 0) {
      result.output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
      continue;
    }
    if (bytes_read == 0) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    break;
  }
  close(stdout_pipe[0]);

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
  } else {
    result.exit_code = -1;
  }
  return result;
}

inline CommandResult ReadGitCommandOutput(const std::filesystem::path& root,
                                          std::vector<std::string> arguments,
                                          bool silence_stderr = true) {
  std::vector<std::string> command;
  command.reserve(arguments.size() + 3);
  command.emplace_back("git");
  command.emplace_back("-C");
  command.push_back(root.lexically_normal().string());
  for (std::string& argument : arguments) {
    command.push_back(std::move(argument));
  }
  return ReadCommandOutput(command, silence_stderr);
}

inline bool CommandSucceeds(const std::vector<std::string>& command,
                            bool silence_stderr = true) {
  return ReadCommandOutput(command, silence_stderr).success();
}

inline bool GitCommandSucceeds(const std::filesystem::path& root,
                               std::vector<std::string> arguments,
                               bool silence_stderr = true) {
  return ReadGitCommandOutput(root, std::move(arguments), silence_stderr).success();
}

}  // namespace microide::project::internal
