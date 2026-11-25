#include "project/GitCommandUtil.h"

#include "project/GitMetadataSource.h"

#include <fstream>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string_view>
#include <system_error>

#include <climits>
#include <sys/stat.h>
#if !defined(PATH_MAX)
#define PATH_MAX 4096
#endif

#include "platform/Subprocess.h"
#include "util/PathMatch.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"

namespace microide::project::internal {

bool HasGitMarker(const std::filesystem::path& root) {
  if (root.empty()) {
    return false;
  }
  {
    const std::string& native = root.native();
    constexpr std::string_view kMarker = "/.git";
    char buffer[PATH_MAX];
    if (native.size() + kMarker.size() + 1 <= sizeof(buffer)) {
      std::size_t end = native.size();
      std::memcpy(buffer, native.data(), end);
      while (end > 0 && buffer[end - 1] == '/') {
        --end;
      }
      std::memcpy(buffer + end, kMarker.data(), kMarker.size());
      end += kMarker.size();
      buffer[end] = '\0';
      struct ::stat info;
      return ::stat(buffer, &info) == 0;
    }
  }
  std::error_code error;
  return std::filesystem::exists(root / ".git", error) && !error;
}

namespace {

std::optional<std::filesystem::path> ComputeAbsoluteToRelativePath(
    const std::filesystem::path& root,
    const std::filesystem::path& absolute_path) {
  const std::filesystem::path normalized_root = util::NormalizedPath(root);
  const std::filesystem::path normalized_path = util::NormalizedPath(absolute_path);

  if (util::NormalizedPathEqualsOrWithin(normalized_path, normalized_root)) {
    const std::string_view relative_text =
        util::NormalizedRelativeView(normalized_path.native(), normalized_root.native());
    if (!relative_text.empty()) {
      return std::filesystem::path(relative_text);
    }
    return std::filesystem::path(".");
  }

  const std::filesystem::path relative = normalized_path.lexically_relative(normalized_root);
  if (relative.empty() ||
      (relative.begin() != relative.end() &&
       *relative.begin() == std::filesystem::path(".."))) {
    return std::nullopt;
  }
  return util::NormalizedPath(relative);
}

}

std::optional<std::filesystem::path> AbsoluteToRelativePath(
    const std::filesystem::path& root,
    const std::filesystem::path& absolute_path) {
  return AbsoluteToRelativePathRef(root, absolute_path);
}

const std::optional<std::filesystem::path>& AbsoluteToRelativePathRef(
    const std::filesystem::path& root,
    const std::filesystem::path& absolute_path) {
  static const std::optional<std::filesystem::path> kNone;
  if (root.empty() || absolute_path.empty()) {
    return kNone;
  }

  struct Entry {
    std::filesystem::path root;
    std::filesystem::path absolute_path;
    std::optional<std::filesystem::path> result;
  };
  constexpr std::size_t kMemoEntries = 4;
  thread_local std::array<Entry, kMemoEntries> memo{};
  thread_local std::size_t memo_used = 0;

  for (std::size_t i = 0; i < memo_used; ++i) {
    if (memo[i].root != root || memo[i].absolute_path != absolute_path) {
      continue;
    }
    if (i != 0) {
      std::rotate(memo.begin(), memo.begin() + static_cast<std::ptrdiff_t>(i),
                  memo.begin() + static_cast<std::ptrdiff_t>(i) + 1);
    }
    return memo[0].result;
  }

  Entry entry{.root = root,
              .absolute_path = absolute_path,
              .result = ComputeAbsoluteToRelativePath(root, absolute_path)};
  std::rotate(memo.begin(), memo.end() - 1, memo.end());
  memo[0] = std::move(entry);
  memo_used = std::min(memo_used + 1, kMemoEntries);
  return memo[0].result;
}

std::optional<std::string> ResolveHeadId(const platform::ProcessLauncher& launcher,
                                         const std::filesystem::path& root) {
  const auto result = ReadGitCommandOutput(launcher, root, {"rev-parse", "--verify", "HEAD"});
  if (!result.success() || result.output.empty()) {
    return std::nullopt;
  }

  std::string head_id = result.output;
  util::TrimTrailingLineEndings(&head_id);
  return head_id.empty() ? std::nullopt : std::make_optional(std::move(head_id));
}

std::filesystem::path ResolveCommonDir(const std::filesystem::path& git_dir) {
  const std::optional<std::string> line = ReadFirstLineOfGitFile(git_dir / "commondir");
  if (!line.has_value()) {
    return git_dir;
  }
  const std::string trimmed = util::TrimAsciiWhitespace(*line);
  if (trimmed.empty()) {
    return git_dir;
  }
  std::filesystem::path resolved(trimmed);
  if (resolved.is_relative()) {
    resolved = git_dir / resolved;
  }
  return resolved.lexically_normal();
}

std::optional<std::string> ReadFirstLineOfGitFile(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(std::filesystem::status(path, error)) || error) {
    return std::nullopt;
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return std::nullopt;
  }
  std::string line;
  line.reserve(64);
  for (std::size_t read = 0; read < kMaxGitMetadataLineBytes; ++read) {
    const int byte = stream.get();
    if (byte == std::char_traits<char>::eof() || byte == '\n') {
      break;
    }
    line.push_back(static_cast<char>(byte));
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
}

std::optional<std::filesystem::path> ResolveGitDirectory(const std::filesystem::path& root) {
  if (root.empty()) {
    return std::nullopt;
  }
  std::filesystem::path marker = root / ".git";
  std::error_code error;
  if (std::filesystem::is_directory(marker, error) && !error) {
    return marker;
  }
  const std::optional<std::string> first_line = ReadFirstLineOfGitFile(marker);
  if (!first_line.has_value()) {
    return std::nullopt;
  }
  constexpr std::string_view kGitDirPrefix = "gitdir:";
  std::string_view line(*first_line);
  if (!line.starts_with(kGitDirPrefix)) {
    return std::nullopt;
  }
  line.remove_prefix(kGitDirPrefix.size());
  while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
    line.remove_prefix(1);
  }
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
    line.remove_suffix(1);
  }
  if (line.empty()) {
    return std::nullopt;
  }
  std::filesystem::path git_dir(line);
  return (git_dir.is_absolute() ? git_dir : root / git_dir).lexically_normal();
}

std::optional<std::string> ReadPendingMergeHeadId(const std::filesystem::path& root,
                                                  const GitMetadataSource& metadata) {
  const std::optional<std::filesystem::path> git_dir = metadata.ReadableGitDirectory(root);
  if (!git_dir.has_value()) {
    return std::nullopt;
  }
  const std::optional<std::string> line = ReadFirstLineOfGitFile(*git_dir / "MERGE_HEAD");
  if (!line.has_value()) {
    return std::nullopt;
  }
  std::string_view first_line(*line);
  while (!first_line.empty() &&
         (first_line.back() == ' ' || first_line.back() == '\t')) {
    first_line.remove_suffix(1);
  }
  if (first_line.size() < 7 || first_line.size() > 64) {
    return std::nullopt;
  }
  for (const char byte : first_line) {
    const bool is_hex = (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') ||
                        (byte >= 'A' && byte <= 'F');
    if (!is_hex) {
      return std::nullopt;
    }
  }
  return std::string(first_line);
}

CommandResult ReadGitCommandOutput(const platform::ProcessLauncher& launcher,
                                   const std::filesystem::path& root,
                                   std::vector<std::string> arguments,
                                   bool silence_stderr,
                                   int timeout_ms) {
  return ReadGitCommandOutputWithStdin(launcher, root, std::move(arguments), {}, silence_stderr,
                                       timeout_ms);
}

CommandResult ReadGitCommandOutputWithStdin(
    const platform::ProcessLauncher& launcher,
    const std::filesystem::path& root,
    std::vector<std::string> arguments,
    std::string stdin_text,
    bool silence_stderr,
    int timeout_ms,
    std::vector<platform::SubprocessEnvironmentOverride> environment) {
  util::PerformanceTrace::ScopeLabel label("git::RunCommand");
  if (!arguments.empty()) {
    label.Field("sub", arguments.front());
  }
  util::PerformanceTrace::Scope perf_scope(label.View());

  std::vector<std::string> command;
  command.reserve(arguments.size() + 4);
  command.emplace_back("git");
  command.emplace_back("--no-optional-locks");
  command.emplace_back("--literal-pathspecs");
  for (std::string& argument : arguments) {
    command.push_back(std::move(argument));
  }
  platform::SubprocessOptions options;
  options.cwd = root.lexically_normal();
  options.environment_overrides = std::move(environment);
  options.environment_overrides.push_back({"GIT_TERMINAL_PROMPT", "0"});
  options.environment_overrides.push_back({"GCM_INTERACTIVE", "never"});
  {
    std::filesystem::path normal = options.cwd;
    if (!normal.has_filename()) {
      normal = normal.parent_path();
    }
    if (normal.is_absolute() && normal.has_relative_path()) {
      options.environment_overrides.push_back(
          {"GIT_CEILING_DIRECTORIES", normal.parent_path().string()});
    }
  }
  options.capture_stdout = true;
  options.capture_stderr = !silence_stderr;
  options.silence_stderr = silence_stderr;
  options.stdin_text = std::move(stdin_text);
  options.timeout_ms = timeout_ms;
  const platform::SubprocessResult result = launcher.Run(std::move(command), std::move(options));
  util::AddPerformanceCounter(util::PerfCounterId::GitCommandsRun);
  util::AddPerformanceCounter(util::PerfCounterId::GitCommandOutputBytes,
                              result.stdout_text.size() + result.stderr_text.size());
  if (result.exit_code != 0) {
    util::AddPerformanceCounter(util::PerfCounterId::GitCommandFailures);
  }
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

bool GitCommandSucceeds(const platform::ProcessLauncher& launcher,
                        const std::filesystem::path& root,
                        std::vector<std::string> arguments,
                        bool silence_stderr) {
  return ReadGitCommandOutput(launcher, root, std::move(arguments), silence_stderr).success();
}

}
