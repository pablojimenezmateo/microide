#include "project/GitCommandUtil.h"

#include <cctype>
#include <system_error>

#include "platform/Subprocess.h"
#include "util/StringUtil.h"

namespace microide::project::internal {

bool HasGitMarker(const std::filesystem::path& root) {
  if (root.empty()) {
    return false;
  }
  // Use the non-throwing overload: this runs on the background-executor thread on
  // essentially every git operation, and the throwing exists() aborts the process
  // when the failure is not "does not exist" (a parent dir losing +x, an unmounted
  // network volume, ENAMETOOLONG). Those must degrade to "repo unavailable", not
  // crash. Matches every other filesystem probe in this subsystem.
  std::error_code error;
  return std::filesystem::exists(root / ".git", error) && !error;
}

std::optional<std::filesystem::path> AbsoluteToRelativePath(
    const std::filesystem::path& root,
    const std::filesystem::path& absolute_path) {
  if (root.empty() || absolute_path.empty()) {
    return std::nullopt;
  }

  const std::filesystem::path relative =
      absolute_path.lexically_normal().lexically_relative(root.lexically_normal());
  if (relative.empty() ||
      (relative.begin() != relative.end() &&
       *relative.begin() == std::filesystem::path(".."))) {
#if defined(_WIN32)
    const std::string root_text = root.lexically_normal().generic_string();
    const std::string path_text = absolute_path.lexically_normal().generic_string();
    std::string lowered_root = util::ToLowerAscii(root_text);
    std::string lowered_path = util::ToLowerAscii(path_text);
    const std::string lowered_root_prefix =
        lowered_root.ends_with('/') ? lowered_root : lowered_root + "/";
    const std::string root_prefix = root_text.ends_with('/') ? root_text : root_text + "/";
    if (lowered_path == lowered_root) {
      return std::filesystem::path(".");
    }
    if (lowered_path.size() > lowered_root_prefix.size() &&
        lowered_path.rfind(lowered_root_prefix, 0) == 0) {
      return std::filesystem::path(path_text.substr(root_prefix.size())).lexically_normal();
    }
#endif
    return std::nullopt;
  }
  return relative.lexically_normal();
}

std::optional<std::string> ResolveHeadId(const std::filesystem::path& root) {
  const auto result = ReadGitCommandOutput(root, {"rev-parse", "--verify", "HEAD"});
  if (!result.success() || result.output.empty()) {
    return std::nullopt;
  }

  std::string head_id = result.output;
  util::TrimTrailingLineEndings(&head_id);
  return head_id.empty() ? std::nullopt : std::make_optional(std::move(head_id));
}

CommandResult ReadGitCommandOutput(const std::filesystem::path& root,
                                   std::vector<std::string> arguments,
                                   bool silence_stderr,
                                   int timeout_ms) {
  return ReadGitCommandOutputWithStdin(root, std::move(arguments), {}, silence_stderr, timeout_ms);
}

CommandResult ReadGitCommandOutputWithStdin(const std::filesystem::path& root,
                                            std::vector<std::string> arguments,
                                            std::string stdin_text,
                                            bool silence_stderr,
                                            int timeout_ms) {
  std::vector<std::string> command;
  command.reserve(arguments.size() + 4);
  command.emplace_back("git");
  // Suppress optional index refresh so read-only commands (status, blame, etc.)
  // never touch .git/index.lock; concurrent invocations and killed subprocesses
  // previously left stale locks that blocked the user's own `git commit`.
  command.emplace_back("--no-optional-locks");
  command.emplace_back("-C");
  command.push_back(root.lexically_normal().string());
  for (std::string& argument : arguments) {
    command.push_back(std::move(argument));
  }
  platform::SubprocessOptions options;
  options.capture_stdout = true;
  options.capture_stderr = !silence_stderr;
  options.silence_stderr = silence_stderr;
  options.stdin_text = std::move(stdin_text);
  options.timeout_ms = timeout_ms;
  const platform::SubprocessResult result = platform::RunSubprocess(command, options);
  std::string output = result.stdout_text;
  if (!silence_stderr && !result.stderr_text.empty()) {
    if (!output.empty() && output.back() != '\n') {
      output.push_back('\n');
    }
    output += result.stderr_text;
  }
  return CommandResult{
      .exit_code = result.exit_code,
      .output = std::move(output),
      .timed_out = result.timed_out,
      .truncated = result.truncated,
  };
}

bool GitCommandSucceeds(const std::filesystem::path& root,
                        std::vector<std::string> arguments,
                        bool silence_stderr) {
  return ReadGitCommandOutput(root, std::move(arguments), silence_stderr).success();
}

}  // namespace microide::project::internal
