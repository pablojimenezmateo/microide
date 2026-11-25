#include "workspace/ProjectRootPresence.h"
#include "workspace/shell/WorkspaceShell.h"

#include "workspace/services/SidebarService.h"
#include "util/InlineVector.h"
#include "util/Log.h"
#include "workspace/services/RemoteHostService.h"
#include "workspace/services/TerminalPanelService.h"

#include "workspace/persistence/WorkspacePersistenceCoordinator.h"

#include <execinfo.h>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util/BackgroundTaskCounter.h"
#include "compare/ComparePresentationModel.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/Waker.h"
#include "workspace/coordinators/EditorPointerHit.h"
#include "workspace/coordinators/SelectionAutoscroll.h"
#include "workspace/coordinators/WorkspaceCompareMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceEditorMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceMergeMouseCoordinator.h"
#include "workspace/coordinators/WorkspacePanelMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceTabMouseCoordinator.h"
#include "workspace/render/TabStripAnimation.h"
#include "workspace/shell/WorkspaceShellBootstrapper.h"
#include "workspace/services/CompareMergeService.h"

namespace microide::workspace {

namespace {

bool TraceProjectEventsEnabled() {
  static const bool enabled =
      util::PerformanceTrace::FlagEnabled("MICROIDE_TRACE_PROJECT_EVENTS");
  return enabled;
}

std::size_t MaxVisualColumns(const editor::TextViewport& viewport) {
  return viewport.max_visual_columns();
}

}

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
  QueueEditorHoverRefresh();
  InvalidateCursorKindFingerprint();
}

void WorkspaceShell::NoteLayoutInputsChanged() {
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
  window_presentation_ = state;
  if (window_presentation_.logical_width != previous_width ||
      window_presentation_.logical_height != previous_height) {
    NoteLayoutInputsChanged();
    if (window_presentation_.logical_width != previous_width) {
      tab_strip_chrome_.EnsureActiveProjectVisible();
      tab_strip_chrome_.EnsureActiveTabVisibleForAllGroups();
      tab_strip_service_.InvalidateTabStripGeometry();
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
  const render::TextClipPadding chrome = text_renderer_.ClipPadding();
  const render::TextClipPadding editor = editor_text_renderer_.ClipPadding();
  return render::TextClipPadding{.left = std::max(chrome.left, editor.left),
                                 .right = std::max(chrome.right, editor.right),
                                 .top = std::max(chrome.top, editor.top),
                                 .bottom = std::max(chrome.bottom, editor.bottom)};
}

std::optional<SDL_FRect> WorkspaceShell::CurrentWindowRect() const {
  if (window_presentation_.logical_width <= 0 || window_presentation_.logical_height <= 0) {
    return std::nullopt;
  }

  return MakeRect(0.0f, 0.0f, static_cast<float>(window_presentation_.logical_width),
                  static_cast<float>(window_presentation_.logical_height));
}

WorkspaceLayoutInputs WorkspaceShell::CurrentLayoutInputs(float window_width,
                                                          float window_height) const {
  const ProjectWorkspaceState& state = context_.current_project_state;
  return WorkspaceLayoutInputs{
      .window_width = window_width,
      .window_height = window_height,
      .sidebar_visible = state.sidebar.visible,
      .bottom_panel_visible = BottomPanelVisible(),
      .sidebar_width = state.sidebar.width,
      .bottom_panel_height = state.panel.height,
      .layout_mode = layout_mode_service_.SnapshotInputs(),
      .reserve_status_bar = layout_mode_service_.StatusBarVisible(),
      .right_pane_visible = state.debug_pane.visible,
      .right_pane_width = state.debug_pane.width,
      .project_tab_strip_visible = ProjectTabStripVisible(),
      .bottom_panel_maximized = state.panel.maximized,
  };
}

std::optional<WorkspaceLayout> WorkspaceShell::CurrentWorkspaceLayout() const {
  const auto window_rect = CurrentWindowRect();
  if (!window_rect.has_value()) {
    return std::nullopt;
  }
  const WorkspaceLayoutInputs inputs = CurrentLayoutInputs(window_rect->w, window_rect->h);
  if (layout_memo_inputs_.has_value() && *layout_memo_inputs_ == inputs) {
    util::AddPerformanceCounter(util::PerfCounterId::WorkspaceLayoutMemoHits);
    return layout_memo_;
  }
  layout_memo_ = ComputeLayout(inputs);
  layout_memo_inputs_ = inputs;
  ++layout_memo_generation_;
  return layout_memo_;
}

const WorkspaceShell::WindowChromeState& WorkspaceShell::CurrentWindowChromeState() const {
  return window_presentation_.chrome;
}

WorkspaceShell::RenderInvalidation WorkspaceShell::ConsumePendingRenderInvalidation() {
  if (pending_render_invalidation_.rects.spilled()) {
    util::AddPerformanceCounter(util::PerfCounterId::WorkspaceRedrawRectSpills);
  }
  RenderInvalidation result = std::move(pending_render_invalidation_);
  pending_render_invalidation_.full = false;
  pending_render_invalidation_.rects.clear();
  pending_render_invalidation_.present = false;
  pending_render_invalidation_.frame = false;
  return result;
}

namespace {

void TraceDamageRequest(const SDL_FRect* rect) {
  static const bool enabled = util::PerformanceTrace::FlagEnabled("MICROIDE_TRACE_DAMAGE");
  if (!enabled) {
    return;
  }
  std::string report = rect == nullptr
                           ? std::string("microide damage: full\n")
                           : std::format("microide damage: {:.0f},{:.0f},{:.0f},{:.0f}\n", rect->x, rect->y,
                                         rect->w, rect->h);
  constexpr int kFrames = 10;
  void* frames[kFrames];
  const int count = ::backtrace(frames, kFrames);
  if (char** symbols = ::backtrace_symbols(frames, count); symbols != nullptr) {
    for (int i = 1; i < count; ++i) {
      report += "  #" + std::to_string(i) + " " + symbols[i] + "\n";
    }
    std::free(symbols);  // NOLINT(cppcoreguidelines-no-malloc): backtrace_symbols' contract
  }
  report.pop_back();
  util::Log(report);
}

}

void WorkspaceShell::RequestFullRedraw() {
  TraceDamageRequest(nullptr);
  util::AddPerformanceCounter(util::PerfCounterId::WorkspaceFullRedrawRequests);
  InvalidateCursorKindFingerprint();
  ++damage_generation_;
  pending_render_invalidation_.full = true;
  pending_render_invalidation_.rects.clear();
  QueueEditorHoverRefresh();
}

void WorkspaceShell::Notify(NotificationService::Tone tone, std::string message) {
  notification_service_.Show(tone, std::move(message), SDL_GetTicks());
  RequestPreparedFrame();
}

void WorkspaceShell::Notify(NotificationService::Request request) {
  if (notification_service_.Show(std::move(request), SDL_GetTicks())) {
    RequestPreparedFrame();
  }
}

void WorkspaceShell::RequestRedrawRect(const SDL_FRect& rect) {
  if (rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }
  TraceDamageRequest(&rect);
  ++damage_generation_;
  if (pending_render_invalidation_.full) {
    return;
  }
  InvalidateCursorKindFingerprint();
  pending_render_invalidation_.Add(rect);
  util::AddPerformanceCounter(util::PerfCounterId::WorkspaceRedrawRectsQueued);
  util::AddPerformanceCounter(util::PerfCounterId::WorkspaceRedrawRectPixels,
                              static_cast<std::uint64_t>(rect.w * rect.h));
  QueueEditorHoverRefresh();
}

void WorkspaceShell::RequestCursorPresent() {
  pending_render_invalidation_.present = true;
}

void WorkspaceShell::RequestPreparedFrame() {
  pending_render_invalidation_.frame = true;
  ++damage_generation_;
  InvalidateCursorKindFingerprint();
}

void WorkspaceShell::RequestOuterLayoutChangeRedraw(const WorkspaceLayout& previous_layout,
                                                    SDL_FRect WorkspaceLayout::*first,
                                                    SDL_FRect WorkspaceLayout::*second) {
  NoteLayoutInputsChanged();
  const auto current_layout = CurrentWorkspaceLayout();
  if (!current_layout.has_value()) {
    RequestPreparedFrame();
    return;
  }
  if (RectsEqual(previous_layout.*first, (*current_layout).*first) &&
      RectsEqual(previous_layout.*second, (*current_layout).*second)) {
    return;
  }
  RequestFullRedraw();
}

void WorkspaceShell::RequestSidebarLayoutChangeRedraw(
    const WorkspaceLayout& previous_layout) {
  RequestOuterLayoutChangeRedraw(previous_layout, &WorkspaceLayout::sidebar,
                                 &WorkspaceLayout::content);
}

void WorkspaceShell::NoteActiveEditableEdit() {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::NoteActiveEditableEdit");
  RequestPreparedFrame();
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr) {
    return;
  }
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::NoteActiveEditableEdit::SyncLsp");
    SyncLspForActiveEditableLastChange();
  }
  RefreshBufferSearchIfStale();
  (void)MakeSidebarService().CatchUpSearchResults(*viewport);
  assist_service_.snippets().NotifyBufferChanged();
  (void)debug_service_.ShiftBreakpointsForViewportEdit(*viewport);
}

void WorkspaceShell::RequestActiveEditableChangeRedraw(const std::vector<std::string>& before_lines,
                                                       const std::vector<std::string>& after_lines) {
  lsp_service_.SyncLspForActiveEditableChange(before_lines, after_lines);
  RequestPreparedFrame();
}

void WorkspaceShell::RequestBottomPanelLayoutChangeRedraw(
    const WorkspaceLayout& previous_layout) {
  RequestOuterLayoutChangeRedraw(previous_layout, &WorkspaceLayout::content,
                                 &WorkspaceLayout::bottom_panel);
}

void WorkspaceShell::QueueEditorHoverRefresh() {
  editor_hover_refresh_pending_ =
      last_mouse_position_valid_ || context_.interaction_state.caret_hover.active;
}

util::InlineVector<SDL_FRect, 5> WorkspaceShell::CurrentChromeRects() const {
  util::InlineVector<SDL_FRect, 5> rects;
  const auto layout = CurrentWorkspaceLayout();
  if (!layout.has_value()) {
    return rects;
  }
  rects.push_back(layout->menu_bar);
  if (context_.menu_state.menu_bar_open) {
    if (const auto popup_rect = ComputePopupMenuRect(layout->menu_bar, context_.menu_state.active_menu_id);
        popup_rect.has_value()) {
      rects.push_back(*popup_rect);
    }
    if (const auto submenu_rect = ActiveSubmenuRect(layout->menu_bar);
        submenu_rect.has_value()) {
      rects.push_back(*submenu_rect);
    }
  }
  if (const auto chevron = MenuOverflowChevronRect(layout->menu_bar);
      context_.menu_state.overflow_popup_open && chevron.has_value()) {
    const auto overflow = ComputeOverflowMenuBarItems(layout->menu_bar);
    rects.push_back(ComputeMenuOverflowPopupRect(*chevron, overflow.size()));
  }
  if (context_.menu_state.tree_context_menu.open) {
    if (const auto tree_menu_rect = ComputeTreeContextMenuRect(); tree_menu_rect.has_value()) {
      rects.push_back(*tree_menu_rect);
    }
  }
  return rects;
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

  const auto panes = ComputeEditorPaneLayouts(*layout);
  const auto active_pane =
      std::find_if(panes.begin(), panes.end(), [](const EditorPaneLayout& pane) { return pane.active; });
  const SDL_FRect pane_rect =
      active_pane != panes.end() ? active_pane->rect : layout->editor_surface;
  const editor::TextViewport* viewport = ActiveEditorViewport();
  if (viewport == nullptr) {
    return std::nullopt;
  }
  const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
      editor_text_renderer_, *viewport, pane_rect, viewport->sticky_band_rows(), LineNumbersEnabled());
  const std::size_t visible_start_row = viewport->scroll_line();
  const std::size_t visible_end_row = visible_start_row + metrics.visible_rows;
  const std::size_t start_row = viewport->VisualRowForLine(start_line);
  const std::size_t end_row =
      end_line == 0 ? start_row + 1
      : end_line >= viewport->line_count()
          ? viewport->visual_line_count()
          : std::max(viewport->VisualRowForLine(end_line),
                     viewport->VisualRowForLine(end_line - 1) + 1);
  if (end_row <= visible_start_row || start_row >= visible_end_row) {
    return std::nullopt;
  }
  const std::size_t clipped_start = std::max(start_row, visible_start_row);
  const std::size_t clipped_end = std::min(end_row, visible_end_row);
  if (context_.current_project_state.plugin_presentation_if_present() != nullptr) {
    std::vector<float> row_tops;
    VisibleRowTops(context_.current_project_state, *viewport, metrics,
                   [this](std::string_view id) { return GetSettingValue(id); }, row_tops);
    const std::size_t first = clipped_start - visible_start_row;
    const std::size_t last = clipped_end - 1 - visible_start_row;
    if (last < row_tops.size()) {
      const float top = row_tops[first];
      return DirtyRectWithHalo(
          MakeRect(pane_rect.x, top, pane_rect.w, row_tops[last] + metrics.line_height - top));
    }
  }
  return DirtyRectWithHalo(MakeRect(
      pane_rect.x,
      metrics.first_line_y +
          static_cast<float>(clipped_start - visible_start_row) * metrics.line_height,
      pane_rect.w, static_cast<float>(clipped_end - clipped_start) * metrics.line_height));
}

std::optional<Uint32> WorkspaceShell::NextTabSlideDelayMs() const {
  if (context_.interaction_state.tab_drag.autoscroll_direction != 0) {
    return static_cast<Uint32>(16);
  }
  const TabSlideState& slide = context_.interaction_state.tab_slide;
  if (!slide.active()) {
    return std::nullopt;
  }
  if (slide.settling) {
    return static_cast<Uint32>(16);
  }
  for (const TabStripSlide& strip : slide.strips) {
    if (!strip.idle() && SlideOffsetsMoving(strip.current, strip.target)) {
      return static_cast<Uint32>(16);
    }
  }
  return std::nullopt;
}

bool WorkspaceShell::AdvanceTabSlide() {
  bool scrolled = false;
  if (context_.interaction_state.tab_drag.autoscroll_direction != 0) {
    scrolled = MakeTabMouseCoordinator().TickDragAutoScroll();
  }
  TabSlideState& slide = context_.interaction_state.tab_slide;
  if (!slide.active()) {
    return scrolled;
  }
  const Uint64 now = SDL_GetTicks();
  const Uint64 raw_dt = now >= slide.last_advance_ms ? now - slide.last_advance_ms : 0;
  const float dt_ms = static_cast<float>(std::min<Uint64>(raw_dt, 100));
  slide.last_advance_ms = now;
  bool moving = false;
  for (TabStripSlide& strip : slide.strips) {
    if (!strip.idle() && AdvanceSlideOffsets(strip.current, strip.target, dt_ms)) {
      moving = true;
    }
  }
  if (!moving) {
    if (slide.settling) {
      slide = TabSlideState{};
      return true;
    }
    return scrolled;
  }
  return true;
}

std::optional<SDL_FRect> WorkspaceShell::TabStripRectForKind(TabDragKind kind,
                                                             std::size_t group_index) const {
  if (kind == TabDragKind::None) {
    return std::nullopt;
  }
  const auto layout = CurrentWorkspaceLayout();
  if (!layout.has_value()) {
    return std::nullopt;
  }
  switch (kind) {
    case TabDragKind::Project:
      return layout->project_tab_strip;
    case TabDragKind::Editor: {
      const EditorGroupRectsLayout groups = ComputeEditorGroupRectsForState(*layout);
      if (group_index < groups.groups.size()) {
        return groups.groups[group_index].tab_strip;
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
  if (const auto autoscroll_delay =
          selection_autoscroll::NextDelayMs(context_.interaction_state);
      autoscroll_delay.has_value()) {
    if (!next_delay.has_value() || *autoscroll_delay < *next_delay) {
      next_delay = *autoscroll_delay;
    }
  }
  if (const auto plugin_delay = plugin_runtime_.NextPollDelay(); plugin_delay.has_value()) {
    const Uint32 plugin_delay_ms =
        static_cast<Uint32>(std::max<std::int64_t>(0, plugin_delay->count()));
    if (!next_delay.has_value() || plugin_delay_ms < *next_delay) {
      next_delay = plugin_delay_ms;
    }
  }
  if (const auto change_delay = project_change_coalescer_.NextReadyDelayMs(SDL_GetTicks());
      change_delay.has_value()) {
    const Uint32 change_delay_ms = static_cast<Uint32>(*change_delay);
    if (!next_delay.has_value() || change_delay_ms < *next_delay) {
      next_delay = change_delay_ms;
    }
  }
  if (const auto toast_delay = notification_service_.NextExpiryDelayMs(SDL_GetTicks());
      toast_delay.has_value()) {
    const Uint32 toast_delay_ms = static_cast<Uint32>(*toast_delay);
    if (!next_delay.has_value() || toast_delay_ms < *next_delay) {
      next_delay = toast_delay_ms;
    }
  }
  if (const auto editor_event_delay = plugin_editor_event_tracker_.NextDelayMs(SDL_GetTicks());
      editor_event_delay.has_value()) {
    const Uint32 editor_event_delay_ms = static_cast<Uint32>(*editor_event_delay);
    if (!next_delay.has_value() || editor_event_delay_ms < *next_delay) {
      next_delay = editor_event_delay_ms;
    }
  }
  if (const std::uint64_t due =
          context_.current_project_state.overlay.workflow.project_search.search_on_type_due_ms;
      due != 0) {
    const std::uint64_t now = SDL_GetTicks();
    const Uint32 delay = due > now ? static_cast<Uint32>(due - now) : 0;
    if (!next_delay.has_value() || delay < *next_delay) {
      next_delay = delay;
    }
  }
  if (const auto autosave_delay = NextAutosaveDelayMs(); autosave_delay.has_value()) {
    if (!next_delay.has_value() || *autosave_delay < *next_delay) {
      next_delay = *autosave_delay;
    }
  }
  if (const auto session_flush_delay = NextSessionFlushDelayMs(); session_flush_delay.has_value()) {
    if (!next_delay.has_value() || *session_flush_delay < *next_delay) {
      next_delay = *session_flush_delay;
    }
  }
  return next_delay;
}

std::optional<Uint32> WorkspaceShell::NextSessionFlushDelayMs() const {
  const std::optional<std::uint64_t> delay = session_flush_.DelayMs(SDL_GetTicks());
  return delay.has_value() ? std::optional<Uint32>(static_cast<Uint32>(*delay)) : std::nullopt;
}

void WorkspaceShell::FlushSessionStateForCrashSafety() {
  MakePersistenceCoordinator().SaveSessionState();
}

WorkspaceShell::IdleWaitState WorkspaceShell::CurrentIdleWaitState() {
  std::optional<Uint32> wait_ms;
  bool in_flight = false;
  const auto release_idle_waiters = [&] {
    if (in_flight || NextTabSlideDelayMs().has_value() ||
        selection_autoscroll::NextDelayMs(context_.interaction_state).has_value() ||
        project_change_coalescer_.NextReadyDelayMs(SDL_GetTicks()).has_value() ||
        SDL_HasEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST)) {
      return;
    }
    control_channel_service_.ReleaseIdleWaiters();
  };
  if (const auto next_delay = NextAnimationDelayMs(); next_delay.has_value()) {
    wait_ms = std::max<Uint32>(1, *next_delay);
  }
  if (pending_render_invalidation_.HasAnyRedraw()) {
    return IdleWaitState{
        .hint = IdleHint::Full,
        .caret_remaining_ms = 0,
    };
  }
  if (DebugEnabled() && debug_service_.HasInFlightDapWork()) {
    in_flight = true;
    constexpr Uint32 kDapPollMs = 64;
    wait_ms = wait_ms.has_value() ? std::min(*wait_ms, kDapPollMs) : kDapPollMs;
  }
  if (util::ConsumeOwedWake()) {
    in_flight = true;
    constexpr Uint32 kWakeOwedPollMs = 32;
    wait_ms = wait_ms.has_value() ? std::min(*wait_ms, kWakeOwedPollMs) : kWakeOwedPollMs;
  }
  if (util::WakeRegistrationDegraded()) {
    in_flight = true;
    constexpr Uint32 kDegradedPollMs = 64;
    wait_ms = wait_ms.has_value() ? std::min(*wait_ms, kDegradedPollMs) : kDegradedPollMs;
  }
  release_idle_waiters();
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
  save_formatter_service_.DrainCompletions();
  file_read_service_.DrainCompletions();
  if (glue_->remote_host_service != nullptr) {
    glue_->remote_host_service->DrainCompletions();
  }
  file_index_refresh_mailbox_.Drain();
  project_replace_mailbox_.Drain();
  project_file_event_pending_.store(false, std::memory_order_release);
  const bool index_metadata_pending =
      file_index_has_pending_changes_.exchange(false, std::memory_order_acq_rel);

  project::ProjectChangeBatch supplemental;
  {
    util::PerformanceTrace::Scope sample_scope("WorkspaceShell::ReloadProjectIfFilesChanged::SampleGitMetadata");
    for (const project::RepositoryChange& change : git_metadata_tracker_.SampleChanges()) {
      supplemental.repository_changes.push_back(change);
    }
  }

  if (force_check) {
    AppendForcedProjectRescanChanges(supplemental);
  }

  if (project_index_truncated_notice_pending_.exchange(false, std::memory_order_acq_rel)) {
    Notify(NotificationService::Request{
        .tone = NotificationService::Tone::Warning,
        .key = std::string(kProjectIndexTruncatedNotificationKey),
        .message = "Project too large to index fully — search and Go to File cover part of it",
        .sticky = true,
    });
  }

  if (!supplemental.repository_changes.empty() || !supplemental.file_changes.empty() ||
      supplemental.tree_rescan_requested) {
    project_change_coalescer_.Ingest(std::move(supplemental));
  }

  const std::optional<project::ProjectChangeBatch> ready = [&] {
    util::PerformanceTrace::Scope consume_scope("WorkspaceShell::ReloadProjectIfFilesChanged::ConsumeReady");
    return force_check ? project_change_coalescer_.ConsumeReady()
                       : project_change_coalescer_.ConsumeReadyAt(SDL_GetTicks());
  }();
  if (!ready.has_value()) {
    if (!index_metadata_pending) {
      return false;
    }
    RefreshProjectFileViews();
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
  if (PublishProjectRootPresence(notification_service_, context_.current_project_state.root, SDL_GetTicks())) {
    RequestFullRedraw();
  }
  return true;
}

WorkspaceShell::EventResult WorkspaceShell::HandleScheduledWake() {
  EventResult result = HandleScheduledWakeSources();
  MakeTerminalPanelService().SyncPanelMaximizedWithFocus();
  const RenderInvalidation pending = ConsumePendingRenderInvalidation();
  if (pending.HasAnyRedraw()) {
    result.redraw.Merge(pending);
    result.handled = true;
  }
  return result;
}

WorkspaceShell::EventResult WorkspaceShell::HandleScheduledWakeSources() {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::HandleScheduledWake");
  if (notification_service_.ExpireDue(SDL_GetTicks())) {
    return EventResult{.handled = true, .redraw = RenderInvalidation{.frame = true}};
  }
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
  if (session_flush_.Due(SDL_GetTicks())) {
    session_flush_.Disarm();
    FlushSessionStateForCrashSafety();
  }
  if (ProjectSearchState& search = context_.current_project_state.overlay.workflow.project_search;
      search.search_on_type_due_ms != 0 && SDL_GetTicks() >= search.search_on_type_due_ms) {
    search.search_on_type_due_ms = 0;
    if (search.editing && search.edit_field != ProjectSearchEditField::Replace) {
      ProjectSearchFieldEditor(search, search.edit_field).SetText(search.edit_buffer.text());
      RefreshProjectSearch();
      RequestPreparedFrame();
    }
  }
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
    return EventResult{.handled = true, .redraw = {}};
  }
  if (context_.interaction_state.tab_slide.active() ||
      context_.interaction_state.tab_drag.autoscroll_direction != 0) {
    const TabSlideState& slide = context_.interaction_state.tab_slide;
    util::SmallVector<SDL_FRect, kInlineRedrawRects> slide_rects;
    bool resolved_every_strip = true;
    for (const TabStripSlide& strip : slide.strips) {
      if (strip.idle()) {
        continue;
      }
      if (const auto rect = TabStripRectForKind(strip.kind, strip.group_index); rect.has_value()) {
        slide_rects.push_back(*rect);
      } else {
        resolved_every_strip = false;
      }
    }
    if (slide_rects.empty()) {
      if (const auto rect = TabStripRectForKind(context_.interaction_state.tab_drag.kind,
                                                FocusedEditorGroupIndex());
          rect.has_value()) {
        slide_rects.push_back(*rect);
      } else {
        resolved_every_strip = false;
      }
    }
    if (AdvanceTabSlide()) {
      return EventResult{
          .handled = true,
          .redraw = resolved_every_strip && !slide_rects.empty()
                        ? RenderInvalidation{.full = false, .rects = std::move(slide_rects)}
                        : RenderInvalidation{.full = true, .rects = {}},
      };
    }
  }
  const auto step_selection_autoscroll = [this]() -> bool {
    using Owner = InteractionState::SelectionAutoscrollOwner;
    if (context_.interaction_state.selection_autoscroll_owner == Owner::Terminal) {
      TerminalPaneState* terminal_tab = ActiveTerminalPane();
      if (terminal_tab == nullptr || !terminal_tab->mouse_selecting || !BottomPanelVisible() ||
          !BottomPanelShowsTerminal()) {
        selection_autoscroll::Disarm(context_.interaction_state);
        return false;
      }
      const std::optional<selection_autoscroll::StepDelta> delta =
          selection_autoscroll::BeginStep(context_.interaction_state);
      if (!delta.has_value()) {
        return false;
      }
      if (delta->rows == 0) {
        return selection_autoscroll::FinishStep(context_.interaction_state, false);
      }
      const auto layout = CurrentWorkspaceLayout();
      if (!layout.has_value()) {
        return selection_autoscroll::FinishStep(context_.interaction_state, false);
      }
      const std::size_t line_count = terminal_tab->session.LineCount();
      const LogSurfaceLayout panel_layout = ComputeBottomPanelLogLayout(*layout, line_count);
      const int before_row = terminal_tab->scroll_row;
      SetBottomPanelScrollRow(terminal_tab->scroll_row + delta->rows, line_count,
                              panel_layout.scroll.visible_rows);
      return selection_autoscroll::FinishStep(context_.interaction_state,
                                              terminal_tab->scroll_row != before_row);
    }
    if (!context_.interaction_state.mouse_selecting) {
      selection_autoscroll::Disarm(context_.interaction_state);
      return false;
    }
    if (ActiveTabIsCompare()) {
      CompareTabState* compare_tab = ActiveCompareTab();
      if (compare_tab == nullptr) {
        return false;
      }
      const std::optional<selection_autoscroll::StepDelta> delta =
          selection_autoscroll::BeginStep(context_.interaction_state);
      if (!delta.has_value()) {
        return false;
      }
      const int before_row = compare_tab->scroll_row;
      const std::size_t before_columns = compare_tab->horizontal_scroll;
      if (delta->rows != 0) {
        MakeCompareMergeService().interactions().ScrollCompareRows(delta->rows);
      }
      if (delta->columns != 0) {
        MakeCompareMergeService().interactions().ScrollCompareColumns(delta->columns);
      }
      SyncCompareViewportScroll(*compare_tab);
      return selection_autoscroll::FinishStep(
          context_.interaction_state,
          compare_tab->scroll_row != before_row ||
              compare_tab->horizontal_scroll != before_columns);
    }
    if (ActiveTabIsMerge()) {
      MergeTabState* merge_tab = ActiveMergeTab();
      if (merge_tab == nullptr) {
        return false;
      }
      const bool stepped =
          selection_autoscroll::Step(context_.interaction_state, merge_tab->result_viewport);
      merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
      merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
      return stepped;
    }
    if (!ActiveTabIsEditor()) {
      selection_autoscroll::Disarm(context_.interaction_state);
      return false;
    }
    editor::TextViewport* viewport = ActiveEditorViewport();
    return viewport != nullptr && selection_autoscroll::Step(context_.interaction_state, *viewport);
  };
  if (context_.interaction_state.selection_autoscroll_active() && step_selection_autoscroll()) {
    const auto layout = CurrentWorkspaceLayout();
    if (layout.has_value()) {
      SDL_Event synthetic{};
      synthetic.type = SDL_EVENT_MOUSE_MOTION;
      synthetic.motion.x = context_.interaction_state.selection_pointer_x;
      synthetic.motion.y = context_.interaction_state.selection_pointer_y;
      synthetic.motion.state = SDL_BUTTON_LMASK;
      if (context_.interaction_state.selection_autoscroll_owner ==
          InteractionState::SelectionAutoscrollOwner::Terminal) {
        (void)MakePanelMouseCoordinator().HandleMotion(synthetic);
      } else if (ActiveTabIsCompare()) {
        (void)MakeCompareMouseCoordinator().HandleSelectionMotion(synthetic, ActiveTabSurface(*layout));
      } else if (ActiveTabIsMerge()) {
        (void)MakeMergeMouseCoordinator().HandleSelectionMotion(synthetic, ActiveTabSurface(*layout));
      } else {
        (void)MakeEditorMouseCoordinator().HandleSelectionMotion(synthetic, *layout);
      }
    }
    return EventResult{.handled = true, .redraw = RenderInvalidation{.frame = true}};
  }
  return Bootstrapper(*this).BuildWakeController().HandleScheduledWake();
}

std::optional<WorkspaceShell::RenderInvalidation>
WorkspaceShell::ConsumePostRenderRedrawRequest() {
  if (post_render_redraws_remaining_ <= 0) {
    return std::nullopt;
  }
  --post_render_redraws_remaining_;
  RenderInvalidation invalidation;
  if (const auto layout = CurrentWorkspaceLayout(); layout.has_value() && BottomPanelVisible()) {
    invalidation.Add(layout->bottom_panel);
  } else {
    invalidation.full = true;
  }
  return invalidation;
}

ScrollSurfaceLayout WorkspaceShell::ComputeEditorScrollLayout(
    const SDL_FRect& rect,
    const editor::TextViewport& viewport,
    const editor::EditorViewMetrics& metrics) const {
  const std::size_t total_columns =
      viewport.soft_wrap()
          ? metrics.visible_columns
          : std::max<std::size_t>(metrics.visible_columns, MaxVisualColumns(viewport));
  return ComputeScrollSurfaceLayout(rect, viewport.visual_line_count(),
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

}
