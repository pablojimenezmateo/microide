#include "workspace/render/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <functional>
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
#include "workspace/services/EditorTabService.h"
#include "workspace/services/TerminalPanelService.h"
#include "workspace/WorkspaceTextSearch.h"
#include "workspace/git/MergeResultValidation.h"
#include "render/ScopedRenderClip.h"
#include "workspace/SettingFlags.h"
#include "workspace/coordinators/WorkspaceDiffTabCoordinator.h"
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
  // The markers are placed by VisualRowForLine now, so the lane depends on the
  // FOLD LAYOUT and not only on how many rows it produces. Two fold states can
  // agree on the total -- collapse one three-line block while expanding another --
  // and place every marker differently. That dependency arrived with the mapping;
  // before it, markers were plain line numbers and folds could not affect them.
  std::size_t fold_revision = 0;
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
  // VISUAL rows, and every marker LINE mapped into that space. The lane sits in
  // the same track as the scrollbar thumb, which is positioned by `scroll_line()`
  // -- a visual row -- so a ruler measured in logical lines drifts out of step
  // with the thumb beside it exactly when soft wrap or a collapsed fold makes the
  // two spaces differ, putting a diagnostic's mark at a fraction of the lane the
  // thumb never reaches at that scroll position. Off the wrapped/folded path
  // VisualRowForLine is the identity, so this is the same number it always was.
  const std::size_t total_rows = viewport.visual_line_count();
  const auto marker_row = [&viewport](std::size_t line) { return viewport.VisualRowForLine(line); };

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
  sig.fold_revision = viewport.folding_revision();

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
    // Search matches (only while the find/replace overlay is open on the active pane).
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

  // Caret line, drawn live (one rect) so cursor movement never invalidates the cache.
  // Uses the allocation-free single-marker builder so a steady frame never touches the heap.
  const int caret_row = static_cast<int>(viewport.cursor_visual_row());
  const overview::MarkerInput caret_input{.start_row = caret_row,
                                          .end_row = caret_row + 1,
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

// What the gutter's fold markers show: every resolved fold opener and every
// collapsed one. Position-sensitive, so a fold that moved also differs.
std::uint64_t FoldMarkerFingerprint(const editor::FoldingModel& folds) {
  std::uint64_t hash = 0xcbf29ce484222325ull;
  const auto mix = [&hash](std::uint64_t value) { hash = (hash ^ value) * 0x100000001b3ull; };
  for (const editor::FoldRange& range : folds.resolved_ranges()) {
    mix(range.opener_line);
  }
  mix(~0ull);
  for (const editor::FoldRange& range : folds.collapsed_ranges()) {
    mix(range.opener_line);
  }
  return hash;
}

}  // namespace

void WorkspaceShell::ResizeTerminalToPanel(const WorkspaceLayout& layout) {
  TerminalTabState* tab = prepare_cached_bottom_panel_vm_.has_value()
                              ? prepare_cached_bottom_panel_vm_->terminal_tab
                              : nullptr;
  if (tab == nullptr) {
    return;
  }

  util::PerformanceTrace::Scope trace_scope("WorkspaceShell::ResizeTerminalToPanel");
  // Every pane of the tab on screen gets its own column count from its own slice
  // of the body; the row count is the panel's, shared by all of them.
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
    // A tab waiting on an asynchronous open is READ-ONLY until its bytes land, so
    // a completion that is never drained does not just delay the content — it
    // leaves a tab that cannot be edited, saved or undone, with no way out.
    //
    // The wake-driven drain (in ReloadProjectIfFilesChanged) is not enough on its
    // own: it depends on the wake being delivered, and a host that renders
    // WITHOUT pumping events never sees one. The perf harness is exactly such a
    // host — it calls Render() in a loop — and that is how this was found: an
    // 8 MB fixture opened asynchronously, stayed Loading forever, and the
    // scenario's Undo() failed. Rendering is the one thing every host does, so a
    // loading tab finishes here whatever else the host skips.
    if (file_read_service_.HasCompletions()) {
      file_read_service_.DrainCompletions();
    }
  }
  // Every caret move, jump and tab switch ends in a frame, so this is where the
  // Go Back / Go Forward history sees them -- one place instead of a hook in each
  // of the dozens of ways the caret can move.
  MakeEditorTabService().ObserveNavigationLocation();
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
  // Before any view model is built: they read the active text-input surface, and
  // a frame that synced it AFTER building them painted the surface focus had
  // just left -- the first frame of a freshly opened palette had no caret.
  SDL_Window* render_window = SDL_GetRenderWindow(renderer);
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::SyncTextInputSurface");
    MakeTextInputCoordinator().SyncTextInputSurface(render_window);
  }
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
  if (prepare_cached_bottom_panel_vm_->content == PanelContentKind::Terminal) {
    prepare_cached_bottom_panel_vm_->terminal_tab = project_state.active_terminal_tab();
  }
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
  // A compare pane's editable side is a view of a buffer an editor pane may have
  // just edited; its model is keyed on that buffer's content revision, so a pane
  // that did not take the keystroke rebuilds here (and only then) rather than
  // painting stale rows. Settled frames pay one integer compare per pane.
  for (EditorGroup& group : project_state.editor_groups) {
    if (!group.has_active_tab() || group.active_tab().kind != TabEntry::Kind::Compare ||
        !group.active_tab().compare.has_value()) {
      continue;
    }
    CompareTabState& compare_tab = *group.active_tab().compare;
    // A large compare opens as a stand-in and loads when first SHOWN — here — and
    // again after a cancelled load (TD-2026-09-29-312). Settled frames pay one
    // bool test; the coordinator is built only when a load is actually owed.
    if (compare_tab.load_pending) {
      if (!compare_tab.pending_load.armed()) {
        (void)MakeDiffTabCoordinator().StartPendingCompareLoad(compare_tab);
      }
      continue;
    }
    if (compare_tab.right_editable &&
        (!compare_tab.derived_fingerprint_valid ||
         compare_tab.derived_right_content_revision !=
             compare_tab.right_viewport.content_revision())) {
      RefreshCompareTabDerivedState(compare_tab);
    }
  }
  prepare_cached_text_input_vm_.emplace(view_models.BuildTextInputSurface());
  // `ComputeLayout` applies these same clamps to whatever it is handed, so the
  // geometry is already correct without this block. What this does is PERSIST the
  // corrected value back onto the project state, so the session, the divider drag
  // and the settings surface all agree with the layout the window is showing.
  const float clamped_sidebar_width =
      ClampSidebarWidth(project_state.sidebar.width, static_cast<float>(width));
  const float clamped_panel_height =
      ClampBottomPanelHeight(project_state.panel.height, static_cast<float>(height));
  const float resolved_sidebar_width = sidebar_vm.visible ? clamped_sidebar_width : 0.0f;
  const float clamped_right_pane_width = ClampRightPaneWidth(
      project_state.debug_pane.width, static_cast<float>(width), resolved_sidebar_width);
  project_state.sidebar.width = clamped_sidebar_width;
  project_state.panel.height = clamped_panel_height;
  project_state.debug_pane.width = clamped_right_pane_width;

  // One layout, one memo: this is the same `CurrentWorkspaceLayout()` every hit
  // test reads, so a frame can never paint a geometry the input paths disagree
  // with. The window size was written into `window_presentation_` above, so the
  // key sees this frame's size. A miss here is the only place the resolved
  // layout mode is committed back to the service (its hysteresis input).
  // Two terminal-panel rules that read focus and must land before the layout is
  // resolved: a maximized panel restores itself the moment another surface takes
  // the keyboard, and the tab on screen has no unseen output.
  {
    TerminalPanelService& terminal_panel = MakeTerminalPanelService();
    terminal_panel.SyncPanelMaximizedWithFocus();
    terminal_panel.NoteActiveTabShown();
  }
  const std::uint64_t layout_generation_before = layout_memo_generation_;
  WorkspaceLayout layout;
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::ComputeLayout");
    layout = *CurrentWorkspaceLayout();
  }
  const bool workspace_layout_recomputed = layout_memo_generation_ != layout_generation_before;
  if (workspace_layout_recomputed) {
    layout_mode_service_.SetCurrentMode(layout.layout_mode);
    ++prepare_frame_layout_compute_count_;
  }
  // `editor.autosave = on_focus_change`: the editor that had the keyboard last frame
  // lost it -- another file opened (Ctrl+P, the tree, a jump), the sidebar or the
  // terminal took the keys. Tab-strip switches and window blur saved before; every
  // other way out did not (QA recents-and-autosave). The find widget, completion
  // and code actions are part of the editor, as in VS Code.
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
    if (frame_state_.focused_editor != nullptr && focused_editor != frame_state_.focused_editor) {
      MaybeAutosaveDirtyTabs(/*on_focus_change=*/true);
    }
    frame_state_.focused_editor = focused_editor;
  }
  // The window's regions as painted. A region that appears, vanishes or moves
  // repaints where it was and where it is, whatever moved it: a plugin clearing
  // its panel surface hid the bottom panel and asked only for the editor, and
  // the sidebar's newly exposed bottom kept the panel's pixels (QA plugins-ui).
  {
    const std::array<SDL_FRect, PreparedFrameState::kTrackedRegions> regions = {
        layout.sidebar,     layout.bottom_panel, layout.editor_surface,
        layout.right_pane,  layout.status_bar,   layout.tab_strip};
    for (std::size_t i = 0; i < regions.size(); ++i) {
      SDL_FRect& painted = frame_state_.painted_regions[i];
      if (frame_state_.painted_regions_valid && !RectsEqual(painted, regions[i])) {
        RequestRedrawRect(painted);
        RequestRedrawRect(regions[i]);
      }
      painted = regions[i];
    }
    frame_state_.painted_regions_valid = true;
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
  if (panel_vm.content == PanelContentKind::Terminal && panel_vm.terminal_tab != nullptr) {
    const SDL_FRect& panel = layout.bottom_panel;
    // The grid depends on the panel rect AND on which tab is up with which pane
    // layout; the key folds the latter so a tab switch, a split or a divider move
    // re-fits without a per-pane mutex read on every frame.
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
    // The pane slices of the tab on screen, so the render TU paints prebuilt
    // rects instead of carving the body itself.
    if (TerminalTabState* tab = prepare_cached_bottom_panel_vm_->terminal_tab; tab != nullptr) {
      prepare_cached_bottom_panel_vm_->terminal_panes = tab->PaneRects(BottomPanelContentRect(layout));
      PrepareTerminalPanesForFrame(*tab, prepare_cached_bottom_panel_vm_->terminal_panes,
                                   layout.bottom_panel.h);
    }
  }
  // 1a) Terminal find bar: rescan and lay the bar out here, where the panel rect
  //     is known, so the render TU paints a fully prepared widget. The rescan is
  //     cheap by construction — settled scrollback is kept and only the visible
  //     grid is re-walked (see TerminalFindService::Refresh).
  if (panel_vm.content == PanelContentKind::Terminal && terminal_find_service_.visible()) {
    TerminalPaneState* terminal_tab = ActiveTerminalPane();
    terminal_find_service_.Refresh(terminal_tab);
    BottomPanelSurfaceViewModel& panel_out = *prepare_cached_bottom_panel_vm_;
    panel_out.find_visible = true;
    panel_out.find_query = &terminal_find_service_.query();
    panel_out.find_matches = &terminal_find_service_.matches();
    panel_out.find_selected_index = terminal_find_service_.selected_index();
    OverlayFindWidgetViewModel& find_vm = panel_out.find;
    find_vm.fw = ComputeFindWidgetLayout(ActiveTerminalBodyRect(layout), /*replace_mode=*/false,
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
    ClampOverlayScrollRow(ComputeOverlayRect(layout.overlay_anchor));
  }
  // 3) Commit-draft body viewport sizing + caret-keep-visible scroll clamp (the
  //    083 residual): moved out of RenderCommitBodyField so paint stays pure.
  if (sidebar_vm.visible && sidebar_vm.mode == SidebarMode::Git) {
    PrepareCommitBodyViewportForFrame(layout.sidebar);
  }
  frame_state_.caret_visible = CaretVisibleNow();
  frame_state_.terminal_owns_caret = project_state.surface.focus == FocusTarget::Panel &&
                                     context_.text_input.active_surface == TextInputSurface::Terminal;
  // Ownership changes the terminal caret's style (steady outline vs blinking block)
  // at the same place, which the caret-region comparison below cannot see.
  if (frame_state_.terminal_owns_caret != frame_state_.painted_terminal_owns_caret) {
    if (const auto caret = ActiveTerminalCaretRect(layout); caret.has_value()) {
      RequestRedrawRect(*caret);
    }
    frame_state_.painted_terminal_owns_caret = frame_state_.terminal_owns_caret;
  }
  // Whatever moves the caret away -- focus to another surface, a menu or prompt
  // that suspends the blink, the blink's own phase -- this frame repaints both the
  // caret region the last frame painted and the one it paints now. Each such
  // transition used to need its own damage request, and several had none: a menu
  // opened over a focused terminal left its caret on screen (QA redraw verifier).
  if (const std::optional<SDL_FRect> caret_rect =
          frame_state_.caret_visible ? CurrentCaretDirtyRect() : std::nullopt;
      caret_rect.has_value() != frame_state_.painted_caret_rect.has_value() ||
      (caret_rect.has_value() && !RectsEqual(*caret_rect, *frame_state_.painted_caret_rect))) {
    if (frame_state_.painted_caret_rect.has_value()) {
      RequestRedrawRect(*frame_state_.painted_caret_rect);
    }
    if (caret_rect.has_value()) {
      RequestRedrawRect(*caret_rect);
    }
    frame_state_.painted_caret_rect = caret_rect;
  }
  // A document shown in more than one pane is edited through one of them, and the
  // edit's damage names lines of the FOCUSED pane. Any other pane whose document
  // changed since it was painted repaints here, whatever made the change (typing,
  // undo, a plugin or LSP edit, a reload): typing in one half of a split left the
  // other half showing the old line (found by the QA redraw verifier).
  {
    const EditorGroupRectsLayout pane_rects = ComputeEditorGroupRectsForState(layout);
    const std::size_t focused_group = project_state.clamped_focused_group_index();
    for (std::size_t gi = 0; gi < project_state.editor_groups.size() && gi < kMaxEditorGroups; ++gi) {
      const editor::TextViewport* viewport = GroupActiveViewport(project_state.editor_groups[gi]);
      const std::uint64_t content =
          viewport == nullptr ? 0
                              : (reinterpret_cast<std::uintptr_t>(viewport) * 0x9E3779B97F4A7C15ull) ^
                                    viewport->content_revision();
      if (content != frame_state_.painted_pane_content[gi] && gi != focused_group && gi < pane_rects.groups.size() &&
          frame_state_.painted_pane_content[gi] != 0) {
        RequestRedrawRect(pane_rects.groups[gi].editor_surface);
      }
      frame_state_.painted_pane_content[gi] = content;
      // The gutter's fold markers: an edit can make or unmake a fold opener on a
      // line it did not touch (lines indented under a closing brace make that
      // brace's line an indentation fold), and its damage named only the edited
      // lines. Hashed only when the model's revision moved, so a settled frame
      // pays two compares.
      const editor::FoldingModel* folds =
          viewport == nullptr ? nullptr : GroupFoldingModelPtr(project_state.editor_groups[gi]);
      const std::uint64_t fold_revision =
          folds == nullptr ? 0 : (content * 31u) ^ static_cast<std::uint64_t>(folds->revision());
      // Plugin decorations are drawn only while the buffer is clean (they were
      // computed for the saved text): the keystroke that dirties it hides every
      // annotation in the file, and an undo or a save back to clean shows them,
      // while the edit's damage names only its own rows (QA plugins-ui).
      const bool decorated_clean =
          viewport != nullptr && !viewport->dirty() && !viewport->path().empty() &&
          project_state.plugin_presentation_if_present() != nullptr &&
          project_state.plugin_presentation_if_present()->decorations.FindByPathKey(
              viewport->path_key()) != nullptr;
      if (decorated_clean != frame_state_.painted_decorations[gi] && gi < pane_rects.groups.size()) {
        RequestRedrawRect(pane_rects.groups[gi].editor_surface);
      }
      frame_state_.painted_decorations[gi] = decorated_clean;
      if (fold_revision != frame_state_.painted_fold_revision[gi]) {
        frame_state_.painted_fold_revision[gi] = fold_revision;
        const std::uint64_t markers = folds == nullptr ? 0 : FoldMarkerFingerprint(*folds);
        if (markers != frame_state_.painted_fold_markers[gi] && gi < pane_rects.groups.size()) {
          RequestRedrawRect(pane_rects.groups[gi].editor_surface);
        }
        frame_state_.painted_fold_markers[gi] = markers;
      }
    }
  }
  // Breakpoints: the gutter dots in every pane and the Breakpoints pane's rows.
  // They change through the gutter, F9, the pane's own keys and menus, the control
  // channel's breakpoint-* commands and the adapter's verification -- and only the
  // paths that also moved the caret repainted anything (QA debug-keys: Delete in
  // the pane removed a breakpoint that stayed on screen).
  {
    const std::uint64_t breakpoints =
        (project_state.breakpoint_store.revision() * 0x9E3779B97F4A7C15ull) ^
        project_state.function_breakpoint_store.revision();
    if (breakpoints != frame_state_.painted_breakpoints) {
      frame_state_.painted_breakpoints = breakpoints;
      RequestEditorSurfaceRedraw();
      RequestDebugPaneRedraw();
    }
  }
  // The modal backdrop stack: a prompt opening over the command palette (or the
  // unsaved-changes prompt over a prompt) dims the whole window once more, and the
  // dialog's own repaint covered only the dialog (QA monkey, seeds 7 and 9).
  {
    const OverlayState& overlay = project_state.overlay;
    const bool overlay_backdrop = overlay.visible && overlay.mode != OverlayMode::BufferSearch &&
                                  overlay.mode != OverlayMode::BufferReplace &&
                                  overlay.mode != OverlayMode::Completion;
    const std::uint8_t backdrops = static_cast<std::uint8_t>(
        (overlay_backdrop ? 1u : 0u) | (settings_overlay_service_.Visible() ? 2u : 0u) |
        (context_.prompts.surface_visible ? 4u : 0u) | (context_.prompts.dirty_visible ? 8u : 0u));
    if (backdrops != frame_state_.painted_backdrops) {
      RequestWindowRedraw();
      frame_state_.painted_backdrops = backdrops;
    }
    // The editor paints find matches on the overlay's MODE (see the pane loop in
    // RenderActiveWorkspaceSurface), so this follows exactly that.
    const bool find_matches = overlay.mode == OverlayMode::BufferSearch ||
                              overlay.mode == OverlayMode::BufferReplace;
    if (find_matches != frame_state_.painted_find_matches) {
      RequestFocusedEditorRedraw();
      frame_state_.painted_find_matches = find_matches;
    }
  }
  {
    PreparedFrameState::CompareScroll compare_scroll;
    if (EditorGroup& group = project_state.focused_group();
        group.has_active_tab() && group.active_tab().compare.has_value()) {
      const CompareTabState& compare = *group.active_tab().compare;
      compare_scroll = {.tab = &compare, .row = compare.scroll_row,
                        .column = compare.horizontal_scroll};
    }
    if (compare_scroll != frame_state_.painted_compare_scroll) {
      if (compare_scroll.tab != nullptr) {
        RequestEditorSurfaceRedraw();
      }
      frame_state_.painted_compare_scroll = compare_scroll;
    }
  }
  {
    std::uint64_t dividers = 0;
    for (const EditorSplitDividerRect& divider : ComputeEditorGroupRectsForState(layout).dividers) {
      for (const float edge : {divider.rect.x, divider.rect.y, divider.rect.w, divider.rect.h}) {
        dividers = dividers * 0x100000001B3ull ^ static_cast<std::uint64_t>(std::lround(edge * 4.0f));
      }
    }
    if (dividers != frame_state_.painted_split_dividers) {
      // The whole column: each pane's tab strip sits above `editor_area`.
      const float top = layout.tab_strip.h > 0.0f ? layout.tab_strip.y : layout.editor_area.y;
      RequestRedrawRect(MakeRect(layout.editor_area.x, top, layout.editor_area.w,
                                 layout.editor_area.y + layout.editor_area.h - top));
      frame_state_.painted_split_dividers = dividers;
    }
  }
  // A project tab's label follows state no input touches -- its dirty marker is
  // "any editor tab in that project is dirty" -- so typing into or saving a file
  // changed it with no damage on the strip (remote QA suite).
  if (layout.project_tab_strip.h > 0.0f) {
    const std::uint64_t version = tab_strip_chrome_.ProjectStripSourceVersion();
    if (version != frame_state_.painted_project_strip_version) {
      RequestRedrawRect(layout.project_tab_strip);
      frame_state_.painted_project_strip_version = version;
    }
  }
  // The overlay card's extent (see PreparedFrameState::painted_overlay_rect).
  {
    std::optional<SDL_FRect> overlay_rect;
    if (project_state.overlay.visible) {
      overlay_rect = ComputeOverlayRect(layout.overlay_anchor);
    }
    const auto& painted = frame_state_.painted_overlay_rect;
    const auto same = [](const SDL_FRect& a, const SDL_FRect& b) {
      return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
    };
    if (painted.has_value() != overlay_rect.has_value() ||
        (painted.has_value() && !same(*painted, *overlay_rect))) {
      if (painted.has_value()) {
        RequestRedrawRect(*painted);
      }
      if (overlay_rect.has_value()) {
        RequestRedrawRect(*overlay_rect);
      }
      frame_state_.painted_overlay_rect = overlay_rect;
    }
  }
  // The merge header's status (see PreparedFrameState::painted_merge_status).
  {
    std::uint64_t status = 0;
    if (const MergeTabState* merge = ActiveMergeTab(); merge != nullptr) {
      status = (reinterpret_cast<std::uintptr_t>(merge) * 0x9E3779B97F4A7C15ull) ^
               (static_cast<std::uint64_t>(ComputeMergeResultState(*merge, merge->file_conflict)) + 1);
      status = status * 0x100000001B3ull ^ CountRemainingMergeConflicts(merge->conflicts);
      status = status * 0x100000001B3ull ^ (merge->selected_hunk + 1);
    }
    if (status != frame_state_.painted_merge_status) {
      if (status != 0) {
        RequestEditorSurfaceRedraw();
      }
      frame_state_.painted_merge_status = status;
    }
  }
  // The compare header (see PreparedFrameState::painted_compare_header).
  {
    std::uint64_t header = 0;
    if (const CompareTabState* compare = ActiveCompareTab(); compare != nullptr) {
      header = (reinterpret_cast<std::uintptr_t>(compare) * 0x9E3779B97F4A7C15ull) ^
               std::hash<std::string_view>{}(compare->review_header.summary_line);
    }
    if (header != frame_state_.painted_compare_header) {
      if (header != 0) {
        RequestEditorSurfaceRedraw();
      }
      frame_state_.painted_compare_header = header;
    }
  }
  // The output panel's lines (see PreparedFrameState::painted_output_revision).
  {
    const bool shows_output =
        panel_vm.content == PanelContentKind::Output && layout.bottom_panel.h > 0.0f;
    const std::uint64_t revision = shows_output ? output_channels_.ContentRevision() + 1 : 0;
    if (revision != frame_state_.painted_output_revision) {
      if (shows_output) {
        RequestBottomPanelContentRedraw();
      }
      frame_state_.painted_output_revision = revision;
    }
  }
  // The debug toolbar's run state (see PreparedFrameState::painted_debug_toolbar).
  {
    PreparedFrameState::DebugToolbar toolbar;
    if (DebugToolbarVisible()) {
      toolbar.visible = true;
      toolbar.stopped = IsDebugSessionStopped();
      toolbar.rect = ComputeDebugToolbarLayout(layout.editor_surface, DebugToolbarAvoidBelowY(layout),
                                               DebugSupportsReverse())
                         .widget;
    }
    if (!(toolbar == frame_state_.painted_debug_toolbar)) {
      for (const auto* painted : {&frame_state_.painted_debug_toolbar, &toolbar}) {
        if (painted->visible && painted->rect.w > 0.0f && painted->rect.h > 0.0f) {
          RequestRedrawRect(painted->rect);
        }
      }
      frame_state_.painted_debug_toolbar = toolbar;
    }
  }
  // The editor tabs' dirty markers: repaint the strips when the set of dirty tabs
  // moved, whatever path moved it (see PreparedFrameState).
  {
    std::uint64_t dirty = 0xCBF29CE484222325ull;
    for (std::size_t g = 0; g < project_state.editor_groups.size(); ++g) {
      const auto& tabs = project_state.editor_groups[g].open_tabs;
      for (std::size_t i = 0; i < tabs.size(); ++i) {
        if (TabEntryIsDirty(tabs[i])) {
          dirty = (dirty ^ ((static_cast<std::uint64_t>(g) << 32) | (i + 1))) * 0x100000001B3ull;
        }
      }
    }
    if (dirty != frame_state_.painted_editor_tab_dirty) {
      RequestTabStripRedraw();
      frame_state_.painted_editor_tab_dirty = dirty;
    }
  }
  // The sidebar's selected row: activating a tab selects its file in the tree (as
  // VS Code's explorer follows the active editor) without repainting the tree
  // (QA monkey, seed 5). Any selection that moved with no repaint of its own
  // repaints the sidebar here -- and so does a scroll or a rebuilt tree (a watcher
  // batch, an expansion) that landed between frames: the command fuzzer caught a
  // frame repainting only the menu bar over a tree that had moved under it.
  if (sidebar_vm.visible) {
    const SidebarState& sidebar = project_state.sidebar;
    std::size_t selected = project_state.directory_tree.selected_index();
    switch (ActiveSidebarMode()) {
      case SidebarMode::Git: selected = sidebar.git.selected_index; break;
      case SidebarMode::Problems: selected = sidebar.problems.selected_index; break;
      case SidebarMode::Tests: selected = sidebar.tests.selected_index; break;
      case SidebarMode::Search: selected = project_state.overlay.workflow.project_search.selected_index; break;
      default:
        if (UsesPluginItemSurface(ActiveSidebarMode())) {
          selected = sidebar.plugin.selected_index;
        }
        break;
    }
    std::uint64_t key = (std::hash<std::string>{}(sidebar.view_id) * 0x9E3779B97F4A7C15ull) ^
                        (static_cast<std::uint64_t>(selected) + 1);
    key = key * 0x100000001B3ull ^ static_cast<std::uint64_t>(static_cast<std::uint32_t>(sidebar.scroll_row));
    if (ActiveSidebarMode() == SidebarMode::Tree) {
      key = key * 0x100000001B3ull ^ project_state.directory_tree.entries_revision();
    }
    key |= 1u;  // never 0: 0 means "nothing painted yet"
    if (key != frame_state_.painted_sidebar_selection && frame_state_.painted_sidebar_selection != 0) {
      RequestRedrawRect(layout.sidebar);
    }
    frame_state_.painted_sidebar_selection = key;
  } else {
    frame_state_.painted_sidebar_selection = 0;
  }
  // A tab strip painted mid-slide that is no longer animating repaints at rest.
  {
    const TabSlideState& slide = context_.interaction_state.tab_slide;
    for (const PreparedFrameState::SlideStrip& painted : frame_state_.painted_slide_strips) {
      if (painted.kind == TabDragKind::None ||
          !slide.OffsetsFor(painted.kind, painted.group_index).empty()) {
        continue;
      }
      if (const auto strip = TabStripRectForKind(painted.kind, painted.group_index); strip.has_value()) {
        RequestRedrawRect(*strip);
      }
    }
    for (std::size_t i = 0; i < slide.strips.size(); ++i) {
      frame_state_.painted_slide_strips[i] = slide.strips[i].idle()
                                                 ? PreparedFrameState::SlideStrip{}
                                                 : PreparedFrameState::SlideStrip{
                                                       .kind = slide.strips[i].kind,
                                                       .group_index = slide.strips[i].group_index};
    }
  }
  frame_state_.pointer_occluder.reset();
  // A modal owns the pointer: inside it, only its own rects hover; outside it,
  // nothing does (an empty occluder contains no rect). The TOPMOST one, in paint
  // order: a prompt opened over Settings (Go to Line from the menu) owns it, and
  // picking Settings first lit its rows up around the prompt (QA monkey).
  std::optional<SDL_FRect> modal;
  if (context_.prompts.dirty_visible) {
    modal = ComputeDirtyPromptRect(layout.full);
  } else if (context_.prompts.surface_visible) {
    modal = ComputePromptSurfaceRect(layout.full);
  } else if (settings_overlay_service_.Visible()) {
    modal = layout.editor_area;
  }
  if (modal.has_value()) {
    frame_state_.pointer_occluder =
        last_mouse_position_valid_ && Contains(*modal, last_mouse_x_, last_mouse_y_) ? *modal
                                                                                      : SDL_FRect{};
  } else if (last_mouse_position_valid_) {
    // Later pieces paint over earlier ones (a submenu over its popup), so the last
    // one under the pointer is the one it is over.
    for (const SDL_FRect& piece : CurrentChromeRects()) {
      if (Contains(piece, last_mouse_x_, last_mouse_y_)) {
        frame_state_.pointer_occluder = piece;
      }
    }
    if (!frame_state_.pointer_occluder.has_value() && project_state.overlay.visible) {
      const SDL_FRect card = ComputeOverlayRect(layout.overlay_anchor);
      if (Contains(card, last_mouse_x_, last_mouse_y_)) {
        frame_state_.pointer_occluder = card;
      }
    }
  }
  // A queued editor-hover refresh (the content under a still pointer changed)
  // runs here, not in paint: RenderClip ran it only when it painted the editor, so
  // a partial frame that skipped the editor kept the old hover card while a full
  // render of the same frame dropped it (QA monkey, seed 7).
  if (editor_hover_refresh_pending_ && last_mouse_position_valid_) {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::RefreshHover");
    UpdateEditorHover(last_mouse_x_, last_mouse_y_);
    editor_hover_refresh_pending_ = false;
  }
  // The element painted hovered and the tooltip card, as this frame paints them.
  // Pointer motion damages its own hover changes; this catches the ones nothing
  // moved -- a modal opening over a hovered status segment and its tooltip, an
  // overlay closing back onto one -- by repainting what the last frame painted and
  // what this one paints whenever they differ.
  {
    std::optional<SDL_FRect> hover_rect;
    if (last_mouse_position_valid_) {
      std::optional<SDL_FRect> rect = HoveredStatusBarSegmentRect(layout, last_mouse_x_, last_mouse_y_);
      if (!rect.has_value()) {
        rect = HoveredInteractiveRect(layout, last_mouse_x_, last_mouse_y_);
      }
      if (rect.has_value() && PointerOver(*rect)) {
        // Hover fills run a few px past the element (the status-bar segments').
        hover_rect = MakeRect(rect->x - 6.0f, rect->y - 1.0f, rect->w + 12.0f, rect->h + 2.0f);
      }
    }
    std::optional<SDL_FRect> tooltip_rect;
    std::size_t tooltip_text_hash = 0;
    if (const auto tooltip = HoveredTooltip(layout); tooltip.has_value()) {
      tooltip_rect = MakeRect(tooltip->rect.x - 1.0f, tooltip->rect.y - 1.0f, tooltip->rect.w + 2.0f,
                              tooltip->rect.h + 2.0f);
      tooltip_text_hash = std::hash<std::string_view>{}(tooltip->text);
    }
    // A card at the same place can say something else (a click reordered the tab
    // under a still pointer): QA monkey, seed 5.
    if (tooltip_text_hash != frame_state_.painted_tooltip_text_hash && tooltip_rect.has_value()) {
      RequestRedrawRect(*tooltip_rect);
    }
    frame_state_.painted_tooltip_text_hash = tooltip_text_hash;
    const auto repaint_if_moved = [this](std::optional<SDL_FRect>& painted,
                                         const std::optional<SDL_FRect>& now) {
      if (painted.has_value() == now.has_value() &&
          (!now.has_value() || RectsEqual(*painted, *now))) {
        return;
      }
      if (painted.has_value()) {
        RequestRedrawRect(*painted);
      }
      if (now.has_value()) {
        RequestRedrawRect(*now);
      }
      painted = now;
    };
    repaint_if_moved(frame_state_.painted_hover_rect, hover_rect);
    repaint_if_moved(frame_state_.painted_tooltip_rect, tooltip_rect);
    // The editor hover card goes when its target does -- the pointer leaving for
    // the tab strip cleared the blame card's target and repainted only the strip
    // (QA monkey, seed 7).
    std::optional<SDL_FRect> hover_popup_rect;
    if (const auto popup = ActiveEditorHoverPopupLayout(); popup.has_value()) {
      hover_popup_rect = MakeRect(popup->rect.x - 2.0f, popup->rect.y - 2.0f, popup->rect.w + 4.0f,
                                  popup->rect.h + 4.0f);
    }
    repaint_if_moved(frame_state_.painted_hover_popup_rect, hover_popup_rect);
  }
  // The editor (and the compare/merge editable panes) paint the caret only while
  // the editor OWNS the blinking caret -- the condition under which the blink
  // repaint names the editor (CurrentCaretDirtyRect). Drawing it in another
  // input's blink phase, with only that input's rect repainted, froze the editor
  // caret in whatever phase the last editor repaint caught.
  prepared_frame_draw_editor_caret_ =
      frame_state_.caret_visible &&
      project_state.surface.focus == FocusTarget::Editor &&
      context_.text_input.active_surface == TextInputSurface::Editor &&
      context_.text_input.composition.text.empty();
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
      *clip_cached_overlay_vm_, layout, ComputeOverlayRect(layout.overlay_anchor), text_renderer_);
  util::AddPerformanceCounter(util::PerfCounterId::RenderViewModelBuildOverlaySurfaceCalls, 1);
  clip_frame_overlay_view_models_layout_ = layout;
  clip_frame_overlay_view_models_frame_id_ = prepared_frame_id_;
  clip_frame_overlay_view_models_valid_ = true;
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
  // `layout.tab_strip` is the EDITOR column only (VS Code's tabs never cross the
  // side bar), but the chrome BAND it sits in runs the width of the window, the
  // way the menu bar and project strip above it do -- otherwise the ~290px above
  // the sidebar would be a hole in the chrome. Painting the band here and the
  // tabs inside `layout.tab_strip` is what makes the strip look the same split or
  // not (TD-2026-08-19-267).
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

  // The right (debug) pane's divider: the layout zeroes the pane when it is hidden.
  if (layout.right_pane.w > 0.0f && layout.right_pane.h > 0.0f) {
    DrawFilledRect(renderer,
                   MakeRect(layout.right_pane.x - kWorkspaceDividerThickness, layout.right_pane.y,
                            kWorkspaceDividerThickness, layout.right_pane.h),
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
        active_compare_tab->right_view_active && prepared_frame_draw_editor_caret_;
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
    const bool draw_merge_caret = prepared_frame_draw_editor_caret_;
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
                   ? editor_blame_overlay_service_.EditorOverlayForFrame(
                         prepared_frame_id_, project_state.root, text_renderer_,
                         git_blame_service_, *viewport,
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

    const EditorBannerButtonLayout buttons = ComputeEditorBannerButtonRects(strip, banner_vm.buttons);
    const float message_right = buttons.controls_left();
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
                             .hovered = PointerOver(rect),
                             .active = false,
                         });
    };
    for (std::size_t i = 0; i < buttons.buttons.size(); ++i) {
      draw_banner_button(buttons.rects[i], buttons.buttons[i].label,
                         buttons.buttons[i].destructive ? ButtonTone::Destructive
                                                        : ButtonTone::Neutral);
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
