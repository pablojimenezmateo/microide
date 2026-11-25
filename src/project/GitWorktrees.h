#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/ProcessLauncher.h"

namespace microide::project {

struct GitWorktreeState {
  std::size_t changed = 0;
  bool has_upstream = false;
  int ahead = 0;
  int behind = 0;

  bool operator==(const GitWorktreeState&) const = default;
};

struct GitWorktree {
  std::string path;
  std::string branch;
  std::string head;
  bool bare = false;
  bool locked = false;
  bool prunable = false;
  std::optional<GitWorktreeState> state;

  bool operator==(const GitWorktree&) const = default;
};

std::vector<GitWorktree> ParseGitWorktreeList(std::string_view porcelain_z);

std::vector<GitWorktree> ListGitWorktrees(const std::filesystem::path& repository_root,
                                          const platform::ProcessLauncher& launcher);

GitWorktreeState ParseGitWorktreeState(std::string_view porcelain_v2_z);

inline constexpr std::size_t kMaxWorktreeStates = 32;
std::vector<GitWorktree> ListGitWorktreesWithState(const std::filesystem::path& repository_root,
                                                   const platform::ProcessLauncher& launcher);

}
