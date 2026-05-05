#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "compare/CompareModel.h"
#include "compare/MergeModel.h"

namespace microide::workspace {

struct WorkspaceLayout {
  SDL_FRect full{};
  SDL_FRect menu_bar{};
  SDL_FRect project_tab_strip{};
  SDL_FRect tab_strip{};
  SDL_FRect bottom_panel{};
  SDL_FRect content{};
  SDL_FRect sidebar{};
  SDL_FRect editor_area{};
  SDL_FRect breadcrumb{};
  SDL_FRect editor_surface{};
};

struct ScrollbarGeometry {
  SDL_FRect track{};
  SDL_FRect thumb{};
  float total_units = 0.0f;
  float visible_units = 0.0f;
  float scroll_units = 0.0f;
  bool vertical = true;
};

struct CompareScrollbarMarker {
  compare::CompareRowKind kind = compare::CompareRowKind::Unchanged;
  int start_row = 0;
  int end_row = 0;
  SDL_FRect rect{};
};

struct MergeScrollbarMarkerInput {
  int start_row = 0;
  int end_row = 0;
  compare::MergeChoice choice = compare::MergeChoice::Base;
  bool valid = true;
};

struct MergeScrollbarMarker {
  int start_row = 0;
  int end_row = 0;
  compare::MergeChoice choice = compare::MergeChoice::Base;
  bool valid = true;
  SDL_FRect rect{};
};

struct StripSlotLayout {
  std::size_t index = 0;
  float x = 0.0f;
  float width = 0.0f;
};

struct ChromeTabRenderItem {
  std::size_t index = 0;
  SDL_FRect rect{};
  SDL_FRect close_rect{};
  bool active = false;
  std::string display_title;
  std::string tooltip_label;
};

struct VisibleLineRangeLayout {
  float first_line_y = 0.0f;
  float line_height = 14.0f;
  std::size_t scroll_line = 0;
  std::size_t visible_rows = 0;
};

struct TextGridInteractionLayout {
  SDL_FRect rect{};
  float text_x = 0.0f;
  float first_line_y = 0.0f;
  float line_height = 14.0f;
  float char_width = 1.0f;
  std::size_t scroll_line = 0;
  std::size_t line_count = 0;
  std::size_t horizontal_scroll = 0;
  std::size_t visible_rows = 1;
  std::size_t visible_columns = 1;
};

struct MergeTrackedConflict {
  std::size_t hunk_index = 0;
  std::size_t incoming_start_line = 0;
  std::size_t incoming_end_line = 0;
  std::size_t current_start_line = 0;
  std::size_t current_end_line = 0;
  std::size_t start_line = 0;
  std::size_t end_line = 0;
  compare::MergeChoice last_choice = compare::MergeChoice::Base;
  bool valid = true;
};

struct MergeHoverState {
  enum class Kind {
    None,
    IncomingConflict,
    IncomingAccept,
    CurrentConflict,
    CurrentAccept,
    ResultConflict,
    ResultAction,
  };

  Kind kind = Kind::None;
  std::size_t conflict_index = 0;
  compare::MergeChoice preview_choice = compare::MergeChoice::Base;
};

struct MergeHoverSurfaceLayout {
  float gutter_width = 28.0f;
  float left_x = 0.0f;
  float center_x = 0.0f;
  float right_x = 0.0f;
  float rows_y = 0.0f;
  float line_height = 14.0f;
};

struct MergeHoverResultLayout {
  SDL_FRect rect{};
  VisibleLineRangeLayout lines{};
  TextGridInteractionLayout text{};
};

struct MergeHoverInteractionLayout {
  float content_bottom = 0.0f;
  TextGridInteractionLayout incoming{};
  TextGridInteractionLayout current{};
  MergeHoverResultLayout result{};
  float incoming_accept_button_width = 0.0f;
  float current_accept_button_width = 0.0f;
  std::array<float, 4> result_action_widths{};
  float button_height = 0.0f;
  float button_gap = 0.0f;
};

struct ScrollableListLayout {
  SDL_FRect list_rect{};
  std::optional<ScrollbarGeometry> scrollbar;
  float row_x = 0.0f;
  float row_y = 0.0f;
  float row_width = 0.0f;
  float row_step = 0.0f;
  float row_height = 0.0f;
  float visible_units = 0.0f;
  int visible_rows = 0;
  int max_scroll = 0;
  int scroll_row = 0;
};

struct ScrollSurfaceLayout {
  SDL_FRect content_rect{};
  std::optional<ScrollbarGeometry> vertical_scrollbar;
  std::optional<ScrollbarGeometry> horizontal_scrollbar;
  bool show_vertical = false;
  bool show_horizontal = false;
  int visible_rows = 1;
  int vertical_scroll = 0;
  int max_vertical_scroll = 0;
  std::size_t visible_columns = 1;
  std::size_t horizontal_scroll = 0;
  std::size_t max_horizontal_scroll = 0;
};

struct EditorSplitAxisLayout {
  bool vertical = true;
  float total_extent = 0.0f;
  float divider_thickness = 0.0f;
  float min_pane_extent = 0.0f;
  std::vector<float> extents;
  std::vector<SDL_FRect> child_rects;
  std::vector<SDL_FRect> divider_rects;
};

inline constexpr float kWorkspaceHeaderHeight = 26.0f;
inline constexpr float kWorkspaceDividerThickness = 1.0f;
inline constexpr float kWorkspaceResizeHandleThickness = 6.0f;
inline constexpr float kWorkspaceScrollbarThickness = 10.0f;
inline constexpr float kWorkspaceScrollbarInset = 2.0f;
inline constexpr float kWorkspaceScrollbarMinThumbLength = 24.0f;
inline constexpr float kWorkspaceMaxSidebarWidth = 520.0f;
inline constexpr float kWorkspaceMinEditorAreaWidth = 280.0f;
inline constexpr float kWorkspaceMinEditorAreaHeight = 120.0f;
inline constexpr float kWorkspaceBottomPanelHeaderHeight = 28.0f;
inline constexpr float kWorkspaceBottomPanelCommandReserveHeight = 56.0f;
inline constexpr float kWorkspaceBottomPanelCommandPromptHeight = 18.0f;
inline constexpr float kWorkspaceBottomPanelCommandInset = 10.0f;
inline constexpr float kWorkspaceBottomPanelCommandTopPadding = 8.0f;
inline constexpr float kWorkspaceBottomPanelCommandBottomPadding = 8.0f;
inline constexpr float kWorkspaceOverlayMaxHeight = 360.0f;
inline constexpr float kWorkspaceEditorSplitDividerThickness = 6.0f;
inline constexpr float kWorkspaceSidebarRowHeight = 20.0f;
inline constexpr float kWorkspaceSidebarHorizontalInset = 10.0f;
inline constexpr float kWorkspaceWindowControlButtonGap = 4.0f;
inline constexpr float kWorkspaceWindowControlButtonRightInset = 8.0f;
inline constexpr float kWorkspaceTabCloseButtonSize = 14.0f;
inline constexpr float kWorkspaceTabCloseButtonRightInset = 6.0f;
inline constexpr float kWorkspaceMenuPopupSeparatorHeight = 8.0f;
inline constexpr float kWorkspaceMenuPopupItemHeight = 22.0f;
inline constexpr float kWorkspaceDiffScrollbarReserve = 12.0f;
inline constexpr float kWorkspaceDiffMarkerLaneWidth = 6.0f;
inline constexpr float kWorkspaceDiffMarkerLaneGap = 3.0f;

SDL_FRect MakeRect(float x, float y, float w, float h);
WorkspaceLayout ComputeLayout(float window_width,
                              float window_height,
                              bool sidebar_visible,
                              bool bottom_panel_visible,
                              float sidebar_width,
                              float bottom_panel_height);
std::optional<EditorSplitAxisLayout> ComputeEditorSplitAxisLayout(
    const SDL_FRect& rect,
    bool vertical,
    std::span<const float> size_fractions);
bool Contains(const SDL_FRect& rect, float x, float y);
float ClampSidebarWidth(float width, float window_width);
float ClampBottomPanelHeight(float height, float window_height);
int BottomPanelVisibleRowsForHeight(float panel_height, float line_height, bool command_mode);
int TailScrollRowForContent(std::size_t line_count, int visible_rows);
int ClampScrollRowToContent(int scroll_row, std::size_t line_count, int visible_rows);
SDL_FRect SidebarResizeHandleRect(const WorkspaceLayout& layout);
SDL_FRect BottomPanelResizeHandleRect(const WorkspaceLayout& layout);
float BottomPanelCommandReservedHeight(bool command_mode);
SDL_FRect BottomPanelContentRect(const WorkspaceLayout& layout, bool command_mode);
SDL_FRect BottomPanelCommandAreaRect(const WorkspaceLayout& layout);
SDL_FRect BottomPanelCommandPromptRect(const WorkspaceLayout& layout);
SDL_FRect ComputeDirtyPromptRect(const SDL_FRect& full);
std::array<SDL_FRect, 3> ComputeDirtyPromptButtonRects(const SDL_FRect& dialog);
SDL_FRect ComputePromptSurfaceRect(const SDL_FRect& full);
std::vector<SDL_FRect> ComputePromptSurfaceButtonRects(const SDL_FRect& dialog,
                                                       int button_count = 2);
SDL_FRect ComputePromptSurfaceInputRect(const SDL_FRect& dialog);
TextGridInteractionLayout ComputeTextGridInteractionLayout(
    const SDL_FRect& rect,
    float text_x,
    float first_line_y,
    float line_height,
    float char_width,
    std::size_t scroll_line,
    std::size_t line_count,
    std::size_t horizontal_scroll,
    std::size_t visible_rows,
    std::size_t visible_columns);
std::optional<std::size_t> VisibleTextGridLineAtY(const TextGridInteractionLayout& layout, float y);
std::size_t ClampTextGridLineAtY(const TextGridInteractionLayout& layout, float y);
std::size_t TextGridVisualColumnAtX(const TextGridInteractionLayout& layout, float x);
float TextGridCursorX(const TextGridInteractionLayout& layout, std::size_t visual_column);
float TextGridLineY(const TextGridInteractionLayout& layout, std::size_t line_index);
ScrollSurfaceLayout ComputeScrollSurfaceLayout(const SDL_FRect& area,
                                               std::size_t total_rows,
                                               int visible_rows,
                                               int requested_vertical_scroll,
                                               std::size_t total_columns = 0,
                                               std::size_t visible_columns = 1,
                                               std::size_t requested_horizontal_scroll = 0);
ScrollableListLayout ComputeScrollableListLayout(const SDL_FRect& container,
                                                 float list_y,
                                                 std::size_t item_count,
                                                 int requested_scroll_row,
                                                 float horizontal_inset,
                                                 float row_step,
                                                 float row_height,
                                                 float list_bottom_padding = 0.0f,
                                                 float scrollbar_bottom_padding = 0.0f,
                                                 bool fractional_visible_units = false);
int RevealScrollableListIndex(const ScrollableListLayout& layout, int selected_index);
std::optional<int> ScrollableListIndexAtY(const ScrollableListLayout& layout, float y);
SDL_FRect ScrollableListRowRect(const ScrollableListLayout& layout, int visible_row);
std::optional<SDL_FRect> ComputeScrollbarThumb(const SDL_FRect& track,
                                               float total_units,
                                               float visible_units,
                                               float scroll_units,
                                               bool vertical);
std::optional<ScrollbarGeometry> MakeVerticalScrollbarGeometry(const SDL_FRect& area,
                                                              float total_units,
                                                              float visible_units,
                                                              float scroll_units,
                                                              bool reserve_horizontal = false);
std::optional<ScrollbarGeometry> MakeHorizontalScrollbarGeometry(const SDL_FRect& area,
                                                                float total_units,
                                                                float visible_units,
                                                                float scroll_units,
                                                                bool reserve_vertical = false);
float ScrollUnitsForPointer(const ScrollbarGeometry& geometry,
                            float pointer_coordinate,
                            float grab_offset);
std::vector<CompareScrollbarMarker> BuildCompareScrollbarMarkers(
    const SDL_FRect& track,
    const compare::CompareModel& model);
std::vector<MergeScrollbarMarker> BuildMergeScrollbarMarkers(
    const SDL_FRect& track,
    std::size_t total_rows,
    const std::vector<MergeScrollbarMarkerInput>& inputs);
float ComputeChromeButtonWidth(float measured_label_width);
std::vector<StripSlotLayout> ComputeVisibleStripLayouts(const std::vector<float>& widths,
                                                        float start_x,
                                                        float gap,
                                                        float max_x,
                                                        std::size_t first_index);
std::size_t EnsureVisibleStripIndex(const std::vector<float>& widths,
                                    float start_x,
                                    float gap,
                                    float max_x,
                                    std::size_t current_first_index,
                                    std::size_t active_index);
std::vector<ChromeTabRenderItem> BuildChromeTabRenderItems(
    std::span<const StripSlotLayout> slots,
    float tab_y,
    float tab_height,
    std::span<const std::size_t> model_indices,
    std::size_t active_index,
    std::span<const std::string> display_titles,
    std::span<const std::string> tooltip_labels,
    float close_button_size,
    float close_button_right_inset);

template <typename TabLike>
std::string HoveredChromeTabTooltipLabel(std::span<const TabLike> tabs, float x, float y) {
  for (const TabLike& tab : tabs) {
    if (Contains(tab.rect, x, y)) {
      return tab.tooltip_label;
    }
  }
  return {};
}

template <typename TabLike>
std::string HoveredChromeTabTooltipLabel(const std::vector<TabLike>& tabs, float x, float y) {
  return HoveredChromeTabTooltipLabel(std::span<const TabLike>(tabs), x, y);
}

SDL_FRect ComputeMergeResultViewportRect(const SDL_FRect& editor_surface,
                                         float center_x,
                                         float rows_y,
                                         float gutter_width,
                                         float center_width,
                                         bool show_horizontal);
std::optional<SDL_FRect> ComputeVisibleLineRangeRect(const SDL_FRect& viewport_rect,
                                                     const VisibleLineRangeLayout& layout,
                                                     std::size_t start_line,
                                                     std::size_t end_line);
SDL_FRect ComputeMergeSourceActionButtonRect(float pane_x,
                                             float gutter_width,
                                             float rows_y,
                                             float line_height,
                                             int scroll_row,
                                             std::size_t end_line,
                                             float content_bottom,
                                             float button_width,
                                             float button_height);
std::array<SDL_FRect, 4> ComputeMergeResultActionButtonRects(
    float start_x,
    float rows_y,
    float content_bottom,
    const std::optional<SDL_FRect>& conflict_rect,
    const std::array<float, 4>& widths,
    float button_height,
    float button_gap);
std::optional<std::size_t> FindMergeTrackedConflictAtSourceLine(
    std::span<const MergeTrackedConflict> conflicts,
    std::size_t line,
    bool incoming);
std::optional<std::size_t> FindMergeTrackedConflictAtResultLine(
    std::span<const MergeTrackedConflict> conflicts,
    std::size_t line);
std::optional<MergeHoverState> ClassifyMergeHoverState(
    const MergeHoverSurfaceLayout& surface,
    const MergeHoverInteractionLayout& interaction,
    std::span<const MergeTrackedConflict> conflicts,
    float x,
    float y);
SDL_FRect ComputeOverlaySurfaceRect(const SDL_FRect& editor_area);

}  // namespace microide::workspace
