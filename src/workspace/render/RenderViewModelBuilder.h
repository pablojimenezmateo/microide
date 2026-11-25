#pragma once

#include <memory>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

#include "util/InlineVector.h"
#include "util/SmallVector.h"
#include "editor/EditorInsetLayout.h"
#include "editor/EditorViewModel.h"
#include "editor/PluginDecorationStore.h"
#include "editor/WelcomeView.h"
#include "workspace/debug/DebugViewModel.h"
#include "workspace/git/GitSidebarCommandCenter.h"
#include "workspace/git/WorkspaceGitSidebarPresentation.h"
#include "workspace/render/NotificationLayout.h"
#include "workspace/services/NotificationService.h"
#include "workspace/services/TabStripService.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/services/SettingsOverlayService.h"
#include "workspace/services/StatusBarService.h"
#include "workspace/state/WorkspaceTabState.h"

#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace microide::render {
class TextRenderer;
}

namespace microide::editor {
struct SurfaceContent;
}

namespace microide::workspace {

class DebugVariablesModel;
class DebugWatchModel;
class DebugBreakpointsModel;

int ParseStickyScrollMaxDepthSetting(const std::optional<std::string>& value);

void ComputeStickyScrollLinesUncached(const editor::TextViewport& viewport,
                                      const editor::FoldingModel* folding_model,
                                      bool sticky_scroll_enabled,
                                      int sticky_max_depth,
                                      std::size_t pane_rows,
                                      std::vector<std::size_t>& out_opener_lines);

struct EditorBannerViewModel {
  std::span<const EditorBannerButton> buttons;
  std::string message;
};

struct NotificationButtonViewModel {
  std::string_view label;
  float label_width = 0.0f;
  SDL_FRect rect{};
  bool hovered = false;
  bool focused = false;
};

struct NotificationEntryViewModel {
  std::size_t index = 0;
  NotificationService::Tone tone = NotificationService::Tone::Info;
  std::string_view message;
  util::InlineVector<std::string_view, kNotificationMaxMessageLines> lines;
  std::optional<float> progress;
  NotificationToastLayout layout{};
  util::InlineVector<NotificationButtonViewModel, NotificationService::MaxActions()> buttons;
  bool focused = false;
};

inline constexpr std::size_t kMaxNotificationRows =
    NotificationService::MaxVisible() + NotificationService::MaxSticky();

struct NotificationsViewModel {
  util::InlineVector<NotificationEntryViewModel, kMaxNotificationRows> entries;
};

struct NotificationHit {
  std::size_t index = 0;
  std::optional<std::size_t> action;
};

std::optional<NotificationHit> NotificationHitAt(const NotificationsViewModel& vm, float x,
                                                 float y);

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

struct OverlayListRowViewModel {
  std::string_view primary;
  std::string_view secondary;
  float secondary_width = 0.0f;
};

struct FindWidgetToggleViewModel {
  std::string_view label;
  bool active = false;
};

struct OverlayFindWidgetViewModel {
  FindWidgetLayout fw{};
  bool replace_mode = false;
  bool search_focused = false;
  bool replace_focused = false;
  std::array<FindWidgetToggleViewModel, kFindWidgetMaxToggles> toggles{};
  bool has_matches = false;
  bool has_query = false;
  std::string_view search_display_text;
  std::string_view replace_display_text;
  std::string_view count_text;
};

inline constexpr float kOverlayInset = 18.0f;

struct OverlaySurfaceViewModel {
  bool visible = false;
  OverlayMode mode = OverlayMode::FileFinder;
  int scroll_row = 0;
  TextInputSurface current_surface = TextInputSurface::None;
  std::string_view buffer_search_query_text;

  bool caret_anchored = false;
  SDL_FRect overlay_rect{};
  ScrollableListLayout list_layout{};
  std::size_t total_rows = 0;
  int selected_row = 0;

  std::string_view title;
  std::string_view note;
  float note_x = 0.0f;
  std::string_view context_label;
  bool has_query_field = false;
  TextInputSurface query_surface = TextInputSurface::None;
  float query_row_y = 0.0f;
  std::string_view query_display_text;
  std::string_view summary_line;
  float summary_y = 0.0f;
  std::string_view hint;
  float hint_x = 0.0f;
  std::string_view error_line;
  bool error_at_title_row = false;
  std::string_view empty_label;

  std::vector<OverlayListRowViewModel> rows;
  OverlayFindWidgetViewModel find_widget;

  std::string label_storage;
};

struct TextInputSurfaceViewModel {
  TextInputSurface current_surface = TextInputSurface::None;
  const editor::SingleLineEditor* prompt_input = nullptr;
  const editor::SingleLineEditor* buffer_search_query = nullptr;
  const editor::SingleLineEditor* buffer_search_replace = nullptr;
  const editor::SingleLineEditor* project_search_edit_buffer = nullptr;
  const editor::SingleLineEditor* commit_picker_query = nullptr;
  const editor::SingleLineEditor* quick_pick_query = nullptr;
  const editor::SingleLineEditor* command_palette_query = nullptr;
  const editor::SingleLineEditor* file_finder_query = nullptr;
};

struct SidebarSurfaceViewModel {
  bool visible = false;
  SidebarMode mode = SidebarMode::Tree;
  int scroll_row = 0;
  bool project_search_editing = false;
  std::string_view query_fallback_text;
  std::string_view replace_fallback_text;
  std::string_view include_fallback_text;
  std::string_view exclude_fallback_text;
  bool project_search_scope_expanded = false;
  std::string_view project_search_status_text;
  std::string_view project_search_empty_text;
  bool project_search_empty_is_error = false;
  std::shared_ptr<const GitSidebarPresentation> git_presentation;
  const GitSidebarViewModel* git_sidebar = nullptr;
  const std::vector<GitSidebarLine>* git_sidebar_lines = nullptr;
  bool commit_field_owns_caret = false;
  ProjectWorkspaceState* project_state = nullptr;
};

struct BottomPanelTabDragViewModel {
  bool active = false;
  std::size_t source_index = 0;
  float pointer_x = 0.0f;
  float grab_offset_x = 0.0f;
  bool sliding = false;
  std::span<const float> offsets;
};

struct BottomPanelSurfaceViewModel {
  PanelContentKind content = PanelContentKind::None;
  float height = 0.0f;
  std::string_view output_channel_id;
  const std::filesystem::path* project_root = nullptr;
  FocusTarget focus = FocusTarget::Sidebar;
  BottomPanelTabDragViewModel tab_drag;
  const std::vector<VisibleStripTab>* tabs = nullptr;
  TabStripOverflowControls tab_overflow{};
  const editor::SurfaceContent* plugin_surface = nullptr;
  float plugin_surface_scroll_y = 0.0f;
  std::string_view empty_label;

  bool find_visible = false;
  OverlayFindWidgetViewModel find{};
  const editor::SingleLineEditor* find_query = nullptr;
  const std::vector<terminal::TerminalSearchMatch>* find_matches = nullptr;
  std::size_t find_selected_index = 0;

  TerminalTabState* terminal_tab = nullptr;
  TerminalPaneRectsLayout terminal_panes;
};

struct DebugPaneSurfaceViewModel {
  bool visible = false;
  DebugPaneMode mode = DebugPaneMode::CallStack;
  int scroll_row = 0;
  std::string_view header_label;
  FocusTarget focus = FocusTarget::Sidebar;
  const DebugExecutionView* execution = nullptr;
  const DebugVariablesModel* variables = nullptr;
  const DebugWatchModel* watch = nullptr;
  const DebugBreakpointsModel* breakpoints = nullptr;
  int breakpoints_selected_row = -1;
};

struct HoverPopupViewModel {
  bool visible = false;
  bool has_active_target = false;
};

struct HoverTargetsViewModel {
  bool hover_enabled = false;
  const editor::DiagnosticsStore* diagnostics_store = nullptr;
  const editor::PluginDecorationStore* decoration_store = nullptr;
  const DebugExecutionView* debug_execution = nullptr;
  const DebugHoverModel* debug_hover = nullptr;
};

struct StatusBarSegmentViewModel {
  StatusBarSegmentId id = StatusBarSegmentId::Project;
  std::string_view text;
  std::string_view tooltip;
  bool clickable = false;
  std::string_view command;
  std::string_view command_arg;
  MenuId menu = MenuId::None;
  StatusTone tone = StatusTone::Default;
};

using StatusBarSegmentList = util::SmallVector<StatusBarSegmentViewModel, 8>;

struct StatusBarViewModel {
  bool visible = false;
  SDL_FRect rect{};
  LayoutMode layout_mode = LayoutMode::Regular;
  StatusBarSegmentList left_segments;
  StatusBarSegmentList right_segments;
};

template <typename TextRendererT, typename Visit>
void ForEachStatusBarSegmentRect(const StatusBarViewModel& vm,
                                 const TextRendererT& text_renderer,
                                 Visit&& visit) {
  constexpr float kStatusPadding = 12.0f;
  constexpr float kStatusGap = 14.0f;
  const float left_edge = vm.rect.x + kStatusPadding;
  const float right_edge = vm.rect.x + vm.rect.w - kStatusPadding;

  std::size_t right_fit = 0;
  float right_start = right_edge;
  {
    float right_x = right_edge;
    for (std::size_t i = vm.right_segments.size(); i > 0; --i) {
      const float width = text_renderer.MeasureWidth(vm.right_segments[i - 1].text);
      if (right_x - width < left_edge) {
        break;
      }
      right_x -= width;
      right_start = right_x;
      ++right_fit;
      right_x -= kStatusGap;
    }
  }
  const float left_limit = right_fit > 0 ? right_start - kStatusGap : right_edge;

  float left_x = left_edge;
  for (const StatusBarSegmentViewModel& segment : vm.left_segments) {
    const float width = text_renderer.MeasureWidth(segment.text);
    if (left_x + width > left_limit) {
      break;
    }
    visit(segment, SDL_FRect{left_x, vm.rect.y, width, vm.rect.h});
    left_x += width + kStatusGap;
  }
  float right_x = right_edge;
  const std::size_t right_count = vm.right_segments.size();
  for (std::size_t i = right_count; i > right_count - right_fit; --i) {
    const StatusBarSegmentViewModel& segment = vm.right_segments[i - 1];
    const float width = text_renderer.MeasureWidth(segment.text);
    right_x -= width;
    visit(segment, SDL_FRect{right_x, vm.rect.y, width, vm.rect.h});
    right_x -= kStatusGap;
  }
}

struct SettingsCategoryViewModel {
  std::string_view label;
  SDL_FRect rect{};
  bool selected = false;
};

struct SettingsControlViewModel {
  SettingsControlKind kind = SettingsControlKind::None;
  bool checkbox_on = false;
  std::string_view display_value;
  bool editing = false;
  std::size_t edit_caret = 0;
  std::string shown_value;
  bool value_is_placeholder = false;
  float caret_offset_x = 0.0f;
  SDL_FRect checkbox_rect{};
  SDL_FRect dec_rect{};
  SDL_FRect inc_rect{};
  SDL_FRect value_rect{};
};

struct SettingsRowViewModel {
  std::string_view id;
  std::string_view label;
  std::string_view description;
  std::vector<std::string_view> description_lines;
  float scope_label_x = -1.0f;
  std::string_view scope_label;
  std::string_view group_subheader;
  SettingsControlViewModel control;
  SDL_FRect row_rect{};
  SDL_FRect reset_rect{};
  SDL_FRect scope_rect{};
  std::string_view scope_text;
  std::string_view scope_help;
  bool scope_is_project = false;
  int row_in_category = 0;
  float advance_height = 0.0f;
  bool selected = false;
  bool resettable = false;
};

struct SettingsPickerItemViewModel {
  std::string_view text;
  SDL_FRect rect{};
  bool highlighted = false;
  bool is_choose_file = false;
  int dropdown_index = -1;
};

struct SettingsPickerViewModel {
  bool visible = false;
  SDL_FRect rect{};
  std::vector<SettingsPickerItemViewModel> items;
  std::optional<ScrollbarGeometry> scrollbar;
  int max_scroll = 0;
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
  int visible_rows = 0;
  std::optional<ScrollbarGeometry> scrollbar;
  int category_scroll_row = 0;
  int category_max_scroll = 0;
  int category_visible_rows = 0;
  std::optional<ScrollbarGeometry> category_scrollbar;
  SettingsPane focused_pane = SettingsPane::Filter;
  std::string_view title;
  std::string_view query;
  std::string_view query_display() const {
    return query_display_owned ? std::string_view(query_display_storage) : query_display_view;
  }
  std::string_view query_display_view;
  std::string query_display_storage;
  bool query_display_owned = false;
  float query_caret_x = 0.0f;
  std::string_view filter_placeholder;
  bool query_empty = true;
  std::vector<SettingsCategoryViewModel> categories;
  SDL_FRect section_header_rect{};
  std::string_view section_title;
  std::string_view section_subtitle;
  std::vector<SettingsRowViewModel> rows;
  std::span<const HelpAboutRow> help_rows;
  SDL_FRect footer_rect{};
  std::string footer_summary;
  std::string_view footer_hint;
  std::string_view empty_label;
  SDL_FRect help_list_rect{};
  float help_label_column = 0.0f;
  float help_detail_x = 0.0f;
  float help_detail_width = 0.0f;
  float help_entry_gap = 0.0f;
  SettingsPickerViewModel value_picker;
};

inline bool SettingsOverlayContains(const SettingsOverlayViewModel& vm, float x, float y) {
  const auto inside = [x, y](const SDL_FRect& rect) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
  };
  return inside(vm.rect) || (vm.value_picker.visible && inside(vm.value_picker.rect));
}

inline bool SettingsRowShownIn(const SDL_FRect& pane, const SDL_FRect& rect) {
  const float bottom = pane.y + pane.h;
  if (rect.y < pane.y - 0.5f || rect.y >= bottom) {
    return false;
  }
  return rect.y + rect.h <= bottom + 0.5f || rect.h > pane.h;
}

inline SDL_FRect SettingsRowVisibleRect(const SDL_FRect& pane, const SDL_FRect& rect) {
  const float top = std::max(rect.y, pane.y);
  const float bottom = std::min(rect.y + rect.h, pane.y + pane.h);
  return SDL_FRect{rect.x, top, rect.w, std::max(0.0f, bottom - top)};
}

class RenderViewModelBuilder {
 public:
  explicit RenderViewModelBuilder(const WorkspaceContext& context);

  FrameSurfaceViewModel BuildFrameSurface(const WorkspaceLayout& layout) const;
  void BuildOverlaySurfaceInto(OverlaySurfaceViewModel& out,
                               const WorkspaceLayout& layout,
                               const SDL_FRect& overlay_rect,
                               const render::TextRenderer& text_renderer) const;
  TextInputSurfaceViewModel BuildTextInputSurface() const;
  SidebarSurfaceViewModel BuildSidebarSurface() const;
  DebugPaneSurfaceViewModel BuildDebugPaneSurface() const;
  std::span<const editor::OccurrenceRange> OccurrenceRanges(const editor::TextViewport& viewport,
                                                             std::size_t visible_rows,
                                                             bool occurrences_case_sensitive) const;

  static std::span<const std::size_t> StickyScrollLines(const editor::TextViewport& viewport,
                                                        const editor::FoldingModel* folding_model,
                                                        bool sticky_scroll_enabled,
                                                        int sticky_max_depth,
                                                        std::size_t pane_rows);

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
  HoverPopupViewModel BuildHoverPopup(bool has_active_target, bool modal_open = false) const;
  HoverTargetsViewModel BuildHoverTargets(bool debug_hover_enabled = false) const;
  StatusBarViewModel BuildStatusBar(const WorkspaceLayout& layout,
                                    const class StatusBarService& service) const;
  NotificationsViewModel BuildNotifications(const NotificationService& service,
                                            const SDL_FRect& status_bar,
                                            const render::TextRenderer& text_renderer) const;
  const editor::WelcomeViewModel& BuildWelcomeView(const class RecentsService& recents) const;
  SettingsOverlayViewModel BuildSettingsOverlay(
      const WorkspaceLayout& layout,
      const class SettingsOverlayService& service,
      const render::TextRenderer& text_renderer) const;

  static void ResetStickyScrollCacheForTesting();
  static std::uint64_t StickyScrollCacheHitsForTesting();
  static std::uint64_t StickyScrollCacheMissesForTesting();
  static void ResetWelcomeViewCacheForTesting();
  static std::uint64_t WelcomeViewCacheHitsForTesting();
  static std::uint64_t WelcomeViewCacheMissesForTesting();

  static void ResetOccurrenceCachesForTesting();
  static std::uint64_t OccurrenceSeedCacheHitsForTesting();
  static std::uint64_t OccurrenceSeedCacheMissesForTesting();
  static std::uint64_t OccurrenceScanCacheHitsForTesting();
  static std::uint64_t OccurrenceScanCacheMissesForTesting();

 private:
  void BuildEditorInsetGaps(editor::EditorViewModel& out, const editor::TextViewport& viewport,
                            std::size_t visible_rows, editor::InsetGapFeatureFlags inset_flags,
                            float line_height) const;

  const WorkspaceContext& context_;
};

}
