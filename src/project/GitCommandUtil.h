#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace microide::project::internal {

struct CommandResult {
  int exit_code = -1;
  std::string output;

  bool success() const { return exit_code == 0; }
};

bool HasGitMarker(const std::filesystem::path& root);
std::optional<std::filesystem::path> AbsoluteToRelativePath(
    const std::filesystem::path& root,
    const std::filesystem::path& absolute_path);
std::optional<std::string> ResolveHeadId(const std::filesystem::path& root);
CommandResult ReadCommandOutput(const std::vector<std::string>& command,
                                bool silence_stderr = true);
CommandResult ReadGitCommandOutput(const std::filesystem::path& root,
                                   std::vector<std::string> arguments,
                                   bool silence_stderr = true);
bool CommandSucceeds(const std::vector<std::string>& command, bool silence_stderr = true);
bool GitCommandSucceeds(const std::filesystem::path& root,
                        std::vector<std::string> arguments,
                        bool silence_stderr = true);

}  // namespace microide::project::internal
