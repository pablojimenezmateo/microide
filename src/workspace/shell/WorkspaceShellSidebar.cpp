#include "workspace/shell/WorkspaceShell.h"

#include "workspace/git/GitSidebarHeaderLayout.h"
#include "workspace/ProjectSearchPanelLayout.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "workspace/render/WorkspaceShellRenderPrimitives.h"
#include "workspace/git/GitSidebarCommandCenter.h"
#include "workspace/git/WorkspaceGitSidebarPresentation.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"
#include "workspace/registries/WorkspaceSidebarRegistry.h"
#include "workspace/coordinators/WorkspaceSidebarCoordinator.h"

namespace microide::workspace {

namespace {

constexpr float kSidebarHeaderHeight = 26.0f;
constexpr float kSidebarInset = 10.0f;
constexpr float kSidebarRowHeight = 20.0f;
// Mode-switch tab row (replaces the old dropdown) at the top of the sidebar header.
constexpr float kSidebarModeRowTop = 4.0f;
constexpr float kSidebarModeRowHeight = 20.0f;
constexpr float kSidebarModeTabGap = 4.0f;
constexpr float kSidebarModeIconSlot = 16.0f;       // icon cell width within a tab
constexpr float kSidebarModeIconLabelGap = 5.0f;    // icon -> label spacing
constexpr float kSidebarModeLabelPadding = 10.0f;   // leading + trailing label padding
constexpr float kSidebarModeOverflowWidth = 24.0f;  // "⋯" overflow button

// Static label for a builtin sidebar mode (tab labels are always builtin views).
std::string_view BuiltinSidebarModeLabel(SidebarMode mode) {
  const SidebarViewSpec* view = FindBuiltinSidebarView(mode);
  return view != nullptr ? view->label : std::string_view{};
}
constexpr float kGitSidebarListGap = 8.0f;
constexpr float kTreeSidebarActionRowTop = 34.0f;
constexpr float kTreeSidebarActionButtonHeight = 18.0f;
constexpr float kTreeSidebarListGap = 8.0f;
// Must match kSummaryLineHeight in WorkspaceShellRenderSidebar.cpp.
constexpr float kGitSidebarSummaryLineHeight = 17.0f;
// Commit button row (panel header) — height plus the gap below it. Must match the
// values used in WorkspaceShellRenderSidebar.cpp / GitSidebarSummaryHeight.
constexpr float kGitSidebarCommitButtonHeight = 22.0f;
constexpr float kGitSidebarCommitButtonGap = 8.0f;
constexpr float kGitSidebarHeaderMenuButtonSize = 16.0f;
constexpr float kSidebarHeaderCompactButtonSize = 18.0f;

bool UseCompactTreeHeader(float sidebar_width,
                          float mode_min_width,
                          float collapse_width,
                          float refresh_width) {
  const float minimum_required_width = 10.0f + mode_min_width + 6.0f + collapse_width + 6.0f +
                                       refresh_width + 10.0f;
  return sidebar_width < minimum_required_width;
}

}  // namespace

std::optional<SDL_FRect> WorkspaceShell::GitSidebarOutgoingBaseButtonRect(
    const SDL_FRect& sidebar_rect) const {
  const auto& lines = BuildGitSidebarLines();
  const auto list_layout = ComputeGitSidebarListLayout(sidebar_rect, lines.size());
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const auto& line = lines[i];
    if (line.kind != GitSidebarLine::Kind::Header ||
        line.section != GitSidebarEntry::Section::Outgoing) {
      continue;
    }

    const int visible_row = static_cast<int>(i) - list_layout.scroll_row;
    if (visible_row < 0 || visible_row >= list_layout.visible_rows) {
      return std::nullopt;
    }

    const SDL_FRect row_rect = ScrollableListRowRect(list_layout, visible_row);
    const float size = std::min(kGitSidebarHeaderMenuButtonSize, row_rect.h - 2.0f);
    return MakeRect(row_rect.x + row_rect.w - size - 4.0f, row_rect.y + (row_rect.h - size) * 0.5f,
                    size, size);
  }
  return std::nullopt;
}

std::vector<std::string> WorkspaceShell::GitSidebarSummaryLines() const {
  std::vector<std::string> lines;

  const GitSidebarViewModel& view_model =
      workspace::CachedGitSidebarPresentation(context_.current_project_state.sidebar.git,
                                              context_.current_project_state.root,
                                              context_.current_project_state.branch_review)
          .view_model;
  lines.insert(lines.end(), view_model.summary_lines.begin(), view_model.summary_lines.end());
  if (!view_model.workflow_summary_line.empty()) {
    lines.push_back(view_model.workflow_summary_line);
  }
  if (!view_model.commit_summary_line.empty()) {
    lines.push_back(view_model.commit_summary_line);
  }
  if (!view_model.selection_summary_line.empty()) {
    lines.push_back(view_model.selection_summary_line);
  }
  return lines;
}

float WorkspaceShell::GitSidebarSummaryHeight() const {
  const GitSidebarViewModel& view_model =
      workspace::CachedGitSidebarPresentation(context_.current_project_state.sidebar.git,
                                              context_.current_project_state.root,
                                              context_.current_project_state.branch_review)
          .view_model;

  float height = 0.0f;
  // Keep list offset in sync with the compact grouped summary block. summary_lines
  // carries only banners now; the branch line moved to the status bar, the per-selection
  // line and the redundant staged/unstaged counts were removed. The commit-readiness
  // line is replaced by the Commit button (or a hint while the draft is open).
  if (view_model.show_commit_button) {
    height += kGitSidebarCommitButtonHeight + kGitSidebarCommitButtonGap;
  } else if (!view_model.commit_summary_line.empty()) {
    height += kGitSidebarSummaryLineHeight;
  }
  height += static_cast<float>(view_model.summary_lines.size()) * kGitSidebarSummaryLineHeight;
  return height;
}

float WorkspaceShell::GitSidebarListTop(const SDL_FRect& sidebar_rect) const {
  const float summary_height = GitSidebarSummaryHeight();
  return sidebar_rect.y + git_sidebar_header::kActionRowTop +
         git_sidebar_header::kActionRowsHeight + kGitSidebarListGap + summary_height +
         (summary_height > 0.0f ? kGitSidebarListGap * 0.5f : 0.0f) + GitSidebarCommitWorkflowHeight() +
         (GitSidebarCommitWorkflowHeight() > 0.0f ? kGitSidebarListGap : 0.0f);
}

float WorkspaceShell::GitSidebarVisibleUnits(const SDL_FRect& sidebar_rect) const {
  return std::max(1.0f, (sidebar_rect.y + sidebar_rect.h - GitSidebarListTop(sidebar_rect)) /
                            kSidebarRowHeight);
}

ScrollableListLayout WorkspaceShell::ComputeProjectSearchSidebarListLayout(
    const SDL_FRect& sidebar_rect,
    std::size_t line_count) const {
  return ComputeScrollableListLayout(sidebar_rect, sidebar_rect.y + project_search_panel::ResultsTop(
                                         ProjectSearchScopeExpanded()),
                                     line_count, context_.current_project_state.sidebar.scroll_row, kSidebarInset,
                                     kSidebarRowHeight, kSidebarRowHeight - 2.0f);
}

ScrollableListLayout WorkspaceShell::ComputeGitSidebarListLayout(const SDL_FRect& sidebar_rect,
                                                                 std::size_t line_count) const {
  return ComputeScrollableListLayout(sidebar_rect, GitSidebarListTop(sidebar_rect), line_count,
                                     context_.current_project_state.sidebar.scroll_row, kSidebarInset, kSidebarRowHeight,
                                     kSidebarRowHeight - 2.0f, 0.0f, 0.0f, true);
}

ScrollableListLayout WorkspaceShell::ComputeTreeSidebarListLayout(const SDL_FRect& sidebar_rect,
                                                                  std::size_t line_count) const {
  return ComputeScrollableListLayout(sidebar_rect,
                                     sidebar_rect.y + kTreeSidebarActionRowTop +
                                         kTreeSidebarActionButtonHeight + kTreeSidebarListGap,
                                     line_count, context_.current_project_state.sidebar.scroll_row, kSidebarInset,
                                     kSidebarRowHeight, kSidebarRowHeight - 2.0f);
}

ScrollableListLayout WorkspaceShell::ComputeProblemsSidebarListLayout(
    const SDL_FRect& sidebar_rect,
    std::size_t line_count) const {
  return ComputeScrollableListLayout(sidebar_rect, sidebar_rect.y + kSidebarHeaderHeight + 6.0f,
                                     line_count, context_.current_project_state.sidebar.scroll_row,
                                     kSidebarInset, kSidebarRowHeight, kSidebarRowHeight - 2.0f);
}

ScrollableListLayout WorkspaceShell::ComputeTestsSidebarListLayout(const SDL_FRect& sidebar_rect,
                                                                   std::size_t line_count) const {
  return ComputeScrollableListLayout(sidebar_rect, sidebar_rect.y + kSidebarHeaderHeight + 6.0f,
                                     line_count, context_.current_project_state.sidebar.scroll_row,
                                     kSidebarInset, kSidebarRowHeight, kSidebarRowHeight - 2.0f);
}

ScrollableListLayout WorkspaceShell::ComputePluginSidebarListLayout(const SDL_FRect& sidebar_rect,
                                                                    std::size_t line_count) const {
  return ComputeScrollableListLayout(sidebar_rect, sidebar_rect.y + kSidebarHeaderHeight + 6.0f,
                                     line_count, context_.current_project_state.sidebar.scroll_row,
                                     kSidebarInset, kSidebarRowHeight, kSidebarRowHeight - 2.0f);
}

SDL_FRect WorkspaceShell::TreeSidebarCollapseButtonRect(const SDL_FRect& sidebar_rect) const {
  if (sidebar_rect.w <= 0.0f || sidebar_rect.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }

  const float refresh_width = std::max(72.0f, text_renderer_.MeasureWidth("Refresh") + 18.0f);
  const float collapse_width = std::max(76.0f, text_renderer_.MeasureWidth("Collapse") + 18.0f);
  const bool compact_header =
      UseCompactTreeHeader(sidebar_rect.w, 0.0f, collapse_width, refresh_width);
  const float action_row_y = sidebar_rect.y + kTreeSidebarActionRowTop;
  const float action_row_x = sidebar_rect.x + kSidebarInset;
  if (compact_header) {
    return MakeRect(action_row_x, action_row_y,
                    kSidebarHeaderCompactButtonSize, kSidebarHeaderCompactButtonSize);
  }

  return MakeRect(action_row_x, action_row_y, collapse_width,
                  kTreeSidebarActionButtonHeight);
}

SDL_FRect WorkspaceShell::TreeSidebarRefreshButtonRect(const SDL_FRect& sidebar_rect) const {
  if (sidebar_rect.w <= 0.0f || sidebar_rect.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }

  const float button_width = std::max(72.0f, text_renderer_.MeasureWidth("Refresh") + 18.0f);
  const float collapse_width = std::max(76.0f, text_renderer_.MeasureWidth("Collapse") + 18.0f);
  const bool compact_header =
      UseCompactTreeHeader(sidebar_rect.w, 0.0f, collapse_width, button_width);
  const float action_row_y = sidebar_rect.y + kTreeSidebarActionRowTop;
  const float action_row_x = sidebar_rect.x + kSidebarInset;
  if (compact_header) {
    return MakeRect(action_row_x + kSidebarHeaderCompactButtonSize + 6.0f, action_row_y,
                    kSidebarHeaderCompactButtonSize,
                    kSidebarHeaderCompactButtonSize);
  }

  return MakeRect(action_row_x + collapse_width + 6.0f, action_row_y,
                  button_width, kTreeSidebarActionButtonHeight);
}

SidebarModeRowLayout WorkspaceShell::SidebarModeRow(const SDL_FRect& sidebar_rect) const {
  SidebarModeRowLayout layout;
  if (sidebar_rect.w <= 0.0f || sidebar_rect.h <= 0.0f) {
    return layout;
  }

  const float left = sidebar_rect.x + kSidebarInset;
  const float top = sidebar_rect.y + kSidebarModeRowTop;
  const float right = sidebar_rect.x + sidebar_rect.w - kSidebarInset;
  layout.row_rect = MakeRect(left, top, std::max(0.0f, right - left), kSidebarModeRowHeight);
  if (layout.row_rect.w <= 0.0f) {
    return layout;
  }

  // Primary tabs are the builtin Project / Search / Source Control views, honoring the user's
  // hidden/order policy. Everything else -- Problems, Tests, Outline, and any
  // non-hidden plugin view -- spills into the overflow menu. Those three builtins
  // used to be filtered out of both the row and the menu, a leftover from when they
  // were retired surfaces; they came back without the rail being told, so they were
  // reachable only by typing `sidebar-show problems` and, once shown, left the rail
  // with no highlighted tab and no control to get back.
  const auto& policies = context_.current_project_state.sidebar_policies;
  for (const SidebarViewInfo& view : OrderedSidebarViews(plugin_runtime_.Host(), policies)) {
    if (view.id == "tree" || view.id == "search" || view.id == "git") {
      if (layout.tab_count < static_cast<int>(layout.tabs.size())) {
        layout.tabs[static_cast<std::size_t>(layout.tab_count++)] =
            SidebarModeTab{.id = view.id, .mode = view.mode, .rect = {}};
      }
    } else {
      layout.has_overflow = true;
    }
  }
  if (layout.tab_count == 0) {
    return layout;
  }

  const float overflow_w =
      layout.has_overflow ? kSidebarModeOverflowWidth + kSidebarModeTabGap : 0.0f;
  const float avail = std::max(0.0f, layout.row_rect.w - overflow_w);
  const float gaps = kSidebarModeTabGap * static_cast<float>(layout.tab_count - 1);

  // Choose icon+label when the labelled row fits, else collapse to equal icon-only cells.
  float labelled_total = gaps;
  for (int i = 0; i < layout.tab_count; ++i) {
    labelled_total += kSidebarModeIconSlot + kSidebarModeIconLabelGap +
                      text_renderer_.MeasureWidth(BuiltinSidebarModeLabel(layout.tabs[i].mode)) +
                      kSidebarModeLabelPadding;
  }
  layout.icon_only = labelled_total > avail;

  float x = layout.row_rect.x;
  for (int i = 0; i < layout.tab_count; ++i) {
    float w = 0.0f;
    if (layout.icon_only) {
      w = std::max(kSidebarModeIconSlot, (avail - gaps) / static_cast<float>(layout.tab_count));
    } else {
      w = kSidebarModeIconSlot + kSidebarModeIconLabelGap +
          text_renderer_.MeasureWidth(BuiltinSidebarModeLabel(layout.tabs[i].mode)) +
          kSidebarModeLabelPadding;
    }
    layout.tabs[static_cast<std::size_t>(i)].rect = MakeRect(x, layout.row_rect.y, w,
                                                             layout.row_rect.h);
    x += w + kSidebarModeTabGap;
  }
  if (layout.has_overflow) {
    layout.overflow_rect =
        MakeRect(layout.row_rect.x + layout.row_rect.w - kSidebarModeOverflowWidth,
                 layout.row_rect.y, kSidebarModeOverflowWidth, layout.row_rect.h);
  }
  return layout;
}

const std::vector<WorkspaceShell::GitSidebarLine>& WorkspaceShell::BuildGitSidebarLines() const {
  // Always reads live git state through the shared revision-exact memo (never the
  // possibly-stale per-frame prepare cache), so hit-testing between frames matches
  // the current collapse/entry state and is cheap when nothing changed. The render
  // TU instead consumes the frame's pre-flattened `git_sidebar_lines` directly.
  return workspace::CachedGitSidebarPresentation(context_.current_project_state.sidebar.git,
                                                 context_.current_project_state.root,
                                                 context_.current_project_state.branch_review)
      .lines;
}

bool WorkspaceShell::ToggleGitSidebarDirectoryCollapsed(const std::string& tree_node_key) {
  if (tree_node_key.empty()) {
    return false;
  }
  auto& git = context_.current_project_state.sidebar.git;
  auto& collapsed = git.collapsed_directory_keys;
  const auto it = collapsed.find(tree_node_key);
  if (it == collapsed.end()) {
    collapsed.insert(tree_node_key);
  } else {
    collapsed.erase(it);
  }

  // Collapsing a directory can hide the selected entry (selection is a flat entry
  // index, the tree filters rows). Snap the flat selection to the nearest still-
  // visible entry so arrow keys and the highlight don't strand on an invisible row.
  if (git.selected_index < git.entries.size()) {
    const std::vector<GitSidebarLine>& lines = BuildGitSidebarLines();
    if (!FindSelectedGitSidebarLineIndex(lines, git.selected_index).has_value()) {
      std::optional<std::size_t> nearest;
      for (const GitSidebarLine& line : lines) {
        if (line.kind == GitSidebarLine::Kind::Entry && line.entry_index >= 0) {
          const auto candidate = static_cast<std::size_t>(line.entry_index);
          // Prefer the first visible entry at/after the old selection; otherwise
          // keep the last visible entry before it.
          if (candidate >= git.selected_index) {
            nearest = candidate;
            break;
          }
          nearest = candidate;
        }
      }
      if (nearest.has_value()) {
        git.selected_index = *nearest;
      }
    }
  }
  return true;
}

std::optional<std::size_t> WorkspaceShell::SelectedGitSidebarLineIndex() const {
  if (context_.current_project_state.sidebar.git.selected_index >= context_.current_project_state.sidebar.git.entries.size()) {
    return std::nullopt;
  }

  return FindSelectedGitSidebarLineIndex(BuildGitSidebarLines(),
                                         context_.current_project_state.sidebar.git.selected_index);
}

const WorkspaceShell::GitSidebarEntry* WorkspaceShell::SelectedGitSidebarEntry() const {
  if (context_.current_project_state.sidebar.git.selected_index >= context_.current_project_state.sidebar.git.entries.size()) {
    return nullptr;
  }
  return &context_.current_project_state.sidebar.git.entries[context_.current_project_state.sidebar.git.selected_index];
}

void WorkspaceShell::SetGitOutgoingBaseChoice(OutgoingBaseChoice choice) {
  context_.current_project_state.sidebar.git.outgoing_base_choice = std::move(choice);
  MakePersistenceCoordinator().SaveSessionState();
  RequestGitSidebarRefresh(GitSidebarRefreshScope::Full);
  ConsumeGitSidebarRefresh();
}

}  // namespace microide::workspace
