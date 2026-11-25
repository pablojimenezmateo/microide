#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "compare/BranchReviewStateService.h"
#include "project/GitStatusService.h"
#include "workspace/state/WorkspaceSidebarState.h"

namespace microide::workspace {

enum class GitSidebarActionId {
  DefaultView,
  Diff,
  Stage,
  Unstage,
  Discard,
  Merge,
  Commit,
  Refresh,
  OpenFile,
};

enum class GitSidebarRowKind {
  Conflict,
  Staged,
  Changed,
  Untracked,
  Outgoing,
};

struct GitSidebarActionAvailability {
  bool default_view = false;
  bool diff = false;
  bool stage = false;
  bool unstage = false;
  bool discard = false;
  bool merge = false;
  bool commit = false;
  bool open_file = false;
};

struct GitSidebarRowViewModel {
  int entry_index = -1;
  GitSidebarRowKind row_kind = GitSidebarRowKind::Changed;
  std::string relative_path;
  std::string primary_label;
  std::string secondary_label;
  std::string review_marker_label;
  std::string primary_action_label;
  project::GitFileStatus status = project::GitFileStatus::Clean;
};

struct GitSidebarSectionViewModel {
  GitSidebarEntry::Section section = GitSidebarEntry::Section::Changed;
  std::string header_label;
  std::string empty_label;
  bool show_header = true;
  std::vector<GitSidebarRowViewModel> rows;
};

struct GitSidebarViewModel {
  std::vector<std::string> summary_lines;
  std::string workflow_summary_line;
  std::string commit_summary_line;
  std::string selection_summary_line;
  std::string selection_action_line;
  std::string stale_banner;
  std::string error_banner;
  bool show_commit_button = false;
  bool commit_ready = false;
  std::string commit_blocked_reason;
  bool refreshing = false;
  std::string branch_button_label;
  std::string sync_button_label;
  std::vector<GitSidebarSectionViewModel> sections;
};

std::string BuildGitSyncButtonTooltip(const GitSidebarState& git_state);

GitSidebarEntry::Section ClassifyGitSidebarSection(bool conflicted,
                                                   bool staged,
                                                   project::GitFileStatus status);

GitSidebarRowKind RowKindFromSection(GitSidebarEntry::Section section);

bool IsGitWorkflowSection(GitSidebarEntry::Section section);

GitSidebarActionAvailability GitSidebarActionAvailabilityForEntry(
    const GitSidebarEntry& entry,
    bool repo_available,
    bool supports_mutations);

std::string GitSidebarDisabledActionMessage(GitSidebarActionId action,
                                            const GitSidebarEntry& entry,
                                            bool repo_available,
                                            bool supports_mutations);

std::string BuildGitStaleBanner(bool snapshot_stale, bool refreshing);

std::string BuildGitRefreshErrorBanner(std::string_view refresh_error);

GitSidebarViewModel BuildGitSidebarViewModel(const GitSidebarState& git_state,
                                             const std::filesystem::path& repository_root,
                                             const compare::BranchReviewStateService& branch_review);

std::string BuildGitDiscardPreviewSummary(const GitSidebarEntry& entry,
                                          std::string_view project_label);

}
