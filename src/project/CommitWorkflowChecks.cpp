#include "project/CommitWorkflowChecks.h"

#include <algorithm>

#include "project/GitRepository.h"
#include "util/Parse.h"

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
  bool staged = false;
  bool unstaged = false;
  for (const GitRepositoryEntry& entry : repository_state.entries) {
    if (entry.path.relative_path != relative_path) {
      continue;
    }
    if (entry.staged) {
      staged = true;
    } else if (entry.kind != GitRepositoryEntryKind::Untracked) {
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
  const GitRepository::CommandResult numstat =
      repo.Execute({"diff", "--cached", "--numstat"}, true);
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

  std::string_view remaining = numstat.output;
  while (!remaining.empty()) {
    const std::size_t line_end = remaining.find('\n');
    const std::string_view line =
        line_end == std::string_view::npos ? remaining : remaining.substr(0, line_end);
    if (!line.empty()) {
      const std::size_t first_tab = line.find('\t');
      const std::size_t second_tab =
          first_tab == std::string_view::npos ? std::string_view::npos : line.find('\t', first_tab + 1);
      if (first_tab != std::string_view::npos && second_tab != std::string_view::npos) {
        CommitStagedFileSummary file_summary;
        const std::string_view added_text = line.substr(0, first_tab);
        const std::string_view deleted_text = line.substr(first_tab + 1, second_tab - first_tab - 1);
        const std::string_view path_text = line.substr(second_tab + 1);
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
    }
    if (line_end == std::string_view::npos) {
      break;
    }
    remaining.remove_prefix(line_end + 1);
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
  return diff.output.find("<<<<<<<") != std::string::npos ||
         diff.output.find(">>>>>>>") != std::string::npos ||
         diff.output.find("=======") != std::string::npos;
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
