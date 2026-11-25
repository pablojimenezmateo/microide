#include "project/CommitWorkflowChecks.h"

#include <algorithm>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "project/GitPorcelainParser.h"
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

std::unordered_set<std::string> PartiallyStagedPaths(const GitRepositoryState& repository_state) {
  struct StageFlags {
    bool staged = false;
    bool unstaged = false;
  };
  std::unordered_map<std::string, StageFlags> flags;
  flags.reserve(repository_state.entries.size());
  for (const GitRepositoryEntry& entry : repository_state.entries) {
    StageFlags& f = flags[entry.path.relative_path];
    if (entry.staged) {
      f.staged = true;
    }
    if (entry.worktree_dirty && entry.kind != GitRepositoryEntryKind::Untracked) {
      f.unstaged = true;
    }
  }
  std::unordered_set<std::string> partial;
  for (auto& [path, f] : flags) {
    if (f.staged && f.unstaged) {
      partial.insert(path);
    }
  }
  return partial;
}

void AppendStagedEntryPaths(const GitRepositoryState& repository_state,
                            std::vector<CommitStagedFileSummary>& files) {
  for (const GitRepositoryEntry& entry : repository_state.entries) {
    if (entry.staged) {
      files.push_back(CommitStagedFileSummary{.relative_path = entry.path.relative_path});
    }
  }
}

}

CommitStagedSummary BuildCommitStagedSummary(const GitRepositoryState& repository_state,
                                             const platform::ProcessLauncher& launcher) {
  CommitStagedSummary summary;
  if (!repository_state.repo_available || repository_state.repository_root.empty()) {
    return summary;
  }

  GitRepository repo(repository_state.repository_root, launcher);
  const GitRepository::CommandResult numstat =
      repo.Execute({"diff", "--cached", "--numstat", "-z"}, true);
  if (!numstat.success() || numstat.output.empty()) {
    AppendStagedEntryPaths(repository_state, summary.files);
    summary.file_count = summary.files.size();
    return summary;
  }

  constexpr std::size_t kMaxStagedSummaryFiles = kMaxGitStatusEntries;
  const std::vector<std::string_view> fields =
      util::SplitNulDelimited(numstat.output, kMaxStagedSummaryFiles * 3 + 2);
  std::int64_t added_total = 0;
  std::int64_t deleted_total = 0;
  for (std::size_t i = 0; i < fields.size() && summary.files.size() < kMaxStagedSummaryFiles;
       ++i) {
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
      path_text = fields[i + 2];
      i += 2;
    }
    if (added_text != "-") {
      if (const auto parsed = util::ParseInt64(added_text)) {
        file_summary.added_lines = static_cast<int>(
            std::clamp<std::int64_t>(*parsed, 0, std::numeric_limits<int>::max()));
      }
    }
    if (deleted_text != "-") {
      if (const auto parsed = util::ParseInt64(deleted_text)) {
        file_summary.deleted_lines = static_cast<int>(
            std::clamp<std::int64_t>(*parsed, 0, std::numeric_limits<int>::max()));
      }
    }
    file_summary.relative_path = std::string(path_text);
    added_total += file_summary.added_lines;
    deleted_total += file_summary.deleted_lines;
    summary.files.push_back(std::move(file_summary));
  }
  summary.added_lines =
      static_cast<int>(std::min<std::int64_t>(added_total, std::numeric_limits<int>::max()));
  summary.deleted_lines =
      static_cast<int>(std::min<std::int64_t>(deleted_total, std::numeric_limits<int>::max()));

  if (summary.files.empty()) {
    AppendStagedEntryPaths(repository_state, summary.files);
  }

  summary.file_count = summary.files.size();
  return summary;
}

std::optional<bool> StagedDiffContainsConflictMarkers(
    const std::filesystem::path& repository_root, const platform::ProcessLauncher& launcher) {
  if (repository_root.empty()) {
    return false;
  }
  GitRepository repo(repository_root, launcher);
  const GitRepository::CommandResult diff = repo.Execute({"diff", "--cached"}, true);
  if (!diff.success()) {
    return std::nullopt;
  }
  return util::StagedDiffIntroducesConflictMarker(diff.output);
}

std::vector<CommitPreCheck> RunCommitPreChecks(
    const GitRepositoryState& repository_state,
    const platform::ProcessLauncher& launcher,
    const std::string_view subject,
    const std::string_view body,
    const std::unordered_set<std::string>& acknowledged_warning_ids,
    const CommitStagedSummary* precomputed_summary,
    const bool scan_staged_diff_for_conflict_markers) {
  (void)body;
  (void)acknowledged_warning_ids;
  std::vector<CommitPreCheck> checks;
  if (!repository_state.repo_available) {
    checks.push_back(MakeCheck(CommitPreCheckKind::UnresolvedConflicts, CommitPreCheckSeverity::Blocking,
                               "Not a git repository"));
    return checks;
  }

  CommitStagedSummary owned_summary;
  const CommitStagedSummary* summary_view = precomputed_summary;
  if (summary_view == nullptr) {
    owned_summary = BuildCommitStagedSummary(repository_state, launcher);
    summary_view = &owned_summary;
  }
  const CommitStagedSummary& staged_summary = *summary_view;
  if (staged_summary.file_count == 0) {
    checks.push_back(MakeCheck(CommitPreCheckKind::EmptySubject, CommitPreCheckSeverity::Blocking,
                               "Nothing is staged for commit"));
    return checks;
  }

  if (subject.empty()) {
    checks.push_back(MakeCheck(CommitPreCheckKind::EmptySubject, CommitPreCheckSeverity::Blocking,
                               "Commit subject is required"));
  } else if (util::Utf8CodepointCount(subject) > kCommitSubjectMaxLength) {
    static_assert(kCommitSubjectMaxLength == 72, "keep the long-subject message in sync");
    checks.push_back(MakeCheck(CommitPreCheckKind::LongSubject, CommitPreCheckSeverity::Warning,
                               "Commit subject exceeds 72 characters"));
  }

  const bool has_conflicts = std::any_of(
      repository_state.entries.begin(), repository_state.entries.end(),
      [](const GitRepositoryEntry& entry) { return entry.conflicted || entry.conflict_kind != GitConflictKind::None; });
  if (has_conflicts) {
    checks.push_back(MakeCheck(CommitPreCheckKind::UnresolvedConflicts, CommitPreCheckSeverity::Blocking,
                               "Resolve merge conflicts before committing"));
  }

  if (scan_staged_diff_for_conflict_markers) {
    const std::optional<bool> has_markers =
        StagedDiffContainsConflictMarkers(repository_state.repository_root, launcher);
    if (!has_markers.has_value()) {
      checks.push_back(MakeCheck(CommitPreCheckKind::ConflictMarkers, CommitPreCheckSeverity::Warning,
                                 "Could not verify staged changes for conflict markers"));
    } else if (*has_markers) {
      checks.push_back(MakeCheck(CommitPreCheckKind::ConflictMarkers,
                                 CommitPreCheckSeverity::Blocking,
                                 "Staged changes still contain conflict markers"));
    }
  }

  const std::unordered_set<std::string> partially_staged = PartiallyStagedPaths(repository_state);
  bool any_partial_stage = false;
  for (const CommitStagedFileSummary& file : staged_summary.files) {
    if (partially_staged.count(file.relative_path) != 0) {
      any_partial_stage = true;
      break;
    }
  }
  if (any_partial_stage) {
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

}
