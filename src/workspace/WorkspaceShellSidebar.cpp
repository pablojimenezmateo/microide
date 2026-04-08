#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "project/GitStatusService.h"
#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

constexpr float kSidebarRowHeight = 20.0f;

}  // namespace

void WorkspaceShell::ShowSidebarMode(SidebarMode mode, bool temporary) {
  if (mode == SidebarMode::None) {
    CloseSidebar();
    return;
  }
  if (mode != SidebarMode::Tree) {
    CloseTreeContextMenu();
  }

  if (sidebar_mode_ == SidebarMode::Search && mode != SidebarMode::Search) {
    StopProjectSearch();
  }

  if (temporary) {
    if (!sidebar_temporary_ && sidebar_visible_) {
      sidebar_prev_mode_ = sidebar_mode_;
    }
  } else {
    sidebar_prev_mode_ = SidebarMode::None;
  }

  sidebar_mode_ = mode;
  sidebar_temporary_ = temporary;
  sidebar_visible_ = true;
  focus_ = FocusTarget::Sidebar;
  sidebar_scroll_row_ = 0;
}

void WorkspaceShell::ShowTreeSidebar(const std::filesystem::path& root) {
  if (!root.empty()) {
    if (!OpenProjectTab(root, true, true)) {
      return;
    }
  }

  ShowSidebarMode(SidebarMode::Tree, false);
}

void WorkspaceShell::ShowSearchSidebar(std::string query, bool temporary) {
  project_search_query_ = std::move(query);
  project_search_edit_buffer_ = project_search_query_;
  project_search_editing_ = project_search_query_.empty();
  project_search_edit_field_ = ProjectSearchEditField::Query;
  project_search_selected_index_ = 0;
  RefreshProjectSearch();
  ShowSidebarMode(SidebarMode::Search, temporary);
  LogMessage(temporary ? "Temporary project search opened" : "Project search sidebar opened");
}

void WorkspaceShell::ShowGitSidebar() {
  RefreshGitSidebar();
  ShowSidebarMode(SidebarMode::Git, false);
  RevealSelectedGitSidebarLine();
  LogMessage("Git sidebar opened");
}

void WorkspaceShell::CloseSidebar() {
  if (sidebar_mode_ == SidebarMode::Search) {
    StopProjectSearch();
  }
  CloseTreeContextMenu();

  if (sidebar_temporary_ && sidebar_prev_mode_ != SidebarMode::None) {
    RestorePreviousSidebar();
    LogMessage("Previous sidebar restored");
    return;
  }

  sidebar_visible_ = false;
  sidebar_temporary_ = false;
  sidebar_prev_mode_ = SidebarMode::None;
  if (focus_ == FocusTarget::Sidebar) {
    focus_ = FocusTarget::Editor;
  }
  LogMessage("Sidebar closed");
}

void WorkspaceShell::ToggleSidebar() {
  if (sidebar_visible_) {
    CloseSidebar();
    return;
  }

  if (sidebar_mode_ == SidebarMode::None) {
    sidebar_mode_ = SidebarMode::Tree;
  }
  sidebar_visible_ = true;
  sidebar_temporary_ = false;
  focus_ = FocusTarget::Sidebar;
  LogMessage("Sidebar shown");
}

void WorkspaceShell::RestorePreviousSidebar() {
  if (sidebar_mode_ == SidebarMode::Search && sidebar_prev_mode_ != SidebarMode::Search) {
    StopProjectSearch();
  }

  if (sidebar_prev_mode_ == SidebarMode::None) {
    sidebar_temporary_ = false;
    return;
  }

  sidebar_mode_ = sidebar_prev_mode_;
  sidebar_prev_mode_ = SidebarMode::None;
  sidebar_temporary_ = false;
  sidebar_visible_ = true;
  focus_ = FocusTarget::Sidebar;
  sidebar_scroll_row_ = 0;
}

void WorkspaceShell::RefreshProjectFiles() {
  directory_tree_.Refresh();
  file_index_.Refresh();
  file_finder_.SetIndex(&file_index_);
  RefreshGitSidebar();
}

void WorkspaceShell::RefreshGitSidebar() {
  const std::filesystem::path previous_path =
      git_sidebar_selected_index_ < git_sidebar_entries_.size()
          ? git_sidebar_entries_[git_sidebar_selected_index_].path
          : std::filesystem::path{};
  const GitSidebarEntry::Section previous_section =
      git_sidebar_selected_index_ < git_sidebar_entries_.size()
          ? git_sidebar_entries_[git_sidebar_selected_index_].section
          : GitSidebarEntry::Section::Modified;

  git_sidebar_entries_.clear();
  git_base_ref_.clear();
  git_base_label_.clear();
  git_repo_available_ = false;
  git_sidebar_selected_index_ = 0;
  if (project_root_.empty()) {
    return;
  }

  const auto working_entries = project::CollectGitWorkingTreeEntries(project_root_);
  for (const auto& entry : working_entries) {
    git_sidebar_entries_.push_back(GitSidebarEntry{
        .section = GitSidebarEntry::Section::Modified,
        .path = (project_root_ / entry.relative_path).lexically_normal(),
        .relative_path = entry.relative_path,
        .status = entry.conflicted ? project::GitFileStatus::Conflicted : entry.status,
        .conflicted = entry.conflicted,
        .staged = entry.staged,
    });
  }

  const auto base_ref = project::ResolveGitBaseReference(project_root_);
  if (base_ref.has_value()) {
    git_repo_available_ = true;
    git_base_ref_ = base_ref->ref;
    git_base_label_ = base_ref->label;
    const auto outgoing_entries =
        project::CollectGitBranchOutgoingFiles(project_root_, git_base_ref_);
    for (const auto& entry : outgoing_entries) {
      git_sidebar_entries_.push_back(GitSidebarEntry{
          .section = GitSidebarEntry::Section::Outgoing,
          .path = (project_root_ / entry.relative_path).lexically_normal(),
          .relative_path = entry.relative_path,
          .status = entry.status,
      });
    }
  } else {
    git_repo_available_ = std::filesystem::exists(project_root_ / ".git");
  }

  for (std::size_t i = 0; i < git_sidebar_entries_.size(); ++i) {
    if (git_sidebar_entries_[i].path == previous_path &&
        git_sidebar_entries_[i].section == previous_section) {
      git_sidebar_selected_index_ = i;
      RevealSelectedGitSidebarLine();
      return;
    }
  }

  RevealSelectedGitSidebarLine();
}

SDL_FRect WorkspaceShell::GitSidebarRefreshButtonRect(const SDL_FRect& sidebar_rect) const {
  if (sidebar_rect.w <= 0.0f || sidebar_rect.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }

  const float button_width = std::max(72.0f, text_renderer_.MeasureWidth("Refresh") + 18.0f);
  return MakeRect(sidebar_rect.x + sidebar_rect.w - 10.0f - button_width, sidebar_rect.y + 4.0f,
                  button_width, 22.0f);
}

std::vector<WorkspaceShell::GitSidebarLine> WorkspaceShell::BuildGitSidebarLines() const {
  std::vector<GitSidebarSection> sections;
  sections.reserve(git_sidebar_entries_.size());
  for (const auto& entry : git_sidebar_entries_) {
    sections.push_back(entry.section == GitSidebarEntry::Section::Modified
                           ? GitSidebarSection::Modified
                           : GitSidebarSection::Outgoing);
  }

  const auto specs =
      BuildGitSidebarLineSpecs(sections, git_repo_available_, git_base_ref_, git_base_label_);
  std::vector<GitSidebarLine> lines;
  lines.reserve(specs.size());
  for (const GitSidebarLineSpec& spec : specs) {
    lines.push_back(GitSidebarLine{
        .kind = spec.kind == GitSidebarLineKind::Header
                    ? GitSidebarLine::Kind::Header
                    : spec.kind == GitSidebarLineKind::Entry ? GitSidebarLine::Kind::Entry
                                                             : GitSidebarLine::Kind::Empty,
        .section = spec.section == GitSidebarSection::Modified
                       ? GitSidebarEntry::Section::Modified
                       : GitSidebarEntry::Section::Outgoing,
        .label = spec.label,
        .entry_index = spec.entry_index,
    });
  }
  return lines;
}

std::optional<std::size_t> WorkspaceShell::SelectedGitSidebarLineIndex() const {
  if (git_sidebar_selected_index_ >= git_sidebar_entries_.size()) {
    return std::nullopt;
  }

  std::vector<GitSidebarSection> sections;
  sections.reserve(git_sidebar_entries_.size());
  for (const auto& entry : git_sidebar_entries_) {
    sections.push_back(entry.section == GitSidebarEntry::Section::Modified
                           ? GitSidebarSection::Modified
                           : GitSidebarSection::Outgoing);
  }
  const auto specs =
      BuildGitSidebarLineSpecs(sections, git_repo_available_, git_base_ref_, git_base_label_);
  return FindSelectedGitSidebarLineIndex(specs, git_sidebar_selected_index_);
}

const WorkspaceShell::GitSidebarEntry* WorkspaceShell::SelectedGitSidebarEntry() const {
  if (git_sidebar_selected_index_ >= git_sidebar_entries_.size()) {
    return nullptr;
  }
  return &git_sidebar_entries_[git_sidebar_selected_index_];
}

void WorkspaceShell::RevealSelectedGitSidebarLine() {
  if (last_window_width_ <= 0 || last_window_height_ <= 0) {
    return;
  }

  const auto selected_line = SelectedGitSidebarLineIndex();
  if (!selected_line.has_value()) {
    return;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
  if (layout.sidebar.h <= 0.0f) {
    return;
  }
  const float visible_units = std::max(1.0f, (layout.sidebar.h - 36.0f) / kSidebarRowHeight);
  const int visible_rows = std::max(1, static_cast<int>(std::floor(visible_units)));
  const int max_scroll = std::max(
      0, static_cast<int>(std::ceil(static_cast<float>(BuildGitSidebarLines().size()) - visible_units)));
  int scroll_row = std::clamp(sidebar_scroll_row_, 0, max_scroll);
  if (*selected_line < static_cast<std::size_t>(scroll_row)) {
    scroll_row = static_cast<int>(*selected_line);
  } else if (*selected_line >= static_cast<std::size_t>(scroll_row + visible_rows)) {
    scroll_row = static_cast<int>(*selected_line) - visible_rows + 1;
  }
  sidebar_scroll_row_ = std::clamp(scroll_row, 0, max_scroll);
}

void WorkspaceShell::MoveGitSidebarSelection(int delta) {
  if (git_sidebar_entries_.empty() || delta == 0) {
    return;
  }
  const int current = static_cast<int>(git_sidebar_selected_index_);
  const int max_index = static_cast<int>(git_sidebar_entries_.size()) - 1;
  git_sidebar_selected_index_ =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  RevealSelectedGitSidebarLine();
}

bool WorkspaceShell::OpenGitSidebarEntry(std::size_t entry_index) {
  if (entry_index >= git_sidebar_entries_.size()) {
    return false;
  }
  const auto& entry = git_sidebar_entries_[entry_index];
  if (entry.section == GitSidebarEntry::Section::Modified) {
    if (entry.conflicted) {
      return OpenGitConflictMerge(entry.path);
    }
    return OpenWorkingTreeComparison(entry.path, "HEAD", "HEAD");
  }
  if (git_base_ref_.empty()) {
    LogMessage("Base branch is unavailable");
    return false;
  }
  return OpenBranchHeadComparison(entry.path, git_base_ref_,
                                  git_base_label_.empty() ? git_base_ref_ : git_base_label_,
                                  "HEAD", "HEAD");
}

bool WorkspaceShell::StageGitSidebarEntry(std::size_t entry_index) {
  if (entry_index >= git_sidebar_entries_.size()) {
    return false;
  }
  const auto& entry = git_sidebar_entries_[entry_index];
  if (entry.section != GitSidebarEntry::Section::Modified || entry.staged) {
    return false;
  }
  if (!project::GitStagePath(project_root_, entry.path)) {
    LogMessage("Git stage failed: " + entry.relative_path.string());
    return false;
  }
  RefreshProjectFiles();
  LogMessage("Staged: " + entry.relative_path.string());
  return true;
}

bool WorkspaceShell::UnstageGitSidebarEntry(std::size_t entry_index) {
  if (entry_index >= git_sidebar_entries_.size()) {
    return false;
  }
  const auto& entry = git_sidebar_entries_[entry_index];
  if (entry.section != GitSidebarEntry::Section::Modified || !entry.staged) {
    return false;
  }
  if (!project::GitUnstagePath(project_root_, entry.path)) {
    LogMessage("Git unstage failed: " + entry.relative_path.string());
    return false;
  }
  RefreshProjectFiles();
  LogMessage("Unstaged: " + entry.relative_path.string());
  return true;
}

bool WorkspaceShell::DiscardGitSidebarEntry(std::size_t entry_index) {
  if (entry_index >= git_sidebar_entries_.size()) {
    return false;
  }
  const auto& entry = git_sidebar_entries_[entry_index];
  if (entry.section != GitSidebarEntry::Section::Modified) {
    return false;
  }

  std::string blocking_label;
  if (HasDirtyEditorTabsForPath(entry.path, &blocking_label)) {
    LogMessage("Discard blocked by dirty tab: " + blocking_label);
    return false;
  }
  if (!project::GitDiscardPath(project_root_, entry.path)) {
    LogMessage("Git discard failed: " + entry.relative_path.string());
    return false;
  }
  if (std::filesystem::exists(entry.path)) {
    ReloadCleanEditorTabsForPath(entry.path);
  }
  RefreshProjectFiles();
  LogMessage("Discarded: " + entry.relative_path.string());
  return true;
}

}  // namespace microide::workspace
