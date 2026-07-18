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

namespace microide::workspace {

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

struct OverlaySurfaceViewModel {
  bool visible = false;
  OverlayMode mode = OverlayMode::FileFinder;
  int scroll_row = 0;
  TextInputSurface current_surface = TextInputSurface::None;
  // View into live SingleLineEditor state; valid for the duration of the frame the view model is
  // used. Storing a view here keeps BuildOverlaySurface allocation-free per frame.
  std::string_view buffer_search_query_text;
  const OverlayState* state = nullptr;
  ProjectWorkspaceState* project_state = nullptr;
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
  // Prebuilt project-search status/hint line (empty unless mode == Search).
  // Points into a builder-owned thread-local cache; consume within the frame.
  std::string_view project_search_status_text;
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
  // Live view into the current project state, so render TUs can drive
  // `TabStripService` queries (which take ProjectWorkspaceState by const ref)
  // without reaching into `context_.current_project_state` directly.
  const ProjectWorkspaceState* project_state = nullptr;
  BottomPanelTabDragViewModel tab_drag;
};

// Right-side debug pane surface (Call Stack / Variables / Watch / Breakpoints).
// The backing models live on `project_state` and already hold prebuilt display
// strings, so the view model only forwards the pointer + the active surface's
// scroll and a static label string_view — no per-frame string materialization.
struct DebugPaneSurfaceViewModel {
  bool visible = false;
  DebugPaneMode mode = DebugPaneMode::CallStack;
  int scroll_row = 0;
  std::string_view header_label;
  FocusTarget focus = FocusTarget::Sidebar;
  ProjectWorkspaceState* project_state = nullptr;
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
  std::vector<HelpAboutRow> help_rows;     // unchanged Help/About path
  SettingsPickerViewModel value_picker;    // font dropdown (visible while editing)
};

class RenderViewModelBuilder {
 public:
  explicit RenderViewModelBuilder(const WorkspaceContext& context);

  FrameSurfaceViewModel BuildFrameSurface(const WorkspaceLayout& layout) const;
  OverlaySurfaceViewModel BuildOverlaySurface() const;
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
