#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "platform/ProcessLauncher.h"

namespace microide::project {

// One working tree of a repository (`git worktree list`): the main one first,
// then every linked one -- where parallel agents each do their work.
struct GitWorktree {
  // As git names it: on the machine the repository is on.
  std::string path;
  // The checked-out branch's short name; "" when detached or bare.
  std::string branch;
  // HEAD's object id; "" for a bare entry.
  std::string head;
  bool bare = false;
  bool locked = false;
  // Its directory is gone: `git worktree prune` would drop it.
  bool prunable = false;

  bool operator==(const GitWorktree&) const = default;
};

// Parses `git worktree list --porcelain -z`: NUL-terminated attribute lines, a
// record ended by an empty one. Pure, so the format is tested without git.
std::vector<GitWorktree> ParseGitWorktreeList(std::string_view porcelain_z);

// The repository's working trees; empty when `repository_root` is not in one or
// git cannot say. The launcher decides which machine's git answers.
std::vector<GitWorktree> ListGitWorktrees(const std::filesystem::path& repository_root,
                                          const platform::ProcessLauncher& launcher);

}  // namespace microide::project
