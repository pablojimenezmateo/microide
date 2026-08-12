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
  // Normalized generic ('/'-separated) text; the presentation layer's grouping keys
  // are views into it (TD-2026-08-11-183).
  std::string relative_path;
  std::string primary_label;
  std::string secondary_label;
  std::string review_marker_label;
  std::string primary_action_label;
  project::GitFileStatus status = project::GitFileStatus::Clean;
  GitSidebarActionAvailability actions{};
  bool show_stage_button = false;
  bool show_discard_button = false;
};

struct GitSidebarSectionViewModel {
  GitSidebarEntry::Section section = GitSidebarEntry::Section::Changed;
  std::string header_label;
  std::string empty_label;
  // False for the two whole-panel states ("Not a git repository", "No changes"),
  // which are a single line of prose rather than a group with a count above it.
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
  // Commit-button state for the panel header. commit_ready drives the enabled state;
  // commit_blocked_reason is a short label shown beside a disabled button (empty when
  // ready). show_commit_button is false when there is no repo or the inline draft is open.
  bool show_commit_button = false;
  bool commit_ready = false;
  std::string commit_blocked_reason;
  bool refreshing = false;
  // Labels for the branch row's two buttons, composed here because the render TU
  // may not materialize strings. `branch_button_label` is the current branch (or a
  // prompt when HEAD is detached/unborn); `sync_button_label` carries the
  // ahead/behind counts so the button says what syncing would actually do.
  std::string branch_button_label;
  std::string sync_button_label;
  std::string sync_button_tooltip;
  std::vector<GitSidebarSectionViewModel> sections;
};

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

}  // namespace microide::workspace
