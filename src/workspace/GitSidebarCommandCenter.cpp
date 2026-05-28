#include "workspace/GitSidebarCommandCenter.h"

#include <algorithm>
#include <array>

#include "compare/BranchReviewStateTypes.h"
#include "workspace/BranchReviewStateBridge.h"
#include "workspace/WorkspaceUiText.h"
#include "workspace/WorkspaceGitSidebarPresentation.h"

namespace microide::workspace {

namespace {

constexpr std::string_view kSectionTitleConflicts = "Conflicts";
constexpr std::string_view kSectionTitleStaged = "Staged";
constexpr std::string_view kSectionTitleChanged = "Unstaged";
constexpr std::string_view kSectionTitleUntracked = "Untracked";
constexpr std::string_view kSectionTitleOutgoing = "Outgoing";

struct GitSidebarCounts {
  std::size_t conflicts = 0;
  std::size_t staged = 0;
  std::size_t changed = 0;
  std::size_t untracked = 0;
  std::size_t outgoing = 0;
};

void AppendHintSegment(std::string& line, std::string_view segment) {
  if (segment.empty()) {
    return;
  }
  if (!line.empty()) {
    line += "  |  ";
  }
  line.append(segment.data(), segment.size());
}

GitSidebarCounts CountEntriesBySection(const GitSidebarState& git_state) {
  GitSidebarCounts counts;
  for (const GitSidebarEntry& entry : git_state.entries) {
    switch (entry.section) {
      case GitSidebarEntry::Section::Conflicts:
        ++counts.conflicts;
        break;
      case GitSidebarEntry::Section::Staged:
        ++counts.staged;
        break;
      case GitSidebarEntry::Section::Changed:
        ++counts.changed;
        break;
      case GitSidebarEntry::Section::Untracked:
        ++counts.untracked;
        break;
      case GitSidebarEntry::Section::Outgoing:
        ++counts.outgoing;
        break;
    }
  }
  return counts;
}

std::string_view SectionTitle(GitSidebarEntry::Section section) {
  switch (section) {
    case GitSidebarEntry::Section::Conflicts:
      return kSectionTitleConflicts;
    case GitSidebarEntry::Section::Staged:
      return kSectionTitleStaged;
    case GitSidebarEntry::Section::Changed:
      return kSectionTitleChanged;
    case GitSidebarEntry::Section::Untracked:
      return kSectionTitleUntracked;
    case GitSidebarEntry::Section::Outgoing:
      return kSectionTitleOutgoing;
  }
  return {};
}

std::string GitSidebarPrimaryActionLabel(const GitSidebarEntry& entry,
                                         const GitSidebarActionAvailability& actions) {
  if (actions.merge) {
    return "merge resolver";
  }
  if (actions.diff) {
    return entry.section == GitSidebarEntry::Section::Outgoing ? "outgoing diff"
                                                               : "diff review";
  }
  if (actions.open_file) {
    return "open file";
  }
  return "inspect";
}

std::string GitSidebarWorkflowSummaryLine(const GitSidebarState& git_state,
                                          const GitSidebarCounts& counts) {
  if (!git_state.repo_available) {
    return "Open a Git repository to review changes.";
  }

  std::string line;
  line.reserve(96);
  if (counts.conflicts > 0) {
    AppendUnsigned(line, counts.conflicts);
    line += counts.conflicts == 1 ? " conflict" : " conflicts";
  }
  if (counts.staged > 0) {
    if (!line.empty()) {
      line += "  ·  ";
    }
    AppendUnsigned(line, counts.staged);
    line += counts.staged == 1 ? " staged" : " staged";
  }
  if (counts.changed > 0) {
    if (!line.empty()) {
      line += "  ·  ";
    }
    AppendUnsigned(line, counts.changed);
    line += counts.changed == 1 ? " unstaged" : " unstaged";
  }
  if (counts.untracked > 0) {
    if (!line.empty()) {
      line += "  ·  ";
    }
    AppendUnsigned(line, counts.untracked);
    line += counts.untracked == 1 ? " untracked" : " untracked";
  }
  if (counts.outgoing > 0) {
    if (!line.empty()) {
      line += "  ·  ";
    }
    AppendUnsigned(line, counts.outgoing);
    line += counts.outgoing == 1 ? " outgoing" : " outgoing";
  }
  if (line.empty()) {
    line = "Working tree clean.";
  }
  return line;
}

std::string GitSidebarCommitSummaryLine(const GitSidebarState& git_state,
                                        const GitSidebarCounts& counts) {
  if (!git_state.repo_available) {
    return {};
  }
  if (git_state.commit_workflow.operation_in_flight) {
    return "Commit in progress...";
  }
  if (git_state.commit_workflow.open) {
    return "Commit draft open  |  Ctrl+Enter commit  |  Esc close";
  }
  if (counts.conflicts > 0) {
    return counts.conflicts == 1 ? "Commit blocked  |  resolve the remaining conflict"
                                 : "Commit blocked  |  resolve conflicts first";
  }
  if (counts.staged == 0) {
    return "Commit blocked  |  nothing staged";
  }
  return "Commit ready  |  c commit";
}

std::string GitSidebarSelectionSummaryLine(const GitSidebarEntry& entry,
                                           std::string_view primary_action_label) {
  const std::string path_label =
      entry.relative_path.empty() ? entry.path.generic_string() : entry.relative_path.generic_string();
  std::string line = "Selected: ";
  line.append(primary_action_label.data(), primary_action_label.size());
  if (!path_label.empty()) {
    line += "  ·  ";
    line += path_label;
  }
  return line;
}

std::string GitSidebarSelectionActionLine(const GitSidebarEntry& entry,
                                          const GitSidebarActionAvailability& actions,
                                          bool commit_ready) {
  std::string line;
  if (actions.default_view) {
    AppendHintSegment(line, "Enter default");
  }
  if (actions.diff) {
    AppendHintSegment(line, "d diff");
  }
  if (actions.stage) {
    AppendHintSegment(line, "s stage");
  }
  if (actions.unstage) {
    AppendHintSegment(line, "u unstage");
  }
  if (actions.discard) {
    AppendHintSegment(line, "x discard");
  }
  if (actions.merge) {
    AppendHintSegment(line, "m merge");
  }
  if (actions.open_file) {
    AppendHintSegment(line, "o open");
  }
  if (commit_ready && entry.section != GitSidebarEntry::Section::Outgoing) {
    AppendHintSegment(line, "c commit");
  }
  AppendHintSegment(line, "r refresh");
  return line;
}

std::string EmptySectionLabel(GitSidebarEntry::Section section,
                              bool git_repo_available,
                              bool refreshing,
                              std::string_view git_base_ref) {
  switch (section) {
    case GitSidebarEntry::Section::Conflicts:
      return "No merge conflicts";
    case GitSidebarEntry::Section::Staged:
      return "No staged changes";
    case GitSidebarEntry::Section::Changed:
      (void)refreshing;
      return git_repo_available ? "No unstaged changes" : "Not a git repository";
    case GitSidebarEntry::Section::Untracked:
      return "No untracked files";
    case GitSidebarEntry::Section::Outgoing:
      return git_base_ref.empty() ? "Base branch unavailable" : "No outgoing files";
  }
  return {};
}

}  // namespace

GitSidebarEntry::Section ClassifyGitSidebarSection(bool conflicted,
                                                   bool staged,
                                                   project::GitFileStatus status) {
  if (conflicted || status == project::GitFileStatus::Conflicted) {
    return GitSidebarEntry::Section::Conflicts;
  }
  if (status == project::GitFileStatus::Untracked) {
    return GitSidebarEntry::Section::Untracked;
  }
  if (staged) {
    return GitSidebarEntry::Section::Staged;
  }
  return GitSidebarEntry::Section::Changed;
}

GitSidebarRowKind RowKindFromSection(GitSidebarEntry::Section section) {
  switch (section) {
    case GitSidebarEntry::Section::Conflicts:
      return GitSidebarRowKind::Conflict;
    case GitSidebarEntry::Section::Staged:
      return GitSidebarRowKind::Staged;
    case GitSidebarEntry::Section::Changed:
      return GitSidebarRowKind::Changed;
    case GitSidebarEntry::Section::Untracked:
      return GitSidebarRowKind::Untracked;
    case GitSidebarEntry::Section::Outgoing:
      return GitSidebarRowKind::Outgoing;
  }
  return GitSidebarRowKind::Changed;
}

bool IsGitWorkflowSection(GitSidebarEntry::Section section) {
  return section != GitSidebarEntry::Section::Outgoing;
}

GitSidebarActionAvailability GitSidebarActionAvailabilityForEntry(
    const GitSidebarEntry& entry,
    const bool repo_available,
    const bool supports_mutations) {
  GitSidebarActionAvailability availability;
  if (!repo_available || !supports_mutations) {
    return availability;
  }

  const GitSidebarRowKind row_kind = RowKindFromSection(entry.section);
  switch (row_kind) {
    case GitSidebarRowKind::Conflict:
      availability.default_view = true;
      availability.merge = true;
      availability.diff = true;
      availability.stage = false;
      availability.unstage = false;
      availability.discard = true;
      availability.open_file = true;
      break;
    case GitSidebarRowKind::Staged:
      availability.default_view = true;
      availability.diff = true;
      availability.unstage = true;
      availability.discard = true;
      availability.commit = true;
      availability.open_file = true;
      break;
    case GitSidebarRowKind::Changed:
      availability.default_view = true;
      availability.diff = true;
      availability.stage = true;
      availability.discard = true;
      availability.open_file = true;
      break;
    case GitSidebarRowKind::Untracked:
      availability.default_view = true;
      availability.stage = true;
      availability.discard = true;
      availability.open_file = true;
      break;
    case GitSidebarRowKind::Outgoing:
      availability.default_view = true;
      availability.diff = true;
      break;
  }
  return availability;
}

std::string GitSidebarDisabledActionMessage(const GitSidebarActionId action,
                                            const GitSidebarEntry& entry,
                                            const bool repo_available,
                                            const bool supports_mutations) {
  if (!repo_available) {
    return "Not a git repository";
  }
  if (!supports_mutations) {
    return "Git mutations are unavailable for this provider";
  }

  const GitSidebarActionAvailability availability =
      GitSidebarActionAvailabilityForEntry(entry, repo_available, supports_mutations);
  switch (action) {
    case GitSidebarActionId::DefaultView:
      return availability.default_view ? std::string{} : "No default view for this row";
    case GitSidebarActionId::Diff:
      return availability.diff ? std::string{} : "Diff is unavailable for this row";
    case GitSidebarActionId::Stage:
      return availability.stage ? std::string{} : "Stage is unavailable for this row";
    case GitSidebarActionId::Unstage:
      return availability.unstage ? std::string{} : "Unstage is unavailable for this row";
    case GitSidebarActionId::Discard:
      return availability.discard ? std::string{} : "Discard is unavailable for this row";
    case GitSidebarActionId::Merge:
      return availability.merge ? std::string{} : "Merge resolver is unavailable for this row";
    case GitSidebarActionId::Commit:
      return availability.commit ? std::string{}
                                 : "Commit is only available when staged changes exist";
    case GitSidebarActionId::Refresh:
      return {};
    case GitSidebarActionId::OpenFile:
      return availability.open_file ? std::string{} : "Working-tree file is unavailable";
  }
  return {};
}

std::string BuildGitBranchSummaryLine(const std::string_view branch_label,
                                      const std::string_view upstream_label,
                                      const int ahead,
                                      const int behind,
                                      const bool repo_available) {
  if (!repo_available) {
    return "Not a git repository";
  }
  std::string summary = branch_label.empty() ? "HEAD" : std::string(branch_label);
  if (!upstream_label.empty()) {
    summary += " → ";
    summary += upstream_label;
  }
  if (ahead > 0 || behind > 0) {
    summary += " ↑";
    summary += std::to_string(ahead);
    summary += " ↓";
    summary += std::to_string(behind);
  }
  return summary;
}

std::string BuildGitStaleBanner(const bool snapshot_stale, const bool refreshing) {
  if (!snapshot_stale || refreshing) {
    return {};
  }
  return "Repository snapshot is stale — refresh pending";
}

std::string BuildGitRefreshErrorBanner(const std::string_view refresh_error) {
  if (refresh_error.empty()) {
    return {};
  }
  return std::string("Git refresh failed: ") + std::string(refresh_error);
}

std::string BuildGitDiscardPreviewSummary(const GitSidebarEntry& entry,
                                          const std::string_view project_label) {
  const std::string path_label = entry.relative_path.empty()
                                     ? entry.path.filename().string()
                                     : entry.relative_path.generic_string();
  switch (entry.section) {
    case GitSidebarEntry::Section::Untracked:
      return "Remove untracked file " + path_label +
             " from the worktree? Existing file-operation policy applies (trash when configured).";
    case GitSidebarEntry::Section::Staged:
      return "Discard staged changes for " + path_label +
             "? This unstages and restores the index/worktree state for that path.";
    case GitSidebarEntry::Section::Conflicts:
      return "Discard conflicted changes for " + path_label +
             "? Resolve or back up conflict markers before confirming.";
    case GitSidebarEntry::Section::Changed:
      return "Discard unstaged changes for " + path_label +
             "? A working-tree diff preview is shown when available.";
    case GitSidebarEntry::Section::Outgoing:
      return "Outgoing entries cannot be discarded from the Git sidebar.";
  }
  if (!project_label.empty()) {
    return "Discard changes for " + path_label + " in " + std::string(project_label) + "?";
  }
  return "Discard changes for " + path_label + "?";
}

GitSidebarViewModel BuildGitSidebarViewModel(
    const GitSidebarState& git_state,
    const std::filesystem::path& repository_root,
    const compare::BranchReviewStateService& branch_review) {
  const std::optional<compare::BranchReviewTargetIdentity> review_target =
      OutgoingBranchReviewTarget(git_state, repository_root);
  GitSidebarViewModel view_model;
  view_model.refreshing = git_state.refreshing;
  view_model.stale_banner = BuildGitStaleBanner(git_state.snapshot_stale, git_state.refreshing);
  view_model.error_banner =
      git_state.error.empty() ? BuildGitRefreshErrorBanner(git_state.refresh_error)
                              : "Git: " + git_state.error;
  const GitSidebarCounts counts = CountEntriesBySection(git_state);
  const bool commit_ready = git_state.repo_available && counts.conflicts == 0 && counts.staged > 0 &&
                            !git_state.commit_workflow.open &&
                            !git_state.commit_workflow.operation_in_flight;

  view_model.summary_lines.push_back(
      BuildGitBranchSummaryLine(git_state.branch_label, git_state.upstream_label, git_state.ahead,
                                git_state.behind, git_state.repo_available));
  if (!view_model.stale_banner.empty()) {
    view_model.summary_lines.push_back(view_model.stale_banner);
  }
  if (!view_model.error_banner.empty()) {
    view_model.summary_lines.push_back(view_model.error_banner);
  }
  view_model.workflow_summary_line = GitSidebarWorkflowSummaryLine(git_state, counts);
  view_model.commit_summary_line = GitSidebarCommitSummaryLine(git_state, counts);

  const std::array<GitSidebarEntry::Section, 5> section_order = {
      GitSidebarEntry::Section::Conflicts,
      GitSidebarEntry::Section::Staged,
      GitSidebarEntry::Section::Changed,
      GitSidebarEntry::Section::Untracked,
      GitSidebarEntry::Section::Outgoing,
  };

  for (const GitSidebarEntry::Section section : section_order) {
    GitSidebarSectionViewModel section_vm{
        .section = section,
        .header_label = {},
        .empty_label = EmptySectionLabel(section, git_state.repo_available, git_state.refreshing,
                                         git_state.base_ref),
        .rows = {},
    };

    std::size_t count = 0;
    for (std::size_t i = 0; i < git_state.entries.size(); ++i) {
      if (git_state.entries[i].section != section) {
        continue;
      }
      ++count;
      const GitSidebarEntry& entry = git_state.entries[i];
      const GitSidebarEntryTextModel text_model =
          BuildGitSidebarEntryTextModel(entry.relative_path, entry.section == GitSidebarEntry::Section::Staged);
      const GitSidebarActionAvailability actions = GitSidebarActionAvailabilityForEntry(
          entry, git_state.repo_available, git_state.supports_mutations);
      std::string review_marker_label;
      if (review_target.has_value() && entry.section == GitSidebarEntry::Section::Outgoing) {
        const compare::BranchReviewStateQueryInput query{
            .target = *review_target,
            .path = entry.relative_path,
        };
        review_marker_label =
            compare::BranchReviewMarkerLabel(branch_review.FileStatus(query));
      }
      section_vm.rows.push_back(GitSidebarRowViewModel{
          .entry_index = static_cast<int>(i),
          .row_kind = RowKindFromSection(entry.section),
          .relative_path = entry.relative_path,
          .primary_label = text_model.primary_label,
          .secondary_label = text_model.secondary_label,
          .review_marker_label = std::move(review_marker_label),
          .primary_action_label = GitSidebarPrimaryActionLabel(entry, actions),
          .status = entry.status,
          .actions = actions,
          .show_stage_button = actions.stage || actions.unstage,
          .show_discard_button = actions.discard,
      });
    }

    section_vm.header_label = std::string(SectionTitle(section)) + " (" + std::to_string(count) + ")";
    if (section == GitSidebarEntry::Section::Outgoing && !git_state.base_label.empty()) {
      section_vm.header_label += "  ";
      section_vm.header_label += git_state.base_label;
    }

    view_model.sections.push_back(std::move(section_vm));
  }

  if (git_state.selected_index < git_state.entries.size()) {
    const GitSidebarEntry& selected_entry = git_state.entries[git_state.selected_index];
    const GitSidebarActionAvailability actions = GitSidebarActionAvailabilityForEntry(
        selected_entry, git_state.repo_available, git_state.supports_mutations);
    const std::string primary_action_label = GitSidebarPrimaryActionLabel(selected_entry, actions);
    view_model.selection_summary_line =
        GitSidebarSelectionSummaryLine(selected_entry, primary_action_label);
    view_model.selection_action_line =
        GitSidebarSelectionActionLine(selected_entry, actions, commit_ready);
  } else if (git_state.repo_available) {
    view_model.selection_summary_line = "Select a file to review or mutate.";
    view_model.selection_action_line = "Enter default  |  r refresh";
  }

  return view_model;
}

}  // namespace microide::workspace
