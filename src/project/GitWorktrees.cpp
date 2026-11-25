#include "project/GitWorktrees.h"

#include <utility>

#include "project/GitCommandUtil.h"
#include "project/GitRepository.h"
#include "util/Parse.h"

namespace microide::project {
namespace {

constexpr std::size_t kMaxWorktrees = 1024;

}

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
    const std::size_t space = line.find(' ');
    const std::string_view key = line.substr(0, space);
    const std::string_view value = space == std::string_view::npos ? std::string_view() : line.substr(space + 1);
    if (key == "worktree") {
      close();
      current.path = std::string(value);
      open = true;
    } else if (key == "HEAD") {
      current.head = std::string(value);
    } else if (key == "branch") {
      current.branch = std::string(internal::StripBranchRefPrefix(value));
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

GitWorktreeState ParseGitWorktreeState(std::string_view porcelain_v2_z) {
  GitWorktreeState state;
  while (!porcelain_v2_z.empty()) {
    const std::size_t end = porcelain_v2_z.find('\0');
    const std::string_view record = porcelain_v2_z.substr(0, end);
    porcelain_v2_z.remove_prefix(end == std::string_view::npos ? porcelain_v2_z.size() : end + 1);
    constexpr std::string_view kAheadBehind = "# branch.ab ";
    if (record.starts_with(kAheadBehind)) {
      const std::string_view counts = record.substr(kAheadBehind.size());
      const std::size_t space = counts.find(' ');
      const auto count = [](std::string_view text, char sign) {
        return !text.empty() && text.front() == sign ? util::ParseInt(text.substr(1)) : std::nullopt;
      };
      const std::optional<int> ahead = count(counts.substr(0, space), '+');
      const std::optional<int> behind =
          space == std::string_view::npos ? std::nullopt : count(counts.substr(space + 1), '-');
      if (ahead.has_value() && behind.has_value()) {
        state.has_upstream = true;
        state.ahead = *ahead;
        state.behind = *behind;
      }
      continue;
    }
    if (record.size() < 2 || record[1] != ' ') {
      continue;
    }
    switch (record.front()) {
      case '2': {
        const std::size_t original_end = porcelain_v2_z.find('\0');
        porcelain_v2_z.remove_prefix(original_end == std::string_view::npos ? porcelain_v2_z.size()
                                                                            : original_end + 1);
        [[fallthrough]];
      }
      case '1':
      case 'u':
      case '?':
        ++state.changed;
        break;
      default:
        break;
    }
  }
  return state;
}

std::vector<GitWorktree> ListGitWorktreesWithState(const std::filesystem::path& repository_root,
                                                   const platform::ProcessLauncher& launcher) {
  std::vector<GitWorktree> worktrees = ListGitWorktrees(repository_root, launcher);
  const GitRepository repo(repository_root, launcher);
  std::size_t asked = 0;
  for (GitWorktree& worktree : worktrees) {
    if (worktree.bare || worktree.prunable || worktree.path.empty()) {
      continue;
    }
    if (asked++ == kMaxWorktreeStates) {
      break;
    }
    const auto status = repo.Execute(
        std::vector<std::string>{"-C", worktree.path, "status", "--porcelain=v2", "--branch", "-z"});
    if (status.success()) {
      worktree.state = ParseGitWorktreeState(status.output);
    }
  }
  return worktrees;
}

}
