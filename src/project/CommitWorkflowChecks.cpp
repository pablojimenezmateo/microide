#include "project/CommitWorkflowChecks.h"

#include <algorithm>

#include "project/GitRepository.h"
#include "util/GitConflictMarkers.h"
#include "util/Parse.h"
#include "util/StringUtil.h"

namespace microide::project {
namespace {

std::string CheckId(const CommitPreCheckKind kind) {
  switch (kind) {
    case CommitPreCheckKind::EmptySubject:
      return "empty_subject";
    case CommitPreCheckKind::LongSubject:
      return "long_subject";
    case CommitPreCheckKind::UnresolvedConflicts:
      return "unresolved_conflicts";
    case CommitPreCheckKind::ConflictMarkers:
      return "conflict_markers";
    case CommitPreCheckKind::UnstagedLeftovers:
      return "unstaged_leftovers";
    case CommitPreCheckKind::BranchBehind:
      return "branch_behind";
    case CommitPreCheckKind::UntrackedFiles:
      return "untracked_files";
  }
  return "unknown";
}

CommitPreCheck MakeCheck(const CommitPreCheckKind kind,
                         const CommitPreCheckSeverity severity,
                         std::string message) {
  return CommitPreCheck{
      .kind = kind,
      .severity = severity,
      .id = CheckId(kind),
      .message = std::move(message),
  };
}

bool PathHasStagedAndUnstaged(const GitRepositoryState& repository_state,
                              const std::filesystem::path& relative_path) {
  // porcelain v2 emits exactly one record per path, so a partially-staged file
  // (staged edit plus a further unstaged edit, `1 MM`) is a single entry that is both
  // `staged` and `worktree_dirty`. An older two-entries-per-path assumption never fired
  // under v2. Still tolerate a split representation (staged in one record, unstaged in
  // another) in case a caller pre-splits the entries for display.
  bool staged = false;
  bool unstaged = false;
  for (const GitRepositoryEntry& entry : repository_state.entries) {
    if (entry.path.relative_path != relative_path) {
      continue;
    }
    if (entry.staged) {
      staged = true;
    }
    if (entry.worktree_dirty && entry.kind != GitRepositoryEntryKind::Untracked) {
      unstaged = true;
    }
  }
  return staged && unstaged;
}

}  // namespace

CommitStagedSummary BuildCommitStagedSummary(const GitRepositoryState& repository_state) {
  CommitStagedSummary summary;
  if (!repository_state.repo_available || repository_state.repository_root.empty()) {
    return summary;
  }

  GitRepository repo(repository_state.repository_root);
  // `-z`: NUL-delimited, unquoted. Without it a path containing a newline splits
  // into bogus rows and a rename shows up as one mangled "old => new" path.
  const GitRepository::CommandResult numstat =
      repo.Execute({"diff", "--cached", "--numstat", "-z"}, true);
  if (!numstat.success() || numstat.output.empty()) {
    for (const GitRepositoryEntry& entry : repository_state.entries) {
      if (!entry.staged) {
        continue;
      }
      summary.files.push_back(CommitStagedFileSummary{
          .relative_path = entry.path.relative_path,
      });
    }
    summary.file_count = summary.files.size();
    return summary;
  }

  // Each numstat record is `<added>\t<deleted>\t<path>` in its own NUL-delimited
  // field. For a rename/copy the path field after the second tab is EMPTY and the
  // old and new paths follow as the next two NUL fields (git emits `... \t \0 old
  // \0 new`); we report the new path.
  const std::vector<std::string_view> fields = util::SplitNulDelimited(numstat.output);
  for (std::size_t i = 0; i < fields.size(); ++i) {
    const std::string_view field = fields[i];
    const std::size_t first_tab = field.find('\t');
    const std::size_t second_tab =
        first_tab == std::string_view::npos ? std::string_view::npos : field.find('\t', first_tab + 1);
    if (first_tab == std::string_view::npos || second_tab == std::string_view::npos) {
      continue;
    }
    CommitStagedFileSummary file_summary;
    const std::string_view added_text = field.substr(0, first_tab);
    const std::string_view deleted_text = field.substr(first_tab + 1, second_tab - first_tab - 1);
    std::string_view path_text = field.substr(second_tab + 1);
    if (path_text.empty() && i + 2 < fields.size()) {
      path_text = fields[i + 2];  // rename: [i+1]=old, [i+2]=new
      i += 2;
    }
    if (added_text != "-") {
      if (const auto parsed = util::ParseInt(added_text)) {
        file_summary.added_lines = std::max(0, *parsed);
      }
    }
    if (deleted_text != "-") {
      if (const auto parsed = util::ParseInt(deleted_text)) {
        file_summary.deleted_lines = std::max(0, *parsed);
      }
    }
    file_summary.relative_path = std::filesystem::path(path_text);
    summary.added_lines += file_summary.added_lines;
    summary.deleted_lines += file_summary.deleted_lines;
    summary.files.push_back(std::move(file_summary));
  }

  if (summary.files.empty()) {
    for (const GitRepositoryEntry& entry : repository_state.entries) {
      if (!entry.staged) {
        continue;
      }
      summary.files.push_back(CommitStagedFileSummary{
          .relative_path = entry.path.relative_path,
      });
    }
  }

  summary.file_count = summary.files.size();
  return summary;
}

bool StagedDiffContainsConflictMarkers(const std::filesystem::path& repository_root) {
  if (repository_root.empty()) {
    return false;
  }
  GitRepository repo(repository_root);
  const GitRepository::CommandResult diff = repo.Execute({"diff", "--cached"}, true);
  if (!diff.success()) {
    return false;
  }
  return util::ContainsAnyConflictMarker(diff.output);
}

std::vector<CommitPreCheck> RunCommitPreChecks(
    const GitRepositoryState& repository_state,
    const std::string_view subject,
    const std::string_view body,
    const std::unordered_set<std::string>& acknowledged_warning_ids) {
  (void)body;
  (void)acknowledged_warning_ids;
  std::vector<CommitPreCheck> checks;
  if (!repository_state.repo_available) {
    checks.push_back(MakeCheck(CommitPreCheckKind::UnresolvedConflicts, CommitPreCheckSeverity::Blocking,
                               "Not a git repository"));
    return checks;
  }

  const CommitStagedSummary staged_summary = BuildCommitStagedSummary(repository_state);
  if (staged_summary.file_count == 0) {
    checks.push_back(MakeCheck(CommitPreCheckKind::EmptySubject, CommitPreCheckSeverity::Blocking,
                               "Nothing is staged for commit"));
    return checks;
  }

  if (subject.empty()) {
    checks.push_back(MakeCheck(CommitPreCheckKind::EmptySubject, CommitPreCheckSeverity::Blocking,
                               "Commit subject is required"));
  } else if (subject.size() > kCommitSubjectMaxLength) {
    checks.push_back(MakeCheck(
        CommitPreCheckKind::LongSubject, CommitPreCheckSeverity::Blocking,
        "Commit subject exceeds " + std::to_string(kCommitSubjectMaxLength) + " characters"));
  }

  const bool has_conflicts = std::any_of(
      repository_state.entries.begin(), repository_state.entries.end(),
      [](const GitRepositoryEntry& entry) { return entry.conflicted || entry.conflict_kind != GitConflictKind::None; });
  if (has_conflicts) {
    checks.push_back(MakeCheck(CommitPreCheckKind::UnresolvedConflicts, CommitPreCheckSeverity::Blocking,
                               "Resolve merge conflicts before committing"));
  }

  if (StagedDiffContainsConflictMarkers(repository_state.repository_root)) {
    checks.push_back(MakeCheck(CommitPreCheckKind::ConflictMarkers, CommitPreCheckSeverity::Blocking,
                               "Staged changes still contain conflict markers"));
  }

  std::vector<std::filesystem::path> partial_stage_paths;
  for (const CommitStagedFileSummary& file : staged_summary.files) {
    if (PathHasStagedAndUnstaged(repository_state, file.relative_path)) {
      partial_stage_paths.push_back(file.relative_path);
    }
  }
  if (!partial_stage_paths.empty()) {
    checks.push_back(MakeCheck(
        CommitPreCheckKind::UnstagedLeftovers, CommitPreCheckSeverity::Warning,
        "Some staged files also have unstaged changes; the commit will include only staged hunks"));
  }

  if (!repository_state.branch.upstream.empty() && repository_state.branch.behind > 0) {
    checks.push_back(MakeCheck(
        CommitPreCheckKind::BranchBehind, CommitPreCheckSeverity::Warning,
        "Current branch is behind its upstream by " + std::to_string(repository_state.branch.behind) +
            " commit(s)"));
  }

  const bool has_untracked = std::any_of(
      repository_state.entries.begin(), repository_state.entries.end(),
      [](const GitRepositoryEntry& entry) {
        return entry.kind == GitRepositoryEntryKind::Untracked && !entry.staged;
      });
  if (has_untracked) {
    checks.push_back(MakeCheck(CommitPreCheckKind::UntrackedFiles, CommitPreCheckSeverity::Warning,
                               "Untracked files are not included in this commit"));
  }

  return checks;
}

bool CommitPreChecksAllowExecution(const std::vector<CommitPreCheck>& checks,
                                   const std::unordered_set<std::string>& acknowledged_warning_ids) {
  for (const CommitPreCheck& check : checks) {
    if (check.severity == CommitPreCheckSeverity::Blocking) {
      return false;
    }
    if (check.severity == CommitPreCheckSeverity::Warning &&
        acknowledged_warning_ids.find(check.id) == acknowledged_warning_ids.end()) {
      return false;
    }
  }
  return true;
}

}  // namespace microide::project
