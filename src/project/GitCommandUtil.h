#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/ProcessLauncher.h"

namespace microide::project {

enum class GitAvailability { Unknown, Repository, NotARepository };

class GitMetadataSource;

}

namespace microide::project::internal {

inline constexpr std::string_view kBranchRefPrefix = "refs/heads/";

[[nodiscard]] inline std::string_view StripBranchRefPrefix(std::string_view ref) {
  return ref.starts_with(kBranchRefPrefix) ? ref.substr(kBranchRefPrefix.size()) : ref;
}

struct CommandResult {
  int exit_code = -1;
  std::string output;
  bool timed_out = false;
  bool truncated = false;

  bool success() const { return exit_code == 0; }
};

inline constexpr int kGitReadTimeoutMs = 60'000;
inline constexpr int kGitWriteTimeoutMs = 300'000;

bool HasGitMarker(const std::filesystem::path& root);
std::optional<std::filesystem::path> AbsoluteToRelativePath(
    const std::filesystem::path& root,
    const std::filesystem::path& absolute_path);
const std::optional<std::filesystem::path>& AbsoluteToRelativePathRef(
    const std::filesystem::path& root,
    const std::filesystem::path& absolute_path);
std::optional<std::string> ResolveHeadId(const platform::ProcessLauncher& launcher,
                                         const std::filesystem::path& root);

inline constexpr std::size_t kMaxGitMetadataLineBytes = 4096;
std::optional<std::string> ReadFirstLineOfGitFile(const std::filesystem::path& path);

std::filesystem::path ResolveCommonDir(const std::filesystem::path& git_dir);

std::optional<std::filesystem::path> ResolveGitDirectory(const std::filesystem::path& root);

std::optional<std::string> ReadPendingMergeHeadId(const std::filesystem::path& root,
                                                  const GitMetadataSource& metadata);
CommandResult ReadGitCommandOutput(const platform::ProcessLauncher& launcher,
                                   const std::filesystem::path& root,
                                   std::vector<std::string> arguments,
                                   bool silence_stderr = true,
                                   int timeout_ms = kGitReadTimeoutMs);
CommandResult ReadGitCommandOutputWithStdin(
    const platform::ProcessLauncher& launcher,
    const std::filesystem::path& root,
    std::vector<std::string> arguments,
    std::string stdin_text,
    bool silence_stderr = true,
    int timeout_ms = kGitReadTimeoutMs,
    std::vector<platform::SubprocessEnvironmentOverride> environment = {});
bool GitCommandSucceeds(const platform::ProcessLauncher& launcher,
                        const std::filesystem::path& root,
                        std::vector<std::string> arguments,
                        bool silence_stderr = true);

}
