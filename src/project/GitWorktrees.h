#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/ProcessLauncher.h"

namespace microide::project {

// What a working tree holds beyond its commit (`git status`): how many paths
// differ from HEAD or are untracked, and how its branch stands against its
// upstream -- whether an agent left something there to look at.
struct GitWorktreeState {
  std::size_t changed = 0;
  bool has_upstream = false;
  int ahead = 0;
  int behind = 0;

  bool operator==(const GitWorktreeState&) const = default;
};

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
  // nullopt: not asked (ListGitWorktrees), or git could not say.
  std::optional<GitWorktreeState> state;

  bool operator==(const GitWorktree&) const = default;
};

// Parses `git worktree list --porcelain -z`: NUL-terminated attribute lines, a
// record ended by an empty one. Pure, so the format is tested without git.
std::vector<GitWorktree> ParseGitWorktreeList(std::string_view porcelain_z);

// The repository's working trees; empty when `repository_root` is not in one or
// git cannot say. The launcher decides which machine's git answers.
std::vector<GitWorktree> ListGitWorktrees(const std::filesystem::path& repository_root,
                                          const platform::ProcessLauncher& launcher);

// Parses `git status --porcelain=v2 --branch -z`: the entries counted, and the
// `# branch.ab` header. Pure.
GitWorktreeState ParseGitWorktreeState(std::string_view porcelain_v2_z);

// ListGitWorktrees, with the state of every tree that has a directory: one `git
// status` a tree, on git's machine -- for a pick that waits off-thread, never
// for a frame. Past kMaxWorktreeStates trees the rest are listed without one.
inline constexpr std::size_t kMaxWorktreeStates = 32;
std::vector<GitWorktree> ListGitWorktreesWithState(const std::filesystem::path& repository_root,
                                                   const platform::ProcessLauncher& launcher);

}  // namespace microide::project
