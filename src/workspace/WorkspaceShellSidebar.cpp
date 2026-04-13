#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "project/GitStatusService.h"
#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

constexpr float kSidebarHeaderHeight = 26.0f;
constexpr float kSidebarInset = 10.0f;
constexpr float kSidebarRowHeight = 20.0f;
constexpr float kGitSidebarActionRowTop = 34.0f;
constexpr float kGitSidebarActionButtonHeight = 18.0f;
constexpr float kGitSidebarActionGap = 6.0f;
constexpr float kGitSidebarListGap = 8.0f;
constexpr float kGitSidebarEntryButtonGap = 4.0f;
constexpr float kGitSidebarEntryButtonHoverPadding = 4.0f;

SDL_FRect ExpandRect(const SDL_FRect& rect, float padding) {
  if (rect.w <= 0.0f || rect.h <= 0.0f) {
    return rect;
  }
  return MakeRect(rect.x - padding, rect.y - padding, rect.w + padding * 2.0f,
                  rect.h + padding * 2.0f);
}

}  // namespace

void WorkspaceShell::ShowSidebarMode(SidebarMode mode, bool temporary) {
  if (mode == SidebarMode::None) {
    CloseSidebar();
    return;
  }
  if (mode != SidebarMode::Tree) {
    CloseTreeContextMenu();
  }

  if (surface_.sidebar_mode == SidebarMode::Search && mode != SidebarMode::Search) {
    StopProjectSearch();
  }

  if (temporary) {
    if (!surface_.sidebar_temporary && surface_.sidebar_visible) {
      surface_.sidebar_prev_mode = surface_.sidebar_mode;
    }
  } else {
    surface_.sidebar_prev_mode = SidebarMode::None;
  }

  surface_.sidebar_mode = mode;
  surface_.sidebar_temporary = temporary;
  surface_.sidebar_visible = true;
  surface_.focus = FocusTarget::Sidebar;
  surface_.sidebar_scroll_row = 0;
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
  if (!query.empty() || overlay_workflow_.project_search.query.empty()) {
    overlay_workflow_.project_search.query = std::move(query);
  }
  overlay_workflow_.project_search.edit_buffer = overlay_workflow_.project_search.query;
  overlay_workflow_.project_search.editing = overlay_workflow_.project_search.query.empty();
  overlay_workflow_.project_search.edit_field = ProjectSearchEditField::Query;
  overlay_workflow_.project_search.selected_index = 0;
  RefreshProjectSearch();
  ShowSidebarMode(SidebarMode::Search, temporary);
}

void WorkspaceShell::ShowGitSidebar() {
  RefreshGitSidebar();
  ShowSidebarMode(SidebarMode::Git, false);
  RevealSelectedGitSidebarLine();
}

void WorkspaceShell::CloseSidebar() {
  if (surface_.sidebar_mode == SidebarMode::Search) {
    StopProjectSearch();
  }
  CloseTreeContextMenu();

  if (surface_.sidebar_temporary && surface_.sidebar_prev_mode != SidebarMode::None) {
    RestorePreviousSidebar();
    return;
  }

  surface_.sidebar_visible = false;
  surface_.sidebar_temporary = false;
  surface_.sidebar_prev_mode = SidebarMode::None;
  if (surface_.focus == FocusTarget::Sidebar) {
    surface_.focus = FocusTarget::Editor;
  }
}

void WorkspaceShell::ToggleSidebar() {
  if (surface_.sidebar_visible) {
    CloseSidebar();
    return;
  }

  if (surface_.sidebar_mode == SidebarMode::None) {
    surface_.sidebar_mode = SidebarMode::Tree;
  }
  surface_.sidebar_visible = true;
  surface_.sidebar_temporary = false;
  surface_.focus = FocusTarget::Sidebar;
}

void WorkspaceShell::RestorePreviousSidebar() {
  if (surface_.sidebar_mode == SidebarMode::Search && surface_.sidebar_prev_mode != SidebarMode::Search) {
    StopProjectSearch();
  }

  if (surface_.sidebar_prev_mode == SidebarMode::None) {
    surface_.sidebar_temporary = false;
    return;
  }

  surface_.sidebar_mode = surface_.sidebar_prev_mode;
  surface_.sidebar_prev_mode = SidebarMode::None;
  surface_.sidebar_temporary = false;
  surface_.sidebar_visible = true;
  surface_.focus = FocusTarget::Sidebar;
  surface_.sidebar_scroll_row = 0;
}

void WorkspaceShell::RefreshProjectFiles() {
  directory_tree_.Refresh();
  file_index_.Refresh();
  file_finder_.SetIndex(&file_index_);
  RefreshGitSidebar();
}

void WorkspaceShell::RefreshGitSidebar() {
  const std::filesystem::path previous_path =
      git_sidebar_.selected_index < git_sidebar_.entries.size()
          ? git_sidebar_.entries[git_sidebar_.selected_index].path
          : std::filesystem::path{};
  const GitSidebarEntry::Section previous_section =
      git_sidebar_.selected_index < git_sidebar_.entries.size()
          ? git_sidebar_.entries[git_sidebar_.selected_index].section
          : GitSidebarEntry::Section::Modified;

  git_sidebar_.entries.clear();
  git_sidebar_.base_ref.clear();
  git_sidebar_.base_label.clear();
  git_sidebar_.repo_available = false;
  git_sidebar_.selected_index = 0;
  if (project_root_.empty()) {
    return;
  }

  const auto working_entries = project::CollectGitWorkingTreeEntries(project_root_);
  for (const auto& entry : working_entries) {
    git_sidebar_.entries.push_back(GitSidebarEntry{
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
    git_sidebar_.repo_available = true;
    git_sidebar_.base_ref = base_ref->ref;
    git_sidebar_.base_label = base_ref->label;
    const auto outgoing_entries =
        project::CollectGitBranchOutgoingFiles(project_root_, git_sidebar_.base_ref);
    for (const auto& entry : outgoing_entries) {
      git_sidebar_.entries.push_back(GitSidebarEntry{
          .section = GitSidebarEntry::Section::Outgoing,
          .path = (project_root_ / entry.relative_path).lexically_normal(),
          .relative_path = entry.relative_path,
          .status = entry.status,
      });
    }
  } else {
    git_sidebar_.repo_available = std::filesystem::exists(project_root_ / ".git");
  }

  for (std::size_t i = 0; i < git_sidebar_.entries.size(); ++i) {
    if (git_sidebar_.entries[i].path == previous_path &&
        git_sidebar_.entries[i].section == previous_section) {
      git_sidebar_.selected_index = i;
      RevealSelectedGitSidebarLine();
      return;
    }
  }

  RevealSelectedGitSidebarLine();
}

SDL_FRect WorkspaceShell::GitSidebarRefreshButtonRect(const SDL_FRect& sidebar_rect) const {
  const SDL_FRect row_rect = GitSidebarActionRowRect(sidebar_rect);
  if (row_rect.w <= 0.0f || row_rect.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }

  const float button_width =
      std::max(0.0f, (row_rect.w - kGitSidebarActionGap * 2.0f) / 3.0f);
  return MakeRect(row_rect.x + (button_width + kGitSidebarActionGap) * 2.0f, row_rect.y,
                  button_width, row_rect.h);
}

SDL_FRect WorkspaceShell::GitSidebarActionRowRect(const SDL_FRect& sidebar_rect) const {
  if (sidebar_rect.w <= 0.0f || sidebar_rect.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }

  return MakeRect(sidebar_rect.x + kSidebarInset, sidebar_rect.y + kGitSidebarActionRowTop,
                  std::max(0.0f, sidebar_rect.w - kSidebarInset * 2.0f),
                  kGitSidebarActionButtonHeight);
}

SDL_FRect WorkspaceShell::GitSidebarStageAllButtonRect(const SDL_FRect& sidebar_rect) const {
  const SDL_FRect row_rect = GitSidebarActionRowRect(sidebar_rect);
  if (row_rect.w <= 0.0f || row_rect.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }

  const float button_width =
      std::max(0.0f, (row_rect.w - kGitSidebarActionGap * 2.0f) / 3.0f);
  return MakeRect(row_rect.x, row_rect.y, button_width, row_rect.h);
}

SDL_FRect WorkspaceShell::GitSidebarDiscardAllButtonRect(const SDL_FRect& sidebar_rect) const {
  const SDL_FRect row_rect = GitSidebarActionRowRect(sidebar_rect);
  if (row_rect.w <= 0.0f || row_rect.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }

  const float button_width =
      std::max(0.0f, (row_rect.w - kGitSidebarActionGap * 2.0f) / 3.0f);
  return MakeRect(row_rect.x + button_width + kGitSidebarActionGap, row_rect.y, button_width,
                  row_rect.h);
}

float WorkspaceShell::GitSidebarListTop(const SDL_FRect& sidebar_rect) const {
  return sidebar_rect.y + kGitSidebarActionRowTop + kGitSidebarActionButtonHeight + kGitSidebarListGap;
}

float WorkspaceShell::GitSidebarVisibleUnits(const SDL_FRect& sidebar_rect) const {
  return std::max(1.0f, (sidebar_rect.y + sidebar_rect.h - GitSidebarListTop(sidebar_rect)) /
                            kSidebarRowHeight);
}

ScrollableListLayout WorkspaceShell::ComputeProjectSearchSidebarListLayout(
    const SDL_FRect& sidebar_rect,
    std::size_t line_count) const {
  return ComputeScrollableListLayout(sidebar_rect, sidebar_rect.y + kProjectSearchResultsTop,
                                     line_count, surface_.sidebar_scroll_row, kSidebarInset,
                                     kSidebarRowHeight, kSidebarRowHeight - 2.0f);
}

ScrollableListLayout WorkspaceShell::ComputeGitSidebarListLayout(const SDL_FRect& sidebar_rect,
                                                                 std::size_t line_count) const {
  return ComputeScrollableListLayout(sidebar_rect, GitSidebarListTop(sidebar_rect), line_count,
                                     surface_.sidebar_scroll_row, kSidebarInset, kSidebarRowHeight,
                                     kSidebarRowHeight - 2.0f, 0.0f, 0.0f, true);
}

ScrollableListLayout WorkspaceShell::ComputeTreeSidebarListLayout(const SDL_FRect& sidebar_rect,
                                                                  std::size_t line_count) const {
  return ComputeScrollableListLayout(sidebar_rect, sidebar_rect.y + kSidebarHeaderHeight + 6.0f,
                                     line_count, surface_.sidebar_scroll_row, kSidebarInset,
                                     kSidebarRowHeight, kSidebarRowHeight - 2.0f);
}

SDL_FRect WorkspaceShell::TreeSidebarRefreshButtonRect(const SDL_FRect& sidebar_rect) const {
  if (sidebar_rect.w <= 0.0f || sidebar_rect.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }

  const float button_width = std::max(72.0f, text_renderer_.MeasureWidth("Refresh") + 18.0f);
  return MakeRect(sidebar_rect.x + sidebar_rect.w - 10.0f - button_width, sidebar_rect.y + 4.0f,
                  button_width, 18.0f);
}

std::string WorkspaceShell::SidebarModeControlLabel() const {
  switch (surface_.sidebar_mode) {
    case SidebarMode::Search:
      return "Search";
    case SidebarMode::Git:
      return "Source Control";
    case SidebarMode::Tree:
    default:
      return "Project";
  }
}

SDL_FRect WorkspaceShell::SidebarModeControlRect(const SDL_FRect& sidebar_rect) const {
  if (sidebar_rect.w <= 0.0f || sidebar_rect.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }

  const std::string label = SidebarModeControlLabel();
  const float width = std::clamp(text_renderer_.MeasureWidth(label) + 30.0f, 92.0f,
                                 std::max(92.0f, sidebar_rect.w - 20.0f));
  return MakeRect(sidebar_rect.x + 10.0f, sidebar_rect.y + 4.0f, width, 18.0f);
}

std::string WorkspaceShell::HoveredGitSidebarTooltipLabel(const SDL_FRect& sidebar_rect) const {
  if (!last_mouse_position_valid_ || !surface_.sidebar_visible || surface_.sidebar_mode != SidebarMode::Git ||
      !Contains(sidebar_rect, last_mouse_x_, last_mouse_y_)) {
    return {};
  }

  if (last_mouse_y_ < GitSidebarListTop(sidebar_rect)) {
    return {};
  }

  const auto lines = BuildGitSidebarLines();
  const auto list_layout = ComputeGitSidebarListLayout(sidebar_rect, lines.size());
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const auto& line = lines[i];
    if (line.kind != GitSidebarLine::Kind::Entry || line.entry_index < 0 ||
        static_cast<std::size_t>(line.entry_index) >= git_sidebar_.entries.size()) {
      continue;
    }

    const int visible_row = static_cast<int>(i) - list_layout.scroll_row;
    if (visible_row < 0 || visible_row >= list_layout.visible_rows) {
      continue;
    }

    const auto& entry = git_sidebar_.entries[static_cast<std::size_t>(line.entry_index)];
    const SDL_FRect row_rect = ScrollableListRowRect(list_layout, visible_row);
    const GitSidebarEntryActionLayout actions = ComputeGitSidebarEntryActionLayout(row_rect, entry);
    if (actions.primary_rect.has_value() &&
        Contains(ExpandRect(*actions.primary_rect, kGitSidebarEntryButtonHoverPadding),
                 last_mouse_x_, last_mouse_y_)) {
      return entry.staged ? "Unstage" : "Stage";
    }
    if (actions.discard_rect.has_value() &&
        Contains(ExpandRect(*actions.discard_rect, kGitSidebarEntryButtonHoverPadding),
                 last_mouse_x_, last_mouse_y_)) {
      return "Discard";
    }
  }
  return {};
}

std::vector<WorkspaceShell::GitSidebarLine> WorkspaceShell::BuildGitSidebarLines() const {
  std::vector<GitSidebarSection> sections;
  sections.reserve(git_sidebar_.entries.size());
  for (const auto& entry : git_sidebar_.entries) {
    sections.push_back(entry.section == GitSidebarEntry::Section::Modified
                           ? GitSidebarSection::Modified
                           : GitSidebarSection::Outgoing);
  }

  const auto specs =
      BuildGitSidebarLineSpecs(sections, git_sidebar_.repo_available, git_sidebar_.base_ref, git_sidebar_.base_label);
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

WorkspaceShell::GitSidebarEntryActionLayout WorkspaceShell::ComputeGitSidebarEntryActionLayout(
    const SDL_FRect& row_rect,
    const GitSidebarEntry& entry) const {
  GitSidebarEntryActionLayout layout;
  layout.content_right_edge = row_rect.x + row_rect.w - 8.0f;
  if (entry.section != GitSidebarEntry::Section::Modified || row_rect.w <= 0.0f ||
      row_rect.h <= 0.0f) {
    return layout;
  }

  const auto button_rect = [&](float right_edge, std::string_view label) {
    const float width = std::max(22.0f, text_renderer_.MeasureWidth(label) + 12.0f);
    return MakeRect(right_edge - width, row_rect.y + 1.0f, width, row_rect.h - 2.0f);
  };

  const SDL_FRect primary_rect = button_rect(layout.content_right_edge, entry.staged ? "U" : "S");
  layout.primary_rect = primary_rect;
  layout.content_right_edge = primary_rect.x - kGitSidebarEntryButtonGap;

  const SDL_FRect discard_rect = button_rect(layout.content_right_edge, "D");
  layout.discard_rect = discard_rect;
  layout.content_right_edge = discard_rect.x - 6.0f;
  return layout;
}

std::optional<std::size_t> WorkspaceShell::SelectedGitSidebarLineIndex() const {
  if (git_sidebar_.selected_index >= git_sidebar_.entries.size()) {
    return std::nullopt;
  }

  std::vector<GitSidebarSection> sections;
  sections.reserve(git_sidebar_.entries.size());
  for (const auto& entry : git_sidebar_.entries) {
    sections.push_back(entry.section == GitSidebarEntry::Section::Modified
                           ? GitSidebarSection::Modified
                           : GitSidebarSection::Outgoing);
  }
  const auto specs =
      BuildGitSidebarLineSpecs(sections, git_sidebar_.repo_available, git_sidebar_.base_ref, git_sidebar_.base_label);
  return FindSelectedGitSidebarLineIndex(specs, git_sidebar_.selected_index);
}

const WorkspaceShell::GitSidebarEntry* WorkspaceShell::SelectedGitSidebarEntry() const {
  if (git_sidebar_.selected_index >= git_sidebar_.entries.size()) {
    return nullptr;
  }
  return &git_sidebar_.entries[git_sidebar_.selected_index];
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
                    surface_.sidebar_visible, BottomPanelVisible(), surface_.sidebar_width, surface_.bottom_panel_height);
  if (layout.sidebar.h <= 0.0f) {
    return;
  }
  const auto lines = BuildGitSidebarLines();
  const auto list_layout = ComputeGitSidebarListLayout(layout.sidebar, lines.size());
  surface_.sidebar_scroll_row =
      RevealScrollableListIndex(list_layout, static_cast<int>(*selected_line));
}

void WorkspaceShell::MoveGitSidebarSelection(int delta) {
  if (git_sidebar_.entries.empty() || delta == 0) {
    return;
  }
  const int current = static_cast<int>(git_sidebar_.selected_index);
  const int max_index = static_cast<int>(git_sidebar_.entries.size()) - 1;
  git_sidebar_.selected_index =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  RevealSelectedGitSidebarLine();
}

bool WorkspaceShell::OpenGitSidebarEntry(std::size_t entry_index) {
  if (entry_index >= git_sidebar_.entries.size()) {
    return false;
  }
  const auto& entry = git_sidebar_.entries[entry_index];
  if (entry.section == GitSidebarEntry::Section::Modified) {
    if (entry.conflicted) {
      return OpenGitConflictMerge(entry.path);
    }
    return OpenWorkingTreeComparison(entry.path, "HEAD", "HEAD");
  }
  if (git_sidebar_.base_ref.empty()) {
    return false;
  }
  return OpenBranchHeadComparison(entry.path, git_sidebar_.base_ref,
                                  git_sidebar_.base_label.empty() ? git_sidebar_.base_ref : git_sidebar_.base_label,
                                  "HEAD", "HEAD");
}

bool WorkspaceShell::CanStageAllGitSidebarEntries() const {
  return std::any_of(git_sidebar_.entries.begin(), git_sidebar_.entries.end(), [](const auto& entry) {
    return entry.section == GitSidebarEntry::Section::Modified && !entry.staged;
  });
}

bool WorkspaceShell::CanDiscardAllGitSidebarEntries() const {
  return std::any_of(git_sidebar_.entries.begin(), git_sidebar_.entries.end(), [](const auto& entry) {
    return entry.section == GitSidebarEntry::Section::Modified;
  });
}

bool WorkspaceShell::StageAllGitSidebarEntries() {
  if (!CanStageAllGitSidebarEntries()) {
    return false;
  }
  std::vector<std::filesystem::path> affected_paths;
  affected_paths.reserve(git_sidebar_.entries.size());
  for (const auto& entry : git_sidebar_.entries) {
    if (entry.section != GitSidebarEntry::Section::Modified || entry.staged) {
      continue;
    }
    affected_paths.push_back(entry.path.lexically_normal());
  }
  std::sort(affected_paths.begin(), affected_paths.end());
  affected_paths.erase(std::unique(affected_paths.begin(), affected_paths.end()),
                       affected_paths.end());
  if (!project::GitStageAll(project_root_)) {
    return false;
  }
  for (const auto& path : affected_paths) {
    InvalidateEditorBlamePath(path);
  }
  RefreshProjectFiles();
  return true;
}

void WorkspaceShell::OpenDiscardAllGitSidebarPrompt() {
  if (!CanDiscardAllGitSidebarEntries()) {
    return;
  }
  OpenPromptSurface(PromptSurfaceState::Action::DiscardGitChanges,
                    PromptSurfaceState::Kind::Confirm, project_root_);
}

bool WorkspaceShell::DiscardAllGitSidebarEntries() {
  if (!CanDiscardAllGitSidebarEntries()) {
    return false;
  }

  std::string blocking_label;
  if (HasDirtyEditorTabsForPath(project_root_, &blocking_label)) {
    return false;
  }

  std::vector<std::filesystem::path> affected_paths;
  affected_paths.reserve(git_sidebar_.entries.size());
  for (const auto& entry : git_sidebar_.entries) {
    if (entry.section != GitSidebarEntry::Section::Modified) {
      continue;
    }
    affected_paths.push_back(entry.path.lexically_normal());
  }
  std::sort(affected_paths.begin(), affected_paths.end());
  affected_paths.erase(std::unique(affected_paths.begin(), affected_paths.end()),
                       affected_paths.end());

  if (!project::GitDiscardAll(project_root_)) {
    return false;
  }

  for (const auto& path : affected_paths) {
    InvalidateEditorBlamePath(path);
    ReconcileOpenTabsAfterPathDiscard(path);
  }
  RefreshProjectFiles();
  return true;
}

bool WorkspaceShell::StageGitSidebarEntry(std::size_t entry_index) {
  if (entry_index >= git_sidebar_.entries.size()) {
    return false;
  }
  const auto& entry = git_sidebar_.entries[entry_index];
  if (entry.section != GitSidebarEntry::Section::Modified || entry.staged) {
    return false;
  }
  if (!project::GitStagePath(project_root_, entry.path)) {
    return false;
  }
  InvalidateEditorBlamePath(entry.path);
  RefreshProjectFiles();
  return true;
}

bool WorkspaceShell::UnstageGitSidebarEntry(std::size_t entry_index) {
  if (entry_index >= git_sidebar_.entries.size()) {
    return false;
  }
  const auto& entry = git_sidebar_.entries[entry_index];
  if (entry.section != GitSidebarEntry::Section::Modified || !entry.staged) {
    return false;
  }
  if (!project::GitUnstagePath(project_root_, entry.path)) {
    return false;
  }
  InvalidateEditorBlamePath(entry.path);
  RefreshProjectFiles();
  return true;
}

bool WorkspaceShell::DiscardGitSidebarEntry(std::size_t entry_index) {
  if (entry_index >= git_sidebar_.entries.size()) {
    return false;
  }
  const auto& entry = git_sidebar_.entries[entry_index];
  if (entry.section != GitSidebarEntry::Section::Modified) {
    return false;
  }

  std::string blocking_label;
  if (HasDirtyEditorTabsForPath(entry.path, &blocking_label)) {
    return false;
  }
  if (!project::GitDiscardPath(project_root_, entry.path)) {
    return false;
  }
  InvalidateEditorBlamePath(entry.path);
  ReconcileOpenTabsAfterPathDiscard(entry.path);
  RefreshProjectFiles();
  return true;
}

void WorkspaceShell::ReconcileOpenTabsAfterPathDiscard(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  std::error_code error;
  if (std::filesystem::exists(normalized_path, error) && !error) {
    ReloadCleanEditorTabsForPath(normalized_path);
    return;
  }
  CloseOpenTabsForPath(normalized_path);
}

}  // namespace microide::workspace
