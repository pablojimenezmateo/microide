#include "project/GitWorktrees.h"

#include <utility>

#include "project/GitRepository.h"

namespace microide::project {
namespace {

// A repository with more working trees than this is listing something else.
constexpr std::size_t kMaxWorktrees = 1024;

}  // namespace

std::vector<GitWorktree> ParseGitWorktreeList(std::string_view porcelain_z) {
  std::vector<GitWorktree> worktrees;
  GitWorktree current;
  bool open = false;
  const auto close = [&]() {
    if (open && !current.path.empty() && worktrees.size() < kMaxWorktrees) {
      worktrees.push_back(std::move(current));
    }
    current = GitWorktree{};
    open = false;
  };
  while (!porcelain_z.empty()) {
    const std::size_t end = porcelain_z.find('\0');
    const std::string_view line = porcelain_z.substr(0, end);
    porcelain_z.remove_prefix(end == std::string_view::npos ? porcelain_z.size() : end + 1);
    if (line.empty()) {
      close();
      continue;
    }
    // "<key>" or "<key> <value>"; `locked` and `prunable` may carry a reason.
    const std::size_t space = line.find(' ');
    const std::string_view key = line.substr(0, space);
    const std::string_view value = space == std::string_view::npos ? std::string_view() : line.substr(space + 1);
    if (key == "worktree") {
      close();  // a record with no terminator before the next one
      current.path = std::string(value);
      open = true;
    } else if (key == "HEAD") {
      current.head = std::string(value);
    } else if (key == "branch") {
      constexpr std::string_view kHeads = "refs/heads/";
      current.branch = std::string(value.starts_with(kHeads) ? value.substr(kHeads.size()) : value);
    } else if (key == "bare") {
      current.bare = true;
    } else if (key == "locked") {
      current.locked = true;
    } else if (key == "prunable") {
      current.prunable = true;
    }
  }
  close();
  return worktrees;
}

std::vector<GitWorktree> ListGitWorktrees(const std::filesystem::path& repository_root,
                                          const platform::ProcessLauncher& launcher) {
  const GitRepository repo(repository_root, launcher);
  if (!repo.IsValid()) {
    return {};
  }
  const auto result = repo.Execute(std::vector<std::string>{"worktree", "list", "--porcelain", "-z"});
  if (!result.success()) {
    return {};
  }
  return ParseGitWorktreeList(result.output);
}

}  // namespace microide::project
