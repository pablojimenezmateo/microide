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

  const auto result = repo.Execute(
      {"status", "--porcelain=v2", "-z", "--branch", "--renames", "--untracked-files=all"},
      false);
  if (!result.success()) {
    state.repo_available = true;
    state.refresh_error = {
        .category = ClassifyGitRefreshFailure(result.exit_code, result.output),
        .detail = result.output,
    };
    state.stale = true;
    state.refreshing = false;
    return state;
  }
  if (result.truncated) {
    // git exited cleanly but its output hit the capture ceiling, so what came
    // back is a PREFIX of the status. Parsing it would produce a change list that
    // is real and incomplete, shown as if it were the whole one — and a user who
    // stages "everything" from a truncated list commits less than they think.
    // The failure branch above already exists for "we cannot show you the truth";
    // this is the same answer arrived at differently.
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
  // Cheap filesystem probes on the same background refresh — porcelain v2 does
  // not report the in-flight operation, and nothing else wrote this field, so it
  // stayed None forever and the merge resolver's rebase/cherry-pick label was
  // unreachable.
  const GitMetadataSource& metadata = GitMetadataFor(repo.launcher());
  state.operation_state = DetectGitOperationState(project_root, metadata);
  if (state.operation_state == GitOperationStateKind::Merge) {
    state.pending_merge_head = internal::ReadPendingMergeHeadId(project_root, metadata).value_or("");
  }
  // D/F (file-vs-directory) conflicts are the one conflict shape porcelain v2
  // cannot express, so probe the worktree here — background thread, and only for
  // the handful of conflicted entries, so a clean repo does no extra I/O.
  //
  // git does NOT report the conflicted path itself. It resolves the collision by
  // leaving the directory in place and moving the file side aside:
  //
  //   CONFLICT (file/directory): directory in the way of thing from file-side;
  //   moving it to thing~file-side instead.
  //   u UA N... 000000 000000 100644 100644 ... thing~file-side
  //
  // so the unmerged record names `thing~file-side` (an ordinary file) while the
  // directory sits at `thing`. Both merge directions produce that shape — only
  // the suffix differs (`~file-side` vs `~HEAD`). Probing the record's own path
  // therefore never fires; the prefix before the last `~` is what to test. The
  // direct probe is kept as a cheap fallback in case a git version ever reports
  // the path itself.
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
    // Search the FINAL component only. git appends `~<branch>` to the last
    // component, and scanning the whole path instead matches a `~` in a DIRECTORY
    // name: a plain content conflict at `dir~x/file.txt` would probe `dir`, find a
    // directory, and be misreported as file/directory — which also sets
    // TextHunksAvailable false and denies the user the three-way merge editor for
    // an ordinary conflict. (Reproduced against real git before this was fixed.)
    const std::string_view relative_text = entry.path.relative_path;
    const std::size_t slash = relative_text.find_last_of('/');
    const std::string_view leaf =
        slash == std::string_view::npos ? relative_text : relative_text.substr(slash + 1);
    const std::size_t tilde = leaf.rfind('~');
    // `tilde == 0` would leave an empty prefix, which would probe the parent.
    if (tilde == std::string_view::npos || tilde == 0) {
      continue;
    }
    // The record's own path with the `~<branch>` suffix stripped off its last
    // component — i.e. the directory git left in place.
    entry.path_is_directory = is_directory_at(
        relative_text.substr(0, (slash == std::string_view::npos ? 0 : slash + 1) + tilde));
  }
  state.refreshing = false;
  return state;  state.refreshing = false;
  return state;
}

}  // namespace microide::project
