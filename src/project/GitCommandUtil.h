#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace microide::project::internal {

struct CommandResult {
  int exit_code = -1;
  std::string output;
  // The command exceeded its wall-clock timeout and was killed. Distinguishes a
  // spurious "failure" (a slow pre-commit hook, a stuck network/credential
  // prompt) from a real non-zero exit so callers can report it accurately.
  bool timed_out = false;
  // Captured output hit the per-stream ceiling and the command was killed; the
  // output is truncated. Callers that display command output (blob/diff reads)
  // should surface this rather than treating it as an outright failure.
  bool truncated = false;

  bool success() const { return exit_code == 0; }
};

// Wall-clock caps for git invocations. Local read commands (status, diff, blame,
// log, show) finish in well under a second, so a short cap catches a genuinely
// stuck git (credential/network stall, filesystem hang) without holding the shell
// hostage. Write/long ops (commit, apply) may legitimately run a slow pre-commit
// hook, so they get a far more generous cap rather than being killed at 60s.
inline constexpr int kGitReadTimeoutMs = 60'000;
inline constexpr int kGitWriteTimeoutMs = 300'000;

bool HasGitMarker(const std::filesystem::path& root);
std::optional<std::filesystem::path> AbsoluteToRelativePath(
    const std::filesystem::path& root,
    const std::filesystem::path& absolute_path);
std::optional<std::string> ResolveHeadId(const std::filesystem::path& root);
CommandResult ReadGitCommandOutput(const std::filesystem::path& root,
                                   std::vector<std::string> arguments,
                                   bool silence_stderr = true,
                                   int timeout_ms = kGitReadTimeoutMs);
CommandResult ReadGitCommandOutputWithStdin(const std::filesystem::path& root,
                                            std::vector<std::string> arguments,
                                            std::string stdin_text,
                                            bool silence_stderr = true,
                                            int timeout_ms = kGitReadTimeoutMs);
bool GitCommandSucceeds(const std::filesystem::path& root,
                        std::vector<std::string> arguments,
                        bool silence_stderr = true);

}  // namespace microide::project::internal
