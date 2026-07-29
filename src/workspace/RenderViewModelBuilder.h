#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "editor/EditorInsetLayout.h"
#include "editor/EditorViewModel.h"
#include "editor/PluginDecorationStore.h"
#include "editor/WelcomeView.h"
#include "workspace/DebugViewModel.h"
#include "workspace/GitSidebarCommandCenter.h"
#include "workspace/NotificationService.h"
#include "workspace/TabStripService.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/SettingsOverlayService.h"
#include "workspace/StatusBarService.h"
#include "workspace/WorkspaceTabState.h"

#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace microide::render {
class TextRenderer;
}  // namespace microide::render

namespace microide::editor {
struct SurfaceContent;
}  // namespace microide::editor

namespace microide::workspace {

class DebugVariablesModel;
class DebugWatchModel;
class DebugBreakpointsModel;

/// Parses user/project `editor.fold.sticky_scroll.max_depth` setting (stored as string): 1..8.
int ParseStickyScrollMaxDepthSetting(const std::optional<std::string>& value);

/// Pure helper for sticky-line resolution & hit-testing (does not consult view-model caches).
void ComputeStickyScrollLinesUncached(const editor::TextViewport& viewport,
                                      const editor::FoldingModel* folding_model,
                                      bool sticky_scroll_enabled,
                                      int sticky_max_depth,
                                      std::vector<std::size_t>& out_opener_lines);

struct EditorBannerViewModel {
  bool has_actions = false;  // ExternalChange => Reload/Overwrite/Keep; Notice => dismiss only
  std::string message;       // prebuilt here so render TUs never materialize it
};

struct NotificationEntryViewModel {
  NotificationService::Tone tone = NotificationService::Tone::Info;
  std::string message;  // prebuilt here so render TUs never materialize strings
};

struct NotificationsViewModel {
  std::vector<NotificationEntryViewModel> entries;  // oldest first; render bottom-up
};

struct FrameSurfaceViewModel {
  struct CompareSurfaceViewModel {
    TabEntry::Kind kind = TabEntry::Kind::Editor;
  };

  WorkspaceLayout layout{};
  bool sidebar_visible = false;
  bool bottom_panel_visible = false;
  std::optional<CompareSurfaceViewModel> compare_surface;
  std::optional<EditorBannerViewModel> editor_banner;
  ProjectWorkspaceState* project_state = nullptr;
};

// One prebuilt overlay list row (visible window only). Views point either into
// state-owned strings that outlive the frame or into the view model's own
// `label_storage` blob; both stay valid until the next BuildOverlaySurfaceInto.
// `primary` is already truncated to its column width, so render just draws it.
struct OverlayListRowViewModel {
  std::string_view primary;
  std::string_view secondary;    // right-aligned muted column; empty when absent
  float secondary_width = 0.0f;  // measured width + trailing inset; 0 when absent
};

// Prebuilt model for the compact find/replace widget (BufferSearch/BufferReplace).
// A find-widget mode toggle: its glyph label and whether it is currently on.
// Labels are static literals, so this stays allocation-free.
struct FindWidgetToggleViewModel {
  std::string_view label;
  bool active = false;
};

// Shared by the in-file find widget (overlay) and the terminal find bar (bottom
// panel), which render identically; only the toggles and the source of the match
// count differ.
struct OverlayFindWidgetViewModel {
  FindWidgetLayout fw{};
  bool replace_mode = false;
  bool search_focused = false;
  bool replace_focused = false;
  std::array<FindWidgetToggleViewModel, kFindWidgetMaxToggles> toggles{};
  bool has_matches = false;
  bool has_query = false;
  std::string_view search_display_text;   // caret-relative tail when focused
  std::string_view replace_display_text;  // caret-relative tail when focused
  std::string_view count_text;            // "n/m" or "0/0"; empty when no query
};

// Fully owned/precomputed overlay surface model (TD-2026-07-17-084): the render
// TU consumes only this — geometry, prebuilt labels, and the visible row window —
// and never dereferences live OverlayState / ProjectWorkspaceState.
struct OverlaySurfaceViewModel {
  bool visible = false;
  OverlayMode mode = OverlayMode::FileFinder;
  int scroll_row = 0;  // clamped to the list layout
  TextInputSurface current_surface = TextInputSurface::None;
  // View into live SingleLineEditor state; valid for the duration of the frame the view model is
  // used (consumed by the editor render path to seed match highlighting).
  std::string_view buffer_search_query_text;

  // Geometry (computed once at build; identical to what hit-testing derives).
  bool caret_anchored = false;  // completion popup: no backdrop, plain card frame
  SDL_FRect overlay_rect{};
  ScrollableListLayout list_layout{};
  std::size_t total_rows = 0;
  int selected_row = 0;  // absolute selected index into the full list

  // Prebuilt chrome text. Empty views mean "not drawn". All composed strings are
  // backed by `label_storage` or by state strings stable for the frame.
  std::string_view title;
  std::string_view note;    // right-aligned note on the title row
  float note_x = 0.0f;
  std::string_view context_label;  // picker subtitle (pre-truncated)
  bool has_query_field = false;
  TextInputSurface query_surface = TextInputSurface::None;
  float query_row_y = 0.0f;  // y of the query field row (absolute)
  std::string_view query_display_text;
  std::string_view summary_line;  // status line under the query field
  float summary_y = 0.0f;         // absolute y of the summary/hint row
  std::string_view hint;          // right-aligned picker key hint
  float hint_x = 0.0f;
  std::string_view error_line;     // completion/code-action error (pre-truncated)
  bool error_at_title_row = false; // completion: error draws at the title row in the
                                   // deleted tint; otherwise muted in the list area
  std::string_view empty_label;    // drawn in the list area when total_rows == 0

  std::vector<OverlayListRowViewModel> rows;  // visible window only
  OverlayFindWidgetViewModel find_widget;     // valid in BufferSearch/BufferReplace

  // Backing storage for composed/truncated labels above. Kept as a member so the
  // cached view model reuses its capacity across frames.
  std::string label_storage;
};

struct TextInputSurfaceViewModel {
  TextInputSurface current_surface = TextInputSurface::None;
  bool prompt_editing = false;
  const editor::SingleLineEditor* prompt_input = nullptr;
  const editor::SingleLineEditor* buffer_search_query = nullptr;
  const editor::SingleLineEditor* buffer_search_replace = nullptr;
  const editor::SingleLineEditor* project_search_query = nullptr;
  const editor::SingleLineEditor* project_search_edit_buffer = nullptr;
  const editor::SingleLineEditor* commit_picker_query = nullptr;
  const editor::SingleLineEditor* launch_config_picker_query = nullptr;
  const editor::SingleLineEditor* command_palette_query = nullptr;
  const editor::SingleLineEditor* file_finder_query = nullptr;
};

struct SidebarSurfaceViewModel {
  bool visible = false;
  SidebarMode mode = SidebarMode::Tree;
  int scroll_row = 0;
  bool project_search_editing = false;
  // Either a constant placeholder or a view into live `project_search.{query,replace_text}` state.
  // Safe to hold as a view because rendering is single-threaded and the underlying state outlives
  // the view model. Avoids per-frame `std::string` allocations on every BuildSidebarSurface() call.
  std::string_view query_fallback_text;
  std::string_view replace_fallback_text;
  // Same contract for the scope glob fields; only meaningful while
  // `project_search_scope_expanded` is true.
  std::string_view include_fallback_text;
  std::string_view exclude_fallback_text;
  bool project_search_scope_expanded = false;
  // Prebuilt project-search status/hint line (empty unless mode == Search).
  // Points into a builder-owned thread-local cache; consume within the frame.
  std::string_view project_search_status_text;
  // Prebuilt placeholder for an empty result list ("Searching…", "No matches",
  // "Error: …"). Same cache contract as the status line. Empty when the list has
  // rows. `project_search_empty_is_error` picks the error colour.
  std::string_view project_search_empty_text;
  bool project_search_empty_is_error = false;
  // Points into the frame-owned `CachedGitSidebarPresentation` memo (thread-local,
  // stable until the next mutating build on this thread — which cannot happen between
  // prep and render of one frame). Null unless the git sidebar is visible. Held as a
  // pointer, not an owned copy, so a hover/caret-blink/progress repaint that changed
  // no git state is genuinely allocation-free instead of deep-copying the whole VM +
  // flattened row list each frame.
  const GitSidebarViewModel* git_sidebar = nullptr;
  // Flattened, render-ready git sidebar rows built once (per state change) so the
  // render TU, hit-testing, and selection all consume one list. Points into the same
  // cached presentation as `git_sidebar`; null unless `git_sidebar` is set.
  const std::vector<GitSidebarLine>* git_sidebar_lines = nullptr;
  ProjectWorkspaceState* project_state = nullptr;
};

// Snapshot of an in-flight bottom-panel tab drag / slide so the render TU can
// draw the floating ghost and apply the Chrome-like slide offsets without
// reading interaction state directly.
struct BottomPanelTabDragViewModel {
  bool active = false;         // active drag: source tab is lifted, ghost shown
  std::size_t source_index = 0;
  float pointer_x = 0.0f;
  float grab_offset_x = 0.0f;
  bool sliding = false;        // slide animation targets this strip (drag or settle)
  std::vector<float> offsets;  // per model-index x offset (empty when not sliding)
};

struct BottomPanelSurfaceViewModel {
  PanelContentKind content = PanelContentKind::None;
  float height = 0.0f;
  std::string output_channel_id;
  std::filesystem::path project_root;
  FocusTarget focus = FocusTarget::Sidebar;
  BottomPanelTabDragViewModel tab_drag;
  // Prepared tab strip: PrepareFrameOnce fills these once the frame layout is
  // known (the strip geometry needs the bottom-panel header rect), so the render
  // TU draws prebuilt tabs instead of re-deriving them from project state.
  std::vector<VisibleStripTab> tabs;
  TabStripOverflowControls tab_overflow{};
  // Resolved plugin content surface (PanelContentKind::PluginSurface only): the
  // render TU paints this directly instead of resolving owner/id through state.
  const editor::SurfaceContent* plugin_surface = nullptr;
  float plugin_surface_scroll_y = 0.0f;

  // Terminal find bar, floating over the terminal body. Filled by frame prep
  // rather than by the builder: the bar's geometry needs the resolved layout, and
  // its state lives in TerminalFindService (which the builder cannot reach).
  bool find_visible = false;
  OverlayFindWidgetViewModel find{};
  const editor::SingleLineEditor* find_query = nullptr;
  // Match list, ordered by (row, column) so the paint loop can walk it alongside
  // the visible rows. Null when nothing is highlighted.
  const std::vector<terminal::TerminalSearchMatch>* find_matches = nullptr;
  std::size_t find_selected_index = 0;
};

// Right-side debug pane surface (Call Stack / Variables / Watch / Breakpoints).
// The backing models already hold prebuilt display strings, so the view model
// forwards a narrow const pointer to the active mode's model (the others stay
// null) + the surface's scroll and a static label — no per-frame string
// materialization, and no broad project-state pointer.
struct DebugPaneSurfaceViewModel {
  bool visible = false;
  DebugPaneMode mode = DebugPaneMode::CallStack;
  int scroll_row = 0;
  std::string_view header_label;
  FocusTarget focus = FocusTarget::Sidebar;
  const DebugExecutionView* execution = nullptr;       // CallStack mode
  const DebugVariablesModel* variables = nullptr;      // Variables mode
  const DebugWatchModel* watch = nullptr;              // Watch mode
  const DebugBreakpointsModel* breakpoints = nullptr;  // Breakpoints mode
};

struct HoverPopupViewModel {
  bool visible = false;
  bool has_active_target = false;
};

struct HoverTargetsViewModel {
  bool hover_enabled = false;
  const editor::DiagnosticsStore* diagnostics_store = nullptr;
  // Plugin-published decorations, exposed so the lint-covered hit-test TU can
  // resolve clickable end-of-line code lenses without reading project state.
  const editor::PluginDecorationStore* decoration_store = nullptr;
  // Debug hover-to-inspect (Phase 5): non-null only when the caller-supplied gate
  // holds (debug.enabled + session Stopped + adapter advertises hover evaluation),
  // so the render-surface resolver reads debug state through the view model rather
  // than reaching into project state directly.
  const DebugExecutionView* debug_execution = nullptr;
  const DebugHoverModel* debug_hover = nullptr;
};

struct StatusBarSegmentViewModel {
  StatusBarSegmentId id = StatusBarSegmentId::Project;
  std::string_view text;
  std::string_view tooltip;
  bool clickable = false;
  StatusBarSegmentTone tone = StatusBarSegmentTone::Default;
};

struct StatusBarViewModel {
  bool visible = false;
  SDL_FRect rect{};
  LayoutMode layout_mode = LayoutMode::Regular;
  std::vector<StatusBarSegmentViewModel> left_segments;
  std::vector<StatusBarSegmentViewModel> right_segments;
};

struct SettingsCategoryViewModel {
  std::string_view label;  // view into service-owned category string
  SDL_FRect rect{};        // clickable row rect in the left pane
  bool selected = false;
};

struct SettingsControlViewModel {
  SettingsControlKind kind = SettingsControlKind::None;
  bool checkbox_on = false;        // Checkbox
  std::string_view display_value;  // Stepper/Segmented value text (or live edit text)
  bool editing = false;            // TextEdit: the inline value editor is active
  std::size_t edit_caret = 0;      // TextEdit: caret byte offset into display_value
  // Pre-truncated value string the render TU draws for Segmented/Stepper/TextEdit,
  // or the "(default)" placeholder for an empty non-editing TextEdit. Built once here
  // so render consumes a ready field instead of constructing "(default)" and
  // truncating display_value to the value box on every paint (TD-2026-07-17A-007).
  // Empty for kinds that draw display_value directly (None).
  std::string shown_value;
  bool value_is_placeholder = false;  // shown_value is the dim "(default)" placeholder
  // Caret x offset (pixels from the value-text start) when `editing`; measured once
  // in the builder so render draws the caret without measuring a substr per frame.
  float caret_offset_x = 0.0f;
  // Hit rects; a rect with w == 0 is absent.
  SDL_FRect checkbox_rect{};
  SDL_FRect dec_rect{};
  SDL_FRect inc_rect{};
  SDL_FRect value_rect{};
};

struct SettingsRowViewModel {
  std::string_view id;               // view into service-owned setting id
  std::string_view label;            // view into service-owned row strings
  std::string_view description;
  // Description word-wrapped to the row's text column (views into `description`,
  // itself a view into the frame-stable service-owned string). Empty when the row
  // has no description. The row grows in height to fit every line.
  std::vector<std::string_view> description_lines;
  // X for the right-aligned scope label on the first description line; < 0 when it
  // is absent or does not fit (the builder owns this placement so render is dumb).
  float scope_label_x = -1.0f;
  std::string_view scope_label;      // "User" / "Project"
  std::string_view group_subheader;  // full group path; non-empty only on first row of a sub-group
  SettingsControlViewModel control;
  SDL_FRect row_rect{};    // full clickable row rect (row select)
  SDL_FRect reset_rect{};  // reset affordance; w == 0 when not resettable
  SDL_FRect scope_rect{};  // "This Project / Default" scope chip; w == 0 when absent
  std::string_view scope_text;    // "Project" or "Default" (the active write target)
  std::string_view scope_help;    // hover tooltip explaining the scope chip
  bool scope_is_project = false;  // true when a per-project override is active
  int row_in_category = 0;
  // Scroll-independent total vertical advance of this row (row height plus any
  // reserved sub-header strip above it). Lets keyboard keep-visible compute the
  // scroll target directly from a single build (TD-2026-07-17A-079).
  float advance_height = 0.0f;
  bool selected = false;
  bool resettable = false;
};

// One row of the font-picker dropdown: a filtered family or the "Choose file…"
// entry. `dropdown_index` is the value passed to ApplySettingsFontPickerIndex.
struct SettingsPickerItemViewModel {
  std::string_view text;
  SDL_FRect rect{};
  bool highlighted = false;
  bool is_choose_file = false;
  int dropdown_index = -1;
};

// Dropdown shown below a font-family row while it is being edited: a windowed list
// of matching installed families plus a pinned "Choose file…" entry.
struct SettingsPickerViewModel {
  bool visible = false;
  SDL_FRect rect{};  // whole dropdown card
  std::vector<SettingsPickerItemViewModel> items;
  bool more_above = false;
  bool more_below = false;
  std::optional<ScrollbarGeometry> scrollbar;  // present when the family list overflows
};

struct SettingsOverlayViewModel {
  bool visible = false;
  SettingsOverlayMode mode = SettingsOverlayMode::Settings;
  SDL_FRect rect{};
  SDL_FRect header_rect{};
  SDL_FRect filter_rect{};
  SDL_FRect left_pane_rect{};
  SDL_FRect right_pane_rect{};
  int scroll_row = 0;
  int max_scroll = 0;
  int visible_rows = 0;  // value rows that fit in the right pane
  std::optional<ScrollbarGeometry> scrollbar;  // right-pane scrollbar (Settings mode)
  // Left-rail (category) scroll model, mirroring the right pane. Categories are
  // fixed-height, so the counts are exact.
  int category_scroll_row = 0;
  int category_max_scroll = 0;
  int category_visible_rows = 0;
  std::optional<ScrollbarGeometry> category_scrollbar;  // left-rail scrollbar when categories overflow
  SettingsPane focused_pane = SettingsPane::Filter;
  std::string_view title;
  std::string_view query;             // view into the service-owned filter text
  std::string_view filter_placeholder;
  bool query_empty = true;
  std::vector<SettingsCategoryViewModel> categories;
  // Fixed header band above the value rows: the selected category's title plus a
  // one-line subtitle. Present in Settings mode whenever a category is selected.
  SDL_FRect section_header_rect{};
  std::string_view section_title;     // view into service-owned category label
  std::string_view section_subtitle;  // static blurb (empty for unknown categories)
  std::vector<SettingsRowViewModel> rows;  // rows of the selected category only
  std::vector<HelpAboutRow> help_rows;
  // Help/About column geometry. The rest of its scroll model reuses the shared
  // fields above (scroll_row / max_scroll / visible_rows / scrollbar), so the
  // wheel handler, the scrollbar grab and the paint all read one source. It used
  // to be resolved inside the render pass and published into a mutable shell
  // member, which left the painted scrollbar with no hit rect and made wheel
  // scrolling depend on a paint having already happened.
  SDL_FRect help_list_rect{};
  float help_label_column = 0.0f;
  float help_detail_x = 0.0f;
  float help_detail_width = 0.0f;
  float help_entry_gap = 0.0f;
  SettingsPickerViewModel value_picker;    // font dropdown (visible while editing)
};

class RenderViewModelBuilder {
 public:
  explicit RenderViewModelBuilder(const WorkspaceContext& context);

  FrameSurfaceViewModel BuildFrameSurface(const WorkspaceLayout& layout) const;
  /// Rebuilds `out` in place (clear+assign patterns) so the retained view model
  /// reuses its row/label capacities across frames. `overlay_rect` is the
  /// shell-computed overlay card rect for the current mode (caret-anchored,
  /// centered menu, find widget, picker, or default), passed in because its
  /// derivation is shared with hit-testing and stays shell-owned.
  void BuildOverlaySurfaceInto(OverlaySurfaceViewModel& out,
                               const WorkspaceLayout& layout,
                               const SDL_FRect& overlay_rect,
                               const render::TextRenderer& text_renderer) const;
  TextInputSurfaceViewModel BuildTextInputSurface() const;
  SidebarSurfaceViewModel BuildSidebarSurface() const;
  DebugPaneSurfaceViewModel BuildDebugPaneSurface() const;
  /// Populates `out` with clear()+push_back / assign patterns so vector capacities are reused
  /// when the workspace render path retains the same `EditorViewModel` object across frames.
  void BuildEditorViewModelInto(editor::EditorViewModel& out,
                                const editor::TextViewport& viewport,
                                std::size_t visible_rows,
                                const editor::FoldingModel* folding_model,
                                bool occurrences_highlight_enabled,
                                bool occurrences_case_sensitive,
                                bool sticky_scroll_enabled = false,
                                int sticky_max_depth = 3,
                                bool render_whitespace_enabled = false,
                                bool debug_enabled = false,
                                const editor::BreakpointStore* breakpoints = nullptr,
                                const DebugExecutionView* debug_execution = nullptr,
                                editor::InsetGapFeatureFlags inset_flags = {},
                                float line_height = 0.0f) const;

  editor::EditorViewModel BuildEditorViewModel(const editor::TextViewport& viewport,
                                               std::size_t visible_rows,
                                               const editor::FoldingModel* folding_model,
                                               bool occurrences_highlight_enabled,
                                               bool occurrences_case_sensitive,
                                               bool sticky_scroll_enabled = false,
                                               int sticky_max_depth = 3,
                                               bool render_whitespace_enabled = false) const;
  BottomPanelSurfaceViewModel BuildBottomPanelSurface() const;
  HoverPopupViewModel BuildHoverPopup(bool has_active_target) const;
  // `debug_hover_enabled` is the shell-computed gate (debug.enabled + session
  // Stopped + supportsEvaluateForHovers); when set, the returned view model carries
  // the focused-frame execution view + the hover-eval cache for the resolver.
  HoverTargetsViewModel BuildHoverTargets(bool debug_hover_enabled = false) const;
  StatusBarViewModel BuildStatusBar(const WorkspaceLayout& layout,
                                    const class StatusBarService& service) const;
  NotificationsViewModel BuildNotifications(const NotificationService& service) const;
  // Welcome / placeholder home surface. The variant is chosen from the live project
  // root (empty => NoProject cold-start home with recent projects + Open Folder; non-empty
  // => ProjectHome with this project's recent files + new/open/find actions). Recent
  // projects/files are pulled from `recents`; the curated shortcut rows come from the
  // command registry so the displayed key chords never drift from the real bindings.
  // Taking the service (not a span) lets both the render and hit-test callsites issue the
  // identical one-arg call, so their layouts can never drift apart.
  editor::WelcomeViewModel BuildWelcomeView(const class RecentsService& recents) const;
  SettingsOverlayViewModel BuildSettingsOverlay(
      const WorkspaceLayout& layout,
      const class SettingsOverlayService& service,
      const render::TextRenderer& text_renderer) const;

  // Thread-local caches (render thread): unit tests reset between cases.
  static void ResetStickyScrollCacheForTesting();
  static std::uint64_t StickyScrollCacheHitsForTesting();
  static std::uint64_t StickyScrollCacheMissesForTesting();

  static void ResetOccurrenceCachesForTesting();
  static std::uint64_t OccurrenceSeedCacheHitsForTesting();
  static std::uint64_t OccurrenceSeedCacheMissesForTesting();
  static std::uint64_t OccurrenceScanCacheHitsForTesting();
  static std::uint64_t OccurrenceScanCacheMissesForTesting();

 private:
  // Phase E1/E2: populate out.row_gaps / out.row_gap_contents from anchored plugin
  // surfaces (Below insets) and above-line code lenses (Above strips) visible in
  // `viewport`. No-op (clears them) when both are disabled. `line_height` sizes the
  // code-lens strip.
  void BuildEditorInsetGaps(editor::EditorViewModel& out, const editor::TextViewport& viewport,
                            std::size_t visible_rows, editor::InsetGapFeatureFlags inset_flags,
                            float line_height) const;

  const WorkspaceContext& context_;
};

}  // namespace microide::workspace
