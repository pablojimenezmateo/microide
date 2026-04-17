#include "project/GitCommandUtil.h"

#include "platform/Subprocess.h"

namespace microide::project::internal {

bool HasGitMarker(const std::filesystem::path& root) {
  return !root.empty() && std::filesystem::exists(root / ".git");
}

std::optional<std::filesystem::path> AbsoluteToRelativePath(
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

CommandResult ReadCommandOutput(const std::vector<std::string>& command, bool silence_stderr) {
  platform::SubprocessOptions options;
  options.capture_stdout = true;
  options.capture_stderr = !silence_stderr;
  options.silence_stderr = silence_stderr;
  const platform::SubprocessResult result = platform::RunSubprocess(command, options);
  return CommandResult{
      .exit_code = result.exit_code,
      .output = result.stdout_text,
  };
}

CommandResult ReadGitCommandOutput(const std::filesystem::path& root,
                                   std::vector<std::string> arguments,
                                   bool silence_stderr) {
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

bool CommandSucceeds(const std::vector<std::string>& command, bool silence_stderr) {
  return ReadCommandOutput(command, silence_stderr).success();
}

bool GitCommandSucceeds(const std::filesystem::path& root,
                        std::vector<std::string> arguments,
                        bool silence_stderr) {
  return ReadGitCommandOutput(root, std::move(arguments), silence_stderr).success();
}

}  // namespace microide::project::internal
