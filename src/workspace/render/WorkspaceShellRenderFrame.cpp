#include "workspace/render/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "editor/TextLayout.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "workspace/render/OverviewRuler.h"
#include "workspace/PluginSurfacePreview.h"
#include "workspace/render/RenderViewModelBuilder.h"
#include "workspace/WorkspaceTextSearch.h"
#include "render/ScopedRenderClip.h"
#include "workspace/SettingFlags.h"
#include "workspace/coordinators/WorkspaceTextInputCoordinator.h"

namespace microide::workspace {

using namespace detail;

namespace {

// Overlap priority for editor overview-ruler markers: when two sources map onto the
// same pixel row the higher value wins it. Errors/warnings beat search; the active
// search match beats plain matches; the caret is drawn on top separately.
constexpr int kOverviewPrioDiagError = 90;
constexpr int kOverviewPrioDiagWarning = 80;
constexpr int kOverviewPrioSearchActive = 70;
constexpr int kOverviewPrioSearchMatch = 60;
constexpr int kOverviewPrioDiagInfo = 40;
constexpr int kOverviewPrioDiagHint = 30;

SDL_Color OverviewSeverityColor(const render::Theme& theme, editor::DiagnosticSeverity severity) {
  switch (severity) {
    case editor::DiagnosticSeverity::Error:
      return theme.diagnostic_error;
    case editor::DiagnosticSeverity::Warning:
      return theme.diagnostic_warning;
    case editor::DiagnosticSeverity::Info:
      return theme.diagnostic_info;
    case editor::DiagnosticSeverity::Hint:
    default:
      return theme.diagnostic_hint;
  }
}

int OverviewSeverityPriority(editor::DiagnosticSeverity severity) {
  switch (severity) {
    case editor::DiagnosticSeverity::Error:
      return kOverviewPrioDiagError;
    case editor::DiagnosticSeverity::Warning:
      return kOverviewPrioDiagWarning;
    case editor::DiagnosticSeverity::Info:
      return kOverviewPrioDiagInfo;
    case editor::DiagnosticSeverity::Hint:
    default:
      return kOverviewPrioDiagHint;
  }
}

// Cache key for a pane's overview markers. Excludes the caret line (drawn live) so
// cursor movement never invalidates the cached search+diagnostic set. `theme_token`
// tracks the colorscheme so a live theme switch rebuilds the baked-in marker colors.
struct EditorOverviewSignature {
  const void* viewport = nullptr;
  std::size_t line_count = 0;
  bool dirty = false;
  std::uint64_t diag_revision = 0;
  int diag_min_severity = 0;
  bool search_active = false;
  std::uint64_t search_revision = 0;
  std::size_t search_selected = 0;
  std::size_t search_count = 0;
  std::uint64_t theme_token = 0;
  bool operator==(const EditorOverviewSignature&) const = default;
};

struct EditorOverviewPaneCache {
  EditorOverviewSignature sig;
  SDL_FRect track{};
  std::vector<overview::Marker> markers;  // search + diagnostics (caret drawn live)
};

// Paints the editor overview ruler beside `track`: a lane with density-reduced markers
// for search matches and diagnostics (rebuilt only when a source or the lane changed),
// plus a live caret marker on top. Scratch vectors are caller-owned/thread-local so a
// steady frame allocates nothing.
void DrawEditorOverviewRuler(SDL_Renderer* renderer, const render::Theme& theme,
                             ProjectWorkspaceState& project_state,
                             const OverlaySurfaceViewModel& overlay_vm,
                             editor::TextViewport& viewport, bool pane_active,
                             editor::DiagnosticSeverity diag_min_severity, const SDL_FRect& track,
                             float pane_left, EditorOverviewPaneCache& cache,
                             std::vector<overview::MarkerInput>& inputs,
                             std::vector<std::uint32_t>& buckets, std::vector<SDL_Color>& palette) {
  const SDL_FRect lane = overview::LaneRect(track, pane_left);
  const SDL_FRect inner_lane = overview::LaneInnerRect(lane);
  const std::size_t total_rows = viewport.line_count();

  const BufferSearchState& buffer_search = project_state.overlay.workflow.buffer_search;
  const bool search_active =
      pane_active &&
      (overlay_vm.mode == OverlayMode::BufferSearch || overlay_vm.mode == OverlayMode::BufferReplace) &&
      !buffer_search.matches.empty();

  EditorOverviewSignature sig;
  sig.viewport = &viewport;
  sig.line_count = total_rows;
  sig.dirty = viewport.dirty();
  sig.diag_revision = project_state.diagnostics_store.revision();
  sig.diag_min_severity = static_cast<int>(diag_min_severity);
  sig.search_active = search_active;
  sig.search_revision = buffer_search.matches_revision;
  sig.search_selected = buffer_search.selected_index;
  sig.search_count = buffer_search.matches.size();
  sig.theme_token = overview::ThemeMarkerToken(theme);

  if (cache.sig != sig || !RectsEqual(cache.track, inner_lane)) {
    inputs.clear();
    // Diagnostics stay visible while the buffer is dirty: LspService slides their
    // positions through each edit so they track the text until the server
    // republishes. Only a pathless (unsaved scratch) buffer has none.
    if (!viewport.path().empty()) {
      if (const std::vector<editor::PublishedDiagnostic>* diags =
              project_state.diagnostics_store.FindByPathKey(viewport.path_key());
          diags != nullptr) {
        thread_local std::vector<editor::PublishedDiagnostic> tls_filtered;
        const std::span<const editor::PublishedDiagnostic> filtered =
            editor::FilterDiagnosticsAtLeastSeverity(
                std::span<const editor::PublishedDiagnostic>(*diags), diag_min_severity,
                tls_filtered);
        // Saturate diagnostic rows into int: a std::size_t line beyond INT_MAX
        // (a plugin/LSP coordinate near the host ceiling) would otherwise wrap to a
        // negative/small row and mis-place the overview marker. (TD-2026-07-16-69.)
        constexpr std::size_t kMaxRow = static_cast<std::size_t>(std::numeric_limits<int>::max());
        const auto clamp_row = [](std::size_t line) {
          return line > kMaxRow ? std::numeric_limits<int>::max() : static_cast<int>(line);
        };
        for (const editor::PublishedDiagnostic& diagnostic : filtered) {
          const int start = clamp_row(diagnostic.range.start.line);
          const std::size_t end_line =
              std::max(diagnostic.range.end.line, diagnostic.range.start.line);
          const int end =
              end_line >= kMaxRow ? std::numeric_limits<int>::max() : static_cast<int>(end_line) + 1;
          inputs.push_back(overview::MarkerInput{.start_row = start,
                                                 .end_row = end,
                                                 .color = OverviewSeverityColor(theme, diagnostic.severity),
                                                 .priority = OverviewSeverityPriority(diagnostic.severity)});
        }
      }
    }
    // Search matches (only while the find/replace overlay is open on the active pane).
    if (search_active) {
      for (std::size_t i = 0; i < buffer_search.matches.size(); ++i) {
        const int line = static_cast<int>(buffer_search.matches[i].start.line);
        const bool selected = i == buffer_search.selected_index;
        inputs.push_back(overview::MarkerInput{
            .start_row = line,
            .end_row = line + 1,
            .color = selected ? theme.search_match_active : theme.search_match,
            .priority = selected ? kOverviewPrioSearchActive : kOverviewPrioSearchMatch});
      }
    }
    overview::ReduceMarkersInto(inner_lane, total_rows, inputs, buckets, palette, cache.markers);
    cache.sig = sig;
    cache.track = inner_lane;
  }

  overview::DrawLane(renderer, theme, lane, cache.markers);

  // Caret line, drawn live (one rect) so cursor movement never invalidates the cache.
  // Uses the allocation-free single-marker builder so a steady frame never touches the heap.
  const overview::MarkerInput caret_input{.start_row = static_cast<int>(viewport.cursor_line()),
                                          .end_row = static_cast<int>(viewport.cursor_line()) + 1,
                                          .color = theme.cursor,
                                          .priority = 0};
  overview::Marker caret_marker;
  if (overview::BuildMarker(inner_lane, total_rows, caret_input, caret_marker)) {
    render::SetDrawColor(renderer, caret_marker.color);
    SDL_RenderFillRect(renderer, &caret_marker.rect);
  }
}

// Pre-split the (possibly multi-line) regex match set into single-line highlight
// fragments the editor renderer can draw per row, cached per (viewport, content,
// match-set) so it rebuilds only when the matches actually change — not per frame.
// `matches` is ascending by (line, column); splitting preserves that order, so the
// renderer can binary-search each row's slice.
std::span<const editor::SelectionRange> BufferSearchHighlightFragments(
    const editor::TextViewport& viewport,
    const std::vector<editor::SelectionRange>& matches,
    std::uint64_t matches_revision) {
  struct Cache {
    const void* viewport = nullptr;
    std::uint64_t content_revision = 0;
    std::uint64_t matches_revision = 0;
    bool valid = false;
    std::vector<editor::SelectionRange> fragments;
  };
  thread_local Cache cache;
  const std::uint64_t content_revision = viewport.content_revision();
  if (cache.valid && cache.viewport == &viewport &&
      cache.content_revision == content_revision && cache.matches_revision == matches_revision) {
    return cache.fragments;
  }

  cache.fragments = SplitRegexMatchHighlightFragments(viewport.lines(), matches);
  cache.viewport = &viewport;
  cache.content_revision = content_revision;
  cache.matches_revision = matches_revision;
  cache.valid = true;
  return cache.fragments;
}

}  // namespace

void WorkspaceShell::ResizeTerminalToPanel(const SDL_FRect& panel_rect) {
  auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return;
  }

  util::PerformanceTrace::Scope trace_scope("WorkspaceShell::ResizeTerminalToPanel");
  const int rows = BottomPanelVisibleRows(panel_rect.h);
  const float usable_width =
      std::max(16.0f, panel_rect.w - 24.0f - kWorkspaceScrollbarThickness - 6.0f);
  const int columns = std::max(
      1, static_cast<int>(
             std::floor(usable_width / std::max(1.0f, terminal_text_renderer_.CharWidth()))));
  if (terminal_tab->session.rows() == static_cast<std::size_t>(rows) &&
      terminal_tab->session.columns() == static_cast<std::size_t>(columns)) {
    return;
  }
  terminal_tab->session.Resize(static_cast<std::size_t>(rows), static_cast<std::size_t>(columns));
  post_render_redraws_remaining_ = std::max(post_render_redraws_remaining_, 2);
}

void WorkspaceShell::DrawFilledRect(SDL_Renderer* renderer,
                                    const SDL_FRect& rect,
                                    SDL_Color color) const {
  render::SetDrawColor(renderer, color);
  SDL_RenderFillRect(renderer, &rect);
}

void WorkspaceShell::DrawSurfaceFocusRing(SDL_Renderer* renderer, const SDL_FRect& region) const {
  // A 1px accent outline just inside the region marks which surface owns the keyboard.
  // Each surface draws its own ring from its render path so partial redraws repaint it
  // consistently with full redraws; HandleEvent repaints both surfaces when focus moves.
  if (renderer == nullptr || region.w <= 2.0f || region.h <= 2.0f) {
    return;
  }
  OutlineRect(renderer,
              MakeRect(region.x + 1.0f, region.y + 1.0f, region.w - 2.0f, region.h - 2.0f),
              theme_.accent);
}

void WorkspaceShell::DrawRect(SDL_Renderer* renderer,
                              const SDL_FRect& rect,
                              SDL_Color color) const {
  render::SetDrawColor(renderer, color);
  SDL_RenderRect(renderer, &rect);
}

std::string_view WorkspaceShell::TruncateLabelView(std::string_view text, float max_width) const {
  return text_renderer_.TruncateToWidthEphemeralView(text, max_width);
}

WorkspaceShell::FrameToken WorkspaceShell::PrepareFrameOnce(SDL_Renderer* renderer,
                                                            int width,
                                                            int height) {
  if (renderer == nullptr || width <= 0 || height <= 0) {
    return FrameToken{};
  }

  clip_frame_overlay_view_models_valid_ = false;
  prepare_cached_sidebar_vm_.reset();
  prepare_cached_bottom_panel_vm_.reset();
  prepare_cached_debug_pane_vm_.reset();
  prepare_cached_text_input_vm_.reset();

  util::PerformanceTrace::Scope trace_scope("WorkspaceShell::PrepareFrameOnce");
  util::AddPerformanceCounter(util::PerfCounterId::FramePrepareCalls);
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::ConsumePendingResults");
    ConsumePendingProjectOpenDialogResult();
    ConsumePendingOpenFileDialogResult();
    ConsumePendingFontFileDialogResult();
    ConsumeProjectSearchUpdates();
  }
  text_renderer_.EnsureInitialized(renderer, presentation_scale_x_, presentation_scale_y_);
  // The terminal renderer is only needed (and only pays for its glyph atlas) once a
  // terminal is actually shown; initialize + apply its font lazily at that point.
  if (BottomPanelShowsTerminal()) {
    terminal_text_renderer_.EnsureInitialized(renderer, presentation_scale_x_,
                                              presentation_scale_y_);
    ApplyTerminalFontPreferences();
  }
  window_presentation_.logical_width = width;
  window_presentation_.logical_height = height;
  const RenderViewModelBuilder view_models(context_);
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::BuildSurfaceViewModels");
    prepare_cached_sidebar_vm_.emplace(view_models.BuildSidebarSurface());
    prepare_cached_bottom_panel_vm_.emplace(view_models.BuildBottomPanelSurface());
  // The debug pane can only ever be visible when the debugger is enabled, so skip
  // building its view model entirely in the common debug-off case. RenderDebugPane
  // tolerates the empty optional.
  if (DebugEnabled()) {
    prepare_cached_debug_pane_vm_.emplace(view_models.BuildDebugPaneSurface());
  }
  }
  const SidebarSurfaceViewModel& sidebar_vm = *prepare_cached_sidebar_vm_;
  const BottomPanelSurfaceViewModel& panel_vm = *prepare_cached_bottom_panel_vm_;
  ProjectWorkspaceState& project_state = *sidebar_vm.project_state;
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::ApplyLiveSettings");
    ApplyLiveSettings();
  }
  // Resolve editor folding freshness once here, before any RenderClip runs, so the
  // state-mutating fold scan executes a single time per prepared frame instead of
  // per pane on every partial-redraw RenderClip. Render then consumes the already-
  // resolved model via GroupFoldingModelPtr (TD-2026-07-17A-004).
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::RefreshEditorFoldingModels");
    RefreshEditorFoldingModels();
  }
  prepare_cached_text_input_vm_.emplace(view_models.BuildTextInputSurface());
  const float clamped_sidebar_width =
      ClampSidebarWidth(project_state.sidebar.width, static_cast<float>(width));
  const float clamped_panel_height =
      ClampBottomPanelHeight(project_state.panel.height, static_cast<float>(height));
  const float resolved_sidebar_width = sidebar_vm.visible ? clamped_sidebar_width : 0.0f;
  const float clamped_right_pane_width = ClampRightPaneWidth(
      project_state.debug_pane.width, static_cast<float>(width), resolved_sidebar_width);
  if (clamped_sidebar_width != project_state.sidebar.width ||
      clamped_panel_height != project_state.panel.height ||
      clamped_right_pane_width != project_state.debug_pane.width) {
    layout_dirty_ = true;
  }
  project_state.sidebar.width = clamped_sidebar_width;
  project_state.panel.height = clamped_panel_height;
  project_state.debug_pane.width = clamped_right_pane_width;

  WorkspaceLayout layout;
  bool workspace_layout_recomputed = false;
  if (layout_dirty_ || !prepared_frame_layout_.has_value()) {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::ComputeLayout");
    layout = ComputeLayout(static_cast<float>(width), static_cast<float>(height), sidebar_vm.visible,
                           panel_vm.content != PanelContentKind::None,
                           project_state.sidebar.width, project_state.panel.height,
                           layout_mode_service_.SnapshotInputs(),
                           layout_mode_service_.StatusBarVisible(),
                           project_state.debug_pane.visible, project_state.debug_pane.width,
                           ProjectTabStripVisible());
    layout_mode_service_.SetCurrentMode(layout.layout_mode);
    ++prepare_frame_layout_compute_count_;
    layout_dirty_ = false;
    workspace_layout_recomputed = true;
  } else {
    layout = *prepared_frame_layout_;
  }
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::RefreshStatusBar");
    RefreshStatusBar();
  }
  // Nothing else asks for the status-bar strip: its content is derived from state
  // owned by other surfaces, so the event that changed it repaints the editor (or
  // the sidebar, or nothing) and the bar keeps stale pixels on a partial frame.
  // Ask for it here, and only when a painted value actually moved — so this costs
  // one thin extra clip on the frames that need it and nothing on the rest.
  // Self-terminating: the repaint does not change the model.
  if (status_bar_service_.TakePaintedStateChanged()) {
    RequestRedrawRect(layout.status_bar);
  }
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::RefreshSettingsOverlayCatalog");
    RefreshSettingsOverlayCatalog();
  }
  SDL_Window* render_window = SDL_GetRenderWindow(renderer);
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::SyncTextInputSurface");
    MakeTextInputCoordinator().SyncTextInputSurface(render_window);
  }
  if (panel_vm.content == PanelContentKind::Terminal && ActiveTerminalTab() != nullptr) {
    const SDL_FRect& panel = layout.bottom_panel;
    const auto& cached = last_terminal_panel_rect_;
    const bool unchanged = cached.has_value() && cached->x == panel.x && cached->y == panel.y &&
                           cached->w == panel.w && cached->h == panel.h;
    if (!unchanged) {
      ResizeTerminalToPanel(panel);
      last_terminal_panel_rect_ = panel;
    }
  } else {
    last_terminal_panel_rect_.reset();
  }
  prepared_frame_layout_ = layout;
  // Frame-prep completions that need the resolved layout (TD-2026-07-17-084/083):
  // the render TUs consume prepared data and no longer mutate state mid-paint.
  // 1) Bottom-panel tab strip: prebuild the visible tabs + overflow controls once
  //    per prepared frame instead of per RenderClip.
  if (panel_vm.content != PanelContentKind::None) {
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                 kWorkspaceBottomPanelHeaderHeight);
    const auto measure = [this](std::string_view text) {
      return text_renderer_.MeasureWidth(text);
    };
    prepare_cached_bottom_panel_vm_->tabs = &tab_strip_service_.ComputeVisibleBottomPanelTabs(
        project_state, panel_header, layout_mode_service_.CurrentMode(), measure,
        output_channels_.Channels());
    prepare_cached_bottom_panel_vm_->tab_overflow =
        tab_strip_service_.ComputeBottomPanelTabOverflowControls(
            project_state, panel_header, layout_mode_service_.CurrentMode(),
            *prepare_cached_bottom_panel_vm_->tabs, output_channels_.Channels());
  }
  // 1a) Terminal find bar: rescan and lay the bar out here, where the panel rect
  //     is known, so the render TU paints a fully prepared widget. The rescan is
  //     cheap by construction — settled scrollback is kept and only the visible
  //     grid is re-walked (see TerminalFindService::Refresh).
  if (panel_vm.content == PanelContentKind::Terminal && terminal_find_service_.visible()) {
    TerminalTabState* terminal_tab = ActiveTerminalTab();
    terminal_find_service_.Refresh(terminal_tab);
    BottomPanelSurfaceViewModel& panel_out = *prepare_cached_bottom_panel_vm_;
    panel_out.find_visible = true;
    panel_out.find_query = &terminal_find_service_.query();
    panel_out.find_matches = &terminal_find_service_.matches();
    panel_out.find_selected_index = terminal_find_service_.selected_index();
    OverlayFindWidgetViewModel& find_vm = panel_out.find;
    find_vm.fw = ComputeFindWidgetLayout(BottomPanelContentRect(layout), /*replace_mode=*/false,
                                         kTerminalFindToggleCount);
    find_vm.search_focused = terminal_find_service_.focused();
    find_vm.toggles[0] = FindWidgetToggleViewModel{
        .label = "Aa", .active = terminal_find_service_.case_sensitive()};
    find_vm.toggles[1] = FindWidgetToggleViewModel{
        .label = "ab", .active = terminal_find_service_.whole_word()};
    find_vm.has_query = !terminal_find_service_.query().text().empty();
    find_vm.has_matches = !terminal_find_service_.matches().empty();
    find_vm.count_text = terminal_find_service_.count_text();
    find_vm.search_display_text = terminal_find_service_.query().text();
    if (find_vm.search_focused) {
      // A focused field scrolls to the caret, so draw the metrics-resolved tail
      // rather than the raw text. The service owns the storage, so the view stays
      // valid through paint.
      terminal_find_service_.SetDisplayText(
          ComputeSingleLineViewMetrics(terminal_find_service_.query(), "",
                                       std::max(1.0f, find_vm.fw.search_field.w - 12.0f))
              .displayed_text);
      find_vm.search_display_text = terminal_find_service_.display_text();
    }
  }
  // 1b) Plugin-surface preview scroll clamp: normalize the stored pixel scroll
  //     against the resolved layout so render consumes an already-clamped value
  //     even after a republish shrinks the content or the panel resizes
  //     (TD-2026-07-16-60). The VM copy is refreshed because it was built from
  //     state before the layout was known.
  if (panel_vm.content == PanelContentKind::PluginSurface &&
      panel_vm.plugin_surface != nullptr) {
    const float body_height =
        std::max(0.0f, layout.bottom_panel.h - kWorkspaceBottomPanelHeaderHeight);
    int& surface_scroll = project_state.panel.surface_scroll_y;
    surface_scroll = std::clamp(
        surface_scroll, 0,
        MaxPluginSurfacePreviewScroll(*panel_vm.plugin_surface, body_height));
    prepare_cached_bottom_panel_vm_->plugin_surface_scroll_y =
        static_cast<float>(surface_scroll);
  }
  // 2) Overlay scroll clamp: the stored scroll row is normalized here so the
  //    overlay view model (and render) consume an already-clamped value.
  if (project_state.overlay.visible) {
    ClampOverlayScrollRow(ComputeOverlayRect(layout.editor_area));
  }
  // 3) Commit-draft body viewport sizing + caret-keep-visible scroll clamp (the
  //    083 residual): moved out of RenderCommitBodyField so paint stays pure.
  if (sidebar_vm.visible && sidebar_vm.mode == SidebarMode::Git) {
    PrepareCommitBodyViewportForFrame(layout.sidebar);
  }
  prepared_frame_draw_editor_caret_ =
      CaretVisibleNow() &&
      !(context_.text_input.active_surface == TextInputSurface::Editor &&
        !context_.text_input.composition.text.empty());
  FrameToken::VisibleLineRange visible_line_range{};
  if (auto* viewport = ActiveEditorViewport(); viewport != nullptr && !viewport->is_placeholder()) {
    visible_line_range.viewport = viewport;
    visible_line_range.start_line = viewport->VisualRowLineIndex(viewport->scroll_line());
    const std::size_t last_visible_row =
        viewport->visible_lines() == 0
            ? viewport->scroll_line()
            : viewport->scroll_line() + viewport->visible_lines() - 1;
    visible_line_range.end_line =
        std::min(viewport->lines().size(), viewport->VisualRowLineIndex(last_visible_row) + 1);
  }
  ++prepared_frame_id_;
  // PrepareFrameOnce stays a pure function of stored, event-sourced state so the
  // retained and full redraws of one frame paint identically. Never poll the live OS
  // pointer here: SDL_GetMouseState returns the real cursor position, which varies
  // run-to-run and would seed last_mouse_* nondeterministically — driving hover
  // highlights (gated on last_mouse_position_valid_) and breaking retained==full
  // redraw equivalence. The pointer position comes only from mouse events (and the
  // event-time SeedPointerPosition on platform stomps); until one arrives there is
  // nothing to hover and the cursor keeps its default. A pending forced reassert is
  // held until a valid position exists, then applied on the next prepared frame.
  if (last_mouse_position_valid_) {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::UpdateMouseCursor");
    UpdateMouseCursor(last_mouse_x_, last_mouse_y_, !MenuSurfaceCapturingMouse(),
                      workspace_layout_recomputed, /*during_frame_prepare=*/true);
  }
  return FrameToken{prepared_frame_id_, visible_line_range};
}

void WorkspaceShell::PrepareRenderFrame(SDL_Renderer* renderer, int width, int height) {
  (void)PrepareFrameOnce(renderer, width, height);
}

void WorkspaceShell::EnsureClipFrameAndOverlayViewModels(const WorkspaceLayout& layout) const {
  if (clip_frame_overlay_view_models_valid_ &&
      clip_frame_overlay_view_models_frame_id_ == prepared_frame_id_ &&
      clip_frame_overlay_view_models_layout_ == layout) {
    return;
  }
  clip_cached_frame_vm_.emplace(RenderViewModelBuilder(context_).BuildFrameSurface(layout));
  util::AddPerformanceCounter(util::PerfCounterId::RenderViewModelBuildFrameSurfaceCalls, 1);
  // Build the overlay model in place so the retained object keeps its row/label
  // capacities across frames. The overlay card rect derivation stays shell-owned
  // (it is shared with hit-testing and redraw-region computation).
  if (!clip_cached_overlay_vm_.has_value()) {
    clip_cached_overlay_vm_.emplace();
  }
  RenderViewModelBuilder(context_).BuildOverlaySurfaceInto(
      *clip_cached_overlay_vm_, layout, ComputeOverlayRect(layout.editor_area), text_renderer_);
  util::AddPerformanceCounter(util::PerfCounterId::RenderViewModelBuildOverlaySurfaceCalls, 1);
  clip_frame_overlay_view_models_layout_ = layout;
  clip_frame_overlay_view_models_frame_id_ = prepared_frame_id_;
  clip_frame_overlay_view_models_valid_ = true;
}

void WorkspaceShell::RootViewRenderFrameBase(SDL_Renderer* renderer,
                                             const WorkspaceLayout& layout) const {
  EnsureClipFrameAndOverlayViewModels(layout);
  RenderFrameBase(renderer, layout, *clip_cached_frame_vm_);
}

void WorkspaceShell::RootViewRenderActiveWorkspaceSurface(
    SDL_Renderer* renderer,
    const WorkspaceLayout& layout,
    const FrameToken& frame_token,
    bool draw_editor_caret,
    std::optional<SDL_FRect>* active_editor_pane_rect) {
  EnsureClipFrameAndOverlayViewModels(layout);
  RenderActiveWorkspaceSurface(renderer, layout, frame_token, draw_editor_caret, active_editor_pane_rect,
                               *clip_cached_frame_vm_, *clip_cached_overlay_vm_);
}

void WorkspaceShell::RootViewRenderOverlaySurface(SDL_Renderer* renderer,
                                                  const WorkspaceLayout& layout) {
  EnsureClipFrameAndOverlayViewModels(layout);
  RenderOverlaySurface(renderer, layout, *clip_cached_overlay_vm_);
}

void WorkspaceShell::RenderFrameBase(SDL_Renderer* renderer,
                                   const WorkspaceLayout& layout,
                                   const FrameSurfaceViewModel& frame_vm) const {
  DrawFilledRect(renderer, layout.full, theme_.window_background);
  DrawFilledRect(renderer, layout.menu_bar, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.menu_bar.x,
                          layout.menu_bar.y + layout.menu_bar.h - kWorkspaceDividerThickness,
                          layout.menu_bar.w, kWorkspaceDividerThickness),
                 theme_.border);
  // A zero-height strip (hidden when only one project is open) draws no band or divider.
  if (layout.project_tab_strip.h > 0.0f) {
    DrawFilledRect(renderer, layout.project_tab_strip, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(layout.project_tab_strip.x,
                            layout.project_tab_strip.y + layout.project_tab_strip.h -
                                kWorkspaceDividerThickness,
                            layout.project_tab_strip.w, kWorkspaceDividerThickness),
                   theme_.border);
  }
  DrawFilledRect(renderer, layout.tab_strip, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.tab_strip.x,
                          layout.tab_strip.y + layout.tab_strip.h - kWorkspaceDividerThickness,
                          layout.tab_strip.w, kWorkspaceDividerThickness),
                 theme_.border);
  DrawFilledRect(renderer, layout.breadcrumb, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.breadcrumb.x,
                          layout.breadcrumb.y + layout.breadcrumb.h - kWorkspaceDividerThickness,
                          layout.breadcrumb.w, kWorkspaceDividerThickness),
                 theme_.border);

  if (frame_vm.sidebar_visible) {
    DrawFilledRect(renderer, layout.sidebar, theme_.surface_background);
    DrawFilledRect(renderer,
                   MakeRect(layout.sidebar.x + layout.sidebar.w, layout.sidebar.y,
                            kWorkspaceDividerThickness, layout.sidebar.h),
                   context_.interaction_state.drag_target == DragTarget::SidebarDivider ? theme_.accent
                                                                      : theme_.border);
    const SDL_FRect sidebar_header = MakeRect(layout.sidebar.x, layout.sidebar.y, layout.sidebar.w,
                                              kWorkspaceHeaderHeight);
    DrawFilledRect(renderer, sidebar_header, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(sidebar_header.x,
                            sidebar_header.y + sidebar_header.h - kWorkspaceDividerThickness,
                            sidebar_header.w, kWorkspaceDividerThickness),
                   theme_.border);
  }

  if (frame_vm.bottom_panel_visible) {
    DrawFilledRect(renderer, layout.bottom_panel, theme_.surface_background);
    DrawFilledRect(renderer,
                   MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                            kWorkspaceDividerThickness),
                   context_.interaction_state.drag_target == DragTarget::BottomPanelDivider ? theme_.accent
                                                                          : theme_.border);
    const SDL_FRect panel_header = MakeRect(layout.bottom_panel.x, layout.bottom_panel.y,
                                            layout.bottom_panel.w,
                                            kWorkspaceBottomPanelHeaderHeight);
    DrawFilledRect(renderer, panel_header, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(panel_header.x,
                            panel_header.y + panel_header.h - kWorkspaceDividerThickness,
                            panel_header.w, kWorkspaceDividerThickness),
                   theme_.border);
  }
}

void WorkspaceShell::RenderActiveWorkspaceSurface(
    SDL_Renderer* renderer,
    const WorkspaceLayout& layout,
    const FrameToken& frame_token,
    bool draw_editor_caret,
    std::optional<SDL_FRect>* active_editor_pane_rect,
    const FrameSurfaceViewModel& frame_vm,
    const OverlaySurfaceViewModel& overlay_vm) {
  (void)frame_token;
  ProjectWorkspaceState& project_state = *frame_vm.project_state;
  CompareTabState* active_compare_tab = nullptr;
  MergeTabState* active_merge_tab = nullptr;
  EditorGroup& focused_group = project_state.focused_group();
  if (frame_vm.compare_surface.has_value() && focused_group.has_active_tab()) {
    TabEntry& active_tab = focused_group.active_tab();
    if (frame_vm.compare_surface->kind == TabEntry::Kind::Compare && active_tab.compare.has_value()) {
      active_compare_tab = &active_tab.compare.value();
    } else if (frame_vm.compare_surface->kind == TabEntry::Kind::Merge &&
               active_tab.merge.has_value()) {
      active_merge_tab = &active_tab.merge.value();
    }
  }
  const bool render_editor_surface = active_compare_tab == nullptr && active_merge_tab == nullptr;
  // Carve the editor surface into per-group rects once; both the pane layouts and
  // the split divider below are derived from this single split.
  const EditorGroupRectsLayout editor_surface_group_rects =
      render_editor_surface ? ComputeEditorGroupRectsForState(layout) : EditorGroupRectsLayout{};
  const EditorPaneLayouts editor_panes =
      render_editor_surface ? EditorPaneLayoutsFromGroupRects(editor_surface_group_rects)
                            : EditorPaneLayouts{};
  if (active_compare_tab != nullptr) {
    const bool draw_compare_caret =
        project_state.surface.focus == FocusTarget::Editor && active_compare_tab->right_view_active &&
        CaretVisibleNow() &&
        !(overlay_vm.current_surface == TextInputSurface::Editor &&
          !context_.text_input.composition.text.empty());
    // Plugin decorations for the EDITABLE right pane only, and resolved here
    // because the compare render TU may not read project state itself. The left
    // pane is a different revision of the file, so a decoration published against
    // the working-tree line numbers would land on the wrong line there
    // (TD-2026-08-13-206).
    const editor::FileDecorations* compare_right_decorations = nullptr;
    if (active_compare_tab->right_editable &&
        !active_compare_tab->right_viewport.path().empty() &&
        !active_compare_tab->right_viewport.dirty()) {
      const auto* presentation = project_state.plugin_presentation_if_present();
      compare_right_decorations =
          presentation != nullptr
              ? presentation->decorations.FindByPathKey(active_compare_tab->right_viewport.path_key())
              : nullptr;
    }
    RenderCompareSurface(renderer, layout.editor_surface, *active_compare_tab, draw_compare_caret,
                         CompareRenderProjectInputs{
                             .project_root = &project_state.root,
                             .diagnostics_store = &project_state.diagnostics_store,
                             .right_plugin_decorations = compare_right_decorations,
                         });
  } else if (active_merge_tab != nullptr) {
    const bool draw_merge_caret =
        project_state.surface.focus == FocusTarget::Editor && CaretVisibleNow();
    RenderMergeSurface(renderer, layout.editor_surface, project_state.root, draw_merge_caret,
                       project_state.diagnostics_store);
  } else {
    // Resolved once per frame; "hint" (the default) means show all, so the filter
    // returns the store span untouched (zero cost).
    const editor::DiagnosticSeverity diagnostics_min_severity =
        editor::ParseDiagnosticSeverity(
            GetSettingValue("diagnostics.min_severity").value_or("hint"));
    const auto diagnostics_for_viewport =
        [&project_state, diagnostics_min_severity](const editor::TextViewport& viewport)
        -> std::span<const editor::PublishedDiagnostic> {
      if (viewport.path().empty()) {
        return {};
      }
      const auto* diagnostics = project_state.diagnostics_store.FindByPathKey(viewport.path_key());
      if (diagnostics == nullptr) {
        return {};
      }
      // Reused across panes: each result is consumed by its Render call before the
      // next diagnostics_for_viewport call, so a single scratch buffer is safe.
      static thread_local std::vector<editor::PublishedDiagnostic> tls_filtered_diagnostics;
      return editor::FilterDiagnosticsAtLeastSeverity(
          std::span<const editor::PublishedDiagnostic>(*diagnostics), diagnostics_min_severity,
          tls_filtered_diagnostics);
    };
    const auto decorations_for_viewport =
        [&project_state](const editor::TextViewport& viewport) -> const editor::FileDecorations* {
      if (viewport.path().empty() || viewport.dirty()) {
        return nullptr;
      }
      const auto* pres = project_state.plugin_presentation_if_present();
      return pres != nullptr ? pres->decorations.FindByPathKey(viewport.path_key()) : nullptr;
    };
    const auto setting_enabled = [this](std::string_view id, bool default_value) {
      return SettingFlagEnabled(GetSettingValue(id), default_value);
    };
    const bool bracket_match_highlight_enabled =
        setting_enabled("editor.brackets.match_highlight.enabled", true);
    const bool indent_guides_enabled =
        setting_enabled("editor.view.indent_guides.enabled", true);
    const bool render_whitespace_enabled =
        setting_enabled("editor.view.render_whitespace", false);
    const bool blame_inline_enabled = setting_enabled("editor.blame.inline.enabled", true);
    const bool line_numbers_enabled = setting_enabled("editor.line_numbers", true);
    const bool overview_ruler_enabled = setting_enabled("editor.view.overview_ruler.enabled", true);
    // Phase E1: inline plugin-surface insets. Experimental and off by default, so
    // the editor geometry stays byte-for-byte identical for everyone else. One
    // master gate, like DebugEnabled(): with no plugin/LSP presentation bundle the
    // inset flags are unused (BuildEditorInsetGaps early-outs on a null bundle), so
    // a no-plugin session skips these three per-frame setting reads entirely.
    const editor::InsetGapFeatureFlags inset_flags =
        project_state.plugin_presentation_if_present() != nullptr
            ? editor::InsetGapFeatureFlags{
                  .inline_surfaces = setting_enabled("plugins.inline_surfaces", false),
                  .code_lens_above = setting_enabled("plugins.code_lens_above", false),
                  .ghost_text = setting_enabled("plugins.ghost_text", false),
              }
            : editor::InsetGapFeatureFlags{};
    const bool fold_enabled = setting_enabled("editor.fold.enabled", true);
    const bool occurrences_highlight_enabled_global =
        setting_enabled("editor.occurrences.enabled", true);
    const bool occurrences_case_sensitive =
        setting_enabled("editor.search.case_sensitive", false);
    // Breakpoint gutter dots only render when the debugger is enabled (default
    // off), keeping the editor byte-for-byte unchanged for non-debug users.
    const bool debug_enabled = DebugEnabled();
    const editor::BreakpointStore* breakpoint_store =
        debug_enabled ? &project_state.breakpoint_store : nullptr;
    const DebugExecutionView* debug_execution =
        debug_enabled ? &project_state.debug_execution : nullptr;
    // Folding freshness (including the fold-disabled expand+detach) already ran once
    // in PrepareFrameOnce::RefreshEditorFoldingModels; the render path only reads the
    // resolved model below via GroupFoldingModelPtr (TD-2026-07-17A-004).
    const RenderViewModelBuilder editor_render_builder(context_);
    const bool sticky_scroll_setting_enabled =
        setting_enabled("editor.fold.sticky_scroll.enabled", true);
    const int sticky_scroll_max_depth =
        ParseStickyScrollMaxDepthSetting(GetSettingValue("editor.fold.sticky_scroll.max_depth"));
    thread_local editor::EditorViewModel tls_editor_surface_vm;
    const EditorPaneLayouts& panes = editor_panes;
    thread_local std::vector<editor::EditorViewMetrics> tls_pane_scroll_metrics;
    thread_local std::vector<unsigned char> tls_pane_scroll_metrics_valid;
    tls_pane_scroll_metrics.resize(panes.size());
    tls_pane_scroll_metrics_valid.assign(panes.size(), 0);
    // Per-group editor render: each pane resolves its own group's active viewport
    // and folding model, so every pane in a split scrolls/folds independently.
    for (std::size_t pane_index = 0; pane_index < panes.size(); ++pane_index) {
      const EditorPaneLayout& pane = panes[pane_index];
      if (pane.group_index >= project_state.editor_groups.size()) {
        continue;
      }
      EditorGroup& group = project_state.editor_groups[pane.group_index];
      editor::TextViewport* viewport = GroupActiveViewport(group);
      if (viewport == nullptr) {
        continue;
      }
      if (pane.active && active_editor_pane_rect != nullptr) {
        *active_editor_pane_rect = pane.rect;
      }

      // An empty group renders the welcome home surface (recents + shortcuts).
      if (viewport->is_placeholder()) {
        editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
            text_renderer_, *viewport, pane.rect, 0, line_numbers_enabled);
        viewport->SetViewportSize(metrics.visible_rows, metrics.visible_columns);
        editor::FoldingModel* welcome_fold =
            fold_enabled ? GroupFoldingModelPtr(group) : nullptr;
        const bool sticky_active =
            fold_enabled && sticky_scroll_setting_enabled && welcome_fold != nullptr;
        // Size the metrics against the sticky band before building, not after:
        // the band's height is resolvable on its own (see StickyScrollLines) and
        // the view model is the expensive part.
        const std::size_t welcome_sticky_rows =
            RenderViewModelBuilder::StickyScrollLines(*viewport, welcome_fold, sticky_active,
                                                      sticky_scroll_max_depth)
                .size();
        if (welcome_sticky_rows != 0) {
          metrics = editor::EditorViewRenderer::ComputeMetrics(
              text_renderer_, *viewport, pane.rect, welcome_sticky_rows, line_numbers_enabled);
          viewport->SetViewportSize(metrics.visible_rows, metrics.visible_columns);
        }
        editor_render_builder.BuildEditorViewModelInto(
            tls_editor_surface_vm, *viewport, metrics.visible_rows, welcome_fold,
            occurrences_highlight_enabled_global, occurrences_case_sensitive, sticky_active,
            sticky_scroll_max_depth, render_whitespace_enabled, debug_enabled, breakpoint_store,
            debug_execution);
        // Borrowed, not copied: BuildWelcomeView owns a memo keyed on (MRU
        // revision, project root) and the renderer only reads the model.
        const editor::WelcomeViewModel& welcome_vm =
            editor_render_builder.BuildWelcomeView(recents_service_);
        editor_view_renderer_.Render(renderer, text_renderer_, theme_, *viewport, pane.rect,
                                     pane.active && draw_editor_caret, "", std::nullopt,
                                     std::nullopt, {}, &tls_editor_surface_vm,
                                     bracket_match_highlight_enabled, indent_guides_enabled,
                                     render_whitespace_enabled, welcome_fold, &welcome_vm);
        continue;
      }

      editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
          text_renderer_, *viewport, pane.rect, 0, line_numbers_enabled);
      viewport->SetViewportSize(metrics.visible_rows, metrics.visible_columns);
      editor::FoldingModel* group_folding_model =
          fold_enabled ? GroupFoldingModelPtr(group) : nullptr;
      const editor::FoldingModel* folding_for_vm = fold_enabled ? group_folding_model : nullptr;
      const bool sticky_active =
          fold_enabled && sticky_scroll_setting_enabled && folding_for_vm != nullptr;
      const bool occurrences_for_pane =
          occurrences_highlight_enabled_global && pane.active;
      // Resolve the sticky band first so the metrics are sized once. Building the
      // view model to discover the band's height meant building it twice per
      // frame -- fold gutter marks, breakpoints, whitespace runs, occurrence
      // scan and inset gaps all recomputed -- for every pane with sticky scroll
      // on, which is the default.
      const std::size_t sticky_rows =
          RenderViewModelBuilder::StickyScrollLines(*viewport, folding_for_vm, sticky_active,
                                                    sticky_scroll_max_depth)
              .size();
      if (sticky_rows != 0) {
        metrics = editor::EditorViewRenderer::ComputeMetrics(
            text_renderer_, *viewport, pane.rect, sticky_rows, line_numbers_enabled);
        viewport->SetViewportSize(metrics.visible_rows, metrics.visible_columns);
      }
      // Named for the whole pane, because that is what it covers: the view-model
      // build, the blame overlay, the find-highlight fragments, the row render and
      // the insets. It used to be called `BuildEditorViewModel`, which is only the
      // first of those -- and on `editor_soft_wrap_long_line_scroll` the pane cost
      // 1.05 ms a frame while its one instrumented child accounted for 0.19, so
      // 0.86 ms a frame had no name at all (TD-2026-08-15-248).
      util::PerformanceTrace::Scope vm_scope("WorkspaceShell::Render::EditorPane");
      {
        util::PerformanceTrace::Scope build_scope(
            "WorkspaceShell::Render::EditorPane::BuildViewModel");
        editor_render_builder.BuildEditorViewModelInto(
            tls_editor_surface_vm, *viewport, metrics.visible_rows, folding_for_vm,
            occurrences_for_pane, occurrences_case_sensitive, sticky_active,
            sticky_scroll_max_depth, render_whitespace_enabled, debug_enabled, breakpoint_store,
            debug_execution, inset_flags, metrics.line_height);
      }
      tls_pane_scroll_metrics[pane_index] = metrics;
      tls_pane_scroll_metrics_valid[pane_index] = 1;
      const auto blame_overlay = [&] {
        util::PerformanceTrace::Scope blame_scope(
            "WorkspaceShell::Render::EditorPane::BlameOverlay");
        return (pane.active && blame_inline_enabled)
                   ? editor_blame_overlay_service_.BuildEditorOverlay(
                         project_state.root, text_renderer_, git_blame_service_, *viewport,
                         pane.rect, 520.0f, tls_editor_surface_vm.sticky_lines.size(),
                         line_numbers_enabled)
                   : std::nullopt;
      }();
      if (pane.active) {
        // When blame is disabled this clears any previously-visible overlay so
        // hover/click targets never resolve against stale blame geometry.
        editor_blame_overlay_service_.SetVisibleOverlay(blame_overlay);
      }
      // In-file find highlights: regex/whole-buffer mode drives the highlight from the
      // precomputed (possibly multi-line) match set — a literal query scan cannot
      // reproduce a regex or `\n`-spanning match — while literal mode keeps the fast
      // cached per-line query scan.
      const BufferSearchState& buffer_search_state =
          project_state.overlay.workflow.buffer_search;
      const bool buffer_find_active =
          pane.active && (overlay_vm.mode == OverlayMode::BufferSearch ||
                          overlay_vm.mode == OverlayMode::BufferReplace);
      const bool buffer_regex_active = buffer_find_active && buffer_search_state.regex;
      const std::span<const editor::SelectionRange> explicit_search_matches =
          buffer_regex_active
              ? BufferSearchHighlightFragments(*viewport, buffer_search_state.matches,
                                               buffer_search_state.matches_revision)
              : std::span<const editor::SelectionRange>{};
      const std::string_view buffer_search_query_for_render =
          (buffer_find_active && !buffer_search_state.regex)
              ? std::string_view(overlay_vm.buffer_search_query_text)
              : std::string_view{};
      editor_view_renderer_.Render(renderer, text_renderer_, theme_, *viewport, pane.rect,
                                   pane.active && draw_editor_caret,
                                   buffer_search_query_for_render,
                                   pane.active ? ActiveBufferSearchMatch() : std::nullopt,
                                   blame_overlay, diagnostics_for_viewport(*viewport),
                                   &tls_editor_surface_vm,
                                   pane.active && bracket_match_highlight_enabled,
                                   indent_guides_enabled, render_whitespace_enabled,
                                   group_folding_model, nullptr,
                                   decorations_for_viewport(*viewport), line_numbers_enabled,
                                   explicit_search_matches);
      {
        util::PerformanceTrace::Scope insets_scope(
            "WorkspaceShell::Render::EditorPane::Insets");
        DrawEditorInsets(renderer, pane.rect, metrics, viewport->scroll_line(),
                         tls_editor_surface_vm);
      }

      // Insertion-point indicator for a selection drag-and-drop in flight. No
      // other gesture here has one, and without it the drop is a guess: the real
      // caret stays inside the selection being dragged (TD-2026-08-13-204).
      // Drawn after the pane so it is never painted over, and only on the pane
      // that owns the gesture.
      if (pane.active && context_.interaction_state.text_dragging() &&
          context_.interaction_state.text_drag_has_drop) {
        const std::size_t drop_line = context_.interaction_state.text_drag_drop_line;
        const std::size_t drop_column = context_.interaction_state.text_drag_drop_column;
        const std::size_t visual_row = viewport->VisualRowForLine(drop_line);
        if (visual_row >= viewport->scroll_line() &&
            visual_row < viewport->scroll_line() + metrics.visible_rows) {
          const std::size_t visual_column = viewport->VisualColumnAt(drop_line, drop_column);
          const float x = metrics.text_x +
                          static_cast<float>(visual_column - std::min(visual_column,
                                                                      viewport->horizontal_scroll())) *
                              text_renderer_.CharWidth();
          const float y = metrics.first_line_y +
                          static_cast<float>(visual_row - viewport->scroll_line()) *
                              metrics.line_height;
          if (x >= metrics.text_x && x < pane.rect.x + pane.rect.w) {
            DrawFilledRect(renderer, MakeRect(x, y, 2.0f, metrics.line_height), theme_.accent);
          }
        }
      }
    }
    // Overview-ruler marker caches, one per pane, keyed by a cheap signature so the
    // search+diagnostic marker set only rebuilds when a source or the lane geometry
    // changes (never on scroll or caret movement).
    thread_local std::vector<EditorOverviewPaneCache> tls_overview_caches;
    thread_local std::vector<overview::MarkerInput> tls_overview_inputs;
    thread_local std::vector<std::uint32_t> tls_overview_buckets;
    thread_local std::vector<SDL_Color> tls_overview_palette;
    if (overview_ruler_enabled) {
      tls_overview_caches.resize(panes.size());
    }
    for (std::size_t pane_index = 0; pane_index < panes.size(); ++pane_index) {
      const EditorPaneLayout& pane = panes[pane_index];
      if (pane.group_index >= project_state.editor_groups.size()) {
        continue;
      }
      editor::TextViewport* viewport =
          GroupActiveViewport(project_state.editor_groups[pane.group_index]);
      if (viewport == nullptr || viewport->is_placeholder() ||
          pane_index >= tls_pane_scroll_metrics_valid.size() ||
          tls_pane_scroll_metrics_valid[pane_index] == 0) {
        continue;
      }
      const editor::EditorViewMetrics& metrics = tls_pane_scroll_metrics[pane_index];
      const auto scroll_layout = ComputeEditorScrollLayout(pane.rect, *viewport, metrics);
      if (scroll_layout.vertical_scrollbar.has_value()) {
        DrawScrollbar(renderer, theme_, scroll_layout.vertical_scrollbar->track,
                      scroll_layout.vertical_scrollbar->thumb,
                      pane.active &&
                         context_.interaction_state.drag_target == DragTarget::EditorVerticalScrollbar);
        if (overview_ruler_enabled) {
          DrawEditorOverviewRuler(renderer, theme_, project_state, overlay_vm, *viewport,
                                  pane.active, diagnostics_min_severity,
                                  scroll_layout.vertical_scrollbar->track, pane.rect.x,
                                  tls_overview_caches[pane_index], tls_overview_inputs,
                                  tls_overview_buckets, tls_overview_palette);
        }
      }
      if (scroll_layout.horizontal_scrollbar.has_value()) {
        DrawScrollbar(renderer, theme_, scroll_layout.horizontal_scrollbar->track,
                      scroll_layout.horizontal_scrollbar->thumb,
                      pane.active &&
                         context_.interaction_state.drag_target == DragTarget::EditorHorizontalScrollbar);
      }
    }
    for (const EditorSplitDividerRect& divider : editor_surface_group_rects.dividers) {
      const bool divider_active =
          context_.interaction_state.drag_target == DragTarget::EditorSplitDivider &&
          divider.node == context_.interaction_state.drag_editor_split_node &&
          divider.boundary == context_.interaction_state.drag_editor_split_boundary;
      DrawFilledRect(renderer, divider.rect, divider_active ? theme_.accent : theme_.border);
    }
  }

  if (active_compare_tab != nullptr) {
    RenderCompareScrollbars(renderer, layout.editor_surface, *active_compare_tab);
  } else if (active_merge_tab != nullptr) {
    RenderMergeScrollbars(renderer, layout.editor_surface);
  }

  if (frame_vm.editor_banner.has_value()) {
    const EditorBannerViewModel& banner_vm = *frame_vm.editor_banner;
    const SDL_FRect strip = ComputeEditorBannerStripRect(layout.editor_surface);
    DrawFilledRect(renderer, strip, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(strip.x, strip.y + strip.h - kWorkspaceDividerThickness, strip.w,
                            kWorkspaceDividerThickness),
                   theme_.border);
    DrawFilledRect(renderer, MakeRect(strip.x, strip.y, 3.0f, strip.h), theme_.accent);

    const EditorBannerButtonLayout buttons =
        ComputeEditorBannerButtonRects(strip, banner_vm.has_actions);
    const float message_right = banner_vm.has_actions ? buttons.reload.x : buttons.dismiss.x;
    const SDL_FRect message_rect = MakeRect(strip.x + 12.0f, strip.y,
                                            std::max(0.0f, message_right - strip.x - 20.0f), strip.h);
    const SDL_Rect clip{static_cast<int>(message_rect.x), static_cast<int>(message_rect.y),
                        static_cast<int>(message_rect.w), static_cast<int>(message_rect.h)};
    {
      const render::ScopedRenderClip clip_scope(renderer, clip);
      DrawVCenteredTextOn(text_renderer_, renderer, message_rect, 0.0f, theme_.text_secondary,
                          theme_.chrome_background, banner_vm.message);
    }

    const auto draw_banner_button = [&](const SDL_FRect& rect, std::string_view label,
                                        ButtonTone tone) {
      if (rect.w <= 0.0f) {
        return;
      }
      DrawButtonCentered(text_renderer_, renderer, theme_, rect, label, tone,
                         ButtonVisualState{
                             .enabled = true,
                             .hovered = last_mouse_position_valid_ &&
                                        Contains(rect, last_mouse_x_, last_mouse_y_),
                             .active = false,
                         });
    };
    if (banner_vm.has_actions) {
      draw_banner_button(buttons.reload, "Reload", ButtonTone::Neutral);
      draw_banner_button(buttons.overwrite, "Overwrite", ButtonTone::Destructive);
      draw_banner_button(buttons.keep, "Keep", ButtonTone::Neutral);
    }
    draw_banner_button(buttons.dismiss, "x", ButtonTone::Neutral);
  }

  if (project_state.surface.focus == FocusTarget::Editor) {
    DrawSurfaceFocusRing(renderer, layout.editor_surface);
  }

  // Floating debug control bar, drawn over the editor whenever a session is live.
  // Stacks below the find widget (which renders later, in the overlay layer) when
  // both are visible, so draw order between them is irrelevant.
  if (render_editor_surface && DebugToolbarVisible()) {
    RenderDebugToolbar(renderer, layout, IsDebugSessionStopped(),
                       DebugToolbarAvoidBelowY(layout));
  }
}

}  // namespace microide::workspace
