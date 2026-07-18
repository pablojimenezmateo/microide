#include "workspace/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "editor/TextLayout.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "workspace/OverviewRuler.h"
#include "workspace/RenderViewModelBuilder.h"
#include "render/ScopedRenderClip.h"
#include "workspace/SettingFlags.h"
#include "workspace/WorkspaceTextInputCoordinator.h"

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
    cache.markers = overview::ReduceMarkers(inner_lane, total_rows, inputs, buckets, palette);
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
    SDL_SetRenderDrawColor(renderer, caret_marker.color.r, caret_marker.color.g,
                           caret_marker.color.b, caret_marker.color.a);
    SDL_RenderFillRect(renderer, &caret_marker.rect);
  }
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
  post_render_full_redraws_remaining_ = std::max(post_render_full_redraws_remaining_, 2);
}

void WorkspaceShell::DrawFilledRect(SDL_Renderer* renderer,
                                    const SDL_FRect& rect,
                                    SDL_Color color) const {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
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
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  SDL_RenderRect(renderer, &rect);
}

std::string WorkspaceShell::TruncateLabel(std::string_view text, float max_width) const {
  return text_renderer_.TruncateToWidth(text, max_width);
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
  ConsumePendingProjectOpenDialogResult();
  ConsumePendingFontFileDialogResult();
  ConsumeProjectSearchUpdates();
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
  prepare_cached_sidebar_vm_.emplace(view_models.BuildSidebarSurface());
  prepare_cached_bottom_panel_vm_.emplace(view_models.BuildBottomPanelSurface());
  // The debug pane can only ever be visible when the debugger is enabled, so skip
  // building its view model entirely in the common debug-off case. RenderDebugPane
  // tolerates the empty optional.
  if (DebugEnabled()) {
    prepare_cached_debug_pane_vm_.emplace(view_models.BuildDebugPaneSurface());
  }
  const SidebarSurfaceViewModel& sidebar_vm = *prepare_cached_sidebar_vm_;
  const BottomPanelSurfaceViewModel& panel_vm = *prepare_cached_bottom_panel_vm_;
  ProjectWorkspaceState& project_state = *sidebar_vm.project_state;
  ApplyLiveSettings();
  // Resolve editor folding freshness once here, before any RenderClip runs, so the
  // state-mutating fold scan executes a single time per prepared frame instead of
  // per pane on every partial-redraw RenderClip. Render then consumes the already-
  // resolved model via GroupFoldingModelPtr (TD-2026-07-17A-004).
  RefreshEditorFoldingModels();
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
  RefreshStatusBar();
  RefreshSettingsOverlayCatalog();
  SDL_Window* render_window = SDL_GetRenderWindow(renderer);
  MakeTextInputCoordinator().SyncTextInputSurface(render_window);
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
  clip_cached_overlay_vm_.emplace(RenderViewModelBuilder(context_).BuildOverlaySurface());
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
      render_editor_surface ? ComputeEditorSurfaceGroupRects(layout.editor_surface)
                            : EditorGroupRectsLayout{};
  const std::vector<EditorPaneLayout> editor_panes =
      render_editor_surface ? EditorPaneLayoutsFromGroupRects(editor_surface_group_rects)
                            : std::vector<EditorPaneLayout>{};
  if (active_compare_tab != nullptr) {
    const bool draw_compare_caret =
        project_state.surface.focus == FocusTarget::Editor && active_compare_tab->right_view_active &&
        CaretVisibleNow() &&
        !(overlay_vm.current_surface == TextInputSurface::Editor &&
          !context_.text_input.composition.text.empty());
    RenderCompareSurface(renderer, layout.editor_surface, *active_compare_tab, project_state.root,
                         draw_compare_caret, project_state.diagnostics_store);
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
        [this, &project_state, diagnostics_min_severity](const editor::TextViewport& viewport)
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
    const auto draw_review_comment_markers =
        [this, renderer](const editor::TextViewport& viewport,
                         const SDL_FRect& pane_rect,
                         const editor::EditorViewMetrics& editor_metrics) {
          if (renderer == nullptr || viewport.path().empty() || viewport.is_placeholder() ||
              review_comments_registry_.Empty()) {
            return;
          }

          // Resolve this document's marker index once (one URI hash) instead of
          // re-hashing the URI twice per visible row.
          const std::string uri = viewport.path().generic_string();
          const auto document_markers = review_comments_registry_.MarkersForUri(uri);
          if (!document_markers) {
            return;
          }
          for (std::size_t row = 0; row < viewport.visible_lines(); ++row) {
            const std::size_t visual_row_index = viewport.scroll_line() + row;
            if (visual_row_index >= viewport.visual_line_count()) {
              break;
            }
            const auto row_meta = viewport.WrappedVisualRowLayout(visual_row_index);
            if (viewport.soft_wrap() && row_meta.visual_start != 0) {
              continue;
            }
            const std::size_t line_index = row_meta.line_index;
            const int one_based_line = static_cast<int>(line_index + 1);
            if (!document_markers.HasMarkerAtLine(one_based_line)) {
              continue;
            }
            const float y =
                editor_metrics.first_line_y + static_cast<float>(row) * editor_metrics.line_height;
            const SDL_FRect marker_rect = SDL_FRect{
                pane_rect.x + editor_metrics.gutter_width - 6.0f,
                y + std::max(1.0f, (editor_metrics.line_height - 6.0f) * 0.5f),
                3.0f,
                6.0f,
            };
            DrawFilledRect(renderer, marker_rect, theme_.accent);
          }
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
    const std::vector<EditorPaneLayout>& panes = editor_panes;
    thread_local std::vector<editor::EditorViewMetrics> tls_pane_scroll_metrics;
    thread_local std::vector<unsigned char> tls_pane_scroll_metrics_valid;
    tls_pane_scroll_metrics.resize(panes.size());
    tls_pane_scroll_metrics_valid.assign(panes.size(), 0);
    // Per-group editor render: each pane resolves its own group's active viewport
    // and folding model, so the two groups in a split scroll/fold independently.
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
        editor_render_builder.BuildEditorViewModelInto(
            tls_editor_surface_vm, *viewport, metrics.visible_rows, welcome_fold,
            occurrences_highlight_enabled_global, occurrences_case_sensitive, sticky_active,
            sticky_scroll_max_depth, render_whitespace_enabled, debug_enabled, breakpoint_store,
            debug_execution);
        if (!tls_editor_surface_vm.sticky_lines.empty()) {
          metrics = editor::EditorViewRenderer::ComputeMetrics(
              text_renderer_, *viewport, pane.rect, tls_editor_surface_vm.sticky_lines.size(),
              line_numbers_enabled);
          viewport->SetViewportSize(metrics.visible_rows, metrics.visible_columns);
          editor_render_builder.BuildEditorViewModelInto(
              tls_editor_surface_vm, *viewport, metrics.visible_rows, welcome_fold,
              occurrences_highlight_enabled_global, occurrences_case_sensitive, sticky_active,
              sticky_scroll_max_depth, render_whitespace_enabled, debug_enabled, breakpoint_store,
              debug_execution);
        }
        thread_local editor::WelcomeViewModel tls_welcome_vm;
        tls_welcome_vm = editor_render_builder.BuildWelcomeView(recents_service_);
        editor_view_renderer_.Render(renderer, text_renderer_, theme_, *viewport, pane.rect,
                                     pane.active && draw_editor_caret, "", std::nullopt,
                                     std::nullopt, {}, &tls_editor_surface_vm,
                                     bracket_match_highlight_enabled, indent_guides_enabled,
                                     render_whitespace_enabled, welcome_fold, &tls_welcome_vm);
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
      editor_render_builder.BuildEditorViewModelInto(
          tls_editor_surface_vm, *viewport, metrics.visible_rows, folding_for_vm,
          occurrences_for_pane, occurrences_case_sensitive, sticky_active, sticky_scroll_max_depth,
          render_whitespace_enabled, debug_enabled, breakpoint_store, debug_execution,
          inset_flags, metrics.line_height);
      if (!tls_editor_surface_vm.sticky_lines.empty()) {
        metrics = editor::EditorViewRenderer::ComputeMetrics(
            text_renderer_, *viewport, pane.rect, tls_editor_surface_vm.sticky_lines.size(),
            line_numbers_enabled);
        viewport->SetViewportSize(metrics.visible_rows, metrics.visible_columns);
        editor_render_builder.BuildEditorViewModelInto(
            tls_editor_surface_vm, *viewport, metrics.visible_rows, folding_for_vm,
            occurrences_for_pane, occurrences_case_sensitive, sticky_active, sticky_scroll_max_depth,
            render_whitespace_enabled, debug_enabled, breakpoint_store, debug_execution,
            inset_flags, metrics.line_height);
      }
      tls_pane_scroll_metrics[pane_index] = metrics;
      tls_pane_scroll_metrics_valid[pane_index] = 1;
      const auto blame_overlay =
          (pane.active && blame_inline_enabled)
              ? editor_blame_overlay_service_.BuildEditorOverlay(
                    project_state.root, text_renderer_, git_blame_service_, *viewport,
                    pane.rect, 520.0f, tls_editor_surface_vm.sticky_lines.size(),
                    line_numbers_enabled)
                      : std::nullopt;
      if (pane.active) {
        // When blame is disabled this clears any previously-visible overlay so
        // hover/click targets never resolve against stale blame geometry.
        editor_blame_overlay_service_.SetVisibleOverlay(blame_overlay);
      }
      editor_view_renderer_.Render(renderer, text_renderer_, theme_, *viewport, pane.rect,
                                   pane.active && draw_editor_caret,
                                   pane.active &&
                                           (overlay_vm.mode == OverlayMode::BufferSearch ||
                                            overlay_vm.mode == OverlayMode::BufferReplace)
                                       ? overlay_vm.buffer_search_query_text
                                       : "",
                                   pane.active ? ActiveBufferSearchMatch() : std::nullopt,
                                   blame_overlay, diagnostics_for_viewport(*viewport),
                                   &tls_editor_surface_vm,
                                   pane.active && bracket_match_highlight_enabled,
                                   indent_guides_enabled, render_whitespace_enabled,
                                   group_folding_model, nullptr,
                                   decorations_for_viewport(*viewport), line_numbers_enabled);
      DrawEditorInsets(renderer, pane.rect, metrics, viewport->scroll_line(),
                       tls_editor_surface_vm);
      draw_review_comment_markers(*viewport, pane.rect, metrics);

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
    for (const EditorSplitDividerLayout& divider :
         EditorSplitDividerLayoutsFromGroupRects(editor_surface_group_rects)) {
      const bool divider_active =
          context_.interaction_state.drag_target == DragTarget::EditorSplitDivider &&
          divider.divider_index == context_.interaction_state.drag_editor_split_divider_index &&
          divider.node_path == context_.interaction_state.drag_editor_split_path;
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
