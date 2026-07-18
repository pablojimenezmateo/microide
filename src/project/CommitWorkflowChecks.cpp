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

// One-pass set of the paths that are both staged and unstaged ("partially staged").
// A prior implementation re-scanned repository_state.entries once per staged file,
// which is O(staged_files * status_entries) on every commit-subject keystroke in a
// large repo; this walks the entries a single time.
//
// porcelain v2 emits exactly one record per path, so a partially-staged file (staged
// edit plus a further unstaged edit, `1 MM`) is a single entry that is both `staged`
// and `worktree_dirty`. An older two-entries-per-path assumption never fired under v2.
// Still tolerate a split representation (staged in one record, unstaged in another) by
// aggregating per-path flags before intersecting.
std::unordered_set<std::string> PartiallyStagedPaths(const GitRepositoryState& repository_state) {
  struct StageFlags {
    bool staged = false;
    bool unstaged = false;
  };
  std::unordered_map<std::string, StageFlags> flags;
  flags.reserve(repository_state.entries.size());
  for (const GitRepositoryEntry& entry : repository_state.entries) {
    StageFlags& f = flags[entry.path.relative_path.generic_string()];
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
  // Bound the numstat summary: a rename consumes up to 3 NUL fields per retained file,
  // so materialize at most 3x the entry cap (+ slack) and retain at most the cap. This
  // stops a hostile/huge staged diff from splitting millions of records for a summary
  // that only needs a bounded file list. (TD-2026-07-16-30.)
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
      path_text = fields[i + 2];  // rename: [i+1]=old, [i+2]=new
      i += 2;
    }
    // Parse in 64-bit and saturate into the int field. A very large (generated or
    // hostile) numstat count exceeds INT_MAX; `ParseInt` returned nullopt for
    // those, silently under-reporting the change as 0. Saturating keeps the
    // summary monotonic and non-zero.
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
    file_summary.relative_path = std::filesystem::path(path_text);
    // Accumulate in 64-bit and clamp: two files each clamped to INT_MAX would overflow
    // a plain `int` aggregate (UB / a wrapped negative "+N" in the sidebar). Both
    // operands are already in [0, INT_MAX], so the running sum stays well within
    // int64 across any realistic file count before the final clamp. (TD-2026-07-16-47.)
    added_total += file_summary.added_lines;
    deleted_total += file_summary.deleted_lines;
    summary.files.push_back(std::move(file_summary));
  }
  summary.added_lines =
      static_cast<int>(std::min<std::int64_t>(added_total, std::numeric_limits<int>::max()));
  summary.deleted_lines =
      static_cast<int>(std::min<std::int64_t>(deleted_total, std::numeric_limits<int>::max()));

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

std::optional<bool> StagedDiffContainsConflictMarkers(const std::filesystem::path& repository_root) {
  if (repository_root.empty()) {
    return false;
  }
  GitRepository repo(repository_root);
  const GitRepository::CommandResult diff = repo.Execute({"diff", "--cached"}, true);
  if (!diff.success()) {
    return std::nullopt;  // could not determine (e.g. locked index) — not "clean"
  }
  return util::StagedDiffIntroducesConflictMarker(diff.output);
}

std::vector<CommitPreCheck> RunCommitPreChecks(
    const GitRepositoryState& repository_state,
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

  // Reuse the caller's summary when supplied; otherwise run the staged-diff
  // subprocess. Hold a *reference* to the caller's summary rather than copying it:
  // the commit workflow passes the same generation-cached summary on open, warning
  // ack, and every subject/body keystroke, and a repo with thousands of staged
  // paths would otherwise deep-copy the whole `files` vector on each edit. Only
  // materialize an owned summary when this function has to build it itself.
  CommitStagedSummary owned_summary;
  const CommitStagedSummary* summary_view = precomputed_summary;
  if (summary_view == nullptr) {
    owned_summary = BuildCommitStagedSummary(repository_state);
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
    // Count Unicode scalar values, not bytes: a non-ASCII subject (accented or
    // CJK text) has more bytes than visible characters, so a byte-length gate
    // would reject a subject the user sees as well within the limit.
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

  if (scan_staged_diff_for_conflict_markers) {
    const std::optional<bool> has_markers =
        StagedDiffContainsConflictMarkers(repository_state.repository_root);
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
    if (partially_staged.count(file.relative_path.generic_string()) != 0) {
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

}  // namespace microide::project
