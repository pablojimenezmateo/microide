#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "app/BackgroundTaskCounter.h"
#include "compare/ComparePresentationModel.h"
#include "util/PerformanceTrace.h"
#include "workspace/TabStripAnimation.h"
#include "workspace/WorkspaceShellBootstrapper.h"

namespace microide::workspace {

namespace {

constexpr std::size_t kBlameNeighborhoodRadius = 1;

bool TraceProjectEventsEnabled() {
  static const bool enabled =
      util::PerformanceTrace::FlagEnabled("MICROIDE_TRACE_PROJECT_EVENTS");
  return enabled;
}

std::size_t MaxVisualColumns(const editor::TextViewport& viewport) {
  return viewport.max_visual_columns();
}

}  // namespace

void WorkspaceShell::InvalidateCursorKindFingerprint() {
  ++cursor_hit_generation_;
}

void WorkspaceShell::ForceCursorReassert() {
  force_cursor_reassert_ = true;
  InvalidateCursorKindFingerprint();
}

void WorkspaceShell::SeedPointerPosition(float render_x, float render_y) {
  last_mouse_x_ = render_x;
  last_mouse_y_ = render_y;
  last_mouse_position_valid_ = true;
  // The position moved without a motion event, so recompute the cursor and refresh
  // editor hover against the new location on the next prepared frame.
  QueueEditorHoverRefresh();
  InvalidateCursorKindFingerprint();
}

void WorkspaceShell::MarkLayoutDirty() {
  layout_dirty_ = true;
  InvalidateCursorKindFingerprint();
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
                       layout_mode_service_.StatusBarVisible(),
                       context_.current_project_state.debug_pane.visible,
                       context_.current_project_state.debug_pane.width);
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
  InvalidateCursorKindFingerprint();
  pending_render_invalidation_.full = true;
  pending_render_invalidation_.rects.clear();
  QueueEditorHoverRefresh();
}

void WorkspaceShell::Notify(NotificationService::Tone tone, std::string message) {
  notification_service_.Show(tone, std::move(message), SDL_GetTicks());
  // Notifications are infrequent, event-driven posts (never per-frame polling),
  // so a full redraw here never spins the CPU. The shell schedules a single wake
  // at the toast's expiry (see NextAnimationDelayMs) to retire it.
  RequestFullRedraw();
}

void WorkspaceShell::RequestRedrawRect(const SDL_FRect& rect) {
  if (pending_render_invalidation_.full || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }
  InvalidateCursorKindFingerprint();
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

void WorkspaceShell::RequestCursorPresent() {
  // A 1x1 damage rect at the pointer is the cheapest way to force
  // Application::Render to run and call SDL_RenderPresent. The retained scene
  // texture is re-blit whole on every present, so the rect's size is irrelevant to
  // correctness — it exists only to make the compositor recomposite and re-latch
  // the hardware cursor plane so an event-time cursor change actually shows.
  RequestRedrawRect(SDL_FRect{last_mouse_x_, last_mouse_y_, 1.0f, 1.0f});
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

  if (viewport->soft_wrap()) {
    util::PerformanceTrace::Scope redraw_scope(
        "WorkspaceShell::RequestActiveEditableLastChangeRedraw::SoftWrapFocusedPane");
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
  const std::size_t start_model_row = CompareRowIndexForRightLine(*compare_tab, start_line);
  const std::size_t end_lookup_line =
      end_line > start_line ? end_line - 1 : start_line;
  const std::size_t end_model_row = CompareRowIndexForRightLine(*compare_tab, end_lookup_line) + 1;
  const std::size_t start_row =
      compare::ComparePresentationModelRowIndex(compare_tab->presentation, start_model_row)
          .value_or(start_model_row);
  const std::size_t end_row =
      compare::ComparePresentationModelRowIndex(compare_tab->presentation, end_model_row - 1)
          .value_or(end_model_row - 1) +
      1;
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

void WorkspaceShell::RequestDebugPaneRedraw() {
  // The debug surfaces (Call Stack / Variables / Watch / Breakpoints) render in the
  // right-side dock (layout.right_pane), not the bottom panel. DAP callbacks deliver
  // variable/stack data asynchronously, so each must invalidate the dock it actually
  // paints into; targeting the bottom panel here would leave the right pane showing a
  // stale "Loading…" until some unrelated redraw repainted the window.
  if (const auto layout = CurrentWorkspaceLayout();
      layout.has_value() && context_.current_project_state.debug_pane.visible) {
    RequestRedrawRect(layout->right_pane);
    return;
  }
  RequestWindowRedraw();
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
      rect = UnionOptionalRects(rect, *popup_rect);
    }
    if (const auto submenu_rect = ActiveSubmenuRect(layout->menu_bar);
        submenu_rect.has_value()) {
      rect = UnionOptionalRects(rect, *submenu_rect);
    }
  }
  if (context_.menu_state.overflow_popup_open &&
      context_.menu_state.overflow_popup_anchor_rect.has_value()) {
    const auto overflow = ComputeOverflowMenuBarItems(layout->menu_bar);
    rect = UnionOptionalRects(rect, ComputeMenuOverflowPopupRect(
                                *context_.menu_state.overflow_popup_anchor_rect, overflow.size()));
  }
  if (context_.menu_state.tree_context_menu.open) {
    if (const auto tree_menu_rect = ComputeTreeContextMenuRect(); tree_menu_rect.has_value()) {
      rect = UnionOptionalRects(rect, *tree_menu_rect);
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

  if (const_cast<WorkspaceShell*>(this)->ActiveEditorTab() == nullptr) {
    return layout->editor_surface;
  }
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
  const std::size_t end_row =
      end_line == 0 ? start_row + 1
                    : end_line >= viewport->line_count() ? viewport->visual_line_count()
                                                         : viewport->VisualRowForLine(end_line);
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
  return BottomPanelContentRect(*layout);
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
    rect = UnionOptionalRects(rect, ComputePromptSurfaceRect(*window_rect));
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

std::optional<Uint32> WorkspaceShell::NextTabSlideDelayMs() const {
  const TabSlideState& slide = context_.interaction_state.tab_slide;
  if (slide.kind == TabDragKind::None) {
    return std::nullopt;
  }
  // During the post-release settle, keep waking until the animation clears itself
  // (AdvanceTabSlide snaps the last sub-pixel and resets the state). Otherwise the
  // final "land at rest" wake would never fire once motion falls below the snap
  // threshold, stranding a sub-pixel offset and a non-None kind.
  if (slide.settling) {
    return static_cast<Uint32>(16);  // ~60 fps
  }
  // Mid-drag: only keep waking while something is actually in motion. A drag whose
  // neighbors have settled (pointer held still) needs no wakes until the next
  // motion event moves the target again.
  if (!SlideOffsetsMoving(slide.current, slide.target)) {
    return std::nullopt;
  }
  return static_cast<Uint32>(16);  // ~60 fps
}

bool WorkspaceShell::AdvanceTabSlide() {
  TabSlideState& slide = context_.interaction_state.tab_slide;
  if (slide.kind == TabDragKind::None) {
    return false;
  }
  const Uint64 now = SDL_GetTicks();
  const Uint64 raw_dt = now >= slide.last_advance_ms ? now - slide.last_advance_ms : 0;
  // Cap the step so a long idle gap (animation had converged, then resumed)
  // can't teleport tabs; it settles in one brisk step instead.
  const float dt_ms = static_cast<float>(std::min<Uint64>(raw_dt, 100));
  slide.last_advance_ms = now;
  const bool moving = AdvanceSlideOffsets(slide.current, slide.target, dt_ms);
  if (!moving) {
    if (slide.settling) {
      // Post-release glide finished: drop the animation so every tab renders at
      // its base rect. Repaint once more to land cleanly at rest.
      slide = TabSlideState{};
      return true;
    }
    // Converged mid-drag with the pointer still: no repaint, no further wakes.
    return false;
  }
  return true;
}

std::optional<SDL_FRect> WorkspaceShell::CurrentTabSlideDirtyRect() const {
  const TabSlideState& slide = context_.interaction_state.tab_slide;
  if (slide.kind == TabDragKind::None) {
    return std::nullopt;
  }
  const auto layout = CurrentWorkspaceLayout();
  if (!layout.has_value()) {
    return std::nullopt;
  }
  switch (slide.kind) {
    case TabDragKind::Project:
      return layout->project_tab_strip;
    case TabDragKind::Editor: {
      const EditorGroupRectsLayout groups = ComputeEditorGroupRectsForState(*layout);
      if (slide.group_index < groups.groups.size()) {
        return groups.groups[slide.group_index].tab_strip;
      }
      return layout->tab_strip;
    }
    case TabDragKind::Terminal:
      return MakeRect(layout->bottom_panel.x, layout->bottom_panel.y, layout->bottom_panel.w,
                      kWorkspaceBottomPanelHeaderHeight);
    case TabDragKind::None:
      break;
  }
  return std::nullopt;
}

std::optional<Uint32> WorkspaceShell::NextAnimationDelayMs() const {
  std::optional<Uint32> next_delay = NextCaretBlinkDelayMs();
  if (const auto slide_delay = NextTabSlideDelayMs(); slide_delay.has_value()) {
    if (!next_delay.has_value() || *slide_delay < *next_delay) {
      next_delay = *slide_delay;
    }
  }
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
  if (const auto toast_delay = notification_service_.NextExpiryDelayMs(SDL_GetTicks());
      toast_delay.has_value()) {
    const Uint32 toast_delay_ms = static_cast<Uint32>(*toast_delay);
    if (!next_delay.has_value() || toast_delay_ms < *next_delay) {
      next_delay = toast_delay_ms;
    }
  }
  // Pending debounced editor events wake the loop near their deadline so the
  // coalesced plugin callback fires promptly after typing settles.
  if (const auto editor_event_delay = plugin_editor_event_tracker_.NextDelayMs(SDL_GetTicks());
      editor_event_delay.has_value()) {
    const Uint32 editor_event_delay_ms = static_cast<Uint32>(*editor_event_delay);
    if (!next_delay.has_value() || editor_event_delay_ms < *next_delay) {
      next_delay = editor_event_delay_ms;
    }
  }
  // "After delay" autosave wakes the loop near the debounce deadline so a settled
  // dirty buffer is written without any input to drive the save.
  if (const auto autosave_delay = NextAutosaveDelayMs(); autosave_delay.has_value()) {
    if (!next_delay.has_value() || *autosave_delay < *next_delay) {
      next_delay = *autosave_delay;
    }
  }
  return next_delay;
}

WorkspaceShell::IdleWaitState WorkspaceShell::CurrentIdleWaitState() const {
  std::optional<Uint32> wait_ms;
  if (const auto next_delay = NextAnimationDelayMs(); next_delay.has_value()) {
    wait_ms = std::max<Uint32>(1, *next_delay);
  }
  // While a debug-adapter request is in flight, poll on a short interval so the
  // async response is applied promptly. The response path already wakes the loop
  // via MainThreadMailbox::PushWake (SDL_PushEvent), but a cross-thread pushed
  // event is not a guaranteed wake for a blocking SDL_WaitEventTimeout on every
  // platform, so this poll is the backstop that stops scopes/variables/evaluate
  // from feeling frozen. 16 ms (~62 wakes/sec) was needlessly tight given the
  // wake usually fires: 64 ms cuts the idle-wake rate ~4x while keeping the
  // worst-case backstop latency (only hit if the wake is dropped) below the
  // ~100 ms perception threshold, so stepping still feels immediate.
  if (DebugEnabled() && debug_service_.HasInFlightDapWork()) {
    constexpr Uint32 kDapPollMs = 64;
    wait_ms = wait_ms.has_value() ? std::min(*wait_ms, kDapPollMs) : kDapPollMs;
  }
  if (wait_ms.has_value()) {
    return IdleWaitState{
        .hint = IdleHint::CaretOnly,
        .caret_remaining_ms = *wait_ms,
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
  const bool index_metadata_pending =
      file_index_has_pending_changes_.exchange(false, std::memory_order_acq_rel);

  project::ProjectChangeBatch supplemental;
  for (const project::RepositoryChange& change : git_metadata_tracker_.SampleChanges()) {
    supplemental.repository_changes.push_back(change);
  }

  const bool tree_polled = [&]() {
    util::PerformanceTrace::Scope scope(
        force_check ? "WorkspaceShell::ReloadProjectIfFilesChanged::ConsumePendingProjectChanges"
                    : "WorkspaceShell::ReloadProjectIfFilesChanged::PollProjectFileMonitor");
    return force_check ? project_file_monitor_.ConsumePendingChanges()
                       : project_file_monitor_.PollForChanges();
  }();
  if (tree_polled) {
    supplemental.tree_rescan_requested = true;
  }

  // Surface a one-time notice when the project tree is too large to live-watch
  // (native watch unavailable + polling suppressed). Done before the early returns
  // below so it fires even when there is no change batch to apply.
  if (project_file_monitor_.ConsumeTreeTooLargeNotice()) {
    Notify(NotificationService::Tone::Warning, "Project too large for live file watching");
  }

  if (!supplemental.repository_changes.empty() || supplemental.tree_rescan_requested) {
    project_change_coalescer_.Ingest(std::move(supplemental));
  }

  const std::optional<project::ProjectChangeBatch> ready =
      project_change_coalescer_.ConsumeReady();
  if (!ready.has_value()) {
    if (!index_metadata_pending) {
      return false;
    }
    context_.current_project_state.directory_tree.Refresh();
    context_.current_project_state.file_finder.InvalidateIndexCache();
    // A changed file set makes cached project-search results stale (e.g. a match
    // in a since-deleted file). Drop the cache marker so re-opening Search
    // re-runs against the new file set, and refresh live if Search is active.
    context_.current_project_state.overlay.workflow.project_search.searched_query.clear();
    if (ActiveSidebarMode() == SidebarMode::Search &&
        !context_.current_project_state.overlay.workflow.project_search.query.text().empty()) {
      RefreshProjectSearch();
    }
    if (context_.current_project_state.overlay.visible &&
        context_.current_project_state.overlay.mode == OverlayMode::FileFinder) {
      context_.current_project_state.file_finder.Refresh();
    }
    return true;
  }

  if (TraceProjectEventsEnabled()) {
    const std::string root_text = context_.current_project_state.root.string();
    SDL_Log(
        "microide project: apply-change-batch root=%s files=%zu repo=%zu tree_rescan=%d generation=%llu",
        root_text.empty() ? "-" : root_text.c_str(), ready->file_changes.size(),
        ready->repository_changes.size(), ready->tree_rescan_requested ? 1 : 0,
        static_cast<unsigned long long>(ready->generation));
  }

  ApplyProjectChangeBatch(*ready);
  return true;
}

WorkspaceShell::EventResult WorkspaceShell::HandleScheduledWake() {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::HandleScheduledWake");
  if (notification_service_.ExpireDue(SDL_GetTicks())) {
    return EventResult{
        .handled = true,
        .redraw = RenderInvalidation{
            .full = true,
            .rects = {},
        },
    };
  }
  // Drain results marshalled back from the plugin worker thread. Gated on a
  // lockless atomic so a project with no plugin work pays a single load here.
  if (plugin_runtime_.PendingPluginThreadActionCount() > 0 &&
      plugin_runtime_.DrainPluginThreadActions() > 0) {
    return EventResult{
        .handled = true,
        .redraw = RenderInvalidation{
            .full = true,
            .rects = {},
        },
    };
  }
  // "After delay" autosave fires once the debounce elapses. Disarm before saving so a
  // clean/manually-saved buffer does not busy-loop the wake; the save clears the dirty
  // marker, so a full redraw refreshes the tab's modified indicator.
  if (autosave_armed_ && (SDL_GetTicks() - autosave_edit_epoch_ms_) >= AutosaveDelayMs()) {
    autosave_armed_ = false;
    MaybeAutosaveDirtyTabs(false);
    return EventResult{
        .handled = true,
        .redraw = RenderInvalidation{
            .full = true,
            .rects = {},
        },
    };
  }
  // Debounced reactive editor events fire here once their deadline elapses. A full
  // redraw covers any decorations a handler republishes in response.
  if (DispatchDuePluginEditorEvents()) {
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
  // Step the Chrome-like tab-slide animation. Capture the dirty strip before
  // advancing so the settle-end frame (which clears the state) still repaints
  // the right region instead of falling back to a full redraw.
  if (context_.interaction_state.tab_slide.kind != TabDragKind::None) {
    const std::optional<SDL_FRect> slide_rect = CurrentTabSlideDirtyRect();
    if (AdvanceTabSlide()) {
      return EventResult{
          .handled = true,
          .redraw = slide_rect.has_value()
                        ? RenderInvalidation{.full = false, .rects = {*slide_rect}}
                        : RenderInvalidation{.full = true, .rects = {}},
      };
    }
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
  if (!ActiveTabIsEditor()) {
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
  if (!ActiveTabIsEditor()) {
    return nullptr;
  }
  const editor::TextViewport* viewport = ActiveEditorViewport();
  return viewport != nullptr && IsReadOnlyVirtualDocument(viewport->path()) ? nullptr : viewport;
}

}  // namespace microide::workspace
