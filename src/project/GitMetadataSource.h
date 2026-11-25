#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "project/GitCommandUtil.h"

namespace microide::platform {
class ProcessLauncher;
}

namespace microide::project {

inline constexpr std::array<std::string_view, 6> kGitStatusArguments = {
    "status", "--porcelain=v2", "-z", "--branch", "--renames", "--untracked-files=all"};

struct GitStatusOutput {
  int exit_code = -1;
  std::string output;
  bool truncated = false;
};

class GitMetadataSource {
 public:
  virtual ~GitMetadataSource() = default;

  virtual GitAvailability Availability(const std::filesystem::path& root) const = 0;

  virtual std::optional<std::filesystem::path> ReadableGitDirectory(
      const std::filesystem::path& root) const = 0;

  bool IsRepository(const std::filesystem::path& root) const {
    return Availability(root) == GitAvailability::Repository;
  }

  virtual std::optional<GitStatusOutput> CurrentStatus(const std::filesystem::path& root) const {
    (void)root;
    return std::nullopt;
  }
};

const GitMetadataSource& LocalGitMetadataSource();

const GitMetadataSource& GitMetadataFor(const platform::ProcessLauncher& launcher);

}
