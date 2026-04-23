#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "workspace/WorkspaceShellBootstrapper.h"

namespace microide::workspace {

namespace {

constexpr Uint64 kCaretBlinkIntervalMs = 530;
constexpr std::size_t kBlameNeighborhoodRadius = 1;

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

void WorkspaceShell::SetWindowPresentationState(WindowPresentationState state) {
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
                       context_.current_project_state.panel.height);
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
    if (ActiveTabIsCompare()) {
      RequestCompareRightLineToBottomRedraw(start_line);
      return;
    }

    if (ActiveTabIsMerge()) {
      RequestMergeResultLineToBottomRedraw(start_line);
      return;
    }

    RequestEditorLineToBottomRedraw(start_line);
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

void WorkspaceShell::RequestActiveEditableBlameNeighborhoodRedraw(std::size_t before_line,
                                                                  std::size_t after_line) {
  const std::size_t min_line = std::min(before_line, after_line);
  const std::size_t max_line = std::max(before_line, after_line);
  const std::size_t start_line =
      min_line > kBlameNeighborhoodRadius ? min_line - kBlameNeighborhoodRadius : 0;
  const std::size_t end_line = max_line + kBlameNeighborhoodRadius + 1;

  if (ActiveTabIsCompare()) {
    RequestCompareRightLineRangeRedraw(start_line, end_line);
    return;
  }
  if (ActiveTabIsMerge()) {
    RequestMergeResultLineRangeRedraw(start_line, end_line);
    return;
  }
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
  const VisibleLineRangeLayout line_layout = {
      .first_line_y = metrics.first_line_y,
      .line_height = metrics.line_height,
      .scroll_line = viewport->scroll_line(),
      .visible_rows = metrics.visible_rows,
  };
  return ComputeVisibleLineRangeRect(pane_rect, line_layout, start_line, end_line);
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
  const Uint64 remaining = kCaretBlinkIntervalMs - (elapsed % kCaretBlinkIntervalMs);
  return static_cast<Uint32>(std::max<Uint64>(1, remaining));
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
  if (plugin_runtime_.PendingAsyncProcessCount() > 0) {
    constexpr Uint32 kPluginAsyncPollDelayMs = 10;
    if (!next_delay.has_value() || kPluginAsyncPollDelayMs < *next_delay) {
      next_delay = kPluginAsyncPollDelayMs;
    }
  }
  return next_delay;
}

WorkspaceShell::EventResult WorkspaceShell::HandleScheduledWake() {
  if (plugin_runtime_.PendingAsyncProcessCount() > 0 && ConsumePluginAsyncProcessCallbacks()) {
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

bool WorkspaceShell::ShouldBlinkCaret() const {
  if (context_.prompts.dirty_visible || context_.menu_state.menu_bar_open ||
      context_.menu_state.tree_context_menu.open) {
    return false;
  }

  switch (CurrentTextInputSurface()) {
    case TextInputSurface::PromptInput:
    case TextInputSurface::Command:
    case TextInputSurface::ChatComposer:
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
    return context_.current_project_state.text_viewport.is_placeholder() ? std::optional<SDL_FRect>(layout.editor_surface)
                                           : std::nullopt;
  }

  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer_, context_.current_project_state.text_viewport, pane_it->rect);
  const float char_width = std::max(1.0f, text_renderer_.CharWidth());
  const float cursor_x =
      metrics.text_x +
      static_cast<float>(context_.current_project_state.text_viewport.cursor_visual_column() - context_.current_project_state.text_viewport.horizontal_scroll()) *
          char_width;
  const float cursor_y =
      metrics.first_line_y +
      static_cast<float>(context_.current_project_state.text_viewport.cursor_line() - context_.current_project_state.text_viewport.scroll_line()) *
          metrics.line_height;
  return MakeRect(cursor_x, cursor_y - 1.0f, char_width, metrics.line_height);
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
  const std::size_t total_columns =
      std::max<std::size_t>(metrics.visible_columns, MaxVisualColumns(viewport));
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
