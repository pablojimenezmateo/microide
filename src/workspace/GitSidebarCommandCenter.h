#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "project/GitStatusService.h"
#include "workspace/WorkspaceSidebarState.h"

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
  std::string primary_label;
  std::string secondary_label;
  project::GitFileStatus status = project::GitFileStatus::Clean;
  GitSidebarActionAvailability actions{};
  bool show_stage_button = false;
  bool show_discard_button = false;
};

struct GitSidebarSectionViewModel {
  GitSidebarEntry::Section section = GitSidebarEntry::Section::Changed;
  std::string header_label;
  std::string empty_label;
  std::vector<GitSidebarRowViewModel> rows;
};

struct GitSidebarViewModel {
  std::vector<std::string> summary_lines;
  std::string stale_banner;
  std::string error_banner;
  bool refreshing = false;
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

std::string BuildGitBranchSummaryLine(std::string_view branch_label,
                                      std::string_view upstream_label,
                                      int ahead,
                                      int behind,
                                      bool repo_available);

std::string BuildGitStaleBanner(bool snapshot_stale, bool refreshing);

std::string BuildGitRefreshErrorBanner(std::string_view refresh_error);

GitSidebarViewModel BuildGitSidebarViewModel(const GitSidebarState& git_state);

std::string BuildGitDiscardPreviewSummary(const GitSidebarEntry& entry,
                                          std::string_view project_label);

}  // namespace microide::workspace
