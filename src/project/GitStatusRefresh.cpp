#include "project/GitStatusRefresh.h"

#include "project/GitMetadataSource.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "project/GitCommandUtil.h"
#include "project/GitPorcelainV2Parser.h"
#include "project/GitRepository.h"

namespace microide::project {

GitRepositoryState BuildGitRepositoryStateFromStatus(const GitRepository& repo,
                                                     const std::filesystem::path& project_root,
                                                     std::uint64_t generation,
                                                     std::uint64_t refreshed_at_ms) {
  GitRepositoryState state{
      .repository_root = project_root,
      .branch = {},
      .entries = {},
      .tree_git_statuses = {},
      .refresh_error = {},
      .generation = generation,
      .refreshed_at_ms = refreshed_at_ms,
      .refreshing = true,
  };

  if (!repo.IsValid()) {
    state.repo_available = false;
    state.refresh_error = {
        .category = GitRefreshErrorCategory::NotARepo,
        .detail = "not a git repository",
    };
    state.refreshing = false;
    return state;
  }

  const GitMetadataSource& metadata = GitMetadataFor(repo.launcher());
  GitRepository::CommandResult result;
  if (std::optional<GitStatusOutput> pushed = metadata.CurrentStatus(project_root)) {
    result.exit_code = pushed->exit_code;
    result.output = std::move(pushed->output);
    result.truncated = pushed->truncated;
  } else {
    result = repo.Execute(std::vector<std::string>(kGitStatusArguments.begin(),
                                                   kGitStatusArguments.end()),
                          false);
  }
  if (!result.success()) {
    state.repo_available = true;
    const GitRefreshErrorCategory category = ClassifyGitRefreshFailure(result.exit_code, result.output);
    state.refresh_error = {
        .category = category,
        .detail = category == GitRefreshErrorCategory::GitNotFound ||
                          result.output.find_first_not_of(" \t\r\n") == std::string::npos
                      ? DescribeSilentGitFailure(category, result.exit_code)
                      : result.output,
    };
    state.stale = true;
    state.refreshing = false;
    return state;
  }
  if (result.truncated) {
    state.repo_available = true;
    state.refresh_error = {
        .category = GitRefreshErrorCategory::UnknownError,
        .detail = "git status output exceeded the capture ceiling; the change list "
                  "would be incomplete",
    };
    state.stale = true;
    state.refreshing = false;
    return state;
  }

  state = GitPorcelainV2Parser::Parse(result.output, project_root,
                                               generation, state.refreshed_at_ms);
  state.repo_available = true;
  state.operation_state = DetectGitOperationState(project_root, metadata);
  state.worktree_count = CountGitWorktrees(project_root, metadata);
  if (state.operation_state == GitOperationStateKind::Merge) {
    state.pending_merge_head = internal::ReadPendingMergeHeadId(project_root, metadata).value_or("");
  }
  for (GitRepositoryEntry& entry : state.entries) {
    if (!entry.conflicted) {
      continue;
    }
    const auto is_directory_at = [&project_root](std::string_view relative) {
      std::error_code error;
      return std::filesystem::is_directory(project_root / relative, error) && !error;
    };
    if (is_directory_at(entry.path.relative_path)) {
      entry.path_is_directory = true;
      continue;
    }
    const std::string_view relative_text = entry.path.relative_path;
    const std::size_t slash = relative_text.find_last_of('/');
    const std::string_view leaf =
        slash == std::string_view::npos ? relative_text : relative_text.substr(slash + 1);
    const std::size_t tilde = leaf.rfind('~');
    if (tilde == std::string_view::npos || tilde == 0) {
      continue;
    }
    entry.path_is_directory = is_directory_at(
        relative_text.substr(0, (slash == std::string_view::npos ? 0 : slash + 1) + tilde));
  }
  state.refreshing = false;
  return state;
}

}
