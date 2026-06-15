#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "editor/EditorViewModel.h"
#include "workspace/GitSidebarCommandCenter.h"
#include "workspace/NotificationService.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/SettingsOverlayService.h"
#include "workspace/StatusBarService.h"
#include "workspace/WorkspaceTabState.h"

#include <filesystem>
#include <string_view>
#include <vector>

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
  bool command_mode = false;
  const editor::SingleLineEditor* command_input = nullptr;
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
  std::optional<GitSidebarViewModel> git_sidebar;
  ProjectWorkspaceState* project_state = nullptr;
};

// Snapshot of an in-flight bottom-panel tab drag so the render TU can draw the
// ghost + insertion caret without reading interaction state directly.
struct BottomPanelTabDragViewModel {
  bool active = false;
  std::size_t source_index = 0;
  std::size_t target_slot = 0;
  float pointer_x = 0.0f;
  float grab_offset_x = 0.0f;
};

struct BottomPanelSurfaceViewModel {
  bool command_mode = false;
  PanelContentKind content = PanelContentKind::None;
  float height = 0.0f;
  std::string output_channel_id;
  std::filesystem::path project_root;
  FocusTarget focus = FocusTarget::Sidebar;
  const CommandState* command_state = nullptr;
  // Live view into the current project state, so render TUs can drive
  // `TabStripService` queries (which take ProjectWorkspaceState by const ref)
  // without reaching into `context_.current_project_state` directly.
  const ProjectWorkspaceState* project_state = nullptr;
  BottomPanelTabDragViewModel tab_drag;
};

struct HoverPopupViewModel {
  bool visible = false;
  bool has_active_target = false;
};

struct HoverTargetsViewModel {
  bool hover_enabled = false;
  const editor::DiagnosticsStore* diagnostics_store = nullptr;
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
  std::string_view display_value;  // Stepper/Segmented value text
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
  std::string_view scope_label;      // "User" / "Project"
  std::string_view group_subheader;  // full group path; non-empty only on first row of a sub-group
  SettingsControlViewModel control;
  SDL_FRect row_rect{};    // full clickable row rect (row select)
  SDL_FRect reset_rect{};  // reset affordance; w == 0 when not resettable
  int row_in_category = 0;
  bool selected = false;
  bool resettable = false;
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
  SettingsPane focused_pane = SettingsPane::Filter;
  std::string_view title;
  std::string_view query;             // view into the service-owned filter text
  std::string_view filter_placeholder;
  bool query_empty = true;
  std::vector<SettingsCategoryViewModel> categories;
  std::vector<SettingsRowViewModel> rows;  // rows of the selected category only
  std::vector<HelpAboutRow> help_rows;     // unchanged Help/About path
};

class RenderViewModelBuilder {
 public:
  explicit RenderViewModelBuilder(const WorkspaceContext& context);

  FrameSurfaceViewModel BuildFrameSurface(const WorkspaceLayout& layout) const;
  OverlaySurfaceViewModel BuildOverlaySurface() const;
  TextInputSurfaceViewModel BuildTextInputSurface() const;
  SidebarSurfaceViewModel BuildSidebarSurface() const;
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
                                bool render_whitespace_enabled = false) const;

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
  HoverTargetsViewModel BuildHoverTargets() const;
  StatusBarViewModel BuildStatusBar(const WorkspaceLayout& layout,
                                    const class StatusBarService& service) const;
  NotificationsViewModel BuildNotifications(const NotificationService& service) const;
  SettingsOverlayViewModel BuildSettingsOverlay(
      const WorkspaceLayout& layout,
      const class SettingsOverlayService& service) const;

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
  const WorkspaceContext& context_;
};

}  // namespace microide::workspace
