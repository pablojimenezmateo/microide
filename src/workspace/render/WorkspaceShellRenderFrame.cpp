#include "render/SurfacePrimitives.h"
#include "workspace/services/SidebarService.h"

#include "workspace/OpenDocumentWatch.h"
#include "workspace/render/BottomPanelHeaderPaint.h"
#include "workspace/render/BottomPanelOutputPaint.h"
#include "workspace/render/SidebarListPaint.h"
#include "workspace/render/BottomPanelTerminalChromePaint.h"
#include "workspace/render/BottomPanelTerminalGridPaint.h"
#include "workspace/render/NotificationToastPaint.h"
#include "workspace/render/DebugPanePaint.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/render/CompareRowUnits.h"
#include "workspace/render/CompareReviewControls.h"
#include "workspace/render/EditorRowUnits.h"
#include "workspace/render/GitSidebarPaint.h"
#include "workspace/render/MenuPopupPaint.h"
#include "workspace/ProjectSearchPanelLayout.h"
#include "workspace/render/SearchSidebarPaint.h"
#include "workspace/render/SettingsCardPaint.h"
#include "workspace/render/TabStripPaint.h"
#include "workspace/render/SidebarModeRowPaint.h"
#include "workspace/render/TreeSidebarPaint.h"
#include "workspace/render/HoverCardLimits.h"
#include "workspace/render/WorkspaceShellRenderPrimitives.h"

#include <cstring>
#include <type_traits>
#include <algorithm>
#include <array>
#include <cassert>
#include <functional>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "editor/PluginDecorationStore.h"
#include "editor/PluginSurfaceStore.h"
#include "editor/TextLayout.h"
#include "util/Fnv1a.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "workspace/render/DiffOverviewMarkers.h"
#include "workspace/render/OverviewRuler.h"
#include "workspace/PluginSurfacePreview.h"
#include "workspace/render/RenderViewModelBuilder.h"
#include "workspace/services/EditorTabService.h"
#include "workspace/services/TerminalPanelService.h"
#include "workspace/WorkspaceTextSearch.h"
#include "workspace/git/MergeResultValidation.h"
#include "workspace/MergeWrapRows.h"
#include "render/ScopedRenderClip.h"
#include "workspace/SettingFlags.h"
#include "workspace/WorkspaceUiText.h"
#include "workspace/coordinators/WorkspaceTabCoordinator.h"
#include "workspace/coordinators/WorkspaceDiffTabCoordinator.h"
#include "workspace/coordinators/WorkspaceTextInputCoordinator.h"

namespace microide::workspace {

using namespace detail;

namespace {

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
  std::size_t fold_revision = 0;
  bool operator==(const EditorOverviewSignature&) const = default;
};

struct EditorOverviewPaneCache {
  EditorOverviewSignature sig;
  SDL_FRect track{};
  std::vector<overview::Marker> markers;
};

void DrawEditorOverviewRuler(SDL_Renderer* renderer, const render::Theme& theme,
                             const editor::TextViewport& viewport,
                             std::span<const editor::PublishedDiagnostic> diagnostics,
                             std::uint64_t diagnostics_revision, editor::DiagnosticSeverity diag_min_severity,
                             const BufferSearchState* search, const SDL_FRect& track,
                             float pane_left, EditorOverviewPaneCache& cache,
                             std::vector<overview::MarkerInput>& inputs,
                             std::vector<std::uint32_t>& buckets, std::vector<SDL_Color>& palette) {
  const SDL_FRect lane = overview::LaneRect(track, pane_left);
  const SDL_FRect inner_lane = overview::LaneInnerRect(lane);
  const std::size_t total_rows = viewport.visual_line_count();
  const auto marker_row = [&viewport](std::size_t line) { return viewport.VisualRowForLine(line); };

  static const BufferSearchState kNoSearch;
  const BufferSearchState& buffer_search = search != nullptr ? *search : kNoSearch;
  const bool search_active = search != nullptr && !buffer_search.matches.empty();

  EditorOverviewSignature sig;
  sig.viewport = &viewport;
  sig.line_count = total_rows;
  sig.dirty = viewport.dirty();
  sig.diag_revision = diagnostics_revision;
  sig.diag_min_severity = static_cast<int>(diag_min_severity);
  sig.search_active = search_active;
  sig.search_revision = buffer_search.matches_revision;
  sig.search_selected = buffer_search.selected_index;
  sig.search_count = buffer_search.matches.size();
  sig.theme_token = overview::ThemeMarkerToken(theme);
  sig.fold_revision = viewport.folding_revision();

  if (cache.sig != sig || !RectsEqual(cache.track, inner_lane)) {
    inputs.clear();
    {
      {
        const std::span<const editor::PublishedDiagnostic> filtered = diagnostics;
        constexpr std::size_t kMaxRow = static_cast<std::size_t>(std::numeric_limits<int>::max());
        const auto clamp_row = [&](std::size_t line) {
          const std::size_t row = marker_row(line);
          return row > kMaxRow ? std::numeric_limits<int>::max() : static_cast<int>(row);
        };
        for (const editor::PublishedDiagnostic& diagnostic : filtered) {
          const int start = clamp_row(diagnostic.range.start.line);
          const std::size_t end_line =
              std::max(diagnostic.range.end.line, diagnostic.range.start.line);
          const int end = std::max(start, clamp_row(end_line)) +
                          (clamp_row(end_line) < std::numeric_limits<int>::max() ? 1 : 0);
          inputs.push_back(overview::MarkerInput{.start_row = start,
                                                 .end_row = end,
                                                 .color = OverviewSeverityColor(theme, diagnostic.severity),
                                                 .priority = OverviewSeverityPriority(diagnostic.severity)});
        }
      }
    }
    if (search_active) {
      for (std::size_t i = 0; i < buffer_search.matches.size(); ++i) {
        const int line = static_cast<int>(marker_row(buffer_search.matches[i].start.line));
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

  const int caret_row = static_cast<int>(viewport.cursor_visual_row());
  const overview::MarkerInput caret_input{.start_row = caret_row,
                                          .end_row = caret_row + 1,
                                          .color = theme.cursor,
                                          .priority = 0};
  overview::Marker caret_marker;
  if (overview::BuildMarker(inner_lane, total_rows, caret_input, caret_marker)) {
    render::SetDrawColor(renderer, caret_marker.color);
    render::FillRectInDrawColor(renderer, caret_marker.rect);
  }
}

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

}

void WorkspaceShell::ResizeTerminalToPanel(const WorkspaceLayout& layout) {
  TerminalTabState* tab = prepare_cached_bottom_panel_vm_.has_value()
                              ? prepare_cached_bottom_panel_vm_->terminal_tab
                              : nullptr;
  if (tab == nullptr) {
    return;
  }

  util::PerformanceTrace::Scope trace_scope("WorkspaceShell::ResizeTerminalToPanel");
  const int rows = BottomPanelVisibleRows(layout.bottom_panel.h);
  const TerminalPaneRectsLayout rects = tab->PaneRects(BottomPanelContentRect(layout));
  bool regridded = false;
  for (std::size_t i = 0; i < tab->panes.size() && i < rects.panes.size(); ++i) {
    TerminalPaneState* pane = tab->panes[i].get();
    if (pane == nullptr) {
      continue;
    }
    const float usable_width =
        std::max(16.0f, rects.panes[i].w - 24.0f - kWorkspaceScrollbarThickness - 6.0f);
    const int columns = std::max(
        1, static_cast<int>(
               std::floor(usable_width / std::max(1.0f, terminal_text_renderer_.CharWidth()))));
    if (pane->session.rows() == static_cast<std::size_t>(rows) &&
        pane->session.columns() == static_cast<std::size_t>(columns)) {
      continue;
    }
    pane->session.Resize(static_cast<std::size_t>(rows), static_cast<std::size_t>(columns));
    regridded = true;
  }
  if (regridded) {
    post_render_redraws_remaining_ = std::max(post_render_redraws_remaining_, 2);
  }
}

void WorkspaceShell::DrawFilledRect(SDL_Renderer* renderer,
                                    const SDL_FRect& rect,
                                    SDL_Color color) const {
  render::FillRect(renderer, rect, color);
}

void WorkspaceShell::DrawSurfaceFocusRing(SDL_Renderer* renderer, const SDL_FRect& region) const {
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
  render::OutlineRect(renderer, rect, color);
}

std::string_view WorkspaceShell::TruncateLabelView(std::string_view text, float max_width) const {
  return text_renderer_.TruncateToWidthEphemeralView(text, max_width);
}

struct WorkspaceShell::FramePrepPass {
  SDL_Renderer* renderer = nullptr;
  int width = 0;
  int height = 0;
  const SidebarSurfaceViewModel* sidebar_vm = nullptr;
  const BottomPanelSurfaceViewModel* panel_vm = nullptr;
  ProjectWorkspaceState* project_state = nullptr;
  WorkspaceLayout layout;
  bool workspace_layout_recomputed = false;
  std::vector<DerivedDamage::Unit>* chrome = nullptr;
  PaintPassExtents* extents = nullptr;
  NotificationsViewModel toasts;
  std::optional<SDL_FRect> modal;

  void ChromeUnit(const std::optional<SDL_FRect>& rect, std::uint64_t key, ChromePainter painter) const {
    const SDL_FRect at = rect.value_or(SDL_FRect{});
    chrome->push_back({at, key});
    if (painter != ChromePainter::Always) {
      (*extents)[static_cast<PaintPass>(painter)].Add(at);
    }
  }
};

WorkspaceShell::FrameToken WorkspaceShell::PrepareFrameOnce(SDL_Renderer* renderer,
                                                            int width,
                                                            int height) {
  if (renderer == nullptr || width <= 0 || height <= 0) {
    return FrameToken{};
  }
  ++prepared_frame_id_;

  clip_frame_overlay_view_models_valid_ = false;
  prepare_cached_sidebar_vm_.reset();
  prepare_cached_bottom_panel_vm_.reset();
  prepare_cached_debug_pane_vm_.reset();
  prepare_cached_text_input_vm_.reset();

  util::PerformanceTrace::Scope trace_scope("WorkspaceShell::PrepareFrameOnce");
  util::AddPerformanceCounter(util::PerfCounterId::FramePrepareCalls);
  FramePrepPass pass{.renderer = renderer, .width = width, .height = height};
  PrepareFrameState(pass);
  pass.sidebar_vm = &*prepare_cached_sidebar_vm_;
  pass.panel_vm = &*prepare_cached_bottom_panel_vm_;
  pass.project_state = pass.sidebar_vm->project_state;
  ResolveFrameLayout(pass);
  pass.chrome = &frame_state_.derived_damage.Begin(DerivedSurface::Chrome);
  {
    PaintPassExtents& extents = frame_state_.paint_pass_extents;
    extents.Clear();
    pass.extents = &extents;
    const WorkspaceLayout& layout = pass.layout;
    const auto region = [&extents](PaintPass paint_pass, const SDL_FRect& rect, bool shown) {
      extents.region_shown[static_cast<std::size_t>(paint_pass)] = shown;
      extents[paint_pass].Add(rect);
    };
    const auto shown = [](const SDL_FRect& r) { return r.w > 0.0f && r.h > 0.0f; };
    region(PaintPass::Editor, layout.editor_surface, shown(layout.editor_surface));
    region(PaintPass::Sidebar, layout.sidebar, shown(layout.sidebar));
    region(PaintPass::RightPane, layout.right_pane, shown(layout.right_pane));
    region(PaintPass::BottomPanel, layout.bottom_panel, shown(layout.bottom_panel));
    region(PaintPass::WindowChrome, layout.menu_bar, true);
    for (const SDL_FRect& band : {layout.project_tab_strip, layout.tab_strip, layout.breadcrumb}) {
      extents[PaintPass::WindowChrome].Add(band);
    }
  }
  PrepareChromeFrame(pass);
  PrepareFrameCompletions(pass);
  PrepareEditorPaneMetrics(pass);
  PrepareEditorPaneUnits(pass);
  PrepareWorkbenchChromeUnits(pass);
  ResolvePointerOccluder(pass);
  PrepareWindowChromeUnits(pass);
  PreparePanelSurfaces(pass);
  PrepareSidebarSurfaces(pass);
  PrepareTextInputOverlays(pass);
  PrepareHoverFrame(pass);
  frame_state_.derived_damage.Commit(
      DerivedSurface::Chrome, [this](const SDL_FRect& rect) { RequestRedrawRect(rect); },
      DerivedDamage::Coalesce::Apart);
  FrameToken::VisibleLineRange visible_line_range{};
  if (auto* viewport = ActiveEditorViewport(); viewport != nullptr && !viewport->is_placeholder()) {
    visible_line_range.viewport = viewport;
    visible_line_range.start_line = viewport->VisualRowLineIndex(viewport->scroll_line());
    const std::size_t last_visible_row = viewport->visible_lines() == 0
                                             ? viewport->scroll_line()
                                             : viewport->scroll_line() + viewport->visible_lines() - 1;
    visible_line_range.end_line =
        std::min(viewport->lines().size(), viewport->VisualRowLineIndex(last_visible_row) + 1);
  }
  if (last_mouse_position_valid_) {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::UpdateMouseCursor");
    UpdateMouseCursor(last_mouse_x_, last_mouse_y_, !MenuSurfaceCapturingMouse(), pass.workspace_layout_recomputed,
                      true);
  }
  return FrameToken{prepared_frame_id_, visible_line_range};
}

void WorkspaceShell::PrepareFrameState(FramePrepPass& pass) {
  SDL_Renderer* const renderer = pass.renderer;
  const int width = pass.width;
  const int height = pass.height;
  surface_texture_cache_.Upload(renderer);
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::ConsumePendingResults");
    ConsumePendingProjectOpenDialogResult();
    ConsumePendingOpenFileDialogResult();
    ConsumePendingFontFileDialogResult();
    ConsumeProjectSearchUpdates();
    if (file_read_service_.HasCompletions()) {
      file_read_service_.DrainCompletions();
    }
  }
  MakeEditorTabService().ReconcileActiveCaretsWithDocuments();
  MakeEditorTabService().ObserveNavigationLocation();
  text_renderer_.EnsureInitialized(renderer, presentation_scale_x_, presentation_scale_y_);
  editor_text_renderer_.EnsureInitialized(renderer, presentation_scale_x_, presentation_scale_y_);
  if (BottomPanelShowsTerminal()) {
    terminal_text_renderer_.EnsureInitialized(renderer, presentation_scale_x_,
                                              presentation_scale_y_);
    ApplyTerminalFontPreferences();
  }
  window_presentation_.logical_width = width;
  window_presentation_.logical_height = height;
  {
    TerminalPanelService& terminal_panel = MakeTerminalPanelService();
    terminal_panel.SyncPanelMaximizedWithFocus();
    terminal_panel.NoteActiveTabShown();
  }
  SDL_Window* render_window = SDL_GetRenderWindow(renderer);
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::SyncTextInputSurface");
    MakeTextInputCoordinator().SyncTextInputSurface(render_window);
  }
  if (const editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
    MakeSidebarService().CatchUpSearchResults(*viewport);
  }
  const RenderViewModelBuilder view_models(context_);
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::BuildSurfaceViewModels");
    prepare_cached_sidebar_vm_.emplace(view_models.BuildSidebarSurface());
    prepare_cached_bottom_panel_vm_.emplace(view_models.BuildBottomPanelSurface());
  if (DebugEnabled()) {
    prepare_cached_debug_pane_vm_.emplace(view_models.BuildDebugPaneSurface());
  }
  }
  ProjectWorkspaceState& project_state = *prepare_cached_sidebar_vm_->project_state;
  if (prepare_cached_bottom_panel_vm_->content == PanelContentKind::Terminal) {
    prepare_cached_bottom_panel_vm_->terminal_tab = project_state.active_terminal_tab();
  }
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::ApplyLiveSettings");
    ApplyLiveSettings();
  }
  SyncOpenDocumentWatch(project_state, file_index_watcher_.get());
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::RefreshEditorFoldingModels");
    RefreshEditorFoldingModels();
  }
  for (EditorGroup& group : project_state.editor_groups) {
    if (!group.has_active_tab() || group.active_tab().kind != TabEntry::Kind::Compare ||
        !group.active_tab().compare.has_value()) {
      continue;
    }
    CompareTabState& compare_tab = *group.active_tab().compare;
    if (compare_tab.load_pending) {
      if (!compare_tab.pending_load.armed()) {
        (void)MakeDiffTabCoordinator().StartPendingCompareLoad(compare_tab);
      }
      continue;
    }
    if (compare_tab.refresh_owed && !compare_tab.pending_refresh.armed()) {
      (void)MakeDiffTabCoordinator().StartCompareTabRefresh(compare_tab);
    }
    if (compare_tab.right_editable &&
        (!compare_tab.derived_fingerprint_valid ||
         compare_tab.derived_right_content_revision !=
             compare_tab.right_viewport.content_revision())) {
      RefreshCompareTabDerivedState(compare_tab);
    }
  }
  prepare_cached_text_input_vm_.emplace(view_models.BuildTextInputSurface());
}

void WorkspaceShell::ResolveFrameLayout(FramePrepPass& pass) {
  ProjectWorkspaceState& project_state = *pass.project_state;

  const std::uint64_t layout_generation_before = layout_memo_generation_;
  WorkspaceLayout& layout = pass.layout;
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::ComputeLayout");
    layout = *CurrentWorkspaceLayout();
  }
  pass.workspace_layout_recomputed = layout_memo_generation_ != layout_generation_before;
  const bool workspace_layout_recomputed = pass.workspace_layout_recomputed;
  if (workspace_layout_recomputed) {
    ++prepare_frame_layout_compute_count_;
  }
  if (layout.layout_mode != layout_mode_service_.CurrentMode()) {
    layout_mode_service_.SetCurrentMode(layout.layout_mode);
  }
  {
    const OverlayMode overlay_mode = project_state.overlay.mode;
    const bool editor_widget = project_state.overlay.visible &&
                               (overlay_mode == OverlayMode::BufferSearch ||
                                overlay_mode == OverlayMode::BufferReplace ||
                                overlay_mode == OverlayMode::Completion ||
                                overlay_mode == OverlayMode::CodeActions);
    const bool editor_has_keys = project_state.surface.focus == FocusTarget::Editor ||
                                 (project_state.surface.focus == FocusTarget::Overlay && editor_widget);
    const void* focused_editor = editor_has_keys ? ActiveEditorViewport() : nullptr;
    if (frame_state_.focus_autosave_armed && frame_state_.focused_editor != nullptr &&
        focused_editor != frame_state_.focused_editor) {
      MaybeAutosaveDirtyTabs(true);
    }
    frame_state_.focused_editor = focused_editor;
  }
}

void WorkspaceShell::PrepareChromeFrame(FramePrepPass& pass) {
  const int width = pass.width;
  const int height = pass.height;
  const BottomPanelSurfaceViewModel& panel_vm = *pass.panel_vm;
  ProjectWorkspaceState& project_state = *pass.project_state;
  WorkspaceLayout& layout = pass.layout;
  const auto chrome_unit = [&pass](const std::optional<SDL_FRect>& rect, std::uint64_t key,
                                   ChromePainter painter) { pass.ChromeUnit(rect, key, painter); };
  {
    static_assert(std::has_unique_object_representations_v<render::Theme>,
                  "the theme is hashed by its bytes: no padding, no pointers");
    std::uint64_t style = MixRowKeyBytes(0x57u, reinterpret_cast<const char*>(&theme_), sizeof theme_);
    style = MixRowKey(style, text_renderer_.MetricsGeneration());
    style = MixRowKey(style, editor_text_renderer_.MetricsGeneration());
    style = MixRowKey(style, terminal_text_renderer_.MetricsGeneration());
    std::uint32_t scale_bits = 0;
    const float scale = UiScale();
    std::memcpy(&scale_bits, &scale, sizeof scale_bits);
    style = MixRowKey(style, scale_bits);
    chrome_unit(MakeRect(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)), style, ChromePainter::Always);
  }
  for (const SDL_FRect& region : {layout.sidebar, layout.bottom_panel, layout.editor_surface, layout.right_pane,
                                  layout.status_bar, layout.tab_strip}) {
    chrome_unit(region, 0, ChromePainter::Always);
  }
  {
    const bool shown = project_state.sidebar.visible && layout.sidebar.w > 0.0f && layout.sidebar.h > 0.0f;
    frame_state_.frame_base = PreparedFrameState::FrameBasePaint{
        .sidebar_visible = project_state.sidebar.visible,
        .bottom_panel_visible = BottomPanelVisible(),
        .sidebar_divider_lit = context_.interaction_state.drag_target == DragTarget::SidebarDivider,
    };
    chrome_unit(shown ? std::optional<SDL_FRect>(MakeRect(layout.sidebar.x + layout.sidebar.w, layout.sidebar.y,
                                                          kWorkspaceDividerThickness, layout.sidebar.h))
                      : std::nullopt,
                frame_state_.frame_base.sidebar_divider_lit ? 1u : 2u, ChromePainter::Always);
  }
  {
    const PanelState& panel = project_state.panel;
    std::uint64_t shows = static_cast<std::uint64_t>(panel.content) + 1;
    if (panel.content == PanelContentKind::Terminal) {
      const TerminalTabState* tab = project_state.active_terminal_tab_index < project_state.terminal_tabs.size()
                                        ? project_state.terminal_tabs[project_state.active_terminal_tab_index].get()
                                        : nullptr;
      shows = MixRowKey(MixRowKey(shows, reinterpret_cast<std::uintptr_t>(tab)),
                        tab != nullptr ? tab->layout_revision : 0);
    } else if (panel.content == PanelContentKind::PluginSurface) {
      shows = MixRowKeyBytes(MixRowKeyBytes(shows, panel.surface_owner.data(), panel.surface_owner.size()),
                             panel.surface_id.data(), panel.surface_id.size());
      const auto* presentation = project_state.plugin_presentation_if_present();
      shows = MixRowKey(shows, presentation != nullptr ? presentation->surfaces.revision() : 0);
      shows = MixRowKey(shows, static_cast<std::uint64_t>(static_cast<std::int64_t>(panel.surface_scroll_y)));
      shows = MixRowKey(shows, context_.interaction_state.drag_target == DragTarget::BottomPanelScrollbar ? 1u : 0u);
      shows = MixRowKey(shows, surface_texture_cache_.residency_generation());
    }
    chrome_unit(panel.content != PanelContentKind::None && layout.bottom_panel.h > 0.0f
                    ? std::optional<SDL_FRect>(BottomPanelContentRect(layout))
                    : std::nullopt,
                shows, ChromePainter::BottomPanel);
  }
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::RefreshStatusBar");
    RefreshStatusBar();
  }
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::RefreshSettingsOverlayCatalog");
    RefreshSettingsOverlayCatalog();
  }
  if (panel_vm.content == PanelContentKind::Terminal && panel_vm.terminal_tab != nullptr) {
    const SDL_FRect& panel = layout.bottom_panel;
    const std::uint64_t grid_key =
        (static_cast<std::uint64_t>(project_state.active_terminal_tab_index + 1) * 0x9E3779B97F4A7C15ull) ^
        (panel_vm.terminal_tab->layout_revision * 0xC2B2AE3D27D4EB4Full) ^
        static_cast<std::uint64_t>(panel_vm.terminal_tab->pane_count());
    const auto& cached = last_terminal_panel_rect_;
    const bool unchanged = cached.has_value() && cached->x == panel.x && cached->y == panel.y &&
                           cached->w == panel.w && cached->h == panel.h &&
                           last_terminal_grid_key_ == grid_key;
    if (!unchanged) {
      ResizeTerminalToPanel(layout);
      last_terminal_panel_rect_ = panel;
      last_terminal_grid_key_ = grid_key;
    }
  } else {
    last_terminal_panel_rect_.reset();
  }
  prepared_frame_layout_ = layout;
}

void WorkspaceShell::PrepareFrameCompletions(FramePrepPass& pass) {
  const SidebarSurfaceViewModel& sidebar_vm = *pass.sidebar_vm;
  const BottomPanelSurfaceViewModel& panel_vm = *pass.panel_vm;
  ProjectWorkspaceState& project_state = *pass.project_state;
  WorkspaceLayout& layout = pass.layout;
  const auto chrome_unit = [&pass](const std::optional<SDL_FRect>& rect, std::uint64_t key,
                                   ChromePainter painter) { pass.ChromeUnit(rect, key, painter); };
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
    if (TerminalTabState* tab = prepare_cached_bottom_panel_vm_->terminal_tab; tab != nullptr) {
      prepare_cached_bottom_panel_vm_->terminal_panes = tab->PaneRects(BottomPanelContentRect(layout));
      PrepareTerminalPanesForFrame(*tab, prepare_cached_bottom_panel_vm_->terminal_panes,
                                   layout.bottom_panel.h);
    }
  }
  if (panel_vm.content == PanelContentKind::Terminal && terminal_find_service_.visible()) {
    TerminalPaneState* terminal_tab = ActiveTerminalPane();
    terminal_find_service_.Refresh(terminal_tab);
    BottomPanelSurfaceViewModel& panel_out = *prepare_cached_bottom_panel_vm_;
    panel_out.find_query = &terminal_find_service_.query();
    panel_out.find_matches = &terminal_find_service_.matches();
    panel_out.find_selected_index = terminal_find_service_.selected_index();
    OverlayFindWidgetViewModel& find_vm = panel_out.find;
    find_vm.fw = ComputeTerminalFindWidgetLayout(ActiveTerminalBodyRect(layout), layout);
    panel_out.find_visible = find_vm.fw.widget.w > 0.0f && find_vm.fw.widget.h > 0.0f;
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
      terminal_find_service_.SetDisplayText(
          ComputeSingleLineViewMetrics(terminal_find_service_.query(), "",
                                       std::max(1.0f, find_vm.fw.search_field.w - 12.0f))
              .displayed_text);
      find_vm.search_display_text = terminal_find_service_.display_text();
    }
  }
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
  if (project_state.overlay.visible) {
    RefreshBufferSearchIfStale();
    ClampOverlayScrollRow(ComputeOverlayRect(layout.overlay_anchor));
  }
  MaybeExpireSignatureHelp();
  {
    const EditorGroupRectsLayout group_rects = ComputeEditorGroupRectsForState(layout);
    for (std::size_t gi = 0; gi < group_rects.groups.size() && gi < project_state.editor_groups.size();
         ++gi) {
      EditorGroup& group = project_state.editor_groups[gi];
      if (!group.has_active_tab()) {
        continue;
      }
      TabEntry& tab = group.active_tab();
      if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
        PopulateCompareSyntaxTokensForWindow(*tab.compare, group_rects.groups[gi].editor_surface);
      } else if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
        PopulateMergeSyntaxTokensForWindow(*tab.merge, group_rects.groups[gi].editor_surface);
      }
    }
  }
  if (sidebar_vm.visible && sidebar_vm.mode == SidebarMode::Git) {
    PrepareCommitBodyViewportForFrame(layout.sidebar);
  }
  {
    const EditorGroup& group = project_state.focused_group();
    const TabEntry* tab = group.has_active_tab() ? &group.active_tab() : nullptr;
    const bool dirty = tab != nullptr && TabCoordinator::TabStateIsDirty(*tab);
    std::uint64_t inputs = util::Fnv1aByte(util::kFnv1aOffsetBasis, dirty ? 1u : 0u);
    inputs = util::Fnv1aBytes(inputs, tab != nullptr ? std::string_view(tab->title) : "");
    inputs = util::Fnv1aByte(inputs, 0u);
    inputs = util::Fnv1aBytes(inputs, project_state.root.native());
    if (inputs != frame_state_.window_title_inputs) {
      frame_state_.window_title_inputs = inputs;
      ComposeWindowTitle(frame_state_.window_title, tab != nullptr ? tab->title : "", dirty,
                         project_state.id.Label());
      if (dialog_window_ != nullptr) {
        SDL_SetWindowTitle(dialog_window_, frame_state_.window_title.c_str());
      }
    }
    std::optional<SDL_FRect>& title_rect = frame_state_.window_chrome.title_rect;
    title_rect.reset();
    if (CurrentWindowChromeState().custom_enabled) {
      title_rect = WindowTitleRect(layout.menu_bar, ComputeVisibleMenuBarItems(layout.menu_bar),
                                   ComputeVisibleWindowControlButtons(layout.menu_bar),
                                   text_renderer_.MeasureWidth(frame_state_.window_title));
    }
    PaintFingerprint title;
    title.Add(title_rect.has_value());
    if (title_rect.has_value()) {
      title.Add(*title_rect).Add(std::string_view(frame_state_.window_title));
    }
    chrome_unit(layout.menu_bar, title.value(), ChromePainter::WindowChrome);
  }
  frame_state_.caret_visible = CaretVisibleNow();
  prepared_frame_draw_editor_caret_ =
      frame_state_.caret_visible &&
      project_state.surface.focus == FocusTarget::Editor &&
      context_.text_input.active_surface == TextInputSurface::Editor &&
      context_.text_input.composition.text.empty();
  frame_state_.terminal_owns_caret = project_state.surface.focus == FocusTarget::Panel &&
                                     context_.text_input.active_surface == TextInputSurface::Terminal;
  {
    const BottomPanelSurfaceViewModel& panel = *prepare_cached_bottom_panel_vm_;
    std::optional<SDL_FRect> bar;
    PaintFingerprint key;
    if (panel.content == PanelContentKind::Terminal && panel.find_visible) {
      const OverlayFindWidgetViewModel& find = panel.find;
      const FindWidgetLayout& fw = find.fw;
      bar = fw.widget;
      key.Add(fw.search_field)
          .Add(find.search_focused)
          .Add(find.search_display_text)
          .Add(fw.count_rect)
          .Add(find.count_text)
          .Add(find.has_matches)
          .Add(find.has_query);
      for (std::size_t index = 0; index < fw.toggle_count; ++index) {
        key.Add(fw.toggle_buttons[index]).Add(find.toggles[index].active).Add(PointerOver(fw.toggle_buttons[index]));
      }
      for (const SDL_FRect* button : {&fw.prev_button, &fw.next_button, &fw.close_button}) {
        key.Add(*button).Add(PointerOver(*button));
      }
    }
    chrome_unit(bar, key.value(), ChromePainter::BottomPanel);
  }
  chrome_unit(frame_state_.caret_visible ? CurrentCaretDirtyRect() : std::nullopt,
              context_.interaction_state.window_has_input_focus ? 1u : 2u, ChromePainter::Always);
}

void WorkspaceShell::PrepareEditorPaneMetrics(FramePrepPass& pass) {
  ProjectWorkspaceState& project_state = *pass.project_state;
  WorkspaceLayout& layout = pass.layout;
  {
    const bool fold_enabled = SettingFlagEnabled(GetSettingValue("editor.fold.enabled"), true);
    const bool sticky_enabled =
        fold_enabled && SettingFlagEnabled(GetSettingValue("editor.fold.sticky_scroll.enabled"), true);
    const int sticky_depth =
        ParseStickyScrollMaxDepthSetting(GetSettingValue("editor.fold.sticky_scroll.max_depth"));
    const bool line_numbers = SettingFlagEnabled(GetSettingValue("editor.line_numbers"), true);
    const bool blame_inline = SettingFlagEnabled(GetSettingValue("editor.blame.inline.enabled"), true);
    frame_state_.pane_metrics_valid.fill(false);
    const EditorPaneLayouts panes = EditorPaneLayoutsFromGroupRects(ComputeEditorGroupRectsForState(layout));
    for (const EditorPaneLayout& pane : panes) {
      if (pane.group_index >= project_state.editor_groups.size() || pane.group_index >= kMaxEditorGroups) {
        continue;
      }
      EditorGroup& group = project_state.editor_groups[pane.group_index];
      group.NoteActiveTabUse();
      editor::TextViewport* viewport = GroupActiveViewport(group);
      if (viewport == nullptr) {
        continue;
      }
      const editor::FoldingModel* folds = fold_enabled ? GroupFoldingModelPtr(group) : nullptr;
      const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::SizeViewportWithStickyBand(
          editor_text_renderer_, *viewport, pane.rect, line_numbers,
          [&](const editor::TextViewport& view, std::size_t pane_rows) {
            return RenderViewModelBuilder::StickyScrollLines(view, folds, sticky_enabled && folds != nullptr,
                                                             sticky_depth, pane_rows)
                .size();
          });
      const std::size_t sticky_rows = metrics.sticky_scroll_rows;
      frame_state_.pane_metrics[pane.group_index] = metrics;
      frame_state_.pane_metrics_valid[pane.group_index] = true;
      if (pane.active && GroupActiveEditorTab(group) != nullptr) {
        editor_blame_overlay_service_.SetVisibleOverlay(
            blame_inline && !viewport->is_placeholder()
                ? editor_blame_overlay_service_.EditorOverlayForFrame(
                      prepared_frame_id_, project_state.root, editor_text_renderer_, git_blame_service_, *viewport,
                      pane.rect, 520.0f, sticky_rows, line_numbers)
                : std::nullopt,
            *viewport);
      }
    }
  }
  {
    const editor::TextViewport* viewport =
        GroupActiveViewport(project_state.editor_groups[project_state.clamped_focused_group_index()]);
    std::optional<editor::BracketMatchPair> pair;
    if (viewport != nullptr && !viewport->is_placeholder() &&
        SettingFlagEnabled(GetSettingValue("editor.brackets.match_highlight.enabled"), true)) {
      pair = editor_view_renderer_.BracketMatch(*viewport);
    }
    frame_state_.bracket_viewport = viewport;
    frame_state_.bracket_pair = pair;
  }
  {
    const editor::TextViewport* viewport =
        GroupActiveViewport(project_state.editor_groups[project_state.clamped_focused_group_index()]);
    auto& lines = frame_state_.occurrence_lines;
    lines.clear();
    if (viewport != nullptr && !viewport->is_placeholder() &&
        SettingFlagEnabled(GetSettingValue("editor.occurrences.enabled"), true)) {
      const std::span<const editor::OccurrenceRange> ranges =
          RenderViewModelBuilder(context_).OccurrenceRanges(
              *viewport, viewport->visible_lines(),
              SettingFlagEnabled(GetSettingValue("editor.search.case_sensitive"), false));
      for (const editor::OccurrenceRange& range : ranges) {
        std::uint64_t hash = util::Fnv1aByte(util::kFnv1aOffsetBasis, range.is_primary_seed ? 1u : 0u);
        hash = util::Fnv1aByte(hash, static_cast<unsigned char>(range.kind));
        hash = (hash ^ range.start_column) * util::kFnv1aPrime;
        hash = (hash ^ range.end_column) * util::kFnv1aPrime;
        lines.emplace_back(range.line_index, hash);
      }
      std::sort(lines.begin(), lines.end());
      std::size_t kept = 0;
      for (std::size_t i = 0; i < lines.size(); ++i) {
        if (kept > 0 && lines[kept - 1].first == lines[i].first) {
          lines[kept - 1].second = (lines[kept - 1].second ^ lines[i].second) * util::kFnv1aPrime;
        } else {
          lines[kept++] = lines[i];
        }
      }
      lines.resize(kept);
    }
    frame_state_.occurrence_viewport = viewport;
  }
}

struct WorkspaceShell::EditorPanesPass {
  std::size_t focused_group = 0;
  PaintFingerprint settings;
  bool debug_enabled = false;
  bool insets_possible = false;
  bool find_shown = false;
  const BufferSearchState* buffer_search = nullptr;
  bool fold_enabled = false;
  bool sticky_enabled = false;
  int sticky_depth = 0;
  editor::DiagnosticSeverity min_severity{};
  editor::InsetGapFeatureFlags inset_flags{};
  const RenderViewModelBuilder* pane_builder = nullptr;
  std::uint64_t marker_theme_token = 0;
  bool render_whitespace = false;
  bool indent_guides = false;
  bool bracket_match = false;
  bool line_numbers = false;
  DragTarget drag = DragTarget::None;
  SDL_FPoint pointer{};
  std::array<bool, kMaxEditorGroups> committed{};
  std::array<std::array<DerivedDamage::Unit, 2>, kMaxEditorGroups> diff_bars{};

  DiffSurfacePaint& FillDiffPaint(DiffSurfacePaint& paint, const SDL_FRect& area, bool focused, DragTarget vertical,
                                  DragTarget horizontal) const {
    paint.render_whitespace = render_whitespace;
    paint.indent_guides = indent_guides;
    paint.bracket_match = bracket_match;
    paint.line_numbers = line_numbers;
    paint.vertical_bar_dragged = focused && drag == vertical;
    paint.horizontal_bar_dragged = focused && drag == horizontal;
    paint.left_divider_dragged = focused && drag == DragTarget::MergeLeftDivider;
    paint.right_divider_dragged = focused && drag == DragTarget::MergeRightDivider;
    paint.pointer = Contains(area, pointer.x, pointer.y) ? std::optional<SDL_FPoint>(pointer) : std::nullopt;
    paint.blame.reset();
    paint.diagnostics = {};
    paint.decorations = nullptr;
    return paint;
  }

  template <typename BuildMarkers>
  void AppendDiffBars(std::size_t gi, const void* tab, const SDL_FRect& area, const ScrollSurfaceLayout& scroll,
                      const DiffSurfacePaint& paint, const BuildMarkers& build_markers) {
    std::array<DerivedDamage::Unit, 2>& units = diff_bars[gi];
    std::size_t slot = 0;
    const auto push = [&](const DerivedDamage::Unit& unit) { units[slot++] = unit; };
    if (scroll.horizontal_scrollbar.has_value()) {
      PaintFingerprint key;
      key.Add(reinterpret_cast<std::uintptr_t>(tab))
          .Add(scroll.horizontal_scrollbar->track)
          .Add(scroll.horizontal_scrollbar->thumb)
          .Add(paint.horizontal_bar_dragged);
      push({scroll.horizontal_scrollbar->track, key.value()});
    } else {
      push({});
    }
    if (scroll.vertical_scrollbar.has_value()) {
      const SDL_FRect& track = scroll.vertical_scrollbar->track;
      const SDL_FRect lane = overview::LaneRect(track, area.x);
      PaintFingerprint key;
      key.Add(reinterpret_cast<std::uintptr_t>(tab))
          .Add(lane)
          .Add(track)
          .Add(scroll.vertical_scrollbar->thumb)
          .Add(paint.vertical_bar_dragged);
      for (const overview::Marker& marker : build_markers(overview::LaneInnerRect(lane))) {
        key.Add(marker.rect).Add(marker.color);
      }
      const float left = std::min(lane.x, track.x);
      const float right = std::max(lane.x + lane.w, track.x + track.w);
      push({SDL_FRect{left, track.y, right - left, track.h}, key.value()});
    } else {
      push({});
    }
  }
};

void WorkspaceShell::PrepareEditorPaneUnits(FramePrepPass& pass) {
  ProjectWorkspaceState& project_state = *pass.project_state;
  WorkspaceLayout& layout = pass.layout;
  const auto chrome_unit = [&pass](const std::optional<SDL_FRect>& rect, std::uint64_t key,
                                   ChromePainter painter) { pass.ChromeUnit(rect, key, painter); };
  static_assert(static_cast<std::size_t>(DerivedSurface::EditorPane7) -
                        static_cast<std::size_t>(DerivedSurface::EditorPane0) + 1 ==
                    kMaxEditorGroups,
                "one derived surface per editor group");
  const EditorPaneLayouts panes = EditorPaneLayoutsFromGroupRects(ComputeEditorGroupRectsForState(layout));
  const std::size_t focused_group = project_state.clamped_focused_group_index();
  const auto setting = [this](std::string_view id, bool default_value) {
    return SettingFlagEnabled(GetSettingValue(id), default_value);
  };
  PaintFingerprint settings;
  settings.Add(setting("editor.brackets.match_highlight.enabled", true))
      .Add(setting("editor.view.indent_guides.enabled", true))
      .Add(setting("editor.view.render_whitespace", false))
      .Add(setting("editor.line_numbers", true))
      .Add(setting("editor.fold.enabled", true))
      .Add(setting("editor.fold.sticky_scroll.enabled", true))
      .Add(setting("editor.occurrences.enabled", true))
      .Add(std::string_view(GetSettingValue("diagnostics.min_severity").value_or("hint")));
  const bool debug_enabled = DebugEnabled();
  const bool insets_possible = project_state.plugin_presentation_if_present() != nullptr;
  const BufferSearchState& buffer_search = project_state.overlay.workflow.buffer_search;
  const bool find_shown = project_state.overlay.visible && (project_state.overlay.mode == OverlayMode::BufferSearch ||
                                                            project_state.overlay.mode == OverlayMode::BufferReplace);
  for (PreparedFrameState::EditorPanePaint& paint : frame_state_.editor_pane_paint) {
    paint.valid = false;
  }
  const bool fold_enabled = setting("editor.fold.enabled", true);
  const bool sticky_enabled = setting("editor.fold.sticky_scroll.enabled", true);
  const int sticky_depth = ParseStickyScrollMaxDepthSetting(GetSettingValue("editor.fold.sticky_scroll.max_depth"));
  const editor::DiagnosticSeverity min_severity =
      editor::ParseDiagnosticSeverity(GetSettingValue("diagnostics.min_severity").value_or("hint"));
  const editor::InsetGapFeatureFlags inset_flags =
      insets_possible ? editor::InsetGapFeatureFlags{.inline_surfaces = setting("plugins.inline_surfaces", false),
                                                     .code_lens_above = setting("plugins.code_lens_above", false),
                                                     .ghost_text = setting("plugins.ghost_text", false)}
                      : editor::InsetGapFeatureFlags{};
  const RenderViewModelBuilder pane_builder(context_);
  EditorPanesPass pane_pass{
      .focused_group = focused_group,
      .settings = settings,
      .debug_enabled = debug_enabled,
      .insets_possible = insets_possible,
      .find_shown = find_shown,
      .buffer_search = &buffer_search,
      .fold_enabled = fold_enabled,
      .sticky_enabled = sticky_enabled,
      .sticky_depth = sticky_depth,
      .min_severity = min_severity,
      .inset_flags = inset_flags,
      .pane_builder = &pane_builder,
      .marker_theme_token = overview::ThemeMarkerToken(theme_),
      .render_whitespace = setting("editor.view.render_whitespace", false),
      .indent_guides = setting("editor.view.indent_guides.enabled", true),
      .bracket_match = setting("editor.brackets.match_highlight.enabled", true),
      .line_numbers = setting("editor.line_numbers", true),
      .drag = context_.interaction_state.drag_target,
      .pointer = SDL_FPoint{last_mouse_x_, last_mouse_y_},
  };
  for (const EditorPaneLayout& pane : panes) {
    const std::size_t gi = pane.group_index;
    if (gi >= kMaxEditorGroups || gi >= project_state.editor_groups.size() || !frame_state_.pane_metrics_valid[gi]) {
      continue;
    }
    EditorGroup& group = project_state.editor_groups[gi];
    const bool diff_in_front = group.has_active_tab() && (group.active_tab().kind == TabEntry::Kind::Compare ||
                                                          group.active_tab().kind == TabEntry::Kind::Merge);
    editor::TextViewport* viewport = GroupActiveViewport(group);
    const auto surface = static_cast<DerivedSurface>(static_cast<std::size_t>(DerivedSurface::EditorPane0) + gi);
    std::vector<DerivedDamage::Unit>& units = frame_state_.derived_damage.Begin(surface);
    pane_pass.committed[gi] = true;
    const bool editor_in_front = !diff_in_front && viewport != nullptr;
    if (diff_in_front && group.active_tab().compare.has_value()) {
      PrepareCompareFrontPaneUnits(pass, pane_pass, gi, units);
    } else if (diff_in_front && group.active_tab().merge.has_value()) {
      PrepareMergeFrontPaneUnits(pass, pane_pass, gi, units);
    } else if (editor_in_front) {
      PrepareTextPaneUnits(pass, pane_pass, pane, viewport, units);
    }
    frame_state_.derived_damage.Commit(surface, [this](const SDL_FRect& rect) { RequestRedrawRect(rect); });
  }
  for (std::size_t gi = 0; gi < kMaxEditorGroups; ++gi) {
    if (!pane_pass.committed[gi]) {
      const auto surface = static_cast<DerivedSurface>(static_cast<std::size_t>(DerivedSurface::EditorPane0) + gi);
      frame_state_.derived_damage.Begin(surface);
      frame_state_.derived_damage.Commit(surface, [this](const SDL_FRect& rect) { RequestRedrawRect(rect); });
    }
  }
  {
    std::vector<PreparedFrameState::EditorPassPane>& dispatch = frame_state_.editor_pass_panes;
    dispatch.clear();
    const EditorGroupRectsLayout group_rects = ComputeEditorGroupRectsForState(layout);
    const bool focused_shows_diff = ActiveTabIsCompare() || ActiveTabIsMerge();
    std::array<bool, kMaxEditorGroups> shows_diff{};
    for (std::size_t gi = 0; gi < group_rects.groups.size() && gi < kMaxEditorGroups &&
                             gi < project_state.editor_groups.size();
         ++gi) {
      EditorGroup& group = project_state.editor_groups[gi];
      if (!group.has_active_tab()) {
        continue;
      }
      TabEntry& tab = group.active_tab();
      const bool compare = tab.kind == TabEntry::Kind::Compare && tab.compare.has_value();
      const bool merge = tab.kind == TabEntry::Kind::Merge && tab.merge.has_value();
      const bool focused = gi == focused_group;
      if ((!compare && !merge) || (focused && !focused_shows_diff)) {
        continue;
      }
      shows_diff[gi] = true;
      dispatch.push_back(PreparedFrameState::EditorPassPane{
          .kind = compare ? PreparedFrameState::EditorPassPane::Kind::Compare
                          : PreparedFrameState::EditorPassPane::Kind::Merge,
          .group = gi,
          .area = group_rects.groups[gi].editor_surface,
          .focused = focused,
          .compare = compare ? &*tab.compare : nullptr,
          .merge = merge ? &*tab.merge : nullptr,
      });
    }
    for (const EditorPaneLayout& pane : panes) {
      const std::size_t gi = pane.group_index;
      if (gi >= kMaxEditorGroups || shows_diff[gi] || !frame_state_.editor_pane_paint[gi].valid) {
        continue;
      }
      dispatch.push_back(PreparedFrameState::EditorPassPane{.kind = PreparedFrameState::EditorPassPane::Kind::Editor,
                                                            .group = gi,
                                                            .area = pane.rect,
                                                            .active = pane.active});
    }
  }
  for (const std::array<DerivedDamage::Unit, 2>& bars : pane_pass.diff_bars) {
    for (const DerivedDamage::Unit& bar : bars) {
      chrome_unit(bar.rect.w > 0.0f && bar.rect.h > 0.0f ? std::optional<SDL_FRect>(bar.rect) : std::nullopt,
                  bar.fingerprint, ChromePainter::Editor);
    }
  }
}

void WorkspaceShell::PrepareCompareFrontPaneUnits(FramePrepPass& pass, EditorPanesPass& panes, std::size_t gi,
                                                  std::vector<DerivedDamage::Unit>& units) {
  ProjectWorkspaceState& project_state = *pass.project_state;
  const WorkspaceLayout& layout = pass.layout;
  EditorGroup& group = project_state.editor_groups[gi];
  const std::size_t focused_group = panes.focused_group;
  const PaintFingerprint& settings = panes.settings;
  const auto setting = [this](std::string_view id, bool default_value) {
    return SettingFlagEnabled(GetSettingValue(id), default_value);
  };
  const auto fill_diff_paint = [&](std::size_t group_index, const SDL_FRect& area, bool focused, DragTarget vertical,
                                   DragTarget horizontal) -> DiffSurfacePaint& {
    return panes.FillDiffPaint(frame_state_.diff_surface_paint[group_index], area, focused, vertical, horizontal);
  };
  const auto append_diff_bars = [&panes](std::size_t group_index, const void* tab, const SDL_FRect& area,
                                         const ScrollSurfaceLayout& scroll, const DiffSurfacePaint& paint,
                                         const auto& build_markers) {
    panes.AppendDiffBars(group_index, tab, area, scroll, paint, build_markers);
  };
  const std::uint64_t marker_theme_token = panes.marker_theme_token;
  CompareTabState& compare = *group.active_tab().compare;
  const SDL_FRect area = ComputeEditorGroupRectsForState(layout).groups[gi].editor_surface;
  const CompareSurfaceLayout compare_surface = ComputeCompareSurfaceLayout(area, compare);
  ClampCompareScrollRow(compare, compare_surface.visible_rows);
  ClampCompareHorizontalScroll(compare, compare_surface.visible_columns);
  const bool focused = gi == focused_group;
  DiffSurfacePaint& diff_paint =
      fill_diff_paint(gi, area, focused, DragTarget::CompareVerticalScrollbar, DragTarget::CompareHorizontalScrollbar);
  if (focused) {
    if (compare.right_editable && compare.right_view_active) {
      diff_paint.blame = editor_blame_overlay_service_.BuildCompareOverlay(
          project_state.root, editor_text_renderer_, git_blame_service_, compare,
          EditorBlameOverlayService::CompareOverlayLayout{
              .pane_rect =
                  MakeRect(compare_surface.right_x, area.y, compare_surface.gutter_width + compare_surface.right_width,
                           std::max(0.0f, area.h - (compare_surface.show_horizontal ? 12.0f : 0.0f))),
              .right_x = compare_surface.right_x,
              .gutter_width = compare_surface.gutter_width,
              .right_width = compare_surface.right_width,
              .rows_y = compare_surface.rows_y,
              .line_height = compare_surface.line_height,
              .visible_rows = compare_surface.visible_rows,
              .visible_columns = compare_surface.visible_columns,
          },
          [&compare](std::size_t line_index,
                     std::size_t line_visual_columns) -> std::optional<EditorBlameOverlayService::CompareBlameAnchor> {
            const CompareRightLineAnchor anchor = CompareRightLineBlameAnchor(compare, line_index, line_visual_columns);
            if (!anchor.valid) {
              return std::nullopt;
            }
            return EditorBlameOverlayService::CompareBlameAnchor{.visual_row = anchor.visual_row,
                                                                 .end_column = anchor.end_column};
          });
    }
    editor_blame_overlay_service_.SetVisibleOverlay(diff_paint.blame);
  }
  PaintFingerprint surface_key;
  surface_key.Add(reinterpret_cast<std::uintptr_t>(&compare))
      .Add(settings.value())
      .Add(area)
      .Add(focused)
      .Add(compare.scroll_row)
      .Add(compare.horizontal_scroll)
      .Add(compare.right_view_active)
      .Add(compare.wrap_layout.active())
      .Add(compare.show_whitespace)
      .Add(compare.syntax_highlighting_enabled)
      .Add(compare_surface.line_height)
      .Add(compare_surface.gutter_width)
      .Add(compare_surface.divider_width)
      .Add(compare_surface.left_x)
      .Add(compare_surface.center_x)
      .Add(compare_surface.right_x)
      .Add(compare_surface.rows_y)
      .Add(compare_surface.visible_rows)
      .Add(compare_surface.show_vertical)
      .Add(compare_surface.show_horizontal);
  const float right_reserved = compare_surface.show_vertical ? kWorkspaceDiffScrollbarReserve : 0.0f;
  {
    PaintFingerprint header;
    header.Add(surface_key.value())
        .Add(std::string_view(compare.review_header.summary_line))
        .Add(std::string_view(compare.left_label))
        .Add(std::string_view(compare.right_label))
        .Add(compare.model.hunks.empty())
        .Add(CompareTabTakesReviewMarks(compare))
        .Add(CompareFileFullyReviewed(compare))
        .Add(compare.hover_state.has_value() && IsCompareHeaderControl(compare.hover_state->kind)
                 ? static_cast<int>(compare.hover_state->kind)
                 : -1);
    units.push_back(
        {SDL_FRect{area.x, area.y, area.w, std::max(0.0f, compare_surface.rows_y - 1.0f - area.y)}, header.value()});
  }
  if (compare.right_editable && !compare.right_viewport.path().empty() && !compare.right_viewport.dirty()) {
    if (const auto* published = project_state.diagnostics_store.FindByPathKey(compare.right_viewport.path_key())) {
      diff_paint.diagnostics = *published;
    }
    if (const auto* presentation = project_state.plugin_presentation_if_present()) {
      diff_paint.decorations = presentation->decorations.FindByPathKey(compare.right_viewport.path_key());
    }
  }
  append_diff_bars(gi, &compare, area, ComputeCompareScrollLayout(area, compare_surface, compare), diff_paint,
                   [&](const SDL_FRect& inner_lane) -> const std::vector<overview::Marker>& {
                     EnsureCompareOverviewMarkers(theme_, inner_lane, marker_theme_token, compare);
                     return compare.scrollbar_marker_cache;
                   });
  AppendCompareRowUnits(
      compare,
      CompareRowGeometry{.surface = area,
                         .rows_y = compare_surface.rows_y,
                         .line_height = compare_surface.line_height,
                         .content_width = std::max(0.0f, area.w - right_reserved),
                         .visible_rows = compare_surface.visible_rows},
      CompareRowKeyInputs{
          .surface_key = surface_key.value(),
          .bracket_pair = CompareBracketMatch(compare, setting("editor.brackets.match_highlight.enabled", true)),
          .blame = diff_paint.blame.has_value() ? &*diff_paint.blame : nullptr,
          .caret_drawn = focused && compare.right_view_active && prepared_frame_draw_editor_caret_,
          .diagnostics = diff_paint.diagnostics,
          .decorations = diff_paint.decorations,
      },
      units);
}

void WorkspaceShell::PrepareMergeFrontPaneUnits(FramePrepPass& pass, EditorPanesPass& panes, std::size_t gi,
                                                std::vector<DerivedDamage::Unit>& units) {
  ProjectWorkspaceState& project_state = *pass.project_state;
  const WorkspaceLayout& layout = pass.layout;
  EditorGroup& group = project_state.editor_groups[gi];
  const std::size_t focused_group = panes.focused_group;
  const PaintFingerprint& settings = panes.settings;
  const auto fill_diff_paint = [&](std::size_t group_index, const SDL_FRect& area, bool focused, DragTarget vertical,
                                   DragTarget horizontal) -> DiffSurfacePaint& {
    return panes.FillDiffPaint(frame_state_.diff_surface_paint[group_index], area, focused, vertical, horizontal);
  };
  const auto append_diff_bars = [&panes](std::size_t group_index, const void* tab, const SDL_FRect& area,
                                         const ScrollSurfaceLayout& scroll, const DiffSurfacePaint& paint,
                                         const auto& build_markers) {
    panes.AppendDiffBars(group_index, tab, area, scroll, paint, build_markers);
  };
  const std::uint64_t marker_theme_token = panes.marker_theme_token;
  MergeTabState& merge = *group.active_tab().merge;
  const SDL_FRect area = ComputeEditorGroupRectsForState(layout).groups[gi].editor_surface;
  const MergeSurfaceLayout merge_surface = ComputeMergeSurfaceLayout(area, merge);
  merge.result_viewport.SetScrollLine(static_cast<std::size_t>(std::max(0, merge.scroll_row)));
  merge.result_viewport.SetHorizontalScroll(merge.horizontal_scroll);
  merge.scroll_row = static_cast<int>(merge.result_viewport.scroll_line());
  merge.horizontal_scroll = merge.result_viewport.horizontal_scroll();
  ClampMergeScrollRow(merge, merge_surface.visible_rows);
  ClampMergeHorizontalScroll(merge, merge_surface.visible_columns);
  merge.result_viewport.SetScrollLine(static_cast<std::size_t>(std::max(0, merge.scroll_row)));
  merge.result_viewport.SetHorizontalScroll(merge.horizontal_scroll);
  const MergeInteractionLayout interaction = BuildMergeInteractionLayout(area, merge_surface, merge);
  merge.scroll_row = static_cast<int>(merge.result_viewport.scroll_line());
  merge.horizontal_scroll = merge.result_viewport.horizontal_scroll();
  const bool focused = gi == focused_group;
  DiffSurfacePaint& diff_paint =
      fill_diff_paint(gi, area, focused, DragTarget::CompareVerticalScrollbar, DragTarget::CompareHorizontalScrollbar);
  if (focused) {
    diff_paint.blame = editor_blame_overlay_service_.EditorOverlayForFrame(
        prepared_frame_id_, project_state.root, editor_text_renderer_, git_blame_service_, merge.result_viewport,
        interaction.result.rect, 280.0f, 0, LineNumbersEnabled());
    editor_blame_overlay_service_.SetVisibleOverlay(diff_paint.blame);
  }
  if (!merge.result_viewport.path().empty() && !merge.result_viewport.dirty()) {
    if (const auto* published = project_state.diagnostics_store.FindByPathKey(merge.result_viewport.path_key())) {
      diff_paint.diagnostics = *published;
    }
  }
  PaintFingerprint surface_key;
  surface_key.Add(reinterpret_cast<std::uintptr_t>(&merge))
      .Add(settings.value())
      .Add(area)
      .Add(focused)
      .Add(merge.scroll_row)
      .Add(merge.horizontal_scroll)
      .Add(merge.base_pane_visible)
      .Add(merge.wrap_layout.active())
      .Add(merge.conflicts.size())
      .Add(merge_surface.line_height)
      .Add(merge_surface.left_x)
      .Add(merge_surface.center_x)
      .Add(merge_surface.right_x)
      .Add(merge_surface.rows_y)
      .Add(merge_surface.visible_rows)
      .Add(merge_surface.show_vertical)
      .Add(merge_surface.show_horizontal);
  for (const MergeTrackedConflict& conflict : merge.conflicts) {
    surface_key.Add(conflict.incoming_start_line)
        .Add(conflict.incoming_end_line)
        .Add(conflict.current_start_line)
        .Add(conflict.current_end_line)
        .Add(conflict.start_line)
        .Add(conflict.end_line);
  }
  surface_key.Add(diff_paint.left_divider_dragged).Add(diff_paint.right_divider_dragged);
  if (merge.hover_state.has_value()) {
    surface_key.Add(merge.hover_state->kind)
        .Add(merge.hover_state->conflict_index)
        .Add(merge.hover_state->preview_choice);
  }

  append_diff_bars(gi, &merge, area, ComputeMergeScrollLayout(area, merge_surface, merge), diff_paint,
                   [&](const SDL_FRect& inner_lane) -> const std::vector<overview::Marker>& {
                     EnsureMergeOverviewMarkers(theme_, inner_lane, MergeTotalVisualRowCount(merge), marker_theme_token,
                                                merge);
                     return merge.scrollbar_marker_cache;
                   });
  const float right_reserved = merge_surface.show_vertical ? kWorkspaceDiffScrollbarReserve : 0.0f;
  {
    PaintFingerprint header;
    header.Add(surface_key.value())
        .Add(std::string_view(merge.incoming_label))
        .Add(std::string_view(merge.result_label))
        .Add(std::string_view(merge.current_label))
        .Add(std::string_view(merge.base_label))
        .Add(std::string_view(merge.status_message))
        .Add(merge.marked_resolved)
        .Add(merge.remaining_conflicted_files)
        .Add(merge.result_viewport.dirty())
        .Add(static_cast<int>(ComputeMergeResultState(merge, merge.file_conflict)))
        .Add(merge.selected_hunk)
        .Add(CountRemainingMergeConflicts(merge.conflicts));
    if (diff_paint.pointer.has_value() && diff_paint.pointer->y < merge_surface.rows_y) {
      header.Add(diff_paint.pointer->x).Add(diff_paint.pointer->y);
    }
    if (merge.base_pane_visible) {
      for (std::size_t row = 0; row < merge.model.base_lines.size() && row < 4; ++row) {
        header.Add(std::string_view(merge.model.base_lines[row]));
      }
    }
    units.push_back(
        {SDL_FRect{area.x, area.y, area.w, std::max(0.0f, merge_surface.rows_y - 1.0f - area.y)}, header.value()});
  }
  AppendMergeSideRowUnits(merge,
                          CompareRowGeometry{.surface = area,
                                             .rows_y = merge_surface.rows_y,
                                             .line_height = merge_surface.line_height,
                                             .content_width = std::max(0.0f, area.w - right_reserved),
                                             .visible_rows = merge_surface.visible_rows},
                          surface_key.value(), units);
  const std::span<const editor::PublishedDiagnostic> result_diagnostics = diff_paint.diagnostics;
  thread_local std::vector<std::pair<std::size_t, std::uint64_t>> conflict_lines;
  conflict_lines.clear();
  const std::size_t selected = merge.conflicts.empty() ? 0 : std::min(merge.selected_hunk, merge.conflicts.size() - 1);
  for (std::size_t i = 0; i < merge.conflicts.size(); ++i) {
    const MergeTrackedConflict& conflict = merge.conflicts[i];
    PaintFingerprint state;
    state.Add(i).Add(conflict.last_choice).Add(conflict.valid).Add(conflict.resolved).Add(i == selected);
    for (std::size_t line = conflict.start_line; line < std::max(conflict.end_line, conflict.start_line + 1); ++line) {
      conflict_lines.emplace_back(line, state.value());
    }
  }
  std::sort(conflict_lines.begin(), conflict_lines.end());
  AppendEditorRowUnits(EditorRowKeyInputs{.viewport = &merge.result_viewport,
                                          .pane_rect = interaction.result.rect,
                                          .metrics = interaction.result.metrics,
                                          .pane_key = surface_key.value(),
                                          .caret_drawn = focused && prepared_frame_draw_editor_caret_,
                                          .diagnostics = result_diagnostics,
                                          .bracket_pair = diff_paint.bracket_match ? editor_view_renderer_.BracketMatch(
                                                                                         merge.result_viewport)
                                                                                   : std::nullopt,
                                          .blame = diff_paint.blame.has_value() ? &*diff_paint.blame : nullptr,
                                          .extra_line_keys = conflict_lines},
                       frame_state_.editor_row_keys, units);
}

void WorkspaceShell::FillEditorPanePaint(FramePrepPass& pass, EditorPanesPass& panes, std::size_t gi,
                                         const EditorPaneLayout& pane, EditorGroup& group,
                                         editor::TextViewport& viewport) {
  ProjectWorkspaceState& project_state = *pass.project_state;
  const auto setting = [this](std::string_view id, bool default_value) {
    return SettingFlagEnabled(GetSettingValue(id), default_value);
  };
  const bool insets_possible = panes.insets_possible;
  const BufferSearchState& buffer_search = *panes.buffer_search;
  const bool find_shown = panes.find_shown;
  const bool fold_enabled = panes.fold_enabled;
  const bool sticky_enabled = panes.sticky_enabled;
  const int sticky_depth = panes.sticky_depth;
  const editor::DiagnosticSeverity min_severity = panes.min_severity;
  const editor::InsetGapFeatureFlags inset_flags = panes.inset_flags;
  const RenderViewModelBuilder& pane_builder = *panes.pane_builder;
  PreparedFrameState::EditorPanePaint& paint = frame_state_.editor_pane_paint[gi];
  paint.valid = true;
  paint.viewport = &viewport;
  paint.folding = fold_enabled ? GroupFoldingModelPtr(group) : nullptr;
  paint.rect = pane.rect;
  paint.active = pane.active;
  paint.metrics = frame_state_.pane_metrics[gi];
  paint.bracket_match = pane.active && setting("editor.brackets.match_highlight.enabled", true);
  paint.indent_guides = setting("editor.view.indent_guides.enabled", true);
  paint.render_whitespace = setting("editor.view.render_whitespace", false);
  paint.line_numbers = setting("editor.line_numbers", true);
  paint.overview_ruler = setting("editor.view.overview_ruler.enabled", true);
  paint.diagnostics_min_severity = min_severity;
  paint.vertical_bar_dragged =
      pane.active && context_.interaction_state.drag_target == DragTarget::EditorVerticalScrollbar;
  paint.horizontal_bar_dragged =
      pane.active && context_.interaction_state.drag_target == DragTarget::EditorHorizontalScrollbar;
  paint.drop_indicator.reset();
  if (pane.active && context_.interaction_state.text_dragging() && context_.interaction_state.text_drag_has_drop) {
    paint.drop_indicator = editor::TextPosition{.line = context_.interaction_state.text_drag_drop_line,
                                                .column = context_.interaction_state.text_drag_drop_column};
  }
  const bool debug_on = DebugEnabled();
  if (viewport.is_placeholder()) {
    pane_builder.BuildEditorViewModelInto(
        paint.view_model, viewport, paint.metrics.visible_rows, paint.folding,
        setting("editor.occurrences.enabled", true), setting("editor.search.case_sensitive", false),
        fold_enabled && sticky_enabled && paint.folding != nullptr, sticky_depth, paint.render_whitespace, debug_on,
        debug_on ? &project_state.breakpoint_store : nullptr, debug_on ? &project_state.debug_execution : nullptr);
    paint.welcome = &pane_builder.BuildWelcomeView(recents_service_);
    paint.diagnostics = {};
    paint.decorations = nullptr;
    paint.blame = nullptr;
    paint.search_query.clear();
    paint.explicit_search_matches = {};
    paint.active_search_match.reset();
    paint.find_scope.reset();
    paint.blame_source = nullptr;
    paint.ruler_search = nullptr;
    return;
  }
  paint.welcome = nullptr;
  pane_builder.BuildEditorViewModelInto(
      paint.view_model, viewport, paint.metrics.visible_rows, paint.folding,
      setting("editor.occurrences.enabled", true) && pane.active, setting("editor.search.case_sensitive", false),
      fold_enabled && sticky_enabled && paint.folding != nullptr, sticky_depth, paint.render_whitespace, debug_on,
      debug_on ? &project_state.breakpoint_store : nullptr, debug_on ? &project_state.debug_execution : nullptr,
      inset_flags, paint.metrics.line_height);
  paint.sticky_lines_storage.assign(paint.view_model.sticky_lines.begin(), paint.view_model.sticky_lines.end());
  paint.view_model.sticky_lines = paint.sticky_lines_storage;
  paint.row_gaps_storage.assign(paint.view_model.row_gaps.begin(), paint.view_model.row_gaps.end());
  paint.view_model.row_gaps = paint.row_gaps_storage;
  paint.row_gap_contents_storage.assign(paint.view_model.row_gap_contents.begin(),
                                        paint.view_model.row_gap_contents.end());
  paint.view_model.row_gap_contents = paint.row_gap_contents_storage;
  paint.diagnostics = {};
  if (!viewport.path().empty()) {
    if (const auto* published = project_state.diagnostics_store.FindByPathKey(viewport.path_key())) {
      paint.diagnostics = editor::FilterDiagnosticsAtLeastSeverity(
          std::span<const editor::PublishedDiagnostic>(*published), min_severity, paint.filtered_diagnostics);
    }
  }
  paint.decorations =
      !viewport.dirty() && !viewport.path().empty() && insets_possible
          ? project_state.plugin_presentation_if_present()->decorations.FindByPathKey(viewport.path_key())
          : nullptr;
  const std::optional<editor::EditorBlameOverlay>& blame = editor_blame_overlay_service_.VisibleOverlay();
  paint.blame = pane.active && blame.has_value() ? &*blame : nullptr;
  paint.blame_source = pane.active ? &blame : nullptr;
  paint.diagnostics_revision =
      viewport.path().empty() ? 0 : project_state.diagnostics_store.PathRevisionByKey(viewport.path_key());
  const bool find_here = pane.active && find_shown;
  const bool explicit_matches = find_here && (buffer_search.regex || buffer_search.scope.has_value());
  paint.explicit_search_matches =
      explicit_matches ? BufferSearchHighlightFragments(viewport, buffer_search.matches, buffer_search.matches_revision)
                       : std::span<const editor::SelectionRange>{};
  paint.search_query.assign(find_here && !explicit_matches ? buffer_search.query.text() : std::string_view{});
  paint.active_search_match = pane.active ? ActiveBufferSearchMatch() : std::nullopt;
  paint.find_scope = find_here && buffer_search.scope.has_value()
                         ? std::optional<editor::SelectionRange>(buffer_search.scope->range)
                         : std::nullopt;
  paint.ruler_search = find_here ? &buffer_search : nullptr;
}

void WorkspaceShell::PrepareTextPaneUnits(FramePrepPass& pass, EditorPanesPass& panes, const EditorPaneLayout& pane,
                                          editor::TextViewport* viewport, std::vector<DerivedDamage::Unit>& units) {
  ProjectWorkspaceState& project_state = *pass.project_state;
  const std::size_t gi = pane.group_index;
  EditorGroup& group = project_state.editor_groups[gi];
  const std::size_t focused_group = panes.focused_group;
  const PaintFingerprint& settings = panes.settings;
  const auto setting = [this](std::string_view id, bool default_value) {
    return SettingFlagEnabled(GetSettingValue(id), default_value);
  };
  const bool debug_enabled = panes.debug_enabled;
  const bool insets_possible = panes.insets_possible;
  const BufferSearchState& buffer_search = *panes.buffer_search;
  const bool find_shown = panes.find_shown;
  const bool focused = gi == focused_group;
  FillEditorPanePaint(pass, panes, gi, pane, group, *viewport);
  if (viewport->is_placeholder()) {
    const editor::WelcomeViewModel* home = frame_state_.editor_pane_paint[gi].welcome;
    PaintFingerprint welcome;
    welcome.Add(pane.rect).Add(home != nullptr ? home->generation : 0).Add(settings.value());
    units.push_back({pane.rect, welcome.value()});
    return;
  }
  const PreparedFrameState::EditorPanePaint& paint = frame_state_.editor_pane_paint[gi];
  const editor::EditorViewMetrics& metrics = frame_state_.pane_metrics[gi];
  const editor::FoldingModel* folds = setting("editor.fold.enabled", true) ? GroupFoldingModelPtr(group) : nullptr;
  const editor::FileDecorations* decorations =
      !viewport->dirty() && !viewport->path().empty() && insets_possible
          ? project_state.plugin_presentation_if_present()->decorations.FindByPathKey(viewport->path_key())
          : nullptr;
  PaintFingerprint pane_key;
  pane_key.Add(reinterpret_cast<std::uintptr_t>(viewport))
      .Add(reinterpret_cast<std::uintptr_t>(viewport->document_identity()))
      .Add(settings.value())
      .Add(pane.rect)
      .Add(focused)
      .Add(viewport->scroll_line())
      .Add(viewport->horizontal_scroll())
      .Add(viewport->soft_wrap())
      .Add(viewport->tab_size())
      .Add(viewport->indent_width())
      .Add(viewport->layout_shape_revision())
      .Add(viewport->syntax_revision())
      .Add(std::string_view(viewport->language_id()))
      .Add(metrics.gutter_width)
      .Add(metrics.text_x)
      .Add(metrics.first_line_y)
      .Add(metrics.line_height)
      .Add(metrics.sticky_scroll_rows)
      .Add(metrics.visible_rows)
      .Add(metrics.visible_columns)
      .Add(reinterpret_cast<std::uintptr_t>(decorations));
  if (debug_enabled) {
    pane_key.Add(project_state.breakpoint_store.revision())
        .Add(project_state.function_breakpoint_store.revision())
        .Add(project_state.debug_execution.stopped)
        .Add(project_state.debug_execution.focused_frame_index);
    const auto& frames = project_state.debug_execution.frames;
    if (project_state.debug_execution.focused_frame_index < frames.size()) {
      const auto& frame = frames[project_state.debug_execution.focused_frame_index];
      pane_key.Add(frame.line).Add(std::string_view(frame.source_path_normalized));
    }
  }
  {
    const editor::EditorViewModel& vm = paint.view_model;
    pane_key.Add(vm.sticky_lines.size());
    for (const std::size_t line : vm.sticky_lines) {
      const std::size_t shown = std::min(line, viewport->line_count() - 1);
      const std::size_t length = viewport->lines().LineLength(shown);
      pane_key.Add(line).Add(length);
      if (length <= kEditorRowKeyMaxHashedBytes) {
        pane_key.Add(viewport->lines().LineView(shown));
      } else {
        pane_key.Add(viewport->content_revision());
      }
    }
    pane_key.Add(vm.row_gaps.size()).Add(vm.row_gap_contents.size()).Add(vm.code_lens_above);
    for (const editor::RowGap& gap : vm.row_gaps) {
      pane_key.Add(gap.visual_row).Add(gap.height).Add(static_cast<int>(gap.placement));
    }
    for (const editor::RowGapContent& content : vm.row_gap_contents) {
      if (content.code_lens != nullptr) {
        pane_key.Add(std::string_view(content.code_lens->text));
      }
      if (content.surface != nullptr) {
        const editor::SurfaceContent& surface = *content.surface;
        pane_key.Add(surface.body.index()).Add(std::string_view(surface.title));
        pane_key.Add(surface.intrinsic_width).Add(surface.intrinsic_height);
        if (const auto* list = std::get_if<render::PluginDisplayList>(&surface.body)) {
          pane_key.Add(list->content_hash);
        } else if (const auto* raster = std::get_if<editor::RasterHandle>(&surface.body)) {
          pane_key.Add(raster->content_hash);
        }
        pane_key.Add(surface_texture_cache_.residency_generation());
      }
      if (content.ghost_text != nullptr) {
        for (const std::string& line : content.ghost_text->below_lines) {
          pane_key.Add(std::string_view(line));
        }
      }
    }
    if (vm.ghost_text_tail.has_value()) {
      pane_key.Add(vm.ghost_text_tail->visual_row).Add(vm.ghost_text_tail->text);
    }
    pane_key.Add(vm.execution_line_index.has_value() ? *vm.execution_line_index + 1 : 0);
    pane_key.Add(vm.breakpoint_gutter_marks.size());
    for (const editor::BreakpointGutterMark& mark : vm.breakpoint_gutter_marks) {
      pane_key.Add(mark.line_index).Add(mark.enabled).Add(mark.verified).Add(mark.has_condition).Add(mark.is_logpoint);
    }
    if (paint.drop_indicator.has_value()) {
      pane_key.Add(paint.drop_indicator->line).Add(paint.drop_indicator->column);
    }
  }
  if (focused && find_shown) {
    pane_key.Add(std::string_view(buffer_search.query.text()))
        .Add(buffer_search.matches_revision)
        .Add(buffer_search.regex)
        .Add(buffer_search.scope.has_value());
  }
  const std::span<const editor::PublishedDiagnostic> diagnostics = paint.diagnostics;
  const EditorRowKeyInputs inputs{
      .viewport = viewport,
      .pane_rect = pane.rect,
      .metrics = metrics,
      .pane_key = pane_key.value(),
      .decorations = decorations,
      .rows_have_gaps = !paint.view_model.row_gaps.empty(),
      .sticky_lines = paint.view_model.sticky_lines,
      .caret_drawn = paint.active && prepared_frame_draw_editor_caret_,
      .diagnostics = diagnostics,
      .occurrence_lines = focused && frame_state_.occurrence_viewport == viewport
                              ? std::span<const std::pair<std::size_t, std::uint64_t>>(frame_state_.occurrence_lines)
                              : std::span<const std::pair<std::size_t, std::uint64_t>>{},
      .bracket_pair = focused && frame_state_.bracket_viewport == viewport ? frame_state_.bracket_pair : std::nullopt,
      .folds = folds,
      .fold_marks = paint.view_model.fold_gutter_marks,
      .active_search_match = focused ? ActiveBufferSearchMatch() : std::nullopt,
      .blame = paint.blame,
  };
  AppendEditorRowUnits(inputs, frame_state_.editor_row_keys, units);
  {
    const ScrollSurfaceLayout scroll = ComputeEditorScrollLayout(pane.rect, *viewport, metrics);
    if (scroll.vertical_scrollbar.has_value()) {
      const SDL_FRect& track = scroll.vertical_scrollbar->track;
      const SDL_FRect lane = overview::LaneRect(track, pane.rect.x);
      const float left = std::min(track.x, lane.x);
      const float right = std::max(track.x + track.w, lane.x + lane.w);
      const bool ruler = setting("editor.view.overview_ruler.enabled", true);
      PaintFingerprint bar;
      bar.Add(pane_key.value())
          .Add(track)
          .Add(scroll.vertical_scrollbar->thumb)
          .Add(pane.active && context_.interaction_state.drag_target == DragTarget::EditorVerticalScrollbar)
          .Add(ruler)
          .Add(frame_state_.editor_pane_paint[gi].diagnostics_revision)
          .Add(frame_state_.editor_pane_paint[gi].diagnostics_min_severity)
          .Add(viewport->visual_line_count())
          .Add(viewport->folding_revision())
          .Add(overview::ThemeMarkerToken(theme_));
      if (focused && find_shown) {
        bar.Add(buffer_search.matches_revision).Add(buffer_search.selected_index).Add(buffer_search.matches.size());
      }
      units.push_back({SDL_FRect{left, track.y, right - left, track.h}, bar.value()});
      overview::Marker caret_mark;
      const int caret_row = static_cast<int>(viewport->cursor_visual_row());
      if (ruler && overview::BuildMarker(
                       overview::LaneInnerRect(lane), viewport->visual_line_count(),
                       overview::MarkerInput{
                           .start_row = caret_row, .end_row = caret_row + 1, .color = theme_.cursor, .priority = 0},
                       caret_mark)) {
        units.push_back({caret_mark.rect, bar.value()});
      } else {
        units.push_back({SDL_FRect{}, bar.value()});
      }
    }
    if (scroll.horizontal_scrollbar.has_value()) {
      PaintFingerprint bar;
      bar.Add(pane_key.value())
          .Add(scroll.horizontal_scrollbar->track)
          .Add(scroll.horizontal_scrollbar->thumb)
          .Add(pane.active && context_.interaction_state.drag_target == DragTarget::EditorHorizontalScrollbar);
      units.push_back({scroll.horizontal_scrollbar->track, bar.value()});
    }
  }
}

void WorkspaceShell::PrepareWorkbenchChromeUnits(FramePrepPass& pass) {
  ProjectWorkspaceState& project_state = *pass.project_state;
  WorkspaceLayout& layout = pass.layout;
  const auto chrome_unit = [&pass](const std::optional<SDL_FRect>& rect, std::uint64_t key,
                                   ChromePainter painter) { pass.ChromeUnit(rect, key, painter); };
  {
    const OverlayState& overlay = project_state.overlay;
    const bool overlay_backdrop = overlay.visible && overlay.mode != OverlayMode::BufferSearch &&
                                  overlay.mode != OverlayMode::BufferReplace &&
                                  overlay.mode != OverlayMode::Completion;
    const std::uint8_t backdrops = static_cast<std::uint8_t>(
        (overlay_backdrop ? 1u : 0u) | (settings_overlay_service_.Visible() ? 2u : 0u) |
        (context_.prompts.surface_visible ? 4u : 0u) | (context_.prompts.dirty_visible ? 8u : 0u));
    chrome_unit(CurrentWindowRect(), backdrops, ChromePainter::Always);
  }
  {
    const InteractionState& interaction = context_.interaction_state;
    const EditorGroupRectsLayout split_rects = ComputeEditorGroupRectsForState(layout);
    std::vector<PreparedFrameState::EditorAreaChromePaint::Divider>& dividers = frame_state_.editor_area_chrome.dividers;
    dividers.clear();
    for (const EditorSplitDividerRect& divider : split_rects.dividers) {
      dividers.push_back({.rect = divider.rect,
                          .lit = interaction.drag_target == DragTarget::EditorSplitDivider &&
                                 divider.node == interaction.drag_editor_split_node &&
                                 divider.boundary == interaction.drag_editor_split_boundary});
    }
    for (std::size_t slot = 0; slot + 1 < kMaxEditorGroups; ++slot) {
      chrome_unit(slot < dividers.size() ? std::optional<SDL_FRect>(dividers[slot].rect) : std::nullopt,
                  slot < dividers.size() ? (dividers[slot].lit ? 1u : 2u) : 0u, ChromePainter::Editor);
    }
  }
  chrome_unit(project_state.overlay.visible ? std::optional<SDL_FRect>(ComputeOverlayRect(layout.overlay_anchor))
                                            : std::nullopt,
              0, ChromePainter::Always);
  {
    std::optional<SDL_FRect> popup_rect;
    std::optional<SDL_FRect> submenu_rect;
    if (context_.menu_state.menu_bar_open) {
      popup_rect = ComputePopupMenuRect(layout.menu_bar, context_.menu_state.active_menu_id);
      if (context_.menu_state.active_submenu_id != MenuId::None) {
        submenu_rect = ActiveSubmenuRect(layout.menu_bar);
      }
    }
    frame_state_.menu_popup_rect = popup_rect;
    frame_state_.menu_submenu_rect = submenu_rect;
  }
  frame_state_.context_menu_rect = ComputeTreeContextMenuRect();
  assert((!prepare_cached_bottom_panel_vm_.has_value() ||
          prepare_cached_bottom_panel_vm_->focus == project_state.surface.focus) &&
         "frame prep moved the keyboard after the surface view models were built");
  chrome_unit(FocusSurfaceRect(layout, project_state.surface.focus),
              static_cast<std::uint64_t>(project_state.surface.focus), ChromePainter::Always);
  {
    const TabSlideState& slide = context_.interaction_state.tab_slide;
    for (std::size_t i = 0; i < kMaxAnimatingTabStrips; ++i) {
      std::optional<SDL_FRect> strip;
      std::uint64_t key = 0;
      if (i < slide.strips.size() && !slide.strips[i].idle()) {
        strip = TabStripRectForKind(slide.strips[i].kind, slide.strips[i].group_index);
        key = (static_cast<std::uint64_t>(slide.strips[i].kind) << 32) | slide.strips[i].group_index;
      }
      chrome_unit(strip, key, ChromePainter::WindowChrome);
    }
  }
}

void WorkspaceShell::PrepareWindowChromeUnits(FramePrepPass& pass) {
  WorkspaceLayout& layout = pass.layout;
  const auto chrome_unit = [&pass](const std::optional<SDL_FRect>& rect, std::uint64_t key,
                                   ChromePainter painter) { pass.ChromeUnit(rect, key, painter); };
  {
    PreparedFrameState::TabStripPaint& strip = frame_state_.project_strip;
    ResetTabStripPaint(strip);
    if (layout.project_tab_strip.h > 0.0f) {
      const TabDragState& drag = context_.interaction_state.tab_drag;
      const bool project_drag = drag.dragging && drag.kind == TabDragKind::Project;
      const std::span<const float> slide = context_.interaction_state.tab_slide.OffsetsFor(TabDragKind::Project, 0);
      const auto pointer_over = [this](const SDL_FRect& rect) { return PointerOver(rect); };
      const std::vector<VisibleStripTab>& visible = tab_strip_chrome_.ComputeVisibleProjectTabs(layout.project_tab_strip);
      strip.shown = true;
      SettleTabStripTabs(strip, visible, slide, project_drag ? std::optional<std::size_t>(drag.source_index) : std::nullopt,
                         slide.empty() && !drag.dragging, pointer_over);
      SettleTabStripOverflow(strip, tab_strip_chrome_.ComputeProjectTabOverflowControls(layout.project_tab_strip, visible),
                             pointer_over);
      if (project_drag) {
        SettleTabStripDrag(strip, visible, layout.project_tab_strip, drag.source_index, drag.pointer_x,
                           drag.grab_offset_x);
      }
    }
    chrome_unit(layout.project_tab_strip.h > 0.0f ? std::optional<SDL_FRect>(layout.project_tab_strip) : std::nullopt,
                FingerprintOf(strip, 0), ChromePainter::WindowChrome);
  }
  {
    std::vector<PreparedFrameState::WindowChromePaint::StatusItem>& items = frame_state_.window_chrome.status_items;
    items.clear();
    for (const VisibleStatusItem& item : ComputeVisibleStatusItems(layout.breadcrumb)) {
      items.push_back(PreparedFrameState::WindowChromePaint::StatusItem{
          .rect = item.rect,
          .text = item.item.text,
          .icon = item.item.icon,
          .tone = item.item.tone,
          .progress = item.item.progress,
          .hovered = item.hovered && !item.item.command.empty(),
          .left_aligned = item.item.alignment == StatusAlignment::Left,
      });
    }
    PaintFingerprint key;
    for (const PreparedFrameState::WindowChromePaint::StatusItem& item : items) {
      key.Add(FingerprintOf(item));
    }
    frame_state_.status_items_hash = key.value();
  }
  PrepareTabStripUnits(pass);
}

void WorkspaceShell::PrepareTabStripUnits(FramePrepPass& pass) {
  ProjectWorkspaceState& project_state = *pass.project_state;
  WorkspaceLayout& layout = pass.layout;
  const auto chrome_unit = [&pass](const std::optional<SDL_FRect>& rect, std::uint64_t key,
                                   ChromePainter painter) { pass.ChromeUnit(rect, key, painter); };
  const EditorGroupRectsLayout group_rects = ComputeEditorGroupRectsForState(layout);
  const std::size_t focused = project_state.clamped_focused_group_index();
  PreparedFrameState::WindowChromePaint& chrome = frame_state_.window_chrome;
  chrome.focused_group = focused;
  chrome.groups.resize(group_rects.groups.size());
  for (std::size_t gi = 0; gi < group_rects.groups.size(); ++gi) {
    chrome.groups[gi].tab_strip = group_rects.groups[gi].tab_strip;
    chrome.groups[gi].breadcrumb = group_rects.groups[gi].breadcrumb;
    chrome.groups[gi].breadcrumb_label = BreadcrumbLabel(gi);
  }
  for (std::size_t gi = 0; gi < kMaxEditorGroups; ++gi) {
    const bool present = gi < group_rects.groups.size() && gi < project_state.editor_groups.size();
    PaintFingerprint strip;
    PaintFingerprint band;
    PreparedFrameState::TabStripPaint& strip_paint = frame_state_.editor_strips[gi];
    ResetTabStripPaint(strip_paint);
    if (present) {
      strip.Add(gi == focused).Add(group_rects.groups.size());
      const SDL_FRect& strip_rect = group_rects.groups[gi].tab_strip;
      if (strip_rect.h > 0.0f && HasActiveProjectCatalogEntry()) {
        const TabDragState& drag = context_.interaction_state.tab_drag;
        const std::span<const float> slide = context_.interaction_state.tab_slide.OffsetsFor(TabDragKind::Editor, gi);
        const bool still = slide.empty() && !drag.dragging;
        const std::vector<VisibleStripTab>& visible = tab_strip_chrome_.ComputeVisibleTabsForGroup(gi, strip_rect);
        const auto overflow = tab_strip_chrome_.ComputeTabOverflowControlsForGroup(gi, strip_rect, visible);
        const auto pointer_over = [this](const SDL_FRect& rect) { return PointerOver(rect); };
        const bool editor_drag = drag.dragging && drag.kind == TabDragKind::Editor;
        strip_paint.shown = true;
        strip_paint.placeholder = visible.empty();
        SettleTabStripTabs(
            strip_paint, visible, slide,
            editor_drag && drag.source_group_index == gi ? std::optional<std::size_t>(drag.source_index) : std::nullopt,
            still, pointer_over);
        SettleTabStripOverflow(strip_paint, overflow, pointer_over);
        if (!strip_paint.placeholder && editor_drag && gi == drag.target_group_index) {
          const bool cross_group = drag.source_group_index != gi;
          const bool source_known = drag.source_group_index < group_rects.groups.size();
          const SDL_FRect& source_strip =
              cross_group && source_known ? group_rects.groups[drag.source_group_index].tab_strip : strip_rect;
          const std::vector<VisibleStripTab>& source_tabs =
              !cross_group   ? visible
              : source_known ? tab_strip_chrome_.ComputeVisibleTabsForGroup(drag.source_group_index, source_strip)
                             : TabStripService::EmptyVisibleTabs();
          SettleTabStripDrag(strip_paint, source_tabs, source_strip, drag.source_index, drag.pointer_x,
                             drag.grab_offset_x);
        }
      }
      strip.Add(FingerprintOf(strip_paint, 0));
      band.Add(gi == focused).Add(std::string_view(chrome.groups[gi].breadcrumb_label)).Add(frame_state_.status_items_hash);
    }
    chrome_unit(present ? std::optional<SDL_FRect>(group_rects.groups[gi].tab_strip) : std::nullopt, strip.value(), ChromePainter::WindowChrome);
    chrome_unit(present ? std::optional<SDL_FRect>(group_rects.groups[gi].breadcrumb) : std::nullopt, band.value(), ChromePainter::WindowChrome);
  }
}

void WorkspaceShell::ResolvePointerOccluder(FramePrepPass& pass) {
  ProjectWorkspaceState& project_state = *pass.project_state;
  WorkspaceLayout& layout = pass.layout;
  frame_state_.pointer_occluder.reset();
  pass.toasts = notification_service_.Empty() ? NotificationsViewModel{}
                                              : RenderViewModelBuilder(context_).BuildNotifications(
                                                    notification_service_, layout.status_bar, text_renderer_);
  const NotificationsViewModel& toasts = pass.toasts;
  std::optional<SDL_FRect> toast_under_pointer;
  if (last_mouse_position_valid_ && !context_.prompts.dirty_visible &&
      context_.interaction_state.drag_target == DragTarget::None) {
    if (const auto hit = NotificationHitAt(toasts, last_mouse_x_, last_mouse_y_); hit.has_value()) {
      for (const NotificationEntryViewModel& entry : toasts.entries) {
        if (entry.index == hit->index) {
          toast_under_pointer = entry.layout.rect;
        }
      }
    }
  }
  std::optional<SDL_FRect>& modal = pass.modal;
  if (context_.prompts.dirty_visible) {
    modal = ComputeDirtyPromptRect(layout.full);
  } else if (context_.prompts.surface_visible) {
    modal = ComputePromptSurfaceRect(layout.full);
  } else if (settings_overlay_service_.Visible()) {
    modal = layout.editor_area;
  }
  if (toast_under_pointer.has_value()) {
    frame_state_.pointer_occluder = toast_under_pointer;
  } else if (IsDividerDrag(context_.interaction_state.drag_target)) {
    frame_state_.pointer_occluder = SDL_FRect{};
  } else if (modal.has_value()) {
    frame_state_.pointer_occluder =
        last_mouse_position_valid_ && Contains(*modal, last_mouse_x_, last_mouse_y_) ? *modal
                                                                                      : SDL_FRect{};
  } else if (last_mouse_position_valid_) {
    for (const SDL_FRect& piece : CurrentChromeRects()) {
      if (Contains(piece, last_mouse_x_, last_mouse_y_)) {
        frame_state_.pointer_occluder = piece;
      }
    }
    if (frame_state_.pointer_occluder.has_value() && project_state.overlay.visible &&
        RectsEqual(*frame_state_.pointer_occluder, layout.menu_bar)) {
      const SDL_FRect card = ComputeOverlayRect(layout.overlay_anchor);
      if (Contains(card, last_mouse_x_, last_mouse_y_)) {
        frame_state_.pointer_occluder = card;
      }
    }
    if (!frame_state_.pointer_occluder.has_value() && MenuSurfaceCapturingMouse()) {
      frame_state_.pointer_occluder = SDL_FRect{};
    }
    if (!frame_state_.pointer_occluder.has_value() && project_state.overlay.visible) {
      const SDL_FRect card = ComputeOverlayRect(layout.overlay_anchor);
      if (Contains(card, last_mouse_x_, last_mouse_y_)) {
        frame_state_.pointer_occluder = card;
      }
    }
  }
}

void WorkspaceShell::PreparePanelSurfaces(FramePrepPass& pass) {
  SDL_Renderer* const renderer = pass.renderer;
  WorkspaceLayout& layout = pass.layout;
  const NotificationsViewModel& toasts = pass.toasts;
  PrepareStatusBarFrame(layout);
  PrepareMenuPopupSurfaces(pass);
  PrepareBottomPanelHeaderSurface(pass);
  PrepareOutputBodySurface(pass);
  PrepareTerminalChromeSurface(pass);
  PrepareTerminalGridSurface(pass);
  PrepareNotificationToastsFrame(frame_state_, renderer, toasts, theme_,
                                 [this](const SDL_FRect& rect) { RequestRedrawRect(rect); });
  PrepareDebugPaneSurface(pass);
}

void WorkspaceShell::PrepareMenuPopupSurfaces(FramePrepPass& pass) {
  SDL_Renderer* const renderer = pass.renderer;
  WorkspaceLayout& layout = pass.layout;
  for (MenuPopupPaint& popup : frame_state_.menu_popups) {
    popup.Clear();
  }
  const auto popup_at = [this](MenuPopupSlot slot) -> MenuPopupPaint& {
    return frame_state_.menu_popups[static_cast<std::size_t>(slot)];
  };
  const auto lay_out = [this](MenuPopupPaint& popup, std::span<const MenuItemSpec> items, int active_item_index,
                              const SDL_FRect& rect) {
    popup.visible = true;
    popup.follows_pointer = true;
    popup.rect = rect;
    ComputeVisiblePopupMenuItemsInto(items, active_item_index, rect, menu_popup_rows_scratch_);
    for (const VisiblePopupMenuItem& item : menu_popup_rows_scratch_) {
      MenuRowPaint row{
          .rect = item.rect, .enabled = item.enabled, .checked = item.checked, .separator = item.separator};
      if (!item.separator) {
        const MenuItemSpec& spec = items[item.index];
        if (PointerOver(item.rect)) {
          popup.pointer_row = static_cast<int>(popup.rows.size());
        }
        row.hovered = item.hovered || popup.pointer_row == static_cast<int>(popup.rows.size());
        popup.SetLabel(row, MenuItemLabel(spec));
        popup.SetAccelerator(row, MenuItemAccelerator(spec));
      }
      popup.rows.push_back(row);
    }
  };
  if (context_.menu_state.menu_bar_open) {
    if (frame_state_.menu_popup_rect.has_value() && FindMenuSpec(context_.menu_state.active_menu_id) != nullptr) {
      lay_out(popup_at(MenuPopupSlot::Popup), MenuItems(context_.menu_state.active_menu_id),
              context_.menu_state.active_menu_item_index, *frame_state_.menu_popup_rect);
    }
    if (context_.menu_state.active_submenu_id != MenuId::None && frame_state_.menu_submenu_rect.has_value() &&
        FindMenuSpec(context_.menu_state.active_submenu_id) != nullptr) {
      lay_out(popup_at(MenuPopupSlot::Submenu), MenuItems(context_.menu_state.active_submenu_id),
              context_.menu_state.active_submenu_item_index, *frame_state_.menu_submenu_rect);
    }
  }
  if (const auto chevron = MenuOverflowChevronRect(layout.menu_bar);
      context_.menu_state.overflow_popup_open && chevron.has_value()) {
    const auto overflow = ComputeOverflowMenuBarItems(layout.menu_bar);
    if (!overflow.empty()) {
      MenuPopupPaint& popup = popup_at(MenuPopupSlot::Overflow);
      popup.visible = true;
      popup.rect = ComputeMenuOverflowPopupRect(*chevron, overflow.size());
      for (std::size_t i = 0; i < overflow.size(); ++i) {
        const MenuSpec* spec = FindMenuSpec(overflow[i]);
        MenuRowPaint row{.rect = MenuOverflowPopupRowRect(popup.rect, i),
                         .enabled = true,
                         .hovered = context_.menu_state.overflow_popup_active_index == static_cast<int>(i)};
        popup.SetLabel(row, spec != nullptr ? std::string_view(spec->label) : std::string_view{});
        popup.SetAccelerator(row, {});
        popup.rows.push_back(row);
      }
    }
  }
  if (context_.menu_state.tree_context_menu.open && frame_state_.context_menu_rect.has_value()) {
    const auto items = WorkspaceTreeContextMenuItems(context_.menu_state.tree_context_menu.target);
    if (!items.empty()) {
      lay_out(popup_at(MenuPopupSlot::Context), items, context_.menu_state.tree_context_menu.active_item_index,
              *frame_state_.context_menu_rect);
    }
  }
  CommitMenuPopupUnits(frame_state_, renderer, theme_, text_renderer_.MetricsGeneration(),
                       [this](const SDL_FRect& rect) { RequestRedrawRect(rect); });
}

void WorkspaceShell::PrepareBottomPanelHeaderSurface(FramePrepPass& pass) {
  SDL_Renderer* const renderer = pass.renderer;
  WorkspaceLayout& layout = pass.layout;
  const BottomPanelSurfaceViewModel& panel = *prepare_cached_bottom_panel_vm_;
  std::string_view label = "Terminal";
  if (panel.content == PanelContentKind::Output) {
    label = "Output";
    for (const auto& channel : output_channels_.Channels()) {
      if (channel.id == panel.output_channel_id) {
        label = channel.label.empty() ? std::string_view(channel.id) : std::string_view(channel.label);
        break;
      }
    }
  }
  const SDL_FRect panel_header =
      MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w, kWorkspaceBottomPanelHeaderHeight);
  PrepareBottomPanelHeaderFrame(
      frame_state_, BottomPanelHeaderInputs{
                        .renderer = renderer,
                        .layout = &layout,
                        .panel = &panel,
                        .theme = &theme_,
                        .label = label,
                        .new_terminal_rect = tab_strip_service_.BottomPanelTerminalNewTabRect(
                            layout_mode_service_.CurrentMode(), panel_header),
                        .maximize_rect = tab_strip_service_.BottomPanelMaximizeButtonRect(
                            layout_mode_service_.CurrentMode(), panel_header),
                        .divider_dragged = context_.interaction_state.drag_target == DragTarget::BottomPanelDivider,
                        .pointer_over = [this](const SDL_FRect& rect) { return PointerOver(rect); },
                        .damage = [this](const SDL_FRect& rect) { RequestRedrawRect(rect); },
                    });
}

void WorkspaceShell::PrepareOutputBodySurface(FramePrepPass& pass) {
  SDL_Renderer* const renderer = pass.renderer;
  WorkspaceLayout& layout = pass.layout;
  const BottomPanelSurfaceViewModel& panel = *prepare_cached_bottom_panel_vm_;
  BottomPanelOutputInputs inputs{
      .renderer = renderer,
      .visible = panel.content == PanelContentKind::Output && layout.bottom_panel.h > 0.0f,
      .channels = &output_channels_,
      .channel_id = panel.output_channel_id,
      .project_root = panel.project_root,
      .theme = &theme_,
      .text_metrics = text_renderer_.MetricsGeneration(),
      .damage = [this](const SDL_FRect& rect) { RequestRedrawRect(rect); },
  };
  if (inputs.visible) {
    const std::vector<std::string>* entries = OutputChannelEntries(panel.output_channel_id);
    const std::size_t count = entries != nullptr ? entries->size() : 0;
    const LogSurfaceLayout log = ComputeBottomPanelLogLayout(layout, count);
    SetBottomPanelScrollRow(log.scroll.vertical_scroll, count, log.scroll.visible_rows);
    inputs.content = log.content_rect;
    inputs.text_x = log.text_x;
    inputs.text_y = log.text_y;
    inputs.text_width = log.text_width;
    inputs.line_height = log.line_height;
    inputs.first_row = log.scroll.vertical_scroll;
    inputs.visible_rows = log.scroll.visible_rows;
    inputs.scrollbar = log.scroll.vertical_scrollbar;
    inputs.scrollbar_active = context_.interaction_state.drag_target == DragTarget::BottomPanelScrollbar;
    inputs.empty_label = panel.empty_label;
  }
  PrepareBottomPanelOutputFrame(frame_state_, inputs);
}

void WorkspaceShell::PrepareTerminalChromeSurface(FramePrepPass& pass) {
  SDL_Renderer* const renderer = pass.renderer;
  WorkspaceLayout& layout = pass.layout;
  const BottomPanelSurfaceViewModel& panel = *prepare_cached_bottom_panel_vm_;
  TerminalChromeInputs inputs{
      .renderer = renderer,
      .visible = panel.content == PanelContentKind::Terminal && layout.bottom_panel.h > 0.0f,
      .theme = &theme_,
      .text_metrics = text_renderer_.MetricsGeneration(),
      .empty_label = panel.empty_label,
      .damage = [this](const SDL_FRect& rect) { RequestRedrawRect(rect); },
  };
  const TerminalTabState* tab = panel.terminal_tab;
  const DragTarget drag = context_.interaction_state.drag_target;
  if (inputs.visible && (tab == nullptr || tab->panes.empty())) {
    const LogSurfaceLayout empty = ComputeBottomPanelLogLayout(layout, 0);
    inputs.no_session_box = MakeRect(empty.text_x, empty.text_y, empty.text_width,
                                     std::max(0.0f, empty.content_rect.y + empty.content_rect.h - empty.text_y));
  } else if (inputs.visible) {
    const TerminalPaneRectsLayout& rects = panel.terminal_panes;
    for (std::size_t index = 0; index < tab->panes.size() && index < rects.panes.size(); ++index) {
      const TerminalPaneState* pane = tab->panes[index].get();
      TerminalPaneChromeInputs& chrome = inputs.panes.emplace_back();
      if (pane == nullptr) {
        continue;
      }
      const bool active = index == tab->active_pane;
      const SDL_FRect& body = rects.panes[index];
      const LogSurfaceLayout log =
          ComputeTerminalPaneLogLayout(body, layout.bottom_panel.h, *pane, pane->frame_line_count);
      chrome.text_box = MakeRect(log.text_x, log.text_y, log.text_width, std::max(0.0f, body.y + body.h - log.text_y));
      chrome.shows_hint = active && pane->frame_line_count == 0;
      chrome.scrollbar = log.scroll.vertical_scrollbar;
      chrome.scrollbar_active = active && drag == DragTarget::BottomPanelScrollbar;
    }
    for (const TerminalPaneDividerRect& divider : rects.dividers) {
      inputs.dividers.push_back(PreparedFrameState::TerminalDividerPaint{
          .rect = divider.rect,
          .active = drag == DragTarget::TerminalPaneDivider &&
                    divider.boundary == context_.interaction_state.drag_terminal_pane_boundary,
      });
    }
  }
  PrepareTerminalChromeFrame(frame_state_, inputs);
}

void WorkspaceShell::PrepareTerminalGridSurface(FramePrepPass& pass) {
  SDL_Renderer* const renderer = pass.renderer;
  WorkspaceLayout& layout = pass.layout;
  const BottomPanelSurfaceViewModel& panel = *prepare_cached_bottom_panel_vm_;
  const TerminalTabState* tab = panel.terminal_tab;
  TerminalGridInputs inputs{
      .renderer = renderer,
      .visible = panel.content == PanelContentKind::Terminal && layout.bottom_panel.h > 0.0f && tab != nullptr &&
                 !tab->panes.empty(),
      .theme = &theme_,
      .text_metrics = terminal_text_renderer_.MetricsGeneration(),
      .char_width = terminal_text_renderer_.CharWidth(),
      .terminal_text = &terminal_text_renderer_,
      .caret_phase_on = frame_state_.caret_visible,
      .damage = [this](const SDL_FRect& rect) { RequestRedrawRect(rect); },
  };
  if (inputs.visible) {
    const TerminalPaneRectsLayout& rects = panel.terminal_panes;
    for (std::size_t index = 0; index < tab->panes.size() && index < rects.panes.size(); ++index) {
      const TerminalPaneState* pane = tab->panes[index].get();
      TerminalGridPaneInputs& grid = inputs.panes.emplace_back();
      if (pane == nullptr) {
        continue;
      }
      const SDL_FRect& body = rects.panes[index];
      const LogSurfaceLayout log =
          ComputeTerminalPaneLogLayout(body, layout.bottom_panel.h, *pane, pane->frame_line_count);
      grid.body = body;
      grid.text_x = log.text_x;
      grid.text_y = log.text_y;
      grid.text_width = log.text_width;
      grid.line_height = log.line_height;
      grid.first_row = static_cast<std::size_t>(std::max(0, log.scroll.vertical_scroll));
      grid.visible_rows = static_cast<std::size_t>(std::max(0, log.scroll.visible_rows));
      grid.line_count = pane->frame_line_count;
      grid.lines = pane->visible_lines_snapshot.lines;
      grid.row_keys = pane->visible_row_keys;
      grid.underline_colors = pane->visible_lines_snapshot.underline_colors;
      grid.reverse_video = pane->frame_reverse_video;
      grid.selection = pane->frame_selection;
      grid.selection_live = pane->frame_selection_live;
      grid.hovered_link = pane->hovered_link;
      grid.caret_cell = TerminalCaretRectIn(log, pane->frame_cursor);
      grid.cursor = pane->frame_cursor;
      grid.caret_owned = index == tab->active_pane && frame_state_.terminal_owns_caret;
      if (index == tab->active_pane && panel.find_matches != nullptr) {
        grid.find_matches = *panel.find_matches;
        grid.find_selected = panel.find_selected_index;
      }
    }
  }
  PrepareTerminalGridFrame(frame_state_, inputs);
}

void WorkspaceShell::PrepareDebugPaneSurface(FramePrepPass& pass) {
  SDL_Renderer* const renderer = pass.renderer;
  WorkspaceLayout& layout = pass.layout;
  DebugPaneInputs inputs{
      .renderer = renderer,
      .visible = prepare_cached_debug_pane_vm_.has_value() && prepare_cached_debug_pane_vm_->visible &&
                 layout.right_pane.w > 0.0f && layout.right_pane.h > 0.0f,
      .theme = &theme_,
      .text_renderer = &text_renderer_,
      .pointer_over = [this](const SDL_FRect& rect) { return PointerOver(rect); },
      .damage = [this](const SDL_FRect& rect) { RequestRedrawRect(rect); },
  };
  if (inputs.visible) {
    inputs.pane = &*prepare_cached_debug_pane_vm_;
    inputs.mode_row = DebugPaneModeRow(layout.right_pane);
    inputs.line_count = DebugPaneActiveRowCount();
    const LogSurfaceLayout list = ComputeDebugPaneListLayout(layout, inputs.line_count);
    SetDebugPaneScrollRow(list.scroll.vertical_scroll, inputs.line_count, list.scroll.visible_rows);
    inputs.content = list.content_rect;
    inputs.text_x = list.text_x;
    inputs.text_y = list.text_y;
    inputs.text_width = list.text_width;
    inputs.line_height = list.line_height;
    inputs.first_row = list.scroll.vertical_scroll;
    inputs.visible_rows = list.scroll.visible_rows;
    inputs.scrollbar = list.scroll.vertical_scrollbar;
    inputs.scrollbar_active = context_.interaction_state.drag_target == DragTarget::DebugPaneScrollbar;
    inputs.edit_caret_shown =
        context_.interaction_state.window_has_input_focus && context_.text_input.composition.text.empty();
  }
  PrepareDebugPaneFrame(frame_state_, inputs);
}

void WorkspaceShell::PrepareSidebarSurfaces(FramePrepPass& pass) {
  SDL_Renderer* const renderer = pass.renderer;
  const SidebarSurfaceViewModel& sidebar_vm = *pass.sidebar_vm;
  WorkspaceLayout& layout = pass.layout;
  PrepareGitPanelSurface(pass);
  PrepareSidebarModeRowFrame(
      frame_state_,
      SidebarModeRowInputs{
          .renderer = renderer,
          .visible = sidebar_vm.visible,
          .row = sidebar_vm.visible ? SidebarModeRow(layout.sidebar) : SidebarModeRowLayout{},
          .active_view_id = sidebar_vm.project_state->sidebar.view_id,
          .overflow_menu_open = context_.menu_state.menu_bar_open &&
                                context_.menu_state.active_menu_id == MenuId::SidebarMode &&
                                context_.menu_state.active_menu_anchor_rect.has_value(),
          .theme = &theme_,
          .text_renderer = &text_renderer_,
          .pointer_over = [this](const SDL_FRect& rect) { return PointerOver(rect); },
          .damage = [this](const SDL_FRect& rect) { RequestRedrawRect(rect); },
      });
  PrepareTreeSidebarSurface(pass);
  PrepareSidebarListSurface(pass);
  PrepareSearchSidebarSurface(pass);
  editor_blame_overlay_service_.ForgetVisibleOverlayIfStale(ActiveEditorViewport());
  if (last_mouse_position_valid_ || context_.interaction_state.caret_hover.active) {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::RefreshHover");
    UpdateEditorHover(last_mouse_x_, last_mouse_y_);
  }
  editor_hover_refresh_pending_ = false;
  PrepareMenuBarSurface(pass);
  PrepareSettingsCardSurface(pass);
}

void WorkspaceShell::PrepareGitPanelSurface(FramePrepPass& pass) {
  SDL_Renderer* const renderer = pass.renderer;
  const SidebarSurfaceViewModel& sidebar_vm = *pass.sidebar_vm;
  WorkspaceLayout& layout = pass.layout;
  const bool git_visible = sidebar_vm.visible && sidebar_vm.mode == SidebarMode::Git;
  ProjectWorkspaceState& project_state = *sidebar_vm.project_state;
  CommitWorkflowState& workflow = project_state.sidebar.git.commit_workflow;
  const bool draft_open = git_visible && workflow.open;
  GitPanelInputs inputs{
      .renderer = renderer,
      .visible = git_visible,
      .sidebar = layout.sidebar,
      .presentation = git_visible ? sidebar_vm.git_presentation : nullptr,
      .workflow = draft_open ? &workflow : nullptr,
      .stage_all_enabled = git_visible && CanStageAllGitSidebarEntries(),
      .discard_all_enabled = git_visible && CanDiscardAllGitSidebarEntries(),
      .commit_can_execute = draft_open && commit_workflow_service_.CanExecuteCommit(workflow),
      .panel_focused = project_state.surface.focus == FocusTarget::Sidebar && git_visible,
      .caret_on = frame_state_.caret_visible && sidebar_vm.commit_field_owns_caret,
      .theme = &theme_,
      .text_renderer = &text_renderer_,
      .pointer_over = [this](const SDL_FRect& rect) { return PointerOver(rect); },
      .damage = [this](const SDL_FRect& rect) { RequestRedrawRect(rect); },
  };
  if (git_visible && sidebar_vm.git_sidebar_lines != nullptr) {
    inputs.list = ComputeGitSidebarListLayout(layout.sidebar, sidebar_vm.git_sidebar_lines->size());
    project_state.sidebar.scroll_row = inputs.list.scroll_row;
    inputs.selected_entry = project_state.sidebar.git.selected_index;
    inputs.outgoing_base_button = GitSidebarOutgoingBaseButtonRect(layout.sidebar);
    inputs.outgoing_base_menu_open = context_.menu_state.menu_bar_open &&
                                     context_.menu_state.active_menu_id == MenuId::GitOutgoingBase &&
                                     context_.menu_state.active_menu_anchor_rect.has_value();
    inputs.scrollbar_active = context_.interaction_state.drag_target == DragTarget::SidebarScrollbar;
  }
  PrepareGitPanelFrame(frame_state_, inputs);
  if (draft_open) {
    const PreparedFrameState::GitPanelPaint& head = frame_state_.git_panel;
    workflow.subject_field_rect = head.subject.frame;
    workflow.body_field_rect = head.body.frame;
    workflow.commit_button_rect = head.commit_button;
    workflow.caret_rect = head.caret;
    workflow.body_visible_rows = head.body_visible_rows;
  }
}

void WorkspaceShell::PrepareTreeSidebarSurface(FramePrepPass& pass) {
  SDL_Renderer* const renderer = pass.renderer;
  const SidebarSurfaceViewModel& sidebar_vm = *pass.sidebar_vm;
  WorkspaceLayout& layout = pass.layout;
  const bool tree_visible = sidebar_vm.visible && sidebar_vm.mode == SidebarMode::Tree;
  ProjectWorkspaceState& project_state = *sidebar_vm.project_state;
  TreeSidebarInputs inputs{
      .renderer = renderer,
      .visible = tree_visible,
      .theme = &theme_,
      .text_renderer = &text_renderer_,
      .pointer_over = [this](const SDL_FRect& rect) { return PointerOver(rect); },
      .damage = [this](const SDL_FRect& rect) { RequestRedrawRect(rect); },
  };
  if (tree_visible) {
    const auto& entries = project_state.directory_tree.entries();
    inputs.entries = entries;
    inputs.selected_index = project_state.directory_tree.selected_index();
    inputs.list = ComputeTreeSidebarListLayout(layout.sidebar, entries.size());
    inputs.collapse_button = TreeSidebarCollapseButtonRect(layout.sidebar);
    inputs.refresh_button = TreeSidebarRefreshButtonRect(layout.sidebar);
    inputs.can_collapse_all = project_state.directory_tree.CanCollapseAll();
    inputs.scrollbar_active = context_.interaction_state.drag_target == DragTarget::SidebarScrollbar;
    auto& icon_cache = project_state.file_icon_cache;
    if (!file_icon_registry_.has_entries() && !SettingFlagEnabled(GetSettingValue("sidebar.file_icons"))) {
      if (!icon_cache.icons.empty()) {
        icon_cache.Reset();
      }
    } else {
      const std::uint64_t tree_revision = project_state.directory_tree.entries_revision();
      const std::uint32_t icon_revision = file_icon_registry_.revision();
      if (icon_cache.tree_revision != tree_revision || icon_cache.icon_revision != icon_revision) {
        icon_cache.icons.assign(entries.size(), std::nullopt);
        for (std::size_t i = 0; i < entries.size(); ++i) {
          if (!entries[i].is_directory) {
            icon_cache.icons[i] = file_icon_registry_.Resolve(entries[i].label);
          }
        }
        icon_cache.tree_revision = tree_revision;
        icon_cache.icon_revision = icon_revision;
      }
      inputs.icons = &icon_cache.icons;
    }
  }
  PrepareTreeSidebarFrame(frame_state_, inputs);
}

void WorkspaceShell::PrepareSidebarListSurface(FramePrepPass& pass) {
  SDL_Renderer* const renderer = pass.renderer;
  const SidebarSurfaceViewModel& sidebar_vm = *pass.sidebar_vm;
  WorkspaceLayout& layout = pass.layout;
  const SidebarMode mode = sidebar_vm.mode;
  const bool list_visible = sidebar_vm.visible && SidebarViewUsesTheList(mode);
  ProjectWorkspaceState& project_state = *sidebar_vm.project_state;
  const auto& problems = project_state.sidebar.problems;
  const auto& tests = project_state.sidebar.tests;
  const auto& plugin = project_state.sidebar.plugin;
  SidebarListInputs inputs{
      .renderer = renderer,
      .visible = list_visible,
      .sidebar = layout.sidebar,
      .theme = &theme_,
      .text_renderer = &text_renderer_,
      .pointer_over = [this](const SDL_FRect& rect) { return PointerOver(rect); },
      .damage = [this](const SDL_FRect& rect) { RequestRedrawRect(rect); },
  };
  if (list_visible) {
    if (mode == SidebarMode::Problems) {
      inputs.row_count = problems.entries.size();
      inputs.selected_index = problems.selected_index;
      inputs.list = ComputeProblemsSidebarListLayout(layout.sidebar, inputs.row_count);
      inputs.row_at = [&problems](std::size_t i) {
        return SidebarListRow{problems.entries[i].primary_label, problems.entries[i].detail_label};
      };
      if (problems.entries.empty()) {
        inputs.placeholder = "No problems detected in this project.";
      }
    } else if (mode == SidebarMode::Tests) {
      inputs.row_count = tests.entries.size();
      inputs.selected_index = tests.selected_index;
      inputs.list = ComputeTestsSidebarListLayout(layout.sidebar, inputs.row_count);
      inputs.row_at = [&tests](std::size_t i) {
        return SidebarListRow{tests.entries[i].label, tests.entries[i].detail_label};
      };
      if (!tests.error.empty()) {
        inputs.placeholder = tests.error;
        inputs.placeholder_is_error = true;
      } else if (tests.entries.empty()) {
        inputs.placeholder =
            tests.running ? "Discovering tests\xE2\x80\xA6" : "No tests discovered \xE2\x80\x94 run Discover Tests.";
      }
    } else {
      inputs.row_count = plugin.items.size();
      inputs.selected_index = plugin.selected_index;
      inputs.list = ComputePluginSidebarListLayout(layout.sidebar, inputs.row_count);
      inputs.row_at = [&plugin](std::size_t i) {
        const auto& item = plugin.items[i];
        return SidebarListRow{item.label, item.detail, item.depth, item.collapsible, item.collapsed};
      };
      inputs.placeholder = plugin.placeholder;
      inputs.placeholder_is_error = plugin.placeholder_is_error;
    }
    inputs.scrollbar_active = context_.interaction_state.drag_target == DragTarget::SidebarScrollbar;
  }
  PrepareSidebarListFrame(frame_state_, inputs);
}

void WorkspaceShell::PrepareSearchSidebarSurface(FramePrepPass& pass) {
  SDL_Renderer* const renderer = pass.renderer;
  const SidebarSurfaceViewModel& sidebar_vm = *pass.sidebar_vm;
  WorkspaceLayout& layout = pass.layout;
  ProjectWorkspaceState& project_state = *sidebar_vm.project_state;
  SearchSidebarInputs inputs{
      .renderer = renderer,
      .visible = sidebar_vm.visible && sidebar_vm.mode == SidebarMode::Search,
      .theme = &theme_,
      .text_renderer = &text_renderer_,
      .pointer_over = [this](const SDL_FRect& rect) { return PointerOver(rect); },
      .damage = [this](const SDL_FRect& rect) { RequestRedrawRect(rect); },
  };
  std::optional<TextInputVisual> visual;
  if (inputs.visible) {
    const SDL_FRect& sidebar = layout.sidebar;
    const ProjectSearchState& search = project_state.overlay.workflow.project_search;
    const TextInputSurface current_surface = prepare_cached_text_input_vm_->current_surface;
    if (IsSidebarSearchFieldSurface(current_surface)) {
      visual = BuildActiveTextInputVisual(layout, std::nullopt);
    }
    const std::string_view fallbacks[] = {
        sidebar_vm.query_fallback_text,
        sidebar_vm.replace_fallback_text,
        sidebar_vm.include_fallback_text,
        sidebar_vm.exclude_fallback_text,
    };
    const auto fields = project_search_panel::SidebarSearchFieldRects(sidebar, search.scope_expanded);
    for (std::size_t index = 0; index < fields.size(); ++index) {
      const project_search_panel::FieldSlot& slot = fields[index];
      SearchSidebarFieldInputs& field = inputs.fields[index];
      field.rect = slot.rect;
      field.text = fallbacks[index];
      field.focused = current_surface == slot.surface;
      field.editing = search.editing && search.edit_field == slot.field;
      if (visual.has_value() && visual->surface == slot.surface) {
        if (!visual->displayed_text.empty()) {
          field.text = visual->displayed_text;
        }
        PaintFingerprint caret;
        caret.Add(visual->cursor_x).Add(visual->selection_bytes.has_value());
        if (visual->selection_bytes.has_value()) {
          caret.Add(visual->selection_bytes->first).Add(visual->selection_bytes->second);
        }
        field.caret_key = caret.value();
      }
    }
    inputs.buttons = {
        SearchSidebarButtonPaint{.rect = project_search_panel::ModeButtonRect(sidebar),
                                 .label = project_search_panel::kModeButtonLabel,
                                 .active = search.options.pattern_mode == project::ProjectSearchPatternMode::Regex},
        SearchSidebarButtonPaint{.rect = project_search_panel::CaseButtonRect(sidebar),
                                 .label = ProjectSearchCaseButtonLabel(),
                                 .active = search.options.case_mode != project::ProjectSearchCaseMode::Smart},
        SearchSidebarButtonPaint{.rect = project_search_panel::HiddenButtonRect(sidebar),
                                 .label = project_search_panel::kHiddenButtonLabel,
                                 .active = search.options.show_hidden},
        SearchSidebarButtonPaint{.rect = project_search_panel::ReplaceAllButtonRect(sidebar),
                                 .label = "All",
                                 .enabled = !search.query.text().empty()},
        SearchSidebarButtonPaint{.rect = project_search_panel::ScopeButtonRect(sidebar),
                                 .label = "\xE2\x80\xA6",
                                 .active = search.scope_expanded},
    };
    inputs.status_rect =
        project_search_panel::StatusRect(sidebar, search.scope_expanded, text_renderer_.LineHeight());
    inputs.status = sidebar_vm.project_search_status_text;
    const std::vector<int>& line_map = BuildProjectSearchLineMap();
    inputs.results = search.results;
    inputs.line_map = line_map;
    inputs.selected_index = search.selected_index;
    inputs.list = ComputeProjectSearchSidebarListLayout(sidebar, line_map.size());
    project_state.sidebar.scroll_row = inputs.list.scroll_row;
    inputs.placeholder_rect = SidebarPlaceholderBox(sidebar, inputs.list);
    inputs.empty_text = sidebar_vm.project_search_empty_text;
    inputs.empty_is_error = sidebar_vm.project_search_empty_is_error;
    inputs.scrollbar_active = context_.interaction_state.drag_target == DragTarget::SidebarScrollbar;
  }
  PrepareSearchSidebarFrame(frame_state_, inputs);
}

void WorkspaceShell::PrepareMenuBarSurface(FramePrepPass& pass) {
  WorkspaceLayout& layout = pass.layout;
  const auto chrome_unit = [&pass](const std::optional<SDL_FRect>& rect, std::uint64_t key,
                                   ChromePainter painter) { pass.ChromeUnit(rect, key, painter); };
  PreparedFrameState::WindowChromePaint& chrome = frame_state_.window_chrome;
  chrome.menu_items.clear();
  for (const VisibleMenuBarItem& item : ComputeVisibleMenuBarItems(layout.menu_bar)) {
    if (const MenuSpec* menu = FindMenuSpec(item.id); menu != nullptr) {
      chrome.menu_items.push_back(PreparedFrameState::WindowChromePaint::MenuItem{
          .rect = item.rect, .label = menu->label, .active = item.active, .hovered = !item.active && PointerOver(item.rect)});
    }
  }
  for (std::size_t slot = 0; slot < kMaxMenuBarItems; ++slot) {
    chrome_unit(slot < chrome.menu_items.size() ? std::optional<SDL_FRect>(chrome.menu_items[slot].rect) : std::nullopt,
                slot < chrome.menu_items.size() ? FingerprintOf(chrome.menu_items[slot]) : 0, ChromePainter::WindowChrome);
  }
  chrome.chevron = MenuOverflowChevronRect(layout.menu_bar);
  chrome.chevron_hovered = chrome.chevron.has_value() && PointerOver(*chrome.chevron);
  chrome_unit(chrome.chevron, chrome.chevron_hovered ? 3u : 1u, ChromePainter::WindowChrome);
  chrome.window_buttons.clear();
  for (const VisibleWindowControlButton& button : ComputeVisibleWindowControlButtons(layout.menu_bar)) {
    chrome.window_buttons.push_back(PreparedFrameState::WindowChromePaint::WindowButton{
        .rect = button.rect, .id = static_cast<int>(button.id), .hovered = PointerOver(button.rect)});
  }
  chrome.expanded = CurrentWindowChromeState().Expanded();
  for (std::size_t slot = 0; slot < kWindowControlButtonCount; ++slot) {
    chrome_unit(slot < chrome.window_buttons.size() ? std::optional<SDL_FRect>(chrome.window_buttons[slot].rect)
                                                    : std::nullopt,
                slot < chrome.window_buttons.size() ? FingerprintOf(chrome.window_buttons[slot], chrome.expanded) : 0,
                ChromePainter::WindowChrome);
  }
}

void WorkspaceShell::PrepareSettingsCardSurface(FramePrepPass& pass) {
  SDL_Renderer* const renderer = pass.renderer;
  WorkspaceLayout& layout = pass.layout;
  const DragTarget drag = context_.interaction_state.drag_target;
  PrepareSettingsCardFrame(
      frame_state_, SettingsCardInputs{
                        .renderer = renderer,
                        .theme = &theme_,
                        .text_renderer = &text_renderer_,
                        .vm = RenderViewModelBuilder(context_).BuildSettingsOverlay(layout, settings_overlay_service_,
                                                                                    text_renderer_),
                        .backdrop = layout.overlay_anchor,
                        .pointer_over = [this](const SDL_FRect& rect) { return PointerOver(rect); },
                        .scrollbar_dragged = drag == DragTarget::SettingsScrollbar,
                        .category_scrollbar_dragged = drag == DragTarget::SettingsCategoryScrollbar,
                        .picker_scrollbar_dragged = drag == DragTarget::SettingsPickerScrollbar,
                        .damage = [this](const SDL_FRect& rect) { RequestRedrawRect(rect); },
                    });
}

void WorkspaceShell::PrepareHoverFrame(FramePrepPass& pass) {
  ProjectWorkspaceState& project_state = *pass.project_state;
  WorkspaceLayout& layout = pass.layout;
  const auto chrome_unit = [&pass](const std::optional<SDL_FRect>& rect, std::uint64_t key,
                                   ChromePainter painter) { pass.ChromeUnit(rect, key, painter); };
  {
    std::optional<SDL_FRect> hover_rect;
    if (last_mouse_position_valid_) {
      std::optional<SDL_FRect> rect = HoveredStatusBarSegmentRect(layout, last_mouse_x_, last_mouse_y_);
      if (!rect.has_value()) {
        rect = HoveredInteractiveRect(layout, last_mouse_x_, last_mouse_y_);
      }
      if (rect.has_value() && PointerOver(*rect)) {
        hover_rect = MakeRect(rect->x - 6.0f, rect->y - 1.0f, rect->w + 12.0f, rect->h + 2.0f);
      }
    }
    std::optional<SDL_FRect> tooltip_rect;
    CardPaint& tooltip_card = frame_state_.tooltip_card;
    tooltip_card.Clear();
    if (const auto tooltip = HoveredTooltip(layout); tooltip.has_value()) {
      tooltip_card.Tooltip(text_renderer_, theme_, tooltip->rect, tooltip->text);
      tooltip_rect = MakeRect(tooltip->rect.x - 1.0f, tooltip->rect.y - 1.0f, tooltip->rect.w + 2.0f,
                              tooltip->rect.h + 2.0f);
    }
    chrome_unit(hover_rect, 0, ChromePainter::Always);
    chrome_unit(tooltip_rect, FingerprintOf(tooltip_card), ChromePainter::Always);
    {
      PreparedFrameState::EditorAreaChromePaint& area = frame_state_.editor_area_chrome;
      PreparedFrameState::EditorAreaChromePaint::Banner& banner = area.banner;
      const bool diff_in_front = ActiveTabIsCompare() || ActiveTabIsMerge();
      EnsureClipFrameAndOverlayViewModels(layout);
      const std::optional<EditorBannerViewModel>& banner_vm = clip_cached_frame_vm_->editor_banner;
      const SDL_FRect strip = diff_in_front || !banner_vm.has_value()
                                  ? SDL_FRect{}
                                  : ActiveEditorBannerStripRect(project_state, ComputeEditorGroupRectsForState(layout));
      banner.shown = strip.h > 0.0f;
      banner.hovered = {};
      banner.dismiss_hovered = false;
      if (banner.shown) {
        banner.strip = strip;
        banner.message = banner_vm->message;
        banner.buttons = ComputeEditorBannerButtonRects(strip, banner_vm->buttons);
        for (std::size_t i = 0; i < banner.buttons.buttons.size(); ++i) {
          banner.hovered[i] = PointerOver(banner.buttons.rects[i]);
        }
        banner.dismiss_hovered = PointerOver(banner.buttons.dismiss);
      }
      chrome_unit(banner.shown ? std::optional<SDL_FRect>(strip) : std::nullopt, FingerprintOf(banner),
                  ChromePainter::Editor);
      area.focus_ring = project_state.surface.focus == FocusTarget::Editor;
    }
    {
      PreparedFrameState::WindowChromePaint& chrome = frame_state_.window_chrome;
      const PathDragState& path_drag = context_.interaction_state.path_drag;
      chrome.path_drop = path_drag.dragging && path_drag.target != PathDragState::Target::None
                             ? std::optional<SDL_FRect>(path_drag.drop_rect)
                             : std::nullopt;
      chrome.path_ghost = path_drag.dragging ? std::optional<SDL_FRect>(PathDragGhostRect(path_drag, text_renderer_))
                                             : std::nullopt;
      chrome.path_label = path_drag.dragging ? path_drag.label : std::string();
      chrome_unit(chrome.path_drop, 1u, ChromePainter::WindowChrome);
      chrome_unit(chrome.path_ghost, PaintFingerprint().Add(std::string_view(chrome.path_label)).value(),
                  ChromePainter::WindowChrome);
      const TabDragState& tab_drag = context_.interaction_state.tab_drag;
      chrome.tab_body_drop = tab_drag.dragging && tab_drag.body_drop()
                                 ? std::optional<SDL_FRect>(tab_drag.body_drop_rect)
                                 : std::nullopt;
      chrome_unit(chrome.tab_body_drop, 1u, ChromePainter::WindowChrome);
    }
    PrepareOverlayUnits(layout, project_state.overlay.visible);
    {
      PreparedFrameState::DebugToolbarPaint& toolbar = frame_state_.debug_toolbar;
      toolbar = {};
      if (DebugToolbarVisible() && !ActiveTabIsCompare() && !ActiveTabIsMerge()) {
        toolbar.visible = true;
        toolbar.stopped = IsDebugSessionStopped();
        toolbar.layout = ComputeDebugToolbarLayout(ActiveTabSurface(layout), DebugToolbarAvoidBelowY(layout),
                                                   debug_service_.SupportsStepBack());
        for (std::size_t i = 0; i < toolbar.layout.button_count; ++i) {
          toolbar.pointer_over[i] = PointerOver(toolbar.layout.buttons[i]);
        }
      }
      chrome_unit(toolbar.visible ? std::optional<SDL_FRect>(toolbar.layout.widget) : std::nullopt,
                  FingerprintOf(toolbar), ChromePainter::Editor);
    }
    {
      PreparedFrameState::PromptDialogPaint& dirty = frame_state_.dirty_prompt_dialog;
      dirty = {};
      std::optional<SDL_FRect> dirty_rect;
      if (context_.prompts.dirty_visible) {
        const SDL_FRect dialog = ComputeDirtyPromptRect(layout.full);
        dirty_rect = dialog;
        dirty.shown = true;
        dirty.title = DirtyPromptTitle();
        dirty.message = DirtyPromptMessage();
        dirty.selected = context_.prompts.dirty.selected_action;
        const auto labels = DirtyPromptActionLabels();
        const auto rects = ComputeDirtyPromptButtonRects(dialog);
        dirty.button_count = std::min({rects.size(), labels.size(), dirty.labels.size()});
        for (std::size_t i = 0; i < dirty.button_count; ++i) {
          dirty.labels[i] = labels[i];
          dirty.hovered[i] = PointerOver(rects[i]);
        }
      }
      chrome_unit(dirty_rect, FingerprintOf(dirty), ChromePainter::Always);
      PreparedFrameState::PromptDialogPaint& prompt = frame_state_.prompt_dialog;
      prompt = {};
      std::optional<SDL_FRect> prompt_rect;
      if (context_.prompts.surface_visible) {
        const PromptSurfaceState& surface = context_.prompts.surface;
        const SDL_FRect dialog = ComputePromptSurfaceRect(layout.full);
        prompt_rect = dialog;
        prompt.shown = true;
        prompt.title = PromptSurfaceTitle();
        prompt.message = PromptSurfaceMessage();
        prompt.detail = PromptSurfaceDetail();
        prompt.text_input = surface.kind == PromptSurfaceState::Kind::TextInput;
        if (prompt.text_input) {
          const std::optional<TextInputVisual>& visual = prepared_text_input_visual_;
          prompt.input_text = visual.has_value() && visual->surface == TextInputSurface::PromptInput &&
                                      !visual->displayed_text.empty()
                                  ? visual->displayed_text
                                  : std::string(surface.input.text());
        }
        prompt.selected = surface.selected_button;
        prompt.laid_out_buttons = surface.button_count;
        const auto labels = PromptSurfaceActionLabels();
        const auto rects = ComputePromptSurfaceButtonRects(dialog, surface.button_count);
        prompt.button_count = std::min({rects.size(), labels.size(), prompt.labels.size()});
        for (std::size_t i = 0; i < prompt.button_count; ++i) {
          prompt.labels[i] = labels[i];
          prompt.hovered[i] = PointerOver(rects[i]);
        }
      }
      chrome_unit(prompt_rect, FingerprintOf(prompt), ChromePainter::Always);
    }
    std::optional<SDL_FRect> card;
    CardPaint& hover_card = frame_state_.editor_hover_card;
    hover_card.Clear();
    if (const std::optional<EditorHoverPopupLayout> hover = ActiveEditorHoverPopupLayout(); hover.has_value()) {
      LayOutEditorHoverCard(*hover, hover_card);
      const SDL_FRect& popup = hover->rect;
      card = MakeRect(popup.x - 2.0f, popup.y - 2.0f, popup.w + 4.0f, popup.h + 4.0f);
    }
    chrome_unit(card, FingerprintOf(hover_card), ChromePainter::Editor);
    std::optional<SDL_FRect> signature_rect;
    CardPaint& signature_card = frame_state_.signature_card;
    signature_card.Clear();
    if (active_signature_help_.has_value() && !MenuSurfaceCapturingMouse()) {
      if (const std::optional<SDL_FRect> caret = ActiveEditorCaretRect(layout); caret.has_value()) {
        signature_rect = ComputeTwoBlockHoverCardRect(
            active_signature_help_->signature, active_signature_help_->documentation,
            kEditorHoverPopupMaxPluginTitleLines, kEditorHoverPopupMaxPluginContentLines, *caret,
            layout.editor_surface);
        LayOutSignatureHelpCard(*active_signature_help_, *signature_rect, signature_card);
      }
    }
    chrome_unit(signature_rect, FingerprintOf(signature_card), ChromePainter::Editor);
  }
}

void WorkspaceShell::PrepareTextInputOverlays(FramePrepPass& pass) {
  const WorkspaceLayout& layout = pass.layout;
  std::optional<SDL_FRect> active_editor_pane;
  for (const EditorPaneLayout& pane : ComputeEditorPaneLayouts(layout)) {
    if (pane.active && pane.group_index < pass.project_state->editor_groups.size() &&
        GroupActiveEditorTab(pass.project_state->editor_groups[pane.group_index]) != nullptr) {
      active_editor_pane = pane.rect;
    }
  }
  prepared_text_input_visual_ = BuildActiveTextInputVisual(layout, active_editor_pane);
  UpdateTextInputArea(pass.renderer, SDL_GetRenderWindow(pass.renderer), prepared_text_input_visual_);
  LayOutTextInputMarks(prepared_text_input_visual_, frame_state_.text_input_marks);
  LayOutTextInputCaret(prepared_text_input_visual_, frame_state_.text_input_caret);
  for (const CardPaint* overlay : {&frame_state_.text_input_marks, &frame_state_.text_input_caret}) {
    pass.ChromeUnit(overlay->shown ? std::optional<SDL_FRect>(overlay->rect) : std::nullopt, FingerprintOf(*overlay),
                    ChromePainter::Always);
  }
}

void WorkspaceShell::PrepareRenderFrame(SDL_Renderer* renderer, int width, int height) {
  (void)PrepareFrameOnce(renderer, width, height);
}

void WorkspaceShell::PrepareOverlayUnits(const WorkspaceLayout& layout, bool overlay_visible) {
  std::vector<DerivedDamage::Unit>& units = frame_state_.derived_damage.Begin(DerivedSurface::Overlay);
  if (overlay_visible) {
    EnsureClipFrameAndOverlayViewModels(layout);
    const OverlaySurfaceViewModel& vm = *clip_cached_overlay_vm_;
    const auto unit = [&](const SDL_FRect& rect, std::uint64_t key) {
      units.push_back(DerivedDamage::Unit{.rect = rect, .fingerprint = key | 1u});
    };
    PaintFingerprint style;
    style.Add(overview::ThemeMarkerToken(theme_)).Add(text_renderer_.MetricsGeneration());
    if (prepare_cached_text_input_vm_.has_value()) {
      const TextInputSurfaceViewModel& inputs = *prepare_cached_text_input_vm_;
      for (const editor::SingleLineEditor* input :
           {inputs.buffer_search_query, inputs.buffer_search_replace, inputs.commit_picker_query,
            inputs.quick_pick_query, inputs.command_palette_query, inputs.file_finder_query}) {
        if (input != nullptr) {
          style.Add(input->caret()).Add(input->selection_anchor().has_value() ? *input->selection_anchor() + 1 : 0);
        }
      }
      style.Add(static_cast<int>(inputs.current_surface));
    }
    PreparedFrameState::OverlayPointerPaint& pointer = frame_state_.overlay_pointer;
    pointer = {};
    const std::uint64_t card = PaintFingerprint().Add(style.value()).Add(FingerprintOfOverlayCard(vm)).value();
    if (vm.mode == OverlayMode::BufferSearch || vm.mode == OverlayMode::BufferReplace) {
      const OverlayFindWidgetViewModel& fw_vm = vm.find_widget;
      const FindWidgetLayout& fw = fw_vm.fw;
      unit(fw.widget, card);
      for (std::size_t i = 0; i < fw.toggle_count && i < fw.toggle_buttons.size(); ++i) {
        pointer.toggle_hovered[i] = PointerOver(fw.toggle_buttons[i]);
        unit(fw.toggle_buttons[i], PaintFingerprint()
                                       .Add(fw_vm.toggles[i].label)
                                       .Add(fw_vm.toggles[i].active)
                                       .Add(pointer.toggle_hovered[i])
                                       .value());
      }
      pointer.prev_hovered = PointerOver(fw.prev_button);
      pointer.next_hovered = PointerOver(fw.next_button);
      pointer.close_hovered = PointerOver(fw.close_button);
      pointer.replace_hovered = PointerOver(fw.replace_button);
      pointer.replace_all_hovered = PointerOver(fw.replace_all_button);
      for (const auto& [button, hovered] :
           {std::pair{fw.prev_button, pointer.prev_hovered}, std::pair{fw.next_button, pointer.next_hovered},
            std::pair{fw.close_button, pointer.close_hovered}, std::pair{fw.replace_button, pointer.replace_hovered},
            std::pair{fw.replace_all_button, pointer.replace_all_hovered}}) {
        unit(button, PaintFingerprint()
                         .Add(button)
                         .Add(hovered)
                         .Add(fw_vm.has_matches)
                         .Add(fw_vm.has_query)
                         .value());
      }
    } else {
      unit(vm.caret_anchored ? vm.overlay_rect : layout.overlay_anchor, card);
      pointer.row_hovered.resize(vm.rows.size());
      for (std::size_t row = 0; row < vm.rows.size(); ++row) {
        const OverlayListRowViewModel& item = vm.rows[row];
        const SDL_FRect row_rect = ScrollableListRowRect(vm.list_layout, static_cast<int>(row));
        const bool selected = vm.scroll_row + static_cast<int>(row) == vm.selected_row;
        pointer.row_hovered[row] = PointerOver(row_rect);
        unit(row_rect, PaintFingerprint()
                           .Add(item.primary)
                           .Add(item.secondary)
                           .Add(item.secondary_width)
                           .Add(selected)
                           .Add(static_cast<bool>(pointer.row_hovered[row]))
                           .value());
      }
      pointer.scrollbar_dragged = context_.interaction_state.drag_target == DragTarget::OverlayScrollbar;
      if (const auto geometry = MakeVerticalScrollbarGeometry(
              vm.list_layout.list_rect, static_cast<float>(vm.total_rows), vm.list_layout.visible_units,
              static_cast<float>(vm.scroll_row));
          geometry.has_value()) {
        unit(geometry->track, PaintFingerprint().Add(geometry->thumb).Add(pointer.scrollbar_dragged).value());
      }
    }
  }
  frame_state_.derived_damage.Commit(DerivedSurface::Overlay, [this](const SDL_FRect& rect) { RequestRedrawRect(rect); });
}

void WorkspaceShell::EnsureClipFrameAndOverlayViewModels(const WorkspaceLayout& layout) const {
  if (clip_frame_overlay_view_models_valid_ &&
      clip_frame_overlay_view_models_frame_id_ == prepared_frame_id_ &&
      clip_frame_overlay_view_models_layout_ == layout) {
    return;
  }
  clip_cached_frame_vm_.emplace(RenderViewModelBuilder(context_).BuildFrameSurface(layout));
  util::AddPerformanceCounter(util::PerfCounterId::RenderViewModelBuildFrameSurfaceCalls, 1);
  if (!clip_cached_overlay_vm_.has_value()) {
    clip_cached_overlay_vm_.emplace();
  }
  RenderViewModelBuilder(context_).BuildOverlaySurfaceInto(
      *clip_cached_overlay_vm_, layout, ComputeOverlayRect(layout.overlay_anchor), text_renderer_);
  util::AddPerformanceCounter(util::PerfCounterId::RenderViewModelBuildOverlaySurfaceCalls, 1);
  clip_frame_overlay_view_models_layout_ = layout;
  clip_frame_overlay_view_models_frame_id_ = prepared_frame_id_;
  clip_frame_overlay_view_models_valid_ = true;
}

void WorkspaceShell::RenderFrameBase(SDL_Renderer* renderer, const WorkspaceLayout& layout) const {
  DrawFilledRect(renderer, layout.full, theme_.window_background);
  DrawFilledRect(renderer, layout.menu_bar, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.menu_bar.x,
                          layout.menu_bar.y + layout.menu_bar.h - kWorkspaceDividerThickness,
                          layout.menu_bar.w, kWorkspaceDividerThickness),
                 theme_.border);
  if (layout.project_tab_strip.h > 0.0f) {
    DrawFilledRect(renderer, layout.project_tab_strip, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(layout.project_tab_strip.x,
                            layout.project_tab_strip.y + layout.project_tab_strip.h -
                                kWorkspaceDividerThickness,
                            layout.project_tab_strip.w, kWorkspaceDividerThickness),
                   theme_.border);
  }
  const SDL_FRect tab_strip_band =
      MakeRect(layout.full.x, layout.tab_strip.y, layout.full.w, layout.tab_strip.h);
  DrawFilledRect(renderer, tab_strip_band, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(tab_strip_band.x,
                          tab_strip_band.y + tab_strip_band.h - kWorkspaceDividerThickness,
                          tab_strip_band.w, kWorkspaceDividerThickness),
                 theme_.border);
  DrawFilledRect(renderer, layout.breadcrumb, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.breadcrumb.x,
                          layout.breadcrumb.y + layout.breadcrumb.h - kWorkspaceDividerThickness,
                          layout.breadcrumb.w, kWorkspaceDividerThickness),
                 theme_.border);

  if (frame_state_.frame_base.sidebar_visible) {
    DrawFilledRect(renderer, layout.sidebar, theme_.surface_background);
    DrawFilledRect(renderer,
                   MakeRect(layout.sidebar.x + layout.sidebar.w, layout.sidebar.y,
                            kWorkspaceDividerThickness, layout.sidebar.h),
                   frame_state_.frame_base.sidebar_divider_lit ? theme_.accent : theme_.border);
    const SDL_FRect sidebar_header = MakeRect(layout.sidebar.x, layout.sidebar.y, layout.sidebar.w,
                                              kWorkspaceHeaderHeight);
    DrawFilledRect(renderer, sidebar_header, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(sidebar_header.x,
                            sidebar_header.y + sidebar_header.h - kWorkspaceDividerThickness,
                            sidebar_header.w, kWorkspaceDividerThickness),
                   theme_.border);
  }

  if (layout.right_pane.w > 0.0f && layout.right_pane.h > 0.0f) {
    DrawFilledRect(renderer,
                   MakeRect(layout.right_pane.x - kWorkspaceDividerThickness, layout.right_pane.y,
                            kWorkspaceDividerThickness, layout.right_pane.h),
                   theme_.border);
  }

  if (frame_state_.frame_base.bottom_panel_visible) {
    DrawFilledRect(renderer, layout.bottom_panel, theme_.surface_background);
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

void WorkspaceShell::PaintEditorPane(SDL_Renderer* renderer, std::size_t group_index, bool draw_caret) {
  const PreparedFrameState::EditorPanePaint& paint = frame_state_.editor_pane_paint[group_index];
  editor::TextViewport& viewport = *paint.viewport;
  const editor::EditorViewMetrics& metrics = paint.metrics;
  const render::ScopedRenderClip pane_clip(renderer, paint.rect);
  if (paint.welcome != nullptr) {
    editor_view_renderer_.Render(renderer, editor_text_renderer_, theme_, viewport, paint.rect, draw_caret, "",
                                 std::nullopt, std::nullopt, {}, &paint.view_model, paint.bracket_match,
                                 paint.indent_guides, paint.render_whitespace, paint.folding, paint.welcome);
    return;
  }
  util::PerformanceTrace::Scope pane_scope("WorkspaceShell::Render::EditorPane");
  static const std::optional<editor::EditorBlameOverlay> kNoBlame;
  editor_view_renderer_.Render(renderer, editor_text_renderer_, theme_, viewport, paint.rect, draw_caret,
                               paint.search_query, paint.active_search_match,
                               paint.blame_source != nullptr ? *paint.blame_source : kNoBlame, paint.diagnostics,
                               &paint.view_model, paint.bracket_match, paint.indent_guides, paint.render_whitespace,
                               paint.folding, nullptr, paint.decorations, paint.line_numbers,
                               paint.explicit_search_matches, paint.find_scope);
  {
    util::PerformanceTrace::Scope insets_scope("WorkspaceShell::Render::EditorPane::Insets");
    DrawEditorInsets(renderer, paint.rect, metrics, viewport.scroll_line(), paint.view_model);
  }
  if (paint.drop_indicator.has_value()) {
    const std::size_t visual_row = viewport.VisualRowForLine(paint.drop_indicator->line);
    if (visual_row >= viewport.scroll_line() && visual_row < viewport.scroll_line() + metrics.visible_rows) {
      const std::size_t visual_column = viewport.VisualColumnAt(paint.drop_indicator->line, paint.drop_indicator->column);
      const float x = metrics.text_x +
                      static_cast<float>(visual_column - std::min(visual_column, viewport.horizontal_scroll())) *
                          editor_text_renderer_.CharWidth();
      const float y =
          metrics.first_line_y + static_cast<float>(visual_row - viewport.scroll_line()) * metrics.line_height;
      if (x >= metrics.text_x && x < paint.rect.x + paint.rect.w) {
        DrawFilledRect(renderer, MakeRect(x, y, 2.0f, metrics.line_height), theme_.accent);
      }
    }
  }
  thread_local std::array<EditorOverviewPaneCache, kMaxEditorGroups> tls_overview_caches;
  thread_local std::vector<overview::MarkerInput> tls_overview_inputs;
  thread_local std::vector<std::uint32_t> tls_overview_buckets;
  thread_local std::vector<SDL_Color> tls_overview_palette;
  const auto scroll_layout = ComputeEditorScrollLayout(paint.rect, viewport, metrics);
  if (scroll_layout.vertical_scrollbar.has_value()) {
    DrawScrollbar(renderer, theme_, scroll_layout.vertical_scrollbar->track, scroll_layout.vertical_scrollbar->thumb,
                  paint.vertical_bar_dragged);
    if (paint.overview_ruler) {
      DrawEditorOverviewRuler(renderer, theme_, viewport, paint.diagnostics, paint.diagnostics_revision,
                              paint.diagnostics_min_severity, paint.ruler_search,
                              scroll_layout.vertical_scrollbar->track, paint.rect.x, tls_overview_caches[group_index],
                              tls_overview_inputs, tls_overview_buckets, tls_overview_palette);
    }
  }
  if (scroll_layout.horizontal_scrollbar.has_value()) {
    DrawScrollbar(renderer, theme_, scroll_layout.horizontal_scrollbar->track,
                  scroll_layout.horizontal_scrollbar->thumb, paint.horizontal_bar_dragged);
  }
}

void WorkspaceShell::RenderActiveWorkspaceSurface(SDL_Renderer* renderer,
                                                  const WorkspaceLayout& layout,
                                                  bool draw_editor_caret) {
  for (const PreparedFrameState::EditorPassPane& pane : frame_state_.editor_pass_panes) {
    if (pane.kind == PreparedFrameState::EditorPassPane::Kind::Editor) {
      PaintEditorPane(renderer, pane.group, pane.active && draw_editor_caret);
      continue;
    }
    const render::ScopedRenderClip surface_clip(renderer, pane.area);
    const DiffSurfacePaint& paint = frame_state_.diff_surface_paint[pane.group];
    if (pane.kind == PreparedFrameState::EditorPassPane::Kind::Compare) {
      CompareTabState& compare_tab = *pane.compare;
      const bool draw_compare_caret = pane.focused && compare_tab.right_view_active && draw_editor_caret;
      RenderCompareSurface(renderer, pane.area, compare_tab, draw_compare_caret,
                           CompareRenderProjectInputs{.paint = &paint});
      RenderCompareScrollbars(renderer, pane.area, compare_tab, paint);
    } else {
      RenderMergeSurface(renderer, pane.area, *pane.merge, pane.focused, paint);
      RenderMergeScrollbars(renderer, pane.area, *pane.merge, paint);
    }
  }
  PaintEditorAreaChrome(renderer, layout);
}

void WorkspaceShell::PaintEditorAreaChrome(SDL_Renderer* renderer, const WorkspaceLayout& layout) const {
  const PreparedFrameState::EditorAreaChromePaint& area = frame_state_.editor_area_chrome;
  for (const PreparedFrameState::EditorAreaChromePaint::Divider& divider : area.dividers) {
    DrawFilledRect(renderer, divider.rect, divider.lit ? theme_.accent : theme_.border);
  }

  if (const PreparedFrameState::EditorAreaChromePaint::Banner& banner = area.banner; banner.shown) {
    const SDL_FRect& strip = banner.strip;
    DrawFilledRect(renderer, strip, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(strip.x, strip.y + strip.h - kWorkspaceDividerThickness, strip.w,
                            kWorkspaceDividerThickness),
                   theme_.border);
    DrawFilledRect(renderer, MakeRect(strip.x, strip.y, 3.0f, strip.h), theme_.accent);

    const EditorBannerButtonLayout& buttons = banner.buttons;
    const float message_right = buttons.controls_left();
    const SDL_FRect message_rect = MakeRect(strip.x + 12.0f, strip.y,
                                            std::max(0.0f, message_right - strip.x - 20.0f), strip.h);
    const SDL_Rect clip{static_cast<int>(message_rect.x), static_cast<int>(message_rect.y),
                        static_cast<int>(message_rect.w), static_cast<int>(message_rect.h)};
    {
      const render::ScopedRenderClip clip_scope(renderer, clip);
      DrawVCenteredTextOn(text_renderer_, renderer, message_rect, 0.0f, theme_.text_secondary,
                          theme_.chrome_background, banner.message);
    }

    const auto draw_banner_button = [&](const SDL_FRect& rect, std::string_view label, ButtonTone tone,
                                        bool hovered) {
      if (rect.w <= 0.0f) {
        return;
      }
      DrawButtonCentered(text_renderer_, renderer, theme_, rect, label, tone,
                         ButtonVisualState{.enabled = true, .hovered = hovered, .active = false});
    };
    for (std::size_t i = 0; i < buttons.buttons.size(); ++i) {
      draw_banner_button(buttons.rects[i], buttons.buttons[i].label,
                         buttons.buttons[i].destructive ? ButtonTone::Destructive : ButtonTone::Neutral,
                         banner.hovered[i]);
    }
    draw_banner_button(buttons.dismiss, "x", ButtonTone::Neutral, banner.dismiss_hovered);
  }

  if (area.focus_ring) {
    DrawSurfaceFocusRing(renderer, layout.editor_surface);
  }

  RenderDebugToolbar(renderer);
}

}
