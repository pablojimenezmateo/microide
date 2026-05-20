#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "app/BackgroundTaskCounter.h"
#include "util/PerformanceTrace.h"
#include "workspace/WorkspaceShellBootstrapper.h"

namespace microide::workspace {

namespace {

constexpr Uint64 kCaretBlinkIntervalMs = 530;
// After this many ms with no caret-blink-resetting input (typing, navigation,
// focus changes, etc.) the caret freezes in its visible phase and we stop
// scheduling blink-only wake-ups. This cuts the idle wake/partial-redraw
// rate to ~0 during long inactivity, matching the dominant terminal
// convention and many editors' "stop-blink" timeout. The next input event
// resets `caret_blink_epoch_ms_` via `ResetCaretBlink()` and resumes blink.
constexpr Uint64 kCaretBlinkIdleStopMs = 8000;
constexpr std::size_t kBlameNeighborhoodRadius = 1;

bool TraceProjectEventsEnabled() {
  static const bool enabled =
      util::PerformanceTrace::FlagEnabled("MICROIDE_TRACE_PROJECT_EVENTS");
  return enabled;
}

std::size_t MaxVisualColumns(const editor::TextViewport& viewport) {
  return viewport.max_visual_columns();
}

std::optional<SDL_FRect> UnionRects(std::optional<SDL_FRect> lhs, const SDL_FRect& rhs) {
  if (!lhs.has_value()) {
    return rhs;
  }

  const float x0 = std::min(lhs->x, rhs.x);
  const float y0 = std::min(lhs->y, rhs.y);
  const float x1 = std::max(lhs->x + lhs->w, rhs.x + rhs.w);
  const float y1 = std::max(lhs->y + lhs->h, rhs.y + rhs.h);
  return MakeRect(x0, y0, x1 - x0, y1 - y0);
}

bool RectsEqual(const SDL_FRect& lhs, const SDL_FRect& rhs) {
  return lhs.x == rhs.x && lhs.y == rhs.y && lhs.w == rhs.w && lhs.h == rhs.h;
}

}  // namespace

void WorkspaceShell::MarkLayoutDirty() {
  layout_dirty_ = true;
}

void WorkspaceShell::SetWindowPresentationState(WindowPresentationState state) {
  const int previous_width = window_presentation_.logical_width;
  const int previous_height = window_presentation_.logical_height;
  presentation_scale_x_ =
      std::isfinite(state.scale_x) && state.scale_x > 0.0f ? state.scale_x : 1.0f;
  presentation_scale_y_ =
      std::isfinite(state.scale_y) && state.scale_y > 0.0f ? state.scale_y : 1.0f;
  if (state.logical_width <= 0) {
    state.logical_width = window_presentation_.logical_width;
  }
  if (state.logical_height <= 0) {
    state.logical_height = window_presentation_.logical_height;
  }
  state.scale_x = presentation_scale_x_;
  state.scale_y = presentation_scale_y_;
  window_presentation_ = std::move(state);
  if (window_presentation_.logical_width != previous_width ||
      window_presentation_.logical_height != previous_height) {
    MarkLayoutDirty();
    // Tab-strip scroll uses the current window width to decide how many tabs
    // fit; the fallback width (1440) is wider than typical windows, so the
    // initial post-session-restore call may leave the active tab/project
    // scrolled off-screen until the user manually scrolls. Re-run both
    // visibility passes on every size change so the active chips stay
    // anchored in view.
    if (window_presentation_.logical_width != previous_width) {
      tab_strip_chrome_.EnsureActiveProjectVisible();
      tab_strip_chrome_.EnsureActiveTabVisible();
      tab_strip_service_.InvalidateEditorTabGeometry();
    }
  }
}

std::optional<WorkspaceShell::WindowPresentationState>
WorkspaceShell::CurrentWindowPresentationState() const {
  if (window_presentation_.logical_width <= 0 || window_presentation_.logical_height <= 0) {
    return std::nullopt;
  }
  return window_presentation_;
}

render::TextClipPadding WorkspaceShell::PartialRedrawClipPadding() const {
  return text_renderer_.ClipPadding();
}

std::optional<SDL_FRect> WorkspaceShell::CurrentWindowRect() const {
  if (window_presentation_.logical_width <= 0 || window_presentation_.logical_height <= 0) {
    return std::nullopt;
  }

  return MakeRect(0.0f, 0.0f, static_cast<float>(window_presentation_.logical_width),
                  static_cast<float>(window_presentation_.logical_height));
}

std::optional<WorkspaceLayout> WorkspaceShell::CurrentWorkspaceLayout() const {
  const auto window_rect = CurrentWindowRect();
  if (!window_rect.has_value()) {
    return std::nullopt;
  }

  return ComputeLayout(window_rect->w, window_rect->h, context_.current_project_state.sidebar.visible,
                       BottomPanelVisible(), context_.current_project_state.sidebar.width,
                       context_.current_project_state.panel.height,
                       layout_mode_service_.SnapshotInputs(),
                       layout_mode_service_.StatusBarVisible());
}

const WorkspaceShell::WindowChromeState& WorkspaceShell::CurrentWindowChromeState() const {
  return window_presentation_.chrome;
}

WorkspaceShell::RenderInvalidation WorkspaceShell::ConsumePendingRenderInvalidation() {
  const RenderInvalidation result = pending_render_invalidation_;
  pending_render_invalidation_ = RenderInvalidation{};
  return result;
}

void WorkspaceShell::RequestFullRedraw() {
  pending_render_invalidation_.full = true;
  pending_render_invalidation_.rects.clear();
  QueueEditorHoverRefresh();
}

void WorkspaceShell::RequestRedrawRect(const SDL_FRect& rect) {
  if (pending_render_invalidation_.full || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }
  pending_render_invalidation_.rects.push_back(rect);
  QueueEditorHoverRefresh();
}

void WorkspaceShell::RequestWindowRedraw() {
  if (const auto window_rect = CurrentWindowRect(); window_rect.has_value()) {
    RequestRedrawRect(*window_rect);
  } else {
    RequestFullRedraw();
  }
}

void WorkspaceShell::RequestChromeRedraw() {
  if (const auto rect = CurrentChromeRedrawRect(); rect.has_value()) {
    RequestRedrawRect(*rect);
  } else {
    RequestWindowRedraw();
  }
}

void WorkspaceShell::RequestSidebarRedraw() {
  if (const auto layout = CurrentWorkspaceLayout(); layout.has_value() && context_.current_project_state.sidebar.visible) {
    RequestRedrawRect(layout->sidebar);
    return;
  }
  RequestWindowRedraw();
}

void WorkspaceShell::RequestSidebarLayoutChangeRedraw(
    const WorkspaceLayout& previous_layout) {
  MarkLayoutDirty();
  const auto current_layout = CurrentWorkspaceLayout();
  if (!current_layout.has_value()) {
    RequestWindowRedraw();
    return;
  }

  if (RectsEqual(previous_layout.sidebar, current_layout->sidebar) &&
      RectsEqual(previous_layout.content, current_layout->content)) {
    return;
  }

  // Sidebar dragging changes the outer workspace geometry. Repaint the whole scene while the
  // divider is moving so retained redraw does not leave stale pixels in the newly exposed area.
  RequestFullRedraw();
}

void WorkspaceShell::RequestBreadcrumbRedraw() {
  if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
    RequestRedrawRect(layout->breadcrumb);
    return;
  }
  RequestWindowRedraw();
}

void WorkspaceShell::RequestTabStripRedraw() {
  if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
    RequestRedrawRect(layout->tab_strip);
    return;
  }
  RequestWindowRedraw();
}

void WorkspaceShell::RequestEditorSurfaceRedraw() {
  if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
    RequestRedrawRect(layout->editor_surface);
    return;
  }
  RequestWindowRedraw();
}

void WorkspaceShell::RequestActiveTabRedraw(bool include_tree_sidebar) {
  RequestBreadcrumbRedraw();
  RequestTabStripRedraw();
  RequestEditorSurfaceRedraw();
  if (include_tree_sidebar && context_.current_project_state.sidebar.visible && ActiveSidebarMode() == SidebarMode::Tree) {
    RequestSidebarRedraw();
  }
}

void WorkspaceShell::RequestFocusedEditorRedraw() {
  if (const auto rect = CurrentFocusedEditorRedrawRect(); rect.has_value()) {
    RequestRedrawRect(*rect);
    return;
  }
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::RequestEditorLineRangeRedraw(std::size_t start_line, std::size_t end_line) {
  if (const auto rect = CurrentEditorLineRangeRect(start_line, end_line); rect.has_value()) {
    RequestRedrawRect(*rect);
    return;
  }
  RequestFocusedEditorRedraw();
}

void WorkspaceShell::RequestEditorLineToBottomRedraw(std::size_t start_line) {
  if (const auto rect = CurrentEditorLineToBottomRect(start_line); rect.has_value()) {
    RequestRedrawRect(*rect);
    return;
  }
  RequestFocusedEditorRedraw();
}

void WorkspaceShell::RequestActiveEditableChangeRedraw(const std::vector<std::string>& before_lines,
                                                       const std::vector<std::string>& after_lines) {
  SyncLspForActiveEditableChange(before_lines, after_lines);
  const auto changed_span = ComputeChangedLineSpan(before_lines, after_lines);
  if (!changed_span.has_value()) {
    RequestFocusedEditorRedraw();
    return;
  }

  const std::size_t start_line = changed_span->old_start;
  if (before_lines.size() != after_lines.size()) {
    RequestFocusedEditorRedraw();
    return;
  }

  if (ActiveTabIsCompare()) {
    RequestCompareRightLineRangeRedraw(start_line, std::max(changed_span->new_end, start_line + 1));
    return;
  }

  if (ActiveTabIsMerge()) {
    RequestMergeResultLineRangeRedraw(start_line, std::max(changed_span->new_end, start_line + 1));
    return;
  }

  RequestEditorLineRangeRedraw(start_line, std::max(changed_span->new_end, start_line + 1));
}

void WorkspaceShell::RequestActiveEditableLastChangeRedraw() {
  util::PerformanceTrace::Scope perf_scope(
      "WorkspaceShell::RequestActiveEditableLastChangeRedraw");
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr) {
    RequestFocusedEditorRedraw();
    return;
  }

  {
    util::PerformanceTrace::Scope scope(
        "WorkspaceShell::RequestActiveEditableLastChangeRedraw::SyncLsp");
    SyncLspForActiveEditableLastChange();
  }
  const auto& applied_edit = viewport->last_applied_edit();
  if (!applied_edit.has_value()) {
    util::PerformanceTrace::Scope scope(
        "WorkspaceShell::RequestActiveEditableLastChangeRedraw::FallbackFullRedraw");
    RequestFocusedEditorRedraw();
    return;
  }

  std::size_t start_line = 0;
  std::size_t removed_lines = 0;
  std::size_t inserted_lines = 0;
  {
    util::PerformanceTrace::Scope scope(
        "WorkspaceShell::RequestActiveEditableLastChangeRedraw::ComputeChangedSpan");
    start_line = applied_edit->range_before.start.line;
    removed_lines =
        applied_edit->range_before.end.line >= applied_edit->range_before.start.line
            ? applied_edit->range_before.end.line - applied_edit->range_before.start.line
            : 0;
    inserted_lines = static_cast<std::size_t>(
        std::count(applied_edit->replacement_text.begin(), applied_edit->replacement_text.end(), '\n'));
  }
  if (removed_lines != inserted_lines) {
    util::PerformanceTrace::Scope redraw_scope(
        "WorkspaceShell::RequestActiveEditableLastChangeRedraw::FallbackLineCountMismatch");
    RequestFocusedEditorRedraw();
    return;
  }

  const std::size_t end_line = start_line + inserted_lines + 1;
  if (ActiveTabIsCompare()) {
    util::PerformanceTrace::Scope redraw_scope(
        "WorkspaceShell::RequestActiveEditableLastChangeRedraw::ComparePartialRedraw");
    RequestCompareRightLineRangeRedraw(start_line, end_line);
    return;
  }
  if (ActiveTabIsMerge()) {
    util::PerformanceTrace::Scope redraw_scope(
        "WorkspaceShell::RequestActiveEditableLastChangeRedraw::MergePartialRedraw");
    RequestMergeResultLineRangeRedraw(start_line, end_line);
    return;
  }
  util::PerformanceTrace::Scope redraw_scope(
      "WorkspaceShell::RequestActiveEditableLastChangeRedraw::EditorPartialRedraw");
  RequestEditorLineRangeRedraw(start_line, end_line);
}

void WorkspaceShell::RequestActiveEditableBlameNeighborhoodRedraw(std::size_t before_line,
                                                                  std::size_t after_line) {
  util::PerformanceTrace::Scope perf_scope(
      "WorkspaceShell::RequestActiveEditableBlameNeighborhoodRedraw");
  const std::size_t min_line = std::min(before_line, after_line);
  const std::size_t max_line = std::max(before_line, after_line);
  const std::size_t start_line =
      min_line > kBlameNeighborhoodRadius ? min_line - kBlameNeighborhoodRadius : 0;
  const std::size_t end_line = max_line + kBlameNeighborhoodRadius + 1;

  if (ActiveTabIsCompare()) {
    util::PerformanceTrace::Scope redraw_scope(
        "WorkspaceShell::RequestActiveEditableBlameNeighborhoodRedraw::Compare");
    RequestCompareRightLineRangeRedraw(start_line, end_line);
    return;
  }
  if (ActiveTabIsMerge()) {
    util::PerformanceTrace::Scope redraw_scope(
        "WorkspaceShell::RequestActiveEditableBlameNeighborhoodRedraw::Merge");
    RequestMergeResultLineRangeRedraw(start_line, end_line);
    return;
  }
  util::PerformanceTrace::Scope redraw_scope(
      "WorkspaceShell::RequestActiveEditableBlameNeighborhoodRedraw::Editor");
  RequestEditorLineRangeRedraw(start_line, end_line);
}

void WorkspaceShell::RequestCompareRowRangeRedraw(std::size_t start_row, std::size_t end_row) {
  if (const auto rect = CurrentCompareRowRangeRect(start_row, end_row); rect.has_value()) {
    RequestRedrawRect(*rect);
    return;
  }
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::RequestCompareRowToBottomRedraw(std::size_t start_row) {
  if (const auto rect = CurrentCompareRowToBottomRect(start_row); rect.has_value()) {
    RequestRedrawRect(*rect);
    return;
  }
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::RequestCompareRightLineRangeRedraw(std::size_t start_line,
                                                        std::size_t end_line) {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr) {
    RequestFocusedEditorRedraw();
    return;
  }
  const std::size_t start_row = CompareRowIndexForRightLine(*compare_tab, start_line);
  const std::size_t end_lookup_line =
      end_line > start_line ? end_line - 1 : start_line;
  const std::size_t end_row = CompareRowIndexForRightLine(*compare_tab, end_lookup_line) + 1;
  RequestCompareRowRangeRedraw(start_row, std::max(start_row + 1, end_row));
}

void WorkspaceShell::RequestCompareRightLineToBottomRedraw(std::size_t start_line) {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr) {
    RequestFocusedEditorRedraw();
    return;
  }
  RequestCompareRowToBottomRedraw(CompareRowIndexForRightLine(*compare_tab, start_line));
}

void WorkspaceShell::RequestMergeResultLineRangeRedraw(std::size_t start_line,
                                                       std::size_t end_line) {
  if (const auto rect = CurrentMergeResultLineRangeRect(start_line, end_line); rect.has_value()) {
    RequestRedrawRect(*rect);
    return;
  }
  RequestFocusedEditorRedraw();
}

void WorkspaceShell::RequestMergeResultLineToBottomRedraw(std::size_t start_line) {
  if (const auto rect = CurrentMergeResultLineToBottomRect(start_line); rect.has_value()) {
    RequestRedrawRect(*rect);
    return;
  }
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::RequestMergeConflictRedraw(std::size_t conflict_index) {
  if (const auto rect = CurrentMergeConflictRect(conflict_index); rect.has_value()) {
    RequestRedrawRect(*rect);
    return;
  }
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::RequestBottomPanelRedraw() {
  if (const auto layout = CurrentWorkspaceLayout(); layout.has_value() && BottomPanelVisible()) {
    RequestRedrawRect(layout->bottom_panel);
    return;
  }
  RequestWindowRedraw();
}

void WorkspaceShell::RequestBottomPanelCommandRedraw() {
  if (const auto rect = CurrentBottomPanelCommandRedrawRect(); rect.has_value()) {
    RequestRedrawRect(*rect);
    return;
  }
  RequestBottomPanelRedraw();
}

void WorkspaceShell::RequestBottomPanelLayoutChangeRedraw(
    const WorkspaceLayout& previous_layout) {
  MarkLayoutDirty();
  const auto current_layout = CurrentWorkspaceLayout();
  if (!current_layout.has_value()) {
    RequestWindowRedraw();
    return;
  }

  if (RectsEqual(previous_layout.content, current_layout->content) &&
      RectsEqual(previous_layout.bottom_panel, current_layout->bottom_panel)) {
    return;
  }

  // Bottom-panel resize changes multiple surface boundaries at once. Until retained redraw
  // can prove equivalence here, fall back to a full redraw for correctness.
  RequestFullRedraw();
}

void WorkspaceShell::RequestCommandModeTransitionRedraw(bool bottom_panel_was_visible) {
  if (bottom_panel_was_visible != BottomPanelVisible()) {
    MarkLayoutDirty();
    RequestFullRedraw();
    return;
  }
  RequestBottomPanelRedraw();
}

void WorkspaceShell::RequestBottomPanelContentRedraw() {
  if (const auto rect = CurrentBottomPanelContentRedrawRect(); rect.has_value()) {
    RequestRedrawRect(*rect);
    return;
  }
  RequestBottomPanelRedraw();
}

void WorkspaceShell::RequestOverlayRedraw() {
  if (const auto rect = CurrentOverlayRedrawRect(); rect.has_value()) {
    RequestRedrawRect(*rect);
    return;
  }
  RequestWindowRedraw();
}

void WorkspaceShell::RequestPromptRedraw() {
  if (const auto rect = CurrentPromptRedrawRect(); rect.has_value()) {
    RequestRedrawRect(*rect);
    return;
  }
  RequestWindowRedraw();
}

void WorkspaceShell::QueueEditorHoverRefresh() {
  editor_hover_refresh_pending_ = last_mouse_position_valid_;
}

std::optional<SDL_FRect> WorkspaceShell::CurrentChromeRedrawRect() const {
  const auto layout = CurrentWorkspaceLayout();
  if (!layout.has_value()) {
    return std::nullopt;
  }

  std::optional<SDL_FRect> rect = layout->menu_bar;
  if (context_.menu_state.menu_bar_open) {
    if (const auto popup_rect = ComputePopupMenuRect(layout->menu_bar, context_.menu_state.active_menu_id);
        popup_rect.has_value()) {
      rect = UnionRects(rect, *popup_rect);
    }
    if (const auto submenu_rect = ActiveSubmenuRect(layout->menu_bar);
        submenu_rect.has_value()) {
      rect = UnionRects(rect, *submenu_rect);
    }
  }
  if (context_.menu_state.overflow_popup_open &&
      context_.menu_state.overflow_popup_anchor_rect.has_value()) {
    const auto overflow = ComputeOverflowMenuBarItems(layout->menu_bar);
    rect = UnionRects(rect, ComputeMenuOverflowPopupRect(
                                *context_.menu_state.overflow_popup_anchor_rect, overflow.size()));
  }
  if (context_.menu_state.tree_context_menu.open) {
    if (const auto tree_menu_rect = ComputeTreeContextMenuRect(); tree_menu_rect.has_value()) {
      rect = UnionRects(rect, *tree_menu_rect);
    }
  }
  return rect;
}

std::optional<SDL_FRect> WorkspaceShell::CurrentFocusedEditorRedrawRect() const {
  const auto layout = CurrentWorkspaceLayout();
  if (!layout.has_value()) {
    return std::nullopt;
  }

  if (ActiveTabIsCompare()) {
    CompareTabState* compare_tab = const_cast<WorkspaceShell*>(this)->ActiveCompareTab();
    if (compare_tab == nullptr || !compare_tab->right_editable || !compare_tab->right_view_active) {
      return std::nullopt;
    }
    const CompareSurfaceLayout surface_layout =
        ComputeCompareSurfaceLayout(layout->editor_surface, *compare_tab);
    return BuildCompareRightInteractionLayout(surface_layout, *compare_tab).rect;
  }

  if (ActiveTabIsMerge()) {
    const MergeTabState* merge_tab = ActiveMergeTab();
    if (merge_tab == nullptr) {
      return std::nullopt;
    }
    const MergeSurfaceLayout surface_layout =
        ComputeMergeSurfaceLayout(layout->editor_surface, *merge_tab);
    return ComputeMergeResultViewportRect(layout->editor_surface, surface_layout.center_x,
                                          surface_layout.rows_y, surface_layout.gutter_width,
                                          surface_layout.center_width,
                                          surface_layout.show_horizontal);
  }

  if (!ActiveTabIsEditor()) {
    return std::nullopt;
  }

  auto* editor_tab = const_cast<WorkspaceShell*>(this)->ActiveEditorTab();
  if (editor_tab == nullptr) {
    return layout->editor_surface;
  }
  const_cast<WorkspaceShell*>(this)->NormalizeEditorSplitTree(*editor_tab);
  const auto panes = ComputeEditorPaneLayouts(layout->editor_surface);
  const auto active_pane =
      std::find_if(panes.begin(), panes.end(), [](const EditorPaneLayout& pane) { return pane.active; });
  return active_pane != panes.end() ? std::optional<SDL_FRect>(active_pane->rect)
                                    : std::optional<SDL_FRect>(layout->editor_surface);
}

std::optional<SDL_FRect> WorkspaceShell::CurrentEditorLineRangeRect(std::size_t start_line,
                                                                    std::size_t end_line) const {
  if (!ActiveTabIsEditor()) {
    return std::nullopt;
  }

  const auto layout = CurrentWorkspaceLayout();
  if (!layout.has_value()) {
    return std::nullopt;
  }

  auto* editor_tab = const_cast<WorkspaceShell*>(this)->ActiveEditorTab();
  if (editor_tab != nullptr) {
    const_cast<WorkspaceShell*>(this)->NormalizeEditorSplitTree(*editor_tab);
  }
  const auto panes = ComputeEditorPaneLayouts(layout->editor_surface);
  const auto active_pane =
      std::find_if(panes.begin(), panes.end(), [](const EditorPaneLayout& pane) { return pane.active; });
  const SDL_FRect pane_rect =
      active_pane != panes.end() ? active_pane->rect : layout->editor_surface;
  const editor::TextViewport* viewport = ActiveEditorViewport();
  if (viewport == nullptr) {
    return std::nullopt;
  }
  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer_, *viewport, pane_rect);
  const std::size_t visible_start_row = viewport->scroll_line();
  const std::size_t visible_end_row = visible_start_row + metrics.visible_rows;
  const std::size_t start_row = viewport->VisualRowForLine(start_line);
  const std::size_t end_row = end_line == 0
                                  ? start_row + 1
                                  : viewport->VisualRowForLine(end_line - 1) + 1;
  if (end_row <= visible_start_row || start_row >= visible_end_row) {
    return std::nullopt;
  }
  const std::size_t clipped_start = std::max(start_row, visible_start_row);
  const std::size_t clipped_end = std::min(end_row, visible_end_row);
  return MakeRect(pane_rect.x,
                  metrics.first_line_y +
                      static_cast<float>(clipped_start - visible_start_row) * metrics.line_height -
                      1.0f,
                  pane_rect.w,
                  static_cast<float>(clipped_end - clipped_start) * metrics.line_height);
}

std::optional<SDL_FRect> WorkspaceShell::CurrentEditorLineToBottomRect(std::size_t start_line) const {
  const auto line_rect = CurrentEditorLineRangeRect(start_line, std::numeric_limits<std::size_t>::max());
  if (!line_rect.has_value()) {
    return std::nullopt;
  }
  const SDL_FRect pane_rect = CurrentFocusedEditorRedrawRect().value_or(*line_rect);
  return MakeRect(pane_rect.x, line_rect->y, pane_rect.w,
                  std::max(0.0f, pane_rect.y + pane_rect.h - line_rect->y));
}

std::optional<SDL_FRect> WorkspaceShell::CurrentBottomPanelContentRedrawRect() const {
  const auto layout = CurrentWorkspaceLayout();
  if (!layout.has_value() || !BottomPanelVisible()) {
    return std::nullopt;
  }
  return BottomPanelContentRect(*layout, context_.current_project_state.panel.command_mode);
}

std::optional<SDL_FRect> WorkspaceShell::CurrentBottomPanelCommandRedrawRect() const {
  const auto layout = CurrentWorkspaceLayout();
  if (!layout.has_value() || !context_.current_project_state.panel.command_mode) {
    return std::nullopt;
  }
  return BottomPanelCommandAreaRect(*layout);
}

std::optional<SDL_FRect> WorkspaceShell::CurrentOverlayRedrawRect() const {
  const auto layout = CurrentWorkspaceLayout();
  if (!layout.has_value() || !context_.current_project_state.overlay.visible) {
    return std::nullopt;
  }
  return ComputeOverlayRect(layout->editor_area);
}

std::optional<SDL_FRect> WorkspaceShell::CurrentPromptRedrawRect() const {
  const auto window_rect = CurrentWindowRect();
  if (!window_rect.has_value()) {
    return std::nullopt;
  }

  std::optional<SDL_FRect> rect;
  if (context_.prompts.dirty_visible) {
    rect = ComputeDirtyPromptRect(*window_rect);
  }
  if (context_.prompts.surface_visible) {
    rect = UnionRects(rect, ComputePromptSurfaceRect(*window_rect));
  }
  return rect;
}

std::optional<WorkspaceShell::ChangedLineSpan> WorkspaceShell::ComputeChangedLineSpan(
    const std::vector<std::string>& before_lines,
    const std::vector<std::string>& after_lines) {
  std::size_t prefix = 0;
  while (prefix < before_lines.size() && prefix < after_lines.size() &&
         before_lines[prefix] == after_lines[prefix]) {
    ++prefix;
  }
  if (prefix == before_lines.size() && prefix == after_lines.size()) {
    return std::nullopt;
  }

  std::size_t suffix = 0;
  while (suffix < before_lines.size() - prefix && suffix < after_lines.size() - prefix &&
         before_lines[before_lines.size() - 1 - suffix] ==
             after_lines[after_lines.size() - 1 - suffix]) {
    ++suffix;
  }

  return ChangedLineSpan{
      .old_start = prefix,
      .old_end = before_lines.size() - suffix,
      .new_end = after_lines.size() - suffix,
  };
}

std::optional<Uint32> WorkspaceShell::NextCaretBlinkDelayMs() const {
  if (!ShouldBlinkCaret()) {
    return std::nullopt;
  }

  const Uint64 elapsed = SDL_GetTicks() - caret_blink_epoch_ms_;
  if (elapsed >= kCaretBlinkIdleStopMs) {
    return std::nullopt;
  }
  const Uint64 remaining = kCaretBlinkIntervalMs - (elapsed % kCaretBlinkIntervalMs);
  const Uint64 until_freeze = kCaretBlinkIdleStopMs - elapsed;
  return static_cast<Uint32>(std::max<Uint64>(1, std::min(remaining, until_freeze)));
}

std::optional<Uint32> WorkspaceShell::NextAnimationDelayMs() const {
  std::optional<Uint32> next_delay = NextCaretBlinkDelayMs();
  if (const auto plugin_delay = plugin_runtime_.NextPollDelay(); plugin_delay.has_value()) {
    const Uint32 plugin_delay_ms =
        static_cast<Uint32>(std::max<std::int64_t>(0, plugin_delay->count()));
    if (!next_delay.has_value() || plugin_delay_ms < *next_delay) {
      next_delay = plugin_delay_ms;
    }
  }
  if (const auto project_delay = project_file_monitor_.NextPollDelay(); project_delay.has_value()) {
    const Uint32 project_delay_ms =
        static_cast<Uint32>(std::max<std::int64_t>(0, project_delay->count()));
    if (!next_delay.has_value() || project_delay_ms < *next_delay) {
      next_delay = project_delay_ms;
    }
  }
  return next_delay;
}

WorkspaceShell::IdleWaitState WorkspaceShell::CurrentIdleWaitState() const {
  if (const auto next_delay = NextAnimationDelayMs(); next_delay.has_value()) {
    return IdleWaitState{
        .hint = IdleHint::CaretOnly,
        .caret_remaining_ms = std::max<Uint32>(1, *next_delay),
    };
  }

  return IdleWaitState{
      .hint = IdleHint::Idle,
      .caret_remaining_ms = 0,
  };
}

bool WorkspaceShell::ReloadProjectIfFilesChanged(bool force_check) {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::ReloadProjectIfFilesChanged");
  project_file_event_pending_.store(false, std::memory_order_release);
  const bool index_changed = [&]() {
    util::PerformanceTrace::Scope scope(
        "WorkspaceShell::ReloadProjectIfFilesChanged::ConsumeIndexFlag");
    return force_check ? false
                       : file_index_has_pending_changes_.exchange(false, std::memory_order_acq_rel);
  }();
  const bool changed = [&]() {
    util::PerformanceTrace::Scope scope(
        force_check ? "WorkspaceShell::ReloadProjectIfFilesChanged::ConsumePendingProjectChanges"
                    : "WorkspaceShell::ReloadProjectIfFilesChanged::PollProjectFileMonitor");
    return force_check ? project_file_monitor_.ConsumePendingChanges()
                       : project_file_monitor_.PollForChanges();
  }();
  if (TraceProjectEventsEnabled()) {
    const std::string root_text = context_.current_project_state.root.string();
    const std::string active_path = context_.current_project_state.welcome_surface.viewport.path().string();
    SDL_Log(
        "microide project: reload-check root=%s force=%d content_changed=%d index_changed=%d active=%s",
        root_text.empty() ? "-" : root_text.c_str(), force_check ? 1 : 0, changed ? 1 : 0,
        index_changed ? 1 : 0, active_path.empty() ? "-" : active_path.c_str());
  }
  if (!changed && !index_changed) {
    return false;
  }

  {
    util::PerformanceTrace::Scope scope(
        "WorkspaceShell::ReloadProjectIfFilesChanged::RefreshDirectoryTree");
    context_.current_project_state.directory_tree.Refresh();
  }
  {
    util::PerformanceTrace::Scope scope(
        "WorkspaceShell::ReloadProjectIfFilesChanged::RefreshFileFinder");
    context_.current_project_state.file_finder.InvalidateIndexCache();
    if (context_.current_project_state.overlay.visible &&
        context_.current_project_state.overlay.mode == OverlayMode::FileFinder) {
      context_.current_project_state.file_finder.Refresh();
    }
  }
  if (changed) {
    util::PerformanceTrace::Scope scope(
        "WorkspaceShell::ReloadProjectIfFilesChanged::ReloadCleanOpenBuffersFromDisk");
    ReloadCleanOpenBuffersFromDisk();
  }
  RequestAutomaticGitSidebarRefresh();
  return true;
}

WorkspaceShell::EventResult WorkspaceShell::HandleScheduledWake() {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::HandleScheduledWake");
  if (plugin_runtime_.PendingAsyncProcessCount() > 0 && ConsumePluginAsyncProcessCallbacks()) {
    return EventResult{
        .handled = true,
        .redraw = RenderInvalidation{
            .full = true,
            .rects = {},
        },
    };
  }
  if (ReloadProjectIfFilesChanged(false)) {
    return EventResult{
        .handled = true,
        .redraw = RenderInvalidation{
            .full = true,
            .rects = {},
        },
    };
  }
  return Bootstrapper(*this).BuildWakeController().HandleScheduledWake();
}

bool WorkspaceShell::ConsumePostRenderFullRedrawRequest() {
  if (post_render_full_redraws_remaining_ <= 0) {
    return false;
  }
  --post_render_full_redraws_remaining_;
  return true;
}

void WorkspaceShell::ResetCaretBlink() {
  caret_blink_epoch_ms_ = SDL_GetTicks();
}

bool WorkspaceShell::CaretBlinkAnimating() const {
  if (!ShouldBlinkCaret()) {
    return false;
  }
  return (SDL_GetTicks() - caret_blink_epoch_ms_) < kCaretBlinkIdleStopMs;
}

bool WorkspaceShell::ShouldBlinkCaret() const {
  if (context_.prompts.dirty_visible || context_.menu_state.menu_bar_open ||
      context_.menu_state.tree_context_menu.open) {
    return false;
  }

  switch (CurrentTextInputSurface()) {
    case TextInputSurface::PromptInput:
    case TextInputSurface::Command:
    case TextInputSurface::FileFinder:
    case TextInputSurface::BufferSearch:
    case TextInputSurface::BufferReplaceSearch:
    case TextInputSurface::BufferReplaceReplace:
    case TextInputSurface::ProjectSearchOverlay:
    case TextInputSurface::CommitPicker:
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace:
      return true;
    case TextInputSurface::None:
    case TextInputSurface::Editor:
    case TextInputSurface::Terminal:
      break;
  }

  if (context_.current_project_state.surface.focus == FocusTarget::Editor) {
    const editor::TextViewport* viewport = ActiveNavigableViewport();
    return viewport != nullptr && !viewport->is_placeholder();
  }

  if (context_.current_project_state.surface.focus == FocusTarget::Panel) {
    return BottomPanelShowsTerminal() && ActiveTerminalTab() != nullptr;
  }

  return false;
}

bool WorkspaceShell::CaretVisibleNow() const {
  if (!ShouldBlinkCaret()) {
    return false;
  }

  const Uint64 elapsed = SDL_GetTicks() - caret_blink_epoch_ms_;
  if (elapsed >= kCaretBlinkIdleStopMs) {
    return true;
  }
  return ((elapsed / kCaretBlinkIntervalMs) % 2) == 0;
}

std::optional<SDL_FRect> WorkspaceShell::CurrentCaretDirtyRect() const {
  if (!ShouldBlinkCaret()) {
    return std::nullopt;
  }

  const auto layout = CurrentWorkspaceLayout();
  if (!layout.has_value()) {
    return std::nullopt;
  }

  if (const auto surface = CurrentTextInputSurface();
      surface != TextInputSurface::None && surface != TextInputSurface::Editor &&
      surface != TextInputSurface::Terminal) {
    const auto visual = BuildActiveTextInputVisual(*layout, std::nullopt);
    return visual.has_value()
               ? std::optional<SDL_FRect>(MakeRect(visual->cursor_x, visual->text_y - 1.0f,
                                                   std::max(1.0f, text_renderer_.CharWidth()),
                                                   text_renderer_.LineHeight()))
               : std::nullopt;
  }

  if (context_.current_project_state.surface.focus == FocusTarget::Editor) {
    return ActiveEditorCaretRect(*layout);
  }
  if (context_.current_project_state.surface.focus == FocusTarget::Panel) {
    return ActiveTerminalCaretRect(*layout);
  }
  return std::nullopt;
}

std::optional<SDL_FRect> WorkspaceShell::ActiveEditorCaretRect(const WorkspaceLayout& layout) const {
  if (ActiveTabIsCompare()) {
    const auto visual = BuildCompareTextInputVisual(layout.editor_surface);
    return visual.has_value() ? std::optional<SDL_FRect>(visual->area) : std::nullopt;
  }
  if (ActiveTabIsMerge()) {
    const auto visual = BuildMergeTextInputVisual(layout.editor_surface);
    return visual.has_value() ? std::optional<SDL_FRect>(visual->area) : std::nullopt;
  }

  const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
  auto pane_it = std::find_if(panes.begin(), panes.end(),
                              [](const EditorPaneLayout& pane) { return pane.active; });
  if (pane_it == panes.end()) {
    return context_.current_project_state.welcome_surface.viewport.is_placeholder() ? std::optional<SDL_FRect>(layout.editor_surface)
                                           : std::nullopt;
  }
  return pane_it->rect;

  (void)text_renderer_;
}

std::optional<SDL_FRect> WorkspaceShell::ActiveTerminalCaretRect(const WorkspaceLayout& layout) const {
  const auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return std::nullopt;
  }
  const terminal::TerminalCursorSnapshot cursor = terminal_tab->session.CursorSnapshot();
  if (!cursor.visible) {
    return std::nullopt;
  }

  const std::size_t line_count = terminal_tab->session.LineCount();
  const BottomPanelLogLayout panel_layout = ComputeBottomPanelLogLayout(layout, line_count);
  if (cursor.row < static_cast<std::size_t>(panel_layout.scroll.vertical_scroll) ||
      cursor.row >= static_cast<std::size_t>(panel_layout.scroll.vertical_scroll +
                                            panel_layout.scroll.visible_rows)) {
    return std::nullopt;
  }

  const float char_width = std::max(1.0f, text_renderer_.CharWidth());
  const float cursor_x = panel_layout.text_x + static_cast<float>(cursor.column) * char_width;
  const float cursor_y =
      panel_layout.text_y +
      static_cast<float>(cursor.row -
                         static_cast<std::size_t>(panel_layout.scroll.vertical_scroll)) *
          panel_layout.line_height;
  if (cursor_x > panel_layout.content_rect.x + panel_layout.content_rect.w - char_width) {
    return std::nullopt;
  }

  return MakeRect(cursor_x, cursor_y - 1.0f, char_width, panel_layout.line_height);
}

ScrollSurfaceLayout WorkspaceShell::ComputeEditorScrollLayout(
    const SDL_FRect& rect,
    const editor::TextViewport& viewport,
    const editor::EditorViewMetrics& metrics) const {
  // Soft-wrap reflows content into the visible window, so horizontal scrolling
  // is meaningless; collapse `total_columns` to `visible_columns` so the
  // scrollbar geometry computes `max_horizontal_scroll == 0` and the bar is
  // hidden entirely.
  const std::size_t total_columns =
      viewport.soft_wrap()
          ? metrics.visible_columns
          : std::max<std::size_t>(metrics.visible_columns, MaxVisualColumns(viewport));
  return ComputeScrollSurfaceLayout(rect, viewport.line_count(),
                                    static_cast<int>(metrics.visible_rows),
                                    static_cast<int>(viewport.scroll_line()), total_columns,
                                    metrics.visible_columns, viewport.horizontal_scroll());
}

editor::TextViewport* WorkspaceShell::ActiveEditableViewport() {
  if (ActiveTabIsCompare()) {
    auto* compare_tab = ActiveCompareTab();
    return compare_tab == nullptr || !compare_tab->right_editable || !compare_tab->right_view_active
               ? nullptr
               : &compare_tab->right_viewport;
  }
  if (ActiveTabIsMerge()) {
    auto* merge_tab = ActiveMergeTab();
    return merge_tab == nullptr ? nullptr : &merge_tab->result_viewport;
  }
  if (!ActiveTabIsEditor() || ActiveTabIsCompare()) {
    return nullptr;
  }
  editor::TextViewport* viewport = ActiveEditorViewport();
  return viewport != nullptr && IsReadOnlyVirtualDocument(viewport->path()) ? nullptr : viewport;
}

const editor::TextViewport* WorkspaceShell::ActiveEditableViewport() const {
  if (ActiveTabIsCompare()) {
    const auto* compare_tab = ActiveCompareTab();
    return compare_tab == nullptr || !compare_tab->right_editable || !compare_tab->right_view_active
               ? nullptr
               : &compare_tab->right_viewport;
  }
  if (ActiveTabIsMerge()) {
    const auto* merge_tab = ActiveMergeTab();
    return merge_tab == nullptr ? nullptr : &merge_tab->result_viewport;
  }
  if (!ActiveTabIsEditor() || ActiveTabIsCompare()) {
    return nullptr;
  }
  const editor::TextViewport* viewport = ActiveEditorViewport();
  return viewport != nullptr && IsReadOnlyVirtualDocument(viewport->path()) ? nullptr : viewport;
}

}  // namespace microide::workspace
