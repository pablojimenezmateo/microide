#include "workspace/GitSidebarCommandCenter.h"

#include <algorithm>

#include "compare/BranchReviewStateTypes.h"
#include "workspace/BranchReviewStateBridge.h"
#include "workspace/WorkspaceGitSidebarPresentation.h"

namespace microide::workspace {

namespace {

constexpr std::string_view kSectionTitleConflicts = "Conflicts";
constexpr std::string_view kSectionTitleStaged = "Staged";
constexpr std::string_view kSectionTitleChanged = "Changed";
constexpr std::string_view kSectionTitleUntracked = "Untracked";
constexpr std::string_view kSectionTitleOutgoing = "Outgoing";

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

std::string EmptySectionLabel(GitSidebarEntry::Section section,
                              bool git_repo_available,
                              bool refreshing,
                              std::string_view git_base_ref) {
  switch (section) {
    case GitSidebarEntry::Section::Conflicts:
      return "No merge conflicts";
    case GitSidebarEntry::Section::Staged:
      return "Nothing staged";
    case GitSidebarEntry::Section::Changed:
      if (refreshing) {
        return "Refreshing git status...";
      }
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
      availability.stage = !entry.staged;
      availability.unstage = entry.staged;
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
  if (!snapshot_stale) {
    return {};
  }
  return refreshing ? "Refreshing repository snapshot..." : "Repository snapshot is stale — refresh pending";
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

  view_model.summary_lines.push_back(
      BuildGitBranchSummaryLine(git_state.branch_label, git_state.upstream_label, git_state.ahead,
                                git_state.behind, git_state.repo_available));
  if (!view_model.stale_banner.empty()) {
    view_model.summary_lines.push_back(view_model.stale_banner);
  }
  if (!view_model.error_banner.empty()) {
    view_model.summary_lines.push_back(view_model.error_banner);
  }

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
          .primary_label = text_model.primary_label,
          .secondary_label = text_model.secondary_label,
          .review_marker_label = std::move(review_marker_label),
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

  return view_model;
}

}  // namespace microide::workspace
