#include "workspace/services/SidebarService.h"

#include "workspace/OpenDocumentWatch.h"
#include "workspace/render/BottomPanelHeaderPaint.h"
#include "workspace/render/BottomPanelOutputPaint.h"
#include "workspace/render/BottomPanelTerminalChromePaint.h"
#include "workspace/render/NotificationToastPaint.h"
#include "workspace/render/DebugPanePaint.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/render/CompareRowUnits.h"
#include "workspace/render/EditorRowUnits.h"
#include "workspace/render/GitSidebarPaint.h"
#include "workspace/render/MenuPopupPaint.h"
#include "workspace/render/SidebarModeRowPaint.h"
#include "workspace/render/TreeSidebarPaint.h"
#include "workspace/render/HoverCardLimits.h"
#include "workspace/render/WorkspaceShellRenderPrimitives.h"

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
// `diagnostics` are the pane's, already filtered to the minimum severity, and
// `search` the find widget's state while it draws matches on this pane (else null):
// the pane's prepared paint inputs (PreparedFrameState::EditorPanePaint).
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
  // VISUAL rows, and every marker LINE mapped into that space. The lane sits in
  // the same track as the scrollbar thumb, which is positioned by `scroll_line()`
  // -- a visual row -- so a ruler measured in logical lines drifts out of step
  // with the thumb beside it exactly when soft wrap or a collapsed fold makes the
  // two spaces differ, putting a diagnostic's mark at a fraction of the lane the
  // thumb never reaches at that scroll position. Off the wrapped/folded path
  // VisualRowForLine is the identity, so this is the same number it always was.
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
    // Diagnostics stay visible while the buffer is dirty: LspService slides their
    // positions through each edit so they track the text until the server
    // republishes. Only a pathless (unsaved scratch) buffer has none.
    {
      {
        const std::span<const editor::PublishedDiagnostic> filtered = diagnostics;
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
  // Through the primitive the paint-overrun recorder sees (clip-discipline D5).
  render::FillRect(renderer, rect, color);
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
  // The frame being prepared has its id from the start: what prep builds once per
  // frame (the blame overlay, the menu's LSP readiness) is keyed on it.
  ++prepared_frame_id_;

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
  // A pane whose document another pane edited keeps the caret set it had before
  // that edit, which can name lines the document no longer has; each pane's
  // carets come back into its document here, before anything below reads them.
  // Done after the status bar had composed "Ln N, Col M" from the stale caret, the
  // frame drew a position the caret was no longer at and asked for no other, so
  // the bar disagreed with the caret until an unrelated repaint (soak
  // 5a2cb8ba7f6b: a tab re-activated in one group after its file was shortened
  // from another).
  MakeEditorTabService().ReconcileActiveCaretsWithDocuments();
  // Every caret move, jump and tab switch ends in a frame, so this is where the
  // Go Back / Go Forward history sees them -- one place instead of a hook in each
  // of the dozens of ways the caret can move.
  MakeEditorTabService().ObserveNavigationLocation();
  // (A compare's or merge's shared scroll is in its rows' surface key, folded
  // after the scroll is synced and clamped below: a frame that finds it moved
  // repaints that surface's units, whatever moved it.)
  text_renderer_.EnsureInitialized(renderer, presentation_scale_x_, presentation_scale_y_);
  editor_text_renderer_.EnsureInitialized(renderer, presentation_scale_x_, presentation_scale_y_);
  // The terminal renderer is only needed (and only pays for its glyph atlas) once a
  // terminal is actually shown; initialize + apply its font lazily at that point.
  if (BottomPanelShowsTerminal()) {
    terminal_text_renderer_.EnsureInitialized(renderer, presentation_scale_x_,
                                              presentation_scale_y_);
    ApplyTerminalFontPreferences();
  }
  window_presentation_.logical_width = width;
  window_presentation_.logical_height = height;
  // Two terminal-panel rules that read focus and can MOVE it: a maximized panel
  // restores itself the moment another surface takes the keyboard, or takes the
  // keyboard from a surface it hides; and the tab on screen has no unseen output.
  // First of all, before the layout is resolved, the text-input surface synced
  // and the view models built -- each reads focus. Run after the view models (as
  // they were), a focus move painted the frame with the old focus while the
  // focus-ring tracking below recorded the new one, so the panel's ring was never
  // repainted (QA monkey, seed 601).
  {
    TerminalPanelService& terminal_panel = MakeTerminalPanelService();
    terminal_panel.SyncPanelMaximizedWithFocus();
    terminal_panel.NoteActiveTabShown();
  }
  // Before any view model is built: they read the active text-input surface, and
  // a frame that synced it AFTER building them painted the surface focus had
  // just left -- the first frame of a freshly opened palette had no caret.
  SDL_Window* render_window = SDL_GetRenderWindow(renderer);
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::SyncTextInputSurface");
    MakeTextInputCoordinator().SyncTextInputSurface(render_window);
  }
  // The search view's hits follow the active buffer whatever edited it: the
  // announced edit paths ask after each edit, and this catches the rest (a
  // reload, a language server's edit). A pointer and a counter when nothing moved.
  // Before the view models are built: the sidebar's carries the "N matches" line,
  // and catching up after it painted the old count in the very frame that
  // repainted the sidebar for the change (QA monkey seed 17).
  if (const editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
    (void)MakeSidebarService().CatchUpSearchResults(*viewport);
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
  // Every open document is watched, wherever it is: a fingerprint per frame, the
  // watch set rebuilt only when the open set moved.
  SyncOpenDocumentWatch(project_state, file_index_watcher_.get());
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
    // A refresh whose read was cancelled (a project closed or switched under it).
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
  // The stored pane sizes are the user's PREFERRED sizes; `ComputeLayout` clamps
  // them to the window it lays out, every time. Writing the clamped value back
  // here (as frame prep did) meant a window shrunk to 320 px and grown again kept
  // a 41-px sidebar and debug pane for good -- the preference was lost to one
  // small frame (QaFlowRegression/EveryElementStaysInsideTheWindowAtEverySize).
  // Everything that needs the size on screen reads the layout.

  // One layout, one memo: this is the same `CurrentWorkspaceLayout()` every hit
  // test reads, so a frame can never paint a geometry the input paths disagree
  // with. The window size was written into `window_presentation_` above, so the
  // key sees this frame's size. A miss here is the only place the resolved
  // layout mode is committed back to the service (its hysteresis input).
  const std::uint64_t layout_generation_before = layout_memo_generation_;
  WorkspaceLayout layout;
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::ComputeLayout");
    layout = *CurrentWorkspaceLayout();
  }
  const bool workspace_layout_recomputed = layout_memo_generation_ != layout_generation_before;
  if (workspace_layout_recomputed) {
    ++prepare_frame_layout_compute_count_;
  }
  // Record the resolved mode whoever recomputed the layout: a hit test or a ui
  // query often resolves it first after a resize, and recording it only on this
  // frame's own recompute left a window grown back from compact folded for good
  // (the stale mode is the resolver's `previous_mode`, which the memo keys on).
  if (layout.layout_mode != layout_mode_service_.CurrentMode()) {
    layout_mode_service_.SetCurrentMode(layout.layout_mode);
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
    if (frame_state_.focus_autosave_armed && frame_state_.focused_editor != nullptr &&
        focused_editor != frame_state_.focused_editor) {
      MaybeAutosaveDirtyTabs(/*on_focus_change=*/true);
    }
    frame_state_.focused_editor = focused_editor;
  }
  // The window's regions as painted. A region that appears, vanishes or moves
  // repaints where it was and where it is, whatever moved it: a plugin clearing
  // its panel surface hid the bottom panel and asked only for the editor, and
  // the sidebar's newly exposed bottom kept the panel's pixels (QA plugins-ui).
  // The chrome units (DerivedSurface::Chrome): pushed in a fixed order through
  // this function, committed at its end.
  std::vector<DerivedDamage::Unit>& chrome = frame_state_.derived_damage.Begin(DerivedSurface::Chrome);
  const auto chrome_unit = [&chrome](const std::optional<SDL_FRect>& rect, std::uint64_t key) {
    chrome.push_back({rect.value_or(SDL_FRect{}), key});
  };
  for (const SDL_FRect& region : {layout.sidebar, layout.bottom_panel, layout.editor_surface, layout.right_pane,
                                  layout.status_bar, layout.tab_strip}) {
    chrome_unit(region, 0);  // a region that appears, vanishes or moves: rect-only
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
    panel_out.find_query = &terminal_find_service_.query();
    panel_out.find_matches = &terminal_find_service_.matches();
    panel_out.find_selected_index = terminal_find_service_.selected_index();
    OverlayFindWidgetViewModel& find_vm = panel_out.find;
    find_vm.fw = ComputeTerminalFindWidgetLayout(ActiveTerminalBodyRect(layout), layout);
    // A panel body too short for the bar lays out none of it (see the helper):
    // nothing to paint, and the QA element source reads this flag too.
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
    // The find widget stays open across a tab switch; its matches and n-of-m
    // must describe the buffer now in front, not the one it was opened over.
    RefreshBufferSearchIfStale();
    ClampOverlayScrollRow(ComputeOverlayRect(layout.overlay_anchor));
  }
  // 2a) A signature-help popup whose caret moved on is dropped here, not in paint:
  //     paint ran it only when a clip touched the editor surface, so whether the
  //     popup outlived its caret depended on which clips a frame painted.
  MaybeExpireSignatureHelp();
  // 2b) The visible syntax tokens of every compare or merge tab in front of a
  //     pane, laid out in that pane, so paint only reads them (see
  //     PopulateCompareSyntaxTokensForWindow). A split shows one per pane.
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
  // 3) Commit-draft body viewport sizing + caret-keep-visible scroll clamp (the
  //    083 residual): moved out of RenderCommitBodyField so paint stays pure.
  if (sidebar_vm.visible && sidebar_vm.mode == SidebarMode::Git) {
    PrepareCommitBodyViewportForFrame(layout.sidebar);
  }
  // 4) The window title follows the focused group's active tab, its dirty mark
  //    and the project. Hashed every frame, composed only when an input moved.
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
    chrome_unit(layout.menu_bar, inputs);  // the custom title bar draws the title
  }
  frame_state_.caret_visible = CaretVisibleNow();
  // The editor (and the compare/merge editable panes) paint the caret only while
  // the editor OWNS the blinking caret -- the condition under which the blink
  // wake asks the editor for a frame (CurrentCaretDirtyRect). Drawing it in another
  // input's blink phase, with only that input's rect repainted, froze the editor
  // caret in whatever phase the last editor repaint caught. Decided here, once,
  // before the panes' units: the rows holding a caret key on it.
  prepared_frame_draw_editor_caret_ =
      frame_state_.caret_visible &&
      project_state.surface.focus == FocusTarget::Editor &&
      context_.text_input.active_surface == TextInputSurface::Editor &&
      context_.text_input.composition.text.empty();
  frame_state_.terminal_owns_caret = project_state.surface.focus == FocusTarget::Panel &&
                                     context_.text_input.active_surface == TextInputSurface::Terminal;
  // Ownership changes the terminal caret's style (steady outline vs blinking block)
  // at the same place, which the caret-region comparison below cannot see.
  chrome_unit(ActiveTerminalCaretRect(layout), frame_state_.terminal_owns_caret ? 1u : 2u);
  // Whatever moves the caret away -- focus to another surface, a menu or prompt
  // that suspends the blink, the blink's own phase -- this frame repaints both the
  // caret region the last frame painted and the one it paints now. Each such
  // transition used to need its own damage request, and several had none: a menu
  // opened over a focused terminal left its caret on screen (QA redraw verifier).
  chrome_unit(frame_state_.caret_visible ? CurrentCaretDirtyRect() : std::nullopt, 0);
  // Each editor pane's metrics and viewport size, and the active pane's inline
  // blame overlay -- here, before anything below (or between frames) resolves a
  // point against the pane. A size change can clamp a pane's scroll, which the
  // scroll fingerprints below then see.
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
      // Ctrl+Tab's most-recently-used order, from whatever made a tab active.
      group.NoteActiveTabUse();
      editor::TextViewport* viewport = GroupActiveViewport(group);
      if (viewport == nullptr) {
        continue;
      }
      const editor::FoldingModel* folds = fold_enabled ? GroupFoldingModelPtr(group) : nullptr;
      const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::SizeViewportWithStickyBand(
          editor_text_renderer_, *viewport, pane.rect, line_numbers, [&](const editor::TextViewport& view) {
            return RenderViewModelBuilder::StickyScrollLines(view, folds, sticky_enabled && folds != nullptr,
                                                             sticky_depth)
                .size();
          });
      const std::size_t sticky_rows = metrics.sticky_scroll_rows;
      // A scroll the sizing settled elsewhere than the input left it (see
      // SizeViewportWithStickyBand) repaints the pane through its row units: the
      // scroll is in the pane key (derived-damage 2.4).
      frame_state_.pane_metrics[pane.group_index] = metrics;
      frame_state_.pane_metrics_valid[pane.group_index] = true;
      // The overlay the active pane paints, its hover card resolves against and a
      // click on Copy SHA hits: built once, here (a compare or merge surface sets
      // its own while it paints).
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
  // The focused pane's bracket-match pair, which its rows key on (EditorRowUnits).
  // Asked through the renderer's cache, which the pane's paint then hits.
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
  // The focused pane's occurrence highlights, one (line, hash) per line, which its
  // rows key on (EditorRowUnits). Asked here first; the view-model build reads the
  // same caches.
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
      // One entry per line: its ranges' hashes folded together.
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
  // Each editor pane's rows as derived units (derived-damage 2.2): a row whose
  // keys moved repaints, whatever moved them. Added to the editor's hand-written
  // damage for now -- syntax colours are not in the keys yet (EditorRowUnits.h).
  {
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
    const bool find_shown = project_state.overlay.visible &&
                            (project_state.overlay.mode == OverlayMode::BufferSearch ||
                             project_state.overlay.mode == OverlayMode::BufferReplace);
    std::array<bool, kMaxEditorGroups> committed{};
    // Every editor pane's paint inputs, built here once (derived-damage 2.3):
    // PaintEditorPane draws from them and nothing else, and the rows key on them.
    for (PreparedFrameState::EditorPanePaint& paint : frame_state_.editor_pane_paint) {
      paint.valid = false;
    }
    const bool fold_enabled = setting("editor.fold.enabled", true);
    const bool sticky_enabled = setting("editor.fold.sticky_scroll.enabled", true);
    const int sticky_depth =
        ParseStickyScrollMaxDepthSetting(GetSettingValue("editor.fold.sticky_scroll.max_depth"));
    const editor::DiagnosticSeverity min_severity =
        editor::ParseDiagnosticSeverity(GetSettingValue("diagnostics.min_severity").value_or("hint"));
    const editor::InsetGapFeatureFlags inset_flags =
        insets_possible ? editor::InsetGapFeatureFlags{.inline_surfaces = setting("plugins.inline_surfaces", false),
                                                       .code_lens_above = setting("plugins.code_lens_above", false),
                                                       .ghost_text = setting("plugins.ghost_text", false)}
                        : editor::InsetGapFeatureFlags{};
    const RenderViewModelBuilder pane_builder(context_);
    const auto fill_pane_paint = [&](std::size_t gi, const EditorPaneLayout& pane, EditorGroup& group,
                                     editor::TextViewport& viewport, bool focused) {
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
      if (pane.active && context_.interaction_state.text_dragging() &&
          context_.interaction_state.text_drag_has_drop) {
        paint.drop_indicator = editor::TextPosition{.line = context_.interaction_state.text_drag_drop_line,
                                                    .column = context_.interaction_state.text_drag_drop_column};
      }
      const bool debug_on = DebugEnabled();
      if (viewport.is_placeholder()) {
        // An empty group's home surface (recents and shortcuts), memoized by the builder.
        pane_builder.BuildEditorViewModelInto(paint.view_model, viewport, paint.metrics.visible_rows, paint.folding,
                                              setting("editor.occurrences.enabled", true),
                                              setting("editor.search.case_sensitive", false),
                                              fold_enabled && sticky_enabled && paint.folding != nullptr, sticky_depth,
                                              paint.render_whitespace, debug_on,
                                              debug_on ? &project_state.breakpoint_store : nullptr,
                                              debug_on ? &project_state.debug_execution : nullptr);
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
      // The model's views into the builder's one cache, kept per pane (see
      // EditorPanePaint): the next pane's build rewrites what they view.
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
      paint.diagnostics_revision = project_state.diagnostics_store.revision();
      // The find widget's highlights, on the focused pane while it is the overlay:
      // a regex or a scoped find draws its precomputed match set, a literal one its
      // query (EditorViewRenderer scans each row for it).
      const bool find_here = pane.active && find_shown;
      const bool explicit_matches = find_here && (buffer_search.regex || buffer_search.scope.has_value());
      paint.explicit_search_matches =
          explicit_matches
              ? BufferSearchHighlightFragments(viewport, buffer_search.matches, buffer_search.matches_revision)
              : std::span<const editor::SelectionRange>{};
      paint.search_query.assign(find_here && !explicit_matches ? buffer_search.query.text() : std::string_view{});
      paint.active_search_match = pane.active ? ActiveBufferSearchMatch() : std::nullopt;
      paint.find_scope = find_here && buffer_search.scope.has_value()
                             ? std::optional<editor::SelectionRange>(buffer_search.scope->range)
                             : std::nullopt;
      paint.ruler_search = find_here ? &buffer_search : nullptr;
      (void)focused;
    };
    // A diff surface's paint inputs beyond its tab (DiffSurfacePaint), and the hash
    // its surface key folds of them.
    const auto fill_diff_paint = [&](std::size_t gi, const SDL_FRect& area, bool focused, DragTarget vertical,
                                     DragTarget horizontal) -> DiffSurfacePaint& {
      DiffSurfacePaint& paint = frame_state_.diff_surface_paint[gi];
      paint.render_whitespace = setting("editor.view.render_whitespace", false);
      paint.indent_guides = setting("editor.view.indent_guides.enabled", true);
      paint.bracket_match = setting("editor.brackets.match_highlight.enabled", true);
      paint.line_numbers = setting("editor.line_numbers", true);
      const DragTarget drag = context_.interaction_state.drag_target;
      paint.vertical_bar_dragged = focused && drag == vertical;
      paint.horizontal_bar_dragged = focused && drag == horizontal;
      paint.left_divider_dragged = focused && drag == DragTarget::MergeLeftDivider;
      paint.right_divider_dragged = focused && drag == DragTarget::MergeRightDivider;
      paint.pointer = Contains(area, last_mouse_x_, last_mouse_y_)
                          ? std::optional<SDL_FPoint>(SDL_FPoint{last_mouse_x_, last_mouse_y_})
                          : std::nullopt;
      paint.blame.reset();
      paint.diagnostics = {};
      paint.decorations = nullptr;
      return paint;
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
      committed[gi] = true;
      // A compare tab in front: its rows are the group's units instead (derived-
      // damage 2.5). Laid out as its paint lays it out -- in the group's surface,
      // with the scroll clamped here first, so the keys read what paint draws.
      if (diff_in_front && group.active_tab().compare.has_value()) {
        CompareTabState& compare = *group.active_tab().compare;
        const SDL_FRect area = ComputeEditorGroupRectsForState(layout).groups[gi].editor_surface;
        const CompareSurfaceLayout compare_surface = ComputeCompareSurfaceLayout(area, compare);
        ClampCompareScrollRow(compare, compare_surface.visible_rows);
        ClampCompareHorizontalScroll(compare, compare_surface.visible_columns);
        const bool focused = gi == focused_group;
        DiffSurfacePaint& diff_paint = fill_diff_paint(gi, area, focused, DragTarget::CompareVerticalScrollbar,
                                                       DragTarget::CompareHorizontalScrollbar);
        // The focused diff's inline blame on its working side, built here once: the
        // hover card and Copy SHA resolve against the same overlay.
        if (focused) {
          if (compare.right_editable && compare.right_view_active) {
            diff_paint.blame = editor_blame_overlay_service_.BuildCompareOverlay(
                project_state.root, editor_text_renderer_, git_blame_service_, compare,
                EditorBlameOverlayService::CompareOverlayLayout{
                    .pane_rect = MakeRect(compare_surface.right_x, area.y,
                                          compare_surface.gutter_width + compare_surface.right_width,
                                          std::max(0.0f, area.h - (compare_surface.show_horizontal ? 12.0f : 0.0f))),
                    .right_x = compare_surface.right_x,
                    .gutter_width = compare_surface.gutter_width,
                    .right_width = compare_surface.right_width,
                    .rows_y = compare_surface.rows_y,
                    .line_height = compare_surface.line_height,
                    .visible_rows = compare_surface.visible_rows,
                    .visible_columns = compare_surface.visible_columns,
                },
                [&compare](std::size_t line_index, std::size_t line_visual_columns)
                    -> std::optional<EditorBlameOverlayService::CompareBlameAnchor> {
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
        // The header above the rows: the review summary line and the two sides' labels.
        {
          PaintFingerprint header;
          header.Add(surface_key.value())
              .Add(std::string_view(compare.review_header.summary_line))
              .Add(std::string_view(compare.left_label))
              .Add(std::string_view(compare.right_label));
          // The whole width: the header runs above the scrollbar's reserve too.
          units.push_back({SDL_FRect{area.x, area.y, area.w, std::max(0.0f, compare_surface.rows_y - 1.0f - area.y)},
                           header.value()});
        }
        // The working side's diagnostics and decorations, shown while its buffer is
        // clean (a decoration published against the saved file's lines would land
        // on the wrong ones in an edited buffer, TD-2026-08-13-206; the left side
        // is another revision and shows none). Resolved here: paint asks no store.
        if (compare.right_editable && !compare.right_viewport.path().empty() && !compare.right_viewport.dirty()) {
          if (const auto* published =
                  project_state.diagnostics_store.FindByPathKey(compare.right_viewport.path_key())) {
            diff_paint.diagnostics = *published;
          }
          if (const auto* presentation = project_state.plugin_presentation_if_present()) {
            diff_paint.decorations = presentation->decorations.FindByPathKey(compare.right_viewport.path_key());
          }
        }
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
        frame_state_.derived_damage.Commit(surface, [this](const SDL_FRect& rect) { RequestRedrawRect(rect); });
        continue;
      }
      // A merge tab in front: its two source columns' rows and its result pane's
      // rows (an editor viewport: the editor's row units) are the group's units.
      // The scroll is synced and clamped and the result viewport sized here, as its
      // paint does, so the keys read what paint draws (paint repeats it, unchanged).
      if (diff_in_front && group.active_tab().merge.has_value()) {
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
        DiffSurfacePaint& diff_paint = fill_diff_paint(gi, area, focused, DragTarget::CompareVerticalScrollbar,
                                                       DragTarget::CompareHorizontalScrollbar);
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
        // Where the conflicts are: their boxes span rows, so a range that moves
        // repaints the surface. What each one shows -- its choice, whether it is
        // resolved, selected or hovered -- is in the rows it covers (the source
        // columns' keys, and the result rows' extra line keys below).
        for (const MergeTrackedConflict& conflict : merge.conflicts) {
          surface_key.Add(conflict.incoming_start_line)
              .Add(conflict.incoming_end_line)
              .Add(conflict.current_start_line)
              .Add(conflict.current_end_line)
              .Add(conflict.start_line)
              .Add(conflict.end_line);
        }
        surface_key.Add(diff_paint.left_divider_dragged).Add(diff_paint.right_divider_dragged);
        // The hover: a conflict's accept buttons and its preview stay in view,
        // pinned to the pane's edge when the conflict is scrolled out -- over any
        // row -- so a hover that comes, goes or moves repaints the surface.
        if (merge.hover_state.has_value()) {
          surface_key.Add(merge.hover_state->kind)
              .Add(merge.hover_state->conflict_index)
              .Add(merge.hover_state->preview_choice);
        }

        const float right_reserved = merge_surface.show_vertical ? kWorkspaceDiffScrollbarReserve : 0.0f;
        // The toolbar, status line, base band and pane labels above the rows.
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
              .Add(merge.selected_hunk)  // the status names the selected conflict
              .Add(CountRemainingMergeConflicts(merge.conflicts));
          // The toolbar's buttons light under the pointer: where it is in the band.
          if (diff_paint.pointer.has_value() && diff_paint.pointer->y < merge_surface.rows_y) {
            header.Add(diff_paint.pointer->x).Add(diff_paint.pointer->y);
          }
          if (merge.base_pane_visible) {
            for (std::size_t row = 0; row < merge.model.base_lines.size() && row < 4; ++row) {
              header.Add(std::string_view(merge.model.base_lines[row]));
            }
          }
          // The whole width: the header runs above the scrollbar's reserve too.
          units.push_back({SDL_FRect{area.x, area.y, area.w, std::max(0.0f, merge_surface.rows_y - 1.0f - area.y)},
                           header.value()});
        }
        AppendMergeSideRowUnits(merge,
                                CompareRowGeometry{.surface = area,
                                                   .rows_y = merge_surface.rows_y,
                                                   .line_height = merge_surface.line_height,
                                                   .content_width = std::max(0.0f, area.w - right_reserved),
                                                   .visible_rows = merge_surface.visible_rows},
                                surface_key.value(), units);
        const std::span<const editor::PublishedDiagnostic> result_diagnostics = diff_paint.diagnostics;
        // Each result line inside a conflict: which conflict, its choice and state,
        // and whether it is the selected one.
        thread_local std::vector<std::pair<std::size_t, std::uint64_t>> conflict_lines;
        conflict_lines.clear();
        const std::size_t selected =
            merge.conflicts.empty() ? 0 : std::min(merge.selected_hunk, merge.conflicts.size() - 1);
        for (std::size_t i = 0; i < merge.conflicts.size(); ++i) {
          const MergeTrackedConflict& conflict = merge.conflicts[i];
          PaintFingerprint state;
          state.Add(i).Add(conflict.last_choice).Add(conflict.valid).Add(conflict.resolved).Add(i == selected);
          for (std::size_t line = conflict.start_line; line < std::max(conflict.end_line, conflict.start_line + 1);
               ++line) {
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
                                                // The pair at the result's caret, which its
                                                // paint matches and lights whichever pane
                                                // has the keyboard: the far bracket sits on
                                                // a row the caret never touches.
                                                .bracket_pair = diff_paint.bracket_match
                                                                    ? editor_view_renderer_.BracketMatch(merge.result_viewport)
                                                                    : std::nullopt,
                                                .blame = diff_paint.blame.has_value() ? &*diff_paint.blame : nullptr,
                                                .extra_line_keys = conflict_lines},
                             frame_state_.editor_row_keys, units);
        frame_state_.derived_damage.Commit(surface, [this](const SDL_FRect& rect) { RequestRedrawRect(rect); });
        continue;
      }
      if (diff_in_front || viewport == nullptr) {
        frame_state_.derived_damage.Commit(surface, [this](const SDL_FRect& rect) { RequestRedrawRect(rect); });
        continue;
      }
      const bool focused = gi == focused_group;
      fill_pane_paint(gi, pane, group, *viewport, focused);
      if (viewport->is_placeholder()) {
        // The home surface repaints whole when anything it shows moves: the pane,
        // its size, the recents list (the builder's memo, a new object per change).
        PaintFingerprint welcome;
        welcome.Add(pane.rect).Add(reinterpret_cast<std::uintptr_t>(frame_state_.editor_pane_paint[gi].welcome))
            .Add(settings.value());
        units.push_back({pane.rect, welcome.value()});
        frame_state_.derived_damage.Commit(surface, [this](const SDL_FRect& rect) { RequestRedrawRect(rect); });
        continue;
      }
      const PreparedFrameState::EditorPanePaint& paint = frame_state_.editor_pane_paint[gi];
      const editor::EditorViewMetrics& metrics = frame_state_.pane_metrics[gi];
      const editor::FoldingModel* folds =
          setting("editor.fold.enabled", true) ? GroupFoldingModelPtr(group) : nullptr;
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
          .Add(viewport->indent_width())  // the indent guides' pitch
          .Add(viewport->layout_shape_revision())
          .Add(viewport->syntax_revision())
          .Add(std::string_view(viewport->language_id()))
          // Not the fold model's revision: an edit recomputes the folds and moves
          // it, which would repaint every row per keystroke. Each row folds its
          // own fold mark, and a collapse moves the rows' lines.
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
      // What the pane's view model adds beyond each row's own line: the sticky band's
      // lines, inset gaps and their contents, the ghost-text tail, the execution line,
      // breakpoint marks; and the drop indicator of a text drag.
      {
        const editor::EditorViewModel& vm = paint.view_model;
        pane_key.Add(vm.sticky_lines.size());
        for (const std::size_t line : vm.sticky_lines) {
          pane_key.Add(line).Add(viewport->lines().LineView(std::min(line, viewport->line_count() - 1)));
        }
        pane_key.Add(vm.row_gaps.size()).Add(vm.row_gap_contents.size()).Add(vm.code_lens_above);
        for (const editor::RowGap& gap : vm.row_gaps) {
          pane_key.Add(gap.visual_row).Add(gap.height).Add(static_cast<int>(gap.placement));
        }
        // And what each gap shows (soak f454f0b1d9b2): a code lens republished at
        // the same height -- "greet (0)" becoming "greet (1)" -- or a surface or the
        // ghost-text block that changed in place moves no gap.
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
          // Gaps this frame lays out, not gaps a plugin might one day publish: any
          // plugin presentation at all (a panel surface, the language server's
          // semantic tokens or inlay hints) made the pane one unit, and a keystroke
          // repainted all of it.
          .rows_have_gaps = !paint.view_model.row_gaps.empty(),
          .sticky_lines = paint.view_model.sticky_lines,
          .caret_drawn = paint.active && prepared_frame_draw_editor_caret_,
          .diagnostics = diagnostics,
          .occurrence_lines = focused && frame_state_.occurrence_viewport == viewport
                                  ? std::span<const std::pair<std::size_t, std::uint64_t>>(
                                        frame_state_.occurrence_lines)
                                  : std::span<const std::pair<std::size_t, std::uint64_t>>{},
          .bracket_pair = focused && frame_state_.bracket_viewport == viewport
                              ? frame_state_.bracket_pair
                              : std::nullopt,
          .folds = folds,
          .active_search_match = focused ? ActiveBufferSearchMatch() : std::nullopt,
          .blame = paint.blame,
      };
      AppendEditorRowUnits(inputs, frame_state_.editor_row_keys, units);
      // The scrollbars, and the overview ruler in the vertical one's track: what
      // they draw is the pane's extent and scroll, and the ruler's marks -- every
      // diagnostic and find match in the file, and the caret's line -- none of
      // which a row shows (QaFlowRegression/EditorRowDamageFollowsEveryStateTransition:
      // a diagnostic far off screen marked the ruler).
      {
        const ScrollSurfaceLayout scroll = ComputeEditorScrollLayout(pane.rect, *viewport, metrics);
        if (scroll.vertical_scrollbar.has_value()) {
          const SDL_FRect& track = scroll.vertical_scrollbar->track;
          const SDL_FRect lane = overview::LaneRect(track, pane.rect.x);
          const float left = std::min(track.x, lane.x);
          const float right = std::max(track.x + track.w, lane.x + lane.w);
          // The track and the lane's marks: keyed on what moves a mark or the thumb,
          // not on the caret (below) or the buffer's dirty flag (diagnostics stay
          // shown while it is dirty). A tall strip repaints every row it crosses, so
          // this must not move per keystroke
          // (QaFlowRegression/TypingRepaintsOnlyTheLinesWhoseOccurrencesChanged).
          const bool ruler = setting("editor.view.overview_ruler.enabled", true);
          PaintFingerprint bar;
          bar.Add(pane_key.value())
              .Add(track)
              .Add(scroll.vertical_scrollbar->thumb)
              .Add(pane.active && context_.interaction_state.drag_target == DragTarget::EditorVerticalScrollbar)
              .Add(ruler)
              .Add(project_state.diagnostics_store.revision())
              .Add(viewport->visual_line_count())
              .Add(overview::ThemeMarkerToken(theme_));
          if (focused && find_shown) {
            bar.Add(buffer_search.matches_revision).Add(buffer_search.selected_index).Add(buffer_search.matches.size());
          }
          units.push_back({SDL_FRect{left, track.y, right - left, track.h}, bar.value()});
          // The caret's mark on the lane, a unit of its own: a caret move repaints the
          // mark it left and the one it made.
          overview::Marker caret_mark;
          const int caret_row = static_cast<int>(viewport->cursor_visual_row());
          if (ruler && overview::BuildMarker(overview::LaneInnerRect(lane), viewport->visual_line_count(),
                                             overview::MarkerInput{.start_row = caret_row,
                                                                   .end_row = caret_row + 1,
                                                                   .color = theme_.cursor,
                                                                   .priority = 0},
                                             caret_mark)) {
            units.push_back({caret_mark.rect, bar.value()});
          } else {
            units.push_back({SDL_FRect{}, bar.value()});  // keeps the units after it at their indices
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
      frame_state_.derived_damage.Commit(surface, [this](const SDL_FRect& rect) { RequestRedrawRect(rect); });
    }
    // A pane that went away repaints where its rows were.
    for (std::size_t gi = 0; gi < kMaxEditorGroups; ++gi) {
      if (!committed[gi]) {
        const auto surface = static_cast<DerivedSurface>(static_cast<std::size_t>(DerivedSurface::EditorPane0) + gi);
        frame_state_.derived_damage.Begin(surface);
        frame_state_.derived_damage.Commit(surface, [this](const SDL_FRect& rect) { RequestRedrawRect(rect); });
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
    // The editor's gutter dots are in its panes' keys (EditorRowUnits); the
    // Breakpoints pane's rows are the debug pane, while it shows.
    chrome_unit(project_state.debug_pane.visible ? std::optional<SDL_FRect>(layout.right_pane) : std::nullopt,
                breakpoints);
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
    // Each backdrop dims the whole window.
    chrome_unit(CurrentWindowRect(), backdrops);
    // (Whether the focused pane paints the find widget's matches is in its pane key.)
  }
  // The prompts' dialogs, keyed on what they say: the unsaved-changes prompt names
  // the tab it closes, looked up when it paints, so a focus that moved under it
  // renamed it with nothing damaged, and the message kept the old name outside
  // whatever else was repainted (soak 6d33d9fa57bc).
  {
    const std::hash<std::string_view> text_hash;
    chrome_unit(context_.prompts.dirty_visible ? std::optional<SDL_FRect>(ComputeDirtyPromptRect(layout.full))
                                               : std::nullopt,
                context_.prompts.dirty_visible
                    ? text_hash(DirtyPromptMessage()) ^
                          static_cast<std::uint64_t>(context_.prompts.dirty.selected_action)
                    : 0);
    chrome_unit(context_.prompts.surface_visible
                    ? std::optional<SDL_FRect>(ComputePromptSurfaceRect(layout.full))
                    : std::nullopt,
                context_.prompts.surface_visible
                    ? (text_hash(PromptSurfaceMessage()) * 0x9E3779B97F4A7C15ull) ^ text_hash(PromptSurfaceDetail()) ^
                          static_cast<std::uint64_t>(context_.prompts.surface.selected_button)
                    : 0);
  }
  // (A compare tab's scroll is in its rows' surface key: CompareRowUnits.)
  {
    PreparedFrameState::DiffSurfaceBars bars;
    // The lane's markers are built here, against the lane this frame paints, so
    // paint only draws them and a change in them is damage this frame knows of.
    const auto take = [&](const void* tab, const ScrollSurfaceLayout& scroll, const auto& build_markers) {
      bars.tab = tab;
      if (scroll.horizontal_scrollbar.has_value()) {
        bars.horizontal_track = scroll.horizontal_scrollbar->track;
        bars.horizontal_thumb = scroll.horizontal_scrollbar->thumb;
      }
      if (scroll.vertical_scrollbar.has_value()) {
        bars.vertical_lane = overview::LaneRect(scroll.vertical_scrollbar->track, ActiveTabSurface(layout).x);
        bars.vertical_track = scroll.vertical_scrollbar->track;
        bars.vertical_thumb = scroll.vertical_scrollbar->thumb;
        const std::vector<overview::Marker>& markers = build_markers(overview::LaneInnerRect(*bars.vertical_lane));
        std::uint64_t hash = util::kFnv1aOffsetBasis;
        for (const overview::Marker& marker : markers) {
          hash = util::Fnv1aBytes(hash, std::string_view(reinterpret_cast<const char*>(&marker.rect), sizeof marker.rect));
          hash = util::Fnv1aBytes(hash, std::string_view(reinterpret_cast<const char*>(&marker.color), sizeof marker.color));
        }
        bars.vertical_markers = hash;
      }
    };
    const SDL_FRect view_surface = ActiveTabSurface(layout);
    const std::uint64_t theme_token = overview::ThemeMarkerToken(theme_);
    if (CompareTabState* compare = ActiveCompareTab(); compare != nullptr) {
      const CompareSurfaceLayout surface = ComputeCompareSurfaceLayout(view_surface, *compare);
      take(compare, ComputeCompareScrollLayout(view_surface, surface, *compare),
           [&](const SDL_FRect& inner_lane) -> const std::vector<overview::Marker>& {
             EnsureCompareOverviewMarkers(theme_, inner_lane, theme_token, *compare);
             return compare->scrollbar_marker_cache;
           });
    } else if (MergeTabState* merge = ActiveMergeTab(); merge != nullptr) {
      const MergeSurfaceLayout surface = ComputeMergeSurfaceLayout(view_surface, *merge);
      take(merge, ComputeMergeScrollLayout(view_surface, surface, *merge),
           [&](const SDL_FRect& inner_lane) -> const std::vector<overview::Marker>& {
             EnsureMergeOverviewMarkers(theme_, inner_lane, MergeTotalVisualRowCount(*merge), theme_token, *merge);
             return merge->scrollbar_marker_cache;
           });
    }
    // The focused diff's bars as units: the horizontal track (its thumb), and the
    // vertical lane with its track (the thumb and the lane's markers). A bar that
    // shows or hides moves the surface's own layout, which its rows key on.
    PaintFingerprint horizontal;
    horizontal.Add(reinterpret_cast<std::uintptr_t>(bars.tab)).Add(bars.horizontal_thumb);
    chrome_unit(bars.horizontal_track, horizontal.value());
    std::optional<SDL_FRect> vertical;
    if (bars.vertical_lane.has_value()) {
      const float left = std::min(bars.vertical_lane->x, bars.vertical_track.x);
      const float right = std::max(bars.vertical_lane->x + bars.vertical_lane->w,
                                   bars.vertical_track.x + bars.vertical_track.w);
      vertical = SDL_FRect{left, bars.vertical_track.y, right - left, bars.vertical_track.h};
    }
    PaintFingerprint vertical_key;
    vertical_key.Add(reinterpret_cast<std::uintptr_t>(bars.tab)).Add(bars.vertical_thumb).Add(bars.vertical_markers);
    chrome_unit(vertical, vertical_key.value());
  }
  {
    std::uint64_t dividers = 0;
    for (const EditorSplitDividerRect& divider : ComputeEditorGroupRectsForState(layout).dividers) {
      for (const float edge : {divider.rect.x, divider.rect.y, divider.rect.w, divider.rect.h}) {
        dividers = dividers * 0x100000001B3ull ^ static_cast<std::uint64_t>(std::lround(edge * 4.0f));
      }
    }
    // The whole column: each pane's tab strip sits above `editor_area`.
    const float top = layout.tab_strip.h > 0.0f ? layout.tab_strip.y : layout.editor_area.y;
    chrome_unit(MakeRect(layout.editor_area.x, top, layout.editor_area.w,
                         layout.editor_area.y + layout.editor_area.h - top),
                dividers);
  }
  // A project tab's label follows state no input touches -- its dirty marker is
  // "any editor tab in that project is dirty" -- so typing into or saving a file
  // changed it with no damage on the strip (remote QA suite).
  // What the strip shows is also where it is scrolled to, which project is the
  // active one and how wide it is: an overflow arrow scrolled it and damaged the
  // arrow alone, and the strip kept the projects it had painted (soak
  // b3797205c752, 713b09d7b62e, aa044991c495).
  {
    const ProjectCatalogState& catalog = context_.project_catalog;
    std::uint64_t version = tab_strip_chrome_.ProjectStripSourceVersion();
    for (const std::uint64_t part : {static_cast<std::uint64_t>(catalog.active_index),
                                     static_cast<std::uint64_t>(catalog.tab_scroll_index),
                                     static_cast<std::uint64_t>(std::lround(layout.project_tab_strip.w * 4.0f))}) {
      version = (version ^ part) * 0x100000001B3ull;
    }
    chrome_unit(layout.project_tab_strip.h > 0.0f ? std::optional<SDL_FRect>(layout.project_tab_strip) : std::nullopt,
                version);
  }
  // The overlay card's extent: one that opens, closes, grows or shrinks repaints
  // where it was and where it is.
  chrome_unit(project_state.overlay.visible ? std::optional<SDL_FRect>(ComputeOverlayRect(layout.overlay_anchor))
                                            : std::nullopt,
              0);
  // The open menu's popup and submenu rects, laid out here for the pointer
  // occluder below and for hit-testing. Their rows -- and the context menu's and
  // the overflow popup's -- are resolved further down, once the occluder says what
  // the pointer is over, as derived units of their own (MenuPopupPaint.h): the
  // popup was one Chrome unit keyed on its rows' enabled and checked flags.
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
  // (The merge header's status and the compare header's summary are header units
  // of their surfaces: see the diff branches of the editor-pane units above.)
  // (The diff's shape -- its presentation rows -- is in its rows' keys: CompareRowUnits.)
  // The debug toolbar: where it is and its run state.
  {
    PreparedFrameState::DebugToolbar toolbar;
    if (DebugToolbarVisible()) {
      toolbar.visible = true;
      toolbar.stopped = IsDebugSessionStopped();
      toolbar.rect = ComputeDebugToolbarLayout(ActiveTabSurface(layout), DebugToolbarAvoidBelowY(layout),
                                               DebugSupportsReverse())
                         .widget;
    }
    chrome_unit(toolbar.visible ? std::optional<SDL_FRect>(toolbar.rect) : std::nullopt, toolbar.stopped ? 1u : 2u);
  }
  // Plugin status items, drawn in the breadcrumb bands.
  {
    std::uint64_t items_hash = 0xCBF29CE484222325ull;
    const auto mix = [&items_hash](std::string_view text) {
      for (const char c : text) {
        items_hash = (items_hash ^ static_cast<unsigned char>(c)) * 0x100000001B3ull;
      }
      items_hash = (items_hash ^ 0xFFu) * 0x100000001B3ull;
    };
    for (const StatusItemView& item : ResolveStatusItems(plugin_runtime_.Host(), status_item_cache_)) {
      mix(item.id);
      mix(item.text);
      mix(item.icon);
      items_hash = (items_hash ^ (static_cast<std::uint64_t>(item.tone) << 8 |
                                  static_cast<std::uint64_t>(item.alignment))) *
                   0x100000001B3ull;
      items_hash = (items_hash ^ static_cast<std::uint64_t>(std::lround(item.progress * 1000.0f) + 1000)) *
                   0x100000001B3ull;
    }
    frame_state_.status_items_hash = items_hash;  // the breadcrumb bands draw them: their units key on it
  }
  // The context menu's rect (its card and rows are derived units: MenuPopupPaint.h).
  frame_state_.context_menu_rect = ComputeTreeContextMenuRect();
  // Each editor group's tab strip and breadcrumb band. A strip draws its tabs'
  // titles and dirty marks and whether its group has the focus; a band draws its
  // pane's breadcrumb label, the plugin status items and the focus. (The focused
  // pane's own rows key on the focus too: EditorRowUnits.)
  {
    const EditorGroupRectsLayout group_rects = ComputeEditorGroupRectsForState(layout);
    const std::size_t focused = project_state.clamped_focused_group_index();
    for (std::size_t gi = 0; gi < kMaxEditorGroups; ++gi) {
      const bool present = gi < group_rects.groups.size() && gi < project_state.editor_groups.size();
      PaintFingerprint strip;
      PaintFingerprint band;
      if (present) {
        strip.Add(gi == focused).Add(group_rects.groups.size());
        for (const TabEntry& tab : project_state.editor_groups[gi].open_tabs) {
          strip.Add(TabEntryIsDirty(tab)).Add(std::string_view(tab.title));
        }
        // What the strip paints of its tabs (RenderWindowChrome): which are shown and
        // where -- the active one, the strip's scroll -- their labels and badges, the
        // hovered tab and close glyph, the overflow chevrons, the slide and the drag.
        // The strip is one unit; a tab activated or scrolled into view repaints it
        // whoever asked (derived-damage 4.1).
        const SDL_FRect& strip_rect = group_rects.groups[gi].tab_strip;
        if (strip_rect.h > 0.0f && HasActiveProjectCatalogEntry()) {
          const TabDragState& drag = context_.interaction_state.tab_drag;
          const std::span<const float> slide = context_.interaction_state.tab_slide.OffsetsFor(TabDragKind::Editor, gi);
          const bool still = slide.empty() && !drag.dragging;
          const std::vector<VisibleStripTab>& visible = tab_strip_chrome_.ComputeVisibleTabsForGroup(gi, strip_rect);
          for (const VisibleStripTab& tab : visible) {
            strip.Add(tab.index).Add(tab.rect).Add(tab.active).Add(std::string_view(tab.display_title));
            strip.Add(std::string_view(tab.badge_text)).Add(tab.badge_color).Add(tab.show_badge);
            strip.Add(still && PointerOver(tab.rect)).Add(still && PointerOver(tab.close_rect));
          }
          for (const float offset : slide) {
            strip.Add(offset);
          }
          const auto overflow = tab_strip_chrome_.ComputeTabOverflowControlsForGroup(gi, strip_rect, visible);
          strip.Add(overflow.hidden_left).Add(overflow.hidden_right).Add(overflow.left_button).Add(overflow.right_button);
          strip.Add(PointerOver(overflow.left_button)).Add(PointerOver(overflow.right_button));
          if (drag.dragging && drag.kind == TabDragKind::Editor) {
            strip.Add(drag.source_group_index).Add(drag.source_index).Add(drag.target_group_index);
            strip.Add(drag.pointer_x).Add(drag.grab_offset_x);
          }
        }
        band.Add(gi == focused).Add(std::string_view(BreadcrumbLabel(gi))).Add(frame_state_.status_items_hash);
      }
      chrome_unit(present ? std::optional<SDL_FRect>(group_rects.groups[gi].tab_strip) : std::nullopt, strip.value());
      chrome_unit(present ? std::optional<SDL_FRect>(group_rects.groups[gi].breadcrumb) : std::nullopt, band.value());
    }
  }
  // The focus ring: the surface that lost it and the one that took it repaint
  // (a Chrome unit at the focused surface's rect).
  // The ring this frame paints is the one its view models were built with, so a
  // focus move between building them and here is a ring painted nowhere.
  assert((!prepare_cached_bottom_panel_vm_.has_value() ||
          prepare_cached_bottom_panel_vm_->focus == project_state.surface.focus) &&
         "frame prep moved the keyboard after the surface view models were built");
  chrome_unit(FocusSurfaceRect(layout, project_state.surface.focus),
              static_cast<std::uint64_t>(project_state.surface.focus));
  // (The tabs' titles and dirty marks are in their strips' units above.)
  // The sidebar's selected row: activating a tab selects its file in the tree (as
  // VS Code's explorer follows the active editor) without repainting the tree
  // (QA monkey, seed 5). Any selection that moved with no repaint of its own
  // repaints the sidebar here -- and so does a scroll or a rebuilt tree (a watcher
  // batch, an expansion) that landed between frames: the command fuzzer caught a
  // frame repainting only the menu bar over a tree that had moved under it.
  // (The file tree and Source Control are not among them: their rows, buttons and
  // scrollbars are derived units of their own -- DerivedSurface::SidebarTree and
  // DerivedSurface::GitPanel -- so a selection, a scroll or a rebuilt list repaints
  // the rows it changed, not the sidebar. The unit keeps its place with no rect,
  // and a switch to or from either view repaints the sidebar.)
  if (sidebar_vm.visible && ActiveSidebarMode() != SidebarMode::Tree && ActiveSidebarMode() != SidebarMode::Git) {
    const SidebarState& sidebar = project_state.sidebar;
    std::size_t selected = 0;
    switch (ActiveSidebarMode()) {
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
    // Never 0 (0 means "nothing painted yet") -- without touching bit 0: `key |= 1`
    // erased any change that lived only there in the last term mixed in, which
    // made a tree revision going from even to odd, or a one-row scroll from an
    // even row in any other view, invisible to this check.
    if (key == 0) {
      key = 1;
    }
    chrome_unit(layout.sidebar, key);
  } else {
    chrome_unit(std::nullopt, 0);
  }
  // A tab strip mid-slide, one unit per animation slot: one that comes to rest
  // repaints its strip at rest.
  {
    const TabSlideState& slide = context_.interaction_state.tab_slide;
    for (std::size_t i = 0; i < kMaxAnimatingTabStrips; ++i) {
      std::optional<SDL_FRect> strip;
      std::uint64_t key = 0;
      if (i < slide.strips.size() && !slide.strips[i].idle()) {
        strip = TabStripRectForKind(slide.strips[i].kind, slide.strips[i].group_index);
        key = (static_cast<std::uint64_t>(slide.strips[i].kind) << 32) | slide.strips[i].group_index;
      }
      chrome_unit(strip, key);
    }
  }
  frame_state_.pointer_occluder.reset();
  const NotificationsViewModel toasts =
      notification_service_.Empty()
          ? NotificationsViewModel{}
          : RenderViewModelBuilder(context_).BuildNotifications(notification_service_, layout.status_bar,
                                                                text_renderer_);
  // A toast paints over everything, and motion over one stops there
  // (HandleMouseMotion), so the pointer on a toast hovers nothing beneath it --
  // not even a modal's rows, nor an open menu's. Taking the menu lit the row a
  // toast covered, beside the toast, with only the toasts repainted (soak, seed 1
  // at ui.scale 0.75). As in the motion handler: not during a drag, which crosses
  // a toast, nor under the unsaved-changes prompt, which takes the pointer first.
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
  if (toast_under_pointer.has_value()) {
    frame_state_.pointer_occluder = toast_under_pointer;
  } else if (IsDividerDrag(context_.interaction_state.drag_target)) {
    // A divider drag owns the pointer: the rows and buttons it crosses must not
    // light up. Nothing invalidated them as it passed -- the drag repaints the
    // layout it changes, not what lies under the pointer -- so a sidebar wide
    // enough to sit under a panel drag kept a stale hovered row.
    frame_state_.pointer_occluder = SDL_FRect{};
  } else if (modal.has_value()) {
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
    // An overlay card paints over the menu bar (under its popups): where a card
    // that reaches up into a short window covers the bar, the pointer is on the
    // card. Taking the bar there lit the File item behind the command palette on
    // a full frame, and the motion probe -- which knew the pointer was on the card
    // -- never repainted it (soak fe815f6e2f46, 640x480 at ui.scale 3).
    if (frame_state_.pointer_occluder.has_value() && project_state.overlay.visible &&
        RectsEqual(*frame_state_.pointer_occluder, layout.menu_bar)) {
      const SDL_FRect card = ComputeOverlayRect(layout.overlay_anchor);
      if (Contains(card, last_mouse_x_, last_mouse_y_)) {
        frame_state_.pointer_occluder = card;
      }
    }
    // An open menu owns the pointer too: a press outside its pieces only closes
    // it, so nothing beneath hovers. A sidebar button lit up under an open menu
    // with no repaint asked for it, and a clip that covered part of it left the
    // rest unhovered (QA monkey, seed 1).
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
  // Derived-damage surfaces settle here: after the pointer occluder, which their
  // hover reads (openspec derived-damage).
  PrepareStatusBarFrame(layout);
  // The open menus: each one's card and visible rows, resolved once -- a row's
  // label, whether it is enabled and checked (the state behind them can move
  // between two paints of one frame: a terminal's exit lands on its reader
  // thread), and whether it is lit, by the menu's active row or by the pointer.
  {
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
        MenuRowPaint row{.rect = item.rect, .enabled = item.enabled, .checked = item.checked, .separator = item.separator};
        if (!item.separator) {
          const MenuItemSpec& spec = items[item.index];
          if (PointerOver(item.rect)) {
            popup.pointer_row = static_cast<int>(popup.rows.size());
          }
          row.hovered = item.hovered || popup.pointer_row == static_cast<int>(popup.rows.size());
          // Each label is copied before the next is resolved: a composed one
          // lives in a buffer the next reuses (MenuItemLabel).
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
    // The compact layout's overflow popup: one row per top-level menu that does
    // not fit the bar, lit by the active index alone, which the motion path and
    // the keyboard both write (TD-2026-09-19-296).
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
    // The context menu (the tree's, a tab's, the terminal's), in the rect laid out
    // above: its height comes from a cache keyed on the language server's
    // readiness, which another thread moves (soak e4353c8cb483).
    if (context_.menu_state.tree_context_menu.open && frame_state_.context_menu_rect.has_value()) {
      const auto items = TreeContextMenuItems(context_.menu_state.tree_context_menu.target);
      if (!items.empty()) {
        lay_out(popup_at(MenuPopupSlot::Context), items, context_.menu_state.tree_context_menu.active_item_index,
                *frame_state_.context_menu_rect);
      }
    }
    CommitMenuPopupUnits(frame_state_, renderer, theme_, text_renderer_.MetricsGeneration(),
                         [this](const SDL_FRect& rect) { RequestRedrawRect(rect); });
  }
  {
    // The bottom panel's header, the same way (derived-damage 3.1): its label is
    // "Terminal", "Output", or the shown output channel's name when it has no tabs.
    // A strip label changes with nothing else drawn there -- a results tab takes
    // its first line's label, a shell's OSC title renames its tab while only the
    // terminal's body repaints -- and each tab is a unit of its own, so that is
    // repainted; this replaced the whole-panel repaint on the tab models'
    // fingerprint (painted_bottom_panel_tabs).
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
    const SDL_FRect panel_header = MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                                            kWorkspaceBottomPanelHeaderHeight);
    PrepareBottomPanelHeaderFrame(
        frame_state_,
        BottomPanelHeaderInputs{
            .renderer = renderer,
            .layout = &layout,
            .panel = &panel,
            .theme = &theme_,
            .label = label,
            .new_terminal_rect =
                tab_strip_service_.BottomPanelTerminalNewTabRect(layout_mode_service_.CurrentMode(), panel_header),
            .maximize_rect =
                tab_strip_service_.BottomPanelMaximizeButtonRect(layout_mode_service_.CurrentMode(), panel_header),
            .divider_dragged = context_.interaction_state.drag_target == DragTarget::BottomPanelDivider,
            .pointer_over = [this](const SDL_FRect& rect) { return PointerOver(rect); },
            .damage = [this](const SDL_FRect& rect) { RequestRedrawRect(rect); },
        });
  }
  {
    // The Output body, the same way: each visible row is a unit named by its
    // entry's serial, so a line into the shown channel repaints the rows it moved
    // and a line into a hidden one repaints nothing. This replaced the whole-panel
    // repaint on any channel's content revision (painted_output_revision), and
    // settles the scroll here rather than in paint.
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
  {
    // The terminal's chrome, the same way: each pane's hint and scrollbar and the
    // dividers are units, so a drag lights its thumb or divider whoever set it.
    // The grid's rows keep their own damage (the pane snapshot's generation).
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
        chrome.text_box = MakeRect(log.text_x, log.text_y, log.text_width,
                                   std::max(0.0f, body.y + body.h - log.text_y));
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
  // The notification toasts, the same way: each toast is a unit, so a row a
  // publisher updates -- its progress, its text -- repaints whoever updated it.
  PrepareNotificationToastsFrame(frame_state_, renderer, toasts, theme_,
                                 [this](const SDL_FRect& rect) { RequestRedrawRect(rect); });
  {
    // The debug pane, the same way: its mode tabs, rows, hint and scrollbar are
    // units laid out here, so a row the adapter rewrites -- a value after a step,
    // a frame's label -- repaints whoever changed it. The scroll settles here, no
    // longer in paint.
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
  // The Source Control panel's head, the same way: its buttons, banners and the
  // commit draft are units laid out here, from the presentation the sidebar view
  // model holds, so a banner a refresh rewrites repaints whoever changed it. The
  // draft's control rects and caret, and the file list's scroll, settle here for
  // hit-testing and the caret's own redraw -- no longer as paint's side effects.
  {
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
  // The sidebar's mode tab row, the same way, whichever view is up: each tab and
  // the overflow button is a unit, so the tab of the view on screen, a hovered tab
  // and the overflow button lit under its open menu repaint whoever changed them.
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
  // The file tree, the same way: its Collapse and Refresh buttons, each visible row
  // and its scrollbar are units laid out here from the tree's entries, so a row
  // repaints whoever changed what it shows -- a watcher's batch, a git status
  // refresh, a tab activated in the editor selecting its file here.
  {
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
      // File icons are opt-in: only when a plugin contributed an icon theme, or the
      // user enabled the built-in defaults (`sidebar.file_icons`). Off (the default),
      // the whole path is skipped and any cache from an earlier opt-in is dropped.
      // On, the icons are resolved once per tree mutation or icon-theme reload --
      // here, where paint used to do it -- and the rows read the cache.
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
  // Before any hover is resolved against the blame overlay or its card's damage is
  // decided: an overlay the active buffer has moved away from is this frame's
  // paint's to rebuild, not prep's to lay a card out of.
  editor_blame_overlay_service_.ForgetVisibleOverlayIfStale(ActiveEditorViewport());
  // A queued editor-hover refresh (the content under a still pointer changed)
  // runs here, not in paint: RenderClip ran it only when it painted the editor, so
  // a partial frame that skipped the editor kept the old hover card while a full
  // render of the same frame dropped it (QA monkey, seed 7).
  // A card pinned to the caret (Show Hover) refreshes with no pointer at all.
  if (editor_hover_refresh_pending_ &&
      (last_mouse_position_valid_ || context_.interaction_state.caret_hover.active)) {
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
    // The hovered element's fill and the tooltip card (a card at the same place can
    // say something else: a click reordered the tab under a still pointer, QA
    // monkey, seed 5).
    chrome_unit(hover_rect, 0);
    chrome_unit(tooltip_rect, tooltip_text_hash);
    // The editor hover card goes when its target does -- the pointer leaving for
    // the tab strip cleared the blame card's target and repainted only the strip
    // (QA monkey, seed 7). Laid out once here; its paint reads this layout.
    frame_state_.hover_popup = ActiveEditorHoverPopupLayout();
    std::optional<SDL_FRect> card;
    // And what the card says: a language server's hover reply that grew from its
    // first to its full answer in the same card ("provides Scheduler" to "provides
    // Scheduler, Task, add, run") repainted nothing once its request stopped
    // naming the editor surface (qa lsp-more).
    PaintFingerprint card_content;
    if (frame_state_.hover_popup.has_value()) {
      const EditorHoverPopupLayout& hover = *frame_state_.hover_popup;
      const SDL_FRect& popup = hover.rect;
      card = MakeRect(popup.x - 2.0f, popup.y - 2.0f, popup.w + 4.0f, popup.h + 4.0f);
      card_content.Add(static_cast<int>(hover.kind)).Add(hover.blame_line_index);
      if (hover.diagnostic.has_value()) {
        card_content.Add(std::string_view(hover.diagnostic->message))
            .Add(static_cast<int>(hover.diagnostic->severity));
      }
      if (hover.plugin_hover.has_value()) {
        card_content.Add(std::string_view(hover.plugin_hover->title))
            .Add(std::string_view(hover.plugin_hover->content));
      }
      if (hover.debug_value.has_value()) {
        card_content.Add(std::string_view(hover.debug_value->value)).Add(std::string_view(hover.debug_value->type));
      }
      if (hover.primary_action_rect.has_value()) {
        card_content.Add(*hover.primary_action_rect)
            .Add(last_mouse_position_valid_ &&
                 EditorHoverPopupPrimaryActionHovered(last_mouse_x_, last_mouse_y_));
      }
    }
    chrome_unit(card, card_content.value());
    // The signature-help card: shown while a request's answer is up and the caret
    // has not moved, hidden while a menu holds the pointer -- so a menu opening
    // and closing over it is a card going and coming back with no edit.
    std::optional<SDL_FRect> signature_rect;
    std::size_t signature_hash = 0;
    if (active_signature_help_.has_value() && !MenuSurfaceCapturingMouse()) {
      if (const std::optional<SDL_FRect> caret = ActiveEditorCaretRect(layout); caret.has_value()) {
        signature_rect = ComputeTwoBlockHoverCardRect(
            active_signature_help_->signature, active_signature_help_->documentation,
            kEditorHoverPopupMaxPluginTitleLines, kEditorHoverPopupMaxPluginContentLines, *caret,
            layout.editor_surface);
        signature_hash = std::hash<std::string_view>{}(active_signature_help_->signature) * 31U +
                         std::hash<std::string_view>{}(active_signature_help_->documentation);
      }
    }
    frame_state_.signature_help_rect = signature_rect;
    chrome_unit(signature_rect, signature_hash);  // the same card saying something else repaints too
  }
  frame_state_.derived_damage.Commit(
      DerivedSurface::Chrome, [this](const SDL_FRect& rect) { RequestRedrawRect(rect); }, DerivedDamage::Coalesce::Apart);
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
    // The panel's divider is drawn by the header (PaintBottomPanelHeader), over it.
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

// One editor pane, from the inputs frame prep built for it and nothing else
// (derived-damage 2.3; CheckDerivedSurfacePaintReadsOnlyItsSnapshot): its rows'
// derived units key on the same inputs, so what this draws is what they compare.
void WorkspaceShell::PaintEditorPane(SDL_Renderer* renderer, std::size_t group_index, bool draw_caret) {
  const PreparedFrameState::EditorPanePaint& paint = frame_state_.editor_pane_paint[group_index];
  editor::TextViewport& viewport = *paint.viewport;
  const editor::EditorViewMetrics& metrics = paint.metrics;
  // A pane paints inside itself: squeezed by a split divider, a glyph at its
  // edge overhung onto the neighbouring border in a full frame and not in a
  // partial one of the pane (QaFlowRegression/
  // EveryDividerDraggedToItsExtremesKeepsEveryFrameFresh).
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
  // Insertion-point indicator for a selection drag-and-drop in flight. No other
  // gesture here has one, and without it the drop is a guess: the real caret stays
  // inside the selection being dragged (TD-2026-08-13-204). Drawn after the pane
  // so it is never painted over, and only on the pane that owns the gesture.
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
  // The scrollbars, and the overview ruler in the vertical one's track. Its marker
  // caches are one per group, keyed by a cheap signature so the marker set rebuilds
  // only when a source or the lane geometry changes (never on scroll or caret moves).
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

void WorkspaceShell::RenderActiveWorkspaceSurface(
    SDL_Renderer* renderer,
    const WorkspaceLayout& layout,
    const FrameToken& frame_token,
    bool draw_editor_caret,
    std::optional<SDL_FRect>* active_editor_pane_rect,
    const FrameSurfaceViewModel& frame_vm) {
  (void)frame_token;
  ProjectWorkspaceState& project_state = *frame_vm.project_state;
  // Every pane paints the tab in front of its own group, inside its own rect: an
  // editor, or a diff or a merge. A diff in one pane of a split used to take the
  // whole editor column, drawing over the other panes' editors while their tab
  // strips stayed on top of it (TD-2026-11-11-439).
  const EditorGroupRectsLayout editor_surface_group_rects = ComputeEditorGroupRectsForState(layout);
  const EditorPaneLayouts editor_panes = EditorPaneLayoutsFromGroupRects(editor_surface_group_rects);
  const std::size_t focused_group_index = project_state.clamped_focused_group_index();
  const auto diff_tab_in_front = [&project_state](std::size_t group_index) -> TabEntry* {
    if (group_index >= project_state.editor_groups.size()) {
      return nullptr;
    }
    EditorGroup& group = project_state.editor_groups[group_index];
    if (!group.has_active_tab()) {
      return nullptr;
    }
    TabEntry& tab = group.active_tab();
    const bool diff = (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) ||
                      (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value());
    return diff ? &tab : nullptr;
  };
  bool focused_pane_is_diff = false;
  std::array<bool, kMaxEditorGroups> pane_shows_diff{};
  for (std::size_t gi = 0; gi < editor_surface_group_rects.groups.size() && gi < kMaxEditorGroups; ++gi) {
    TabEntry* tab = diff_tab_in_front(gi);
    if (tab == nullptr) {
      continue;
    }
    const bool focused = gi == focused_group_index;
    // The focused one only when the frame's view model has it (as before): the
    // view model is what says the frame shows it.
    if (focused && !frame_vm.compare_surface.has_value()) {
      continue;
    }
    focused_pane_is_diff = focused_pane_is_diff || focused;
    pane_shows_diff[gi] = true;
    const SDL_FRect& area = editor_surface_group_rects.groups[gi].editor_surface;
    // A diff or a merge paints inside its pane, as an editor pane does inside its
    // own: in an area too short for its header and a row (a large font, the panel
    // grown over the window) it laid out a row below the area anyway and painted
    // it over the panel -- in a full frame, not in a partial one that left the
    // editor alone (soak 498dc546c6a9).
    const render::ScopedRenderClip surface_clip(renderer, area);
    if (tab->kind == TabEntry::Kind::Compare) {
      CompareTabState& compare_tab = *tab->compare;
      const bool draw_compare_caret =
          focused && compare_tab.right_view_active && prepared_frame_draw_editor_caret_;
      // Its working side's diagnostics and decorations are in the prepared paint
      // inputs (DiffSurfacePaint), resolved at frame prep.
      RenderCompareSurface(renderer, area, compare_tab, draw_compare_caret,
                           CompareRenderProjectInputs{.paint = &frame_state_.diff_surface_paint[gi]});
      RenderCompareScrollbars(renderer, area, compare_tab, frame_state_.diff_surface_paint[gi]);
    } else {
      MergeTabState& merge_tab = *tab->merge;
      RenderMergeSurface(renderer, area, merge_tab, focused, frame_state_.diff_surface_paint[gi]);
      RenderMergeScrollbars(renderer, area, merge_tab, frame_state_.diff_surface_paint[gi]);
    }
  }
  {
    // Every editor pane in front, from the inputs frame prep built for it
    // (PreparedFrameState::EditorPanePaint): PaintEditorPane reads nothing else.
    for (const EditorPaneLayout& pane : editor_panes) {
      const std::size_t gi = pane.group_index;
      if (gi >= kMaxEditorGroups || pane_shows_diff[gi] || !frame_state_.editor_pane_paint[gi].valid) {
        continue;
      }
      if (pane.active && active_editor_pane_rect != nullptr) {
        *active_editor_pane_rect = pane.rect;
      }
      PaintEditorPane(renderer, gi, pane.active && draw_editor_caret);
    }
    for (const EditorSplitDividerRect& divider : editor_surface_group_rects.dividers) {
      const bool divider_active =
          context_.interaction_state.drag_target == DragTarget::EditorSplitDivider &&
          divider.node == context_.interaction_state.drag_editor_split_node &&
          divider.boundary == context_.interaction_state.drag_editor_split_boundary;
      DrawFilledRect(renderer, divider.rect, divider_active ? theme_.accent : theme_.border);
    }
  }

  // The external-change banner belongs to an editor; a diff or merge in front
  // shows none (it never laid one out).
  if (const SDL_FRect strip =
          focused_pane_is_diff
              ? SDL_FRect{}
              : ActiveEditorBannerStripRect(project_state, editor_surface_group_rects);
      frame_vm.editor_banner.has_value() && strip.h > 0.0f) {
    const EditorBannerViewModel& banner_vm = *frame_vm.editor_banner;
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
  if (!focused_pane_is_diff && DebugToolbarVisible()) {
    RenderDebugToolbar(renderer, layout, IsDebugSessionStopped(),
                       DebugToolbarAvoidBelowY(layout));
  }
}

}  // namespace microide::workspace
