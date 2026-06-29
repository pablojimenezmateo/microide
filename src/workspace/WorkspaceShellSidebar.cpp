#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "workspace/WorkspaceShellRenderPrimitives.h"
#include "workspace/GitSidebarCommandCenter.h"
#include "workspace/WorkspaceGitSidebarPresentation.h"
#include "workspace/WorkspacePersistenceCoordinator.h"
#include "workspace/WorkspaceSidebarRegistry.h"
#include "workspace/WorkspaceSidebarCoordinator.h"

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
constexpr float kGitSidebarActionRowTop = 34.0f;
constexpr float kGitSidebarActionButtonHeight = 18.0f;
constexpr float kGitSidebarActionGap = 6.0f;
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

SDL_FRect WorkspaceShell::GitSidebarRefreshButtonRect(const SDL_FRect& sidebar_rect) const {
  if (sidebar_rect.w <= 0.0f || sidebar_rect.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }

  const SDL_FRect row_rect = GitSidebarActionRowRect(sidebar_rect);
  if (row_rect.w <= 0.0f || row_rect.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }

  const float button_width =
      std::max(0.0f, (row_rect.w - kGitSidebarActionGap * 2.0f) / 3.0f);
  return MakeRect(row_rect.x + (button_width + kGitSidebarActionGap) * 2.0f, row_rect.y,
                  button_width, row_rect.h);
}

SDL_FRect WorkspaceShell::GitSidebarCommitButtonRect(const SDL_FRect& sidebar_rect) const {
  const SDL_FRect row_rect = GitSidebarActionRowRect(sidebar_rect);
  if (row_rect.w <= 0.0f || row_rect.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }
  // Positioned as the first element of the summary block (kept in sync with the render
  // start offset of action-row-bottom + 10).
  const float button_width = std::min(row_rect.w, 96.0f);
  return MakeRect(row_rect.x, row_rect.y + row_rect.h + 10.0f, button_width,
                  kGitSidebarCommitButtonHeight);
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

std::optional<SDL_FRect> WorkspaceShell::GitSidebarOutgoingBaseButtonRect(
    const SDL_FRect& sidebar_rect) const {
  const auto lines = BuildGitSidebarLines();
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

  const GitSidebarViewModel view_model =
      BuildGitSidebarViewModel(context_.current_project_state.sidebar.git,
                             context_.current_project_state.root,
                             context_.current_project_state.branch_review);
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
  const GitSidebarViewModel view_model =
      BuildGitSidebarViewModel(context_.current_project_state.sidebar.git,
                               context_.current_project_state.root,
                               context_.current_project_state.branch_review);

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
  return sidebar_rect.y + kGitSidebarActionRowTop + kGitSidebarActionButtonHeight +
         kGitSidebarListGap + summary_height +
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
  return ComputeScrollableListLayout(sidebar_rect, sidebar_rect.y + kProjectSearchResultsTop,
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
  // hidden/order policy. Any non-hidden plugin view spills into the overflow menu.
  const auto& policies = context_.current_project_state.sidebar_policies;
  for (const SidebarViewInfo& view : OrderedSidebarViews(plugin_runtime_.Host(), policies)) {
    if (view.id == "tree" || view.id == "search" || view.id == "git") {
      if (layout.tab_count < static_cast<int>(layout.tabs.size())) {
        layout.tabs[static_cast<std::size_t>(layout.tab_count++)] =
            SidebarModeTab{.id = view.id, .mode = view.mode, .rect = {}};
      }
    } else if (view.id != "chat" && view.id != "problems" && view.id != "tests" &&
               view.id != "outline") {
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

std::string WorkspaceShell::HoveredSidebarModeTooltipLabel(const SDL_FRect& sidebar_rect) const {
  if (!last_mouse_position_valid_ || !context_.current_project_state.sidebar.visible ||
      MenuSurfaceCapturingMouse()) {
    return {};
  }
  const SidebarModeRowLayout row = SidebarModeRow(sidebar_rect);
  if (!row.icon_only) {
    return {};  // labels are already visible
  }
  for (int i = 0; i < row.tab_count; ++i) {
    if (Contains(row.tabs[static_cast<std::size_t>(i)].rect, last_mouse_x_, last_mouse_y_)) {
      return std::string(BuiltinSidebarModeLabel(row.tabs[static_cast<std::size_t>(i)].mode));
    }
  }
  if (row.has_overflow && Contains(row.overflow_rect, last_mouse_x_, last_mouse_y_)) {
    return "More views";
  }
  return {};
}

std::string WorkspaceShell::HoveredGitSidebarTooltipLabel(const SDL_FRect& sidebar_rect) const {
  if (!last_mouse_position_valid_ || !context_.current_project_state.sidebar.visible ||
      ActiveSidebarMode() != SidebarMode::Git || MenuSurfaceCapturingMouse() ||
      !Contains(sidebar_rect, last_mouse_x_, last_mouse_y_)) {
    return {};
  }

  if (Contains(GitSidebarRefreshButtonRect(sidebar_rect), last_mouse_x_, last_mouse_y_)) {
    return context_.current_project_state.sidebar.git.refreshing ? "Refreshing repository snapshot"
                                                                 : "Refresh";
  }

  // Per-entry actions live on the right-click context menu, so the only git
  // sidebar tooltip is the header refresh button above.
  return {};
}

std::optional<SDL_FRect> WorkspaceShell::HoveredGitSidebarTooltipRect(const WorkspaceLayout& layout) const {
  const std::string label = HoveredGitSidebarTooltipLabel(layout.sidebar);
  if (label.empty()) {
    return std::nullopt;
  }

  const auto tooltip = detail::BuildTooltipLayout(
      text_renderer_, label, std::max(180.0f, layout.full.w - layout.sidebar.w - 24.0f));
  const float tooltip_x =
      std::clamp(last_mouse_x_ + 12.0f, layout.full.x + 8.0f,
                 layout.full.x + layout.full.w - tooltip.rect.w - 8.0f);
  const float tooltip_y =
      last_mouse_y_ - tooltip.rect.h - 10.0f >= layout.full.y + 8.0f
          ? last_mouse_y_ - tooltip.rect.h - 10.0f
          : std::clamp(last_mouse_y_ + 14.0f, layout.full.y + 8.0f,
                       layout.full.y + layout.full.h - tooltip.rect.h - 8.0f);
  return MakeRect(tooltip_x, tooltip_y, tooltip.rect.w, tooltip.rect.h);
}

std::vector<WorkspaceShell::GitSidebarLine> WorkspaceShell::BuildGitSidebarLines() const {
  const GitSidebarViewModel view_model =
      BuildGitSidebarViewModel(context_.current_project_state.sidebar.git,
                             context_.current_project_state.root,
                             context_.current_project_state.branch_review);
  const auto specs = BuildGitSidebarLineSpecs(
      view_model, &context_.current_project_state.sidebar.git.collapsed_directory_keys);
  std::vector<GitSidebarLine> lines;
  lines.reserve(specs.size());
  for (const GitSidebarLineSpec& spec : specs) {
    lines.push_back(GitSidebarLine{
        .kind = spec.kind == GitSidebarLineKind::Header
                    ? GitSidebarLine::Kind::Header
                    : spec.kind == GitSidebarLineKind::Directory
                        ? GitSidebarLine::Kind::Directory
                    : spec.kind == GitSidebarLineKind::Entry ? GitSidebarLine::Kind::Entry
                                                             : GitSidebarLine::Kind::Empty,
        .section = spec.section,
        .label = spec.label,
        .tree_node_key = spec.tree_node_key,
        .expanded = spec.expanded,
        .depth = spec.depth,
        .entry_index = spec.entry_index,
    });
  }
  return lines;
}

bool WorkspaceShell::ToggleGitSidebarDirectoryCollapsed(const std::string& tree_node_key) {
  if (tree_node_key.empty()) {
    return false;
  }
  auto& collapsed = context_.current_project_state.sidebar.git.collapsed_directory_keys;
  const auto it = collapsed.find(tree_node_key);
  if (it == collapsed.end()) {
    collapsed.insert(tree_node_key);
  } else {
    collapsed.erase(it);
  }
  return true;
}

std::optional<std::size_t> WorkspaceShell::SelectedGitSidebarLineIndex() const {
  if (context_.current_project_state.sidebar.git.selected_index >= context_.current_project_state.sidebar.git.entries.size()) {
    return std::nullopt;
  }

  const GitSidebarViewModel view_model =
      BuildGitSidebarViewModel(context_.current_project_state.sidebar.git,
                             context_.current_project_state.root,
                             context_.current_project_state.branch_review);
  const auto specs = BuildGitSidebarLineSpecs(
      view_model, &context_.current_project_state.sidebar.git.collapsed_directory_keys);
  return FindSelectedGitSidebarLineIndex(specs, context_.current_project_state.sidebar.git.selected_index);
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
