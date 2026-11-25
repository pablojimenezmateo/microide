#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "compare/CompareModel.h"
#include "compare/ComparePresentationModel.h"
#include "compare/MergeModel.h"
#include "util/InlineVector.h"
#include "workspace/EditorSplitTree.h"
#include "workspace/RectsEqual.h"

namespace microide::workspace {

enum class LayoutMode : std::uint8_t {
  Regular = 0,
  Compact = 1,
};

struct LayoutModeInputs {
  enum class Override : std::uint8_t { Auto = 0, Regular = 1, Compact = 2 };
  Override user_override = Override::Auto;
  float compact_breakpoint_px = 720.0f;
  LayoutMode previous_mode = LayoutMode::Regular;

  friend bool operator==(const LayoutModeInputs&, const LayoutModeInputs&) = default;
};

struct WorkspaceLayoutInputs {
  float window_width = 0.0f;
  float window_height = 0.0f;
  bool sidebar_visible = false;
  bool bottom_panel_visible = false;
  float sidebar_width = 0.0f;
  float bottom_panel_height = 0.0f;
  LayoutModeInputs layout_mode{};
  bool reserve_status_bar = false;
  bool right_pane_visible = false;
  float right_pane_width = 0.0f;
  bool project_tab_strip_visible = true;
  bool bottom_panel_maximized = false;

  friend bool operator==(const WorkspaceLayoutInputs&, const WorkspaceLayoutInputs&) = default;
};

struct WorkspaceLayout {
  SDL_FRect full{};
  SDL_FRect menu_bar{};
  SDL_FRect project_tab_strip{};
  SDL_FRect tab_strip{};
  SDL_FRect bottom_panel{};
  SDL_FRect content{};
  SDL_FRect sidebar{};
  SDL_FRect editor_area{};
  SDL_FRect right_pane{};
  SDL_FRect breadcrumb{};
  SDL_FRect editor_surface{};
  SDL_FRect status_bar{};
  SDL_FRect overlay_anchor{};
  LayoutMode layout_mode = LayoutMode::Regular;
  bool panel_maximized = false;
};

bool operator==(const WorkspaceLayout& lhs, const WorkspaceLayout& rhs) noexcept;
inline bool operator!=(const WorkspaceLayout& lhs, const WorkspaceLayout& rhs) noexcept {
  return !(lhs == rhs);
}

struct ScrollbarGeometry {
  SDL_FRect track{};
  SDL_FRect thumb{};
  float total_units = 0.0f;
  float visible_units = 0.0f;
  float scroll_units = 0.0f;
  bool vertical = true;
};

struct CompareScrollbarRun {
  compare::CompareRowKind kind = compare::CompareRowKind::Unchanged;
  bool layout_only = false;
  bool moved = false;
  int start_row = 0;
  int end_row = 0;
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
  compare::MergeChoice bootstrap_choice = compare::MergeChoice::Base;
  bool valid = true;
  bool resolved = false;
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
  bool operator==(const MergeHoverState&) const = default;
};

struct MergeHoverSurfaceLayout {
  float gutter_width = 28.0f;
  float left_x = 0.0f;
  float center_x = 0.0f;
  float right_x = 0.0f;
  float rows_y = 0.0f;
  float line_height = 14.0f;
  float left_width = 0.0f;
  float center_width = 0.0f;
  float right_width = 0.0f;
};

struct MergeHoverResultLayout {
  SDL_FRect rect{};
  VisibleLineRangeLayout lines{};
  TextGridInteractionLayout text{};
};

inline bool MergeRowsOnScreen(std::size_t start, std::size_t end, std::size_t scroll_line,
                              std::size_t visible_rows) {
  return start < scroll_line + visible_rows && std::max(end, start + 1) > scroll_line;
}

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

struct EditorGroupRects {
  SDL_FRect tab_strip{};
  SDL_FRect breadcrumb{};
  SDL_FRect editor_surface{};
};

struct EditorSplitDividerRect {
  SDL_FRect rect{};
  std::uint8_t node = 0;
  std::uint8_t boundary = 0;
  bool vertical = true;
  float pair_start = 0.0f;
  float pair_extent = 0.0f;
};

struct EditorGroupRectsLayout {
  util::InlineVector<EditorGroupRects, kMaxEditorGroups> groups;
  util::InlineVector<EditorSplitDividerRect, kMaxEditorGroups - 1> dividers;
};

inline constexpr float kWorkspaceHeaderHeight = 26.0f;
inline constexpr float kWorkspaceDividerThickness = 1.0f;
inline constexpr float kWorkspaceResizeHandleThickness = 6.0f;
inline constexpr float kWorkspaceScrollbarThickness = 10.0f;
inline constexpr float kWorkspaceScrollbarInset = 2.0f;
inline constexpr float kWorkspaceScrollbarMinThumbLength = 24.0f;
inline constexpr float kWorkspaceMaxRightPaneWidth = 520.0f;
inline constexpr float kWorkspaceDefaultSidebarWidth = 288.0f;
inline constexpr float kWorkspaceDefaultBottomPanelHeight = 156.0f;
inline constexpr float kWorkspaceDefaultRightPaneWidth = 320.0f;
inline constexpr float kWorkspaceDefaultEditorSplitFraction = 0.5f;
inline constexpr float kWorkspaceDefaultCompareDividerFraction = 0.5f;
inline constexpr float kWorkspaceDefaultMergeLeftDividerFraction = 1.0f / 3.0f;
inline constexpr float kWorkspaceDefaultMergeRightDividerFraction = 2.0f / 3.0f;
inline constexpr float kWorkspaceMinEditorAreaWidth = 280.0f;
inline constexpr float kWorkspaceMinEditorAreaHeight = 120.0f;
inline constexpr float kWorkspaceBottomPanelHeaderHeight = 28.0f;
inline constexpr float kWorkspaceOverlayMaxHeight = 360.0f;
inline constexpr float kWorkspaceEditorSplitDividerThickness = 6.0f;
inline constexpr float kWorkspaceSidebarRowHeight = 20.0f;
inline constexpr float kWorkspaceSidebarTreeIndentWidth = 14.0f;
inline constexpr float kWorkspaceSidebarTreeChevronSlotWidth = 12.0f;
inline constexpr float kWorkspaceSidebarHorizontalInset = 10.0f;
inline constexpr float kWorkspaceWindowControlButtonGap = 4.0f;
inline constexpr float kWorkspaceWindowControlButtonRightInset = 8.0f;
inline constexpr float kWorkspaceTabCloseButtonSize = 14.0f;
inline constexpr float kWorkspaceTabCloseButtonRightInset = 6.0f;
inline constexpr float kWorkspaceMenuPopupSeparatorHeight = 8.0f;
inline constexpr float kWorkspaceMenuPopupItemHeight = 22.0f;
inline constexpr float kWorkspaceDiffScrollbarReserve = 12.0f;
inline constexpr float kWorkspaceStatusBarHeight = 22.0f;
inline constexpr float kWorkspaceMenuOverflowChevronWidth = 28.0f;
inline constexpr float kWorkspaceTabCloseHitInflate = 3.0f;
inline constexpr float kWorkspaceResizeHandleCursorInflate = 1.0f;
inline constexpr float kWorkspaceScrollbarHitInflate = 4.0f;
inline constexpr float kWorkspaceWindowControlButtonHitInflate = 2.0f;
inline constexpr float kWorkspaceLayoutCompactBreakpointDefault = 720.0f;
inline constexpr float kWorkspaceLayoutCompactHysteresis = 12.0f;
inline constexpr float kWorkspaceEditorBannerHeight = 30.0f;

enum class EditorBannerAction {
  Compare,
  Reload,
  Overwrite,
  Keep,
  Retry,
  OverwriteReadOnly,
  SaveAs,
};
struct EditorBannerButton {
  EditorBannerAction action = EditorBannerAction::Keep;
  std::string_view label;
  float width = 0.0f;
  bool destructive = false;
};
inline constexpr std::size_t kMaxEditorBannerButtons = 4;
struct EditorBannerButtonLayout {
  std::span<const EditorBannerButton> buttons;
  std::array<SDL_FRect, kMaxEditorBannerButtons> rects{};
  SDL_FRect dismiss{};
  [[nodiscard]] float controls_left() const { return buttons.empty() ? dismiss.x : rects[0].x; }
};

SDL_FRect MakeRect(float x, float y, float w, float h);

inline SDL_FRect DirtyRectWithHalo(const SDL_FRect& content) {
  return SDL_FRect{content.x - 1.0f, content.y - 1.0f, content.w + 2.0f, content.h + 2.0f};
}
WorkspaceLayout ComputeLayout(const WorkspaceLayoutInputs& inputs);
inline WorkspaceLayout ComputeLayout(float window_width,
                                     float window_height,
                                     bool sidebar_visible,
                                     bool bottom_panel_visible,
                                     float sidebar_width,
                                     float bottom_panel_height,
                                     LayoutModeInputs layout_mode_inputs = {},
                                     bool reserve_status_bar = false,
                                     bool right_pane_visible = false,
                                     float right_pane_width = 0.0f,
                                     bool project_tab_strip_visible = true) {
  return ComputeLayout(WorkspaceLayoutInputs{
      .window_width = window_width,
      .window_height = window_height,
      .sidebar_visible = sidebar_visible,
      .bottom_panel_visible = bottom_panel_visible,
      .sidebar_width = sidebar_width,
      .bottom_panel_height = bottom_panel_height,
      .layout_mode = layout_mode_inputs,
      .reserve_status_bar = reserve_status_bar,
      .right_pane_visible = right_pane_visible,
      .right_pane_width = right_pane_width,
      .project_tab_strip_visible = project_tab_strip_visible,
  });
}
EditorGroupRectsLayout ComputeEditorGroupRects(const WorkspaceLayout& layout,
                                               const EditorSplitTree& split);
SDL_FRect FocusedGroupSurface(const WorkspaceLayout& layout,
                              const EditorSplitTree& split,
                              std::size_t focused_group);
SDL_FRect FocusedGroupSurface(const EditorGroupRectsLayout& rects,
                              std::size_t focused_group,
                              const WorkspaceLayout& layout);

enum class EditorGroupDirection : std::uint8_t { Left, Right, Up, Down };

inline constexpr std::size_t kNoEditorGroup = static_cast<std::size_t>(-1);

std::size_t AdjacentEditorGroup(const EditorGroupRectsLayout& rects,
                                std::size_t from,
                                EditorGroupDirection direction);
inline bool Contains(const SDL_FRect& rect, float x, float y) {
  return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}
std::optional<SDL_FRect> UnionOptionalRects(std::optional<SDL_FRect> lhs,
                                            const SDL_FRect& rhs);
float ClampSidebarWidth(float width, float window_width);
float ClampRightPaneWidth(float width, float window_width, float sidebar_width);
bool RightPaneFits(float window_width, float sidebar_width);
float ClampBottomPanelHeight(float height, float window_height);
int BottomPanelVisibleRowsForHeight(float panel_height, float line_height);
std::optional<std::size_t> BottomPanelLineIndexAtY(float text_y,
                                                   float line_height,
                                                   int visible_rows,
                                                   int vertical_scroll,
                                                   float y,
                                                   std::size_t line_count);
int TailScrollRowForContent(std::size_t line_count, int visible_rows);
int ClampScrollRowToContent(int scroll_row, std::size_t line_count, int visible_rows);
SDL_FRect SidebarResizeHandleRect(const WorkspaceLayout& layout);
SDL_FRect BottomPanelResizeHandleRect(const WorkspaceLayout& layout);
SDL_FRect RightPaneResizeHandleRect(const WorkspaceLayout& layout);
SDL_FRect SidebarResizeCursorRect(const WorkspaceLayout& layout);
SDL_FRect BottomPanelResizeCursorRect(const WorkspaceLayout& layout);
SDL_FRect RightPaneResizeCursorRect(const WorkspaceLayout& layout);
SDL_FRect SidebarResizeHitRect(const WorkspaceLayout& layout);
SDL_FRect BottomPanelResizeHitRect(const WorkspaceLayout& layout);
SDL_FRect RightPaneResizeHitRect(const WorkspaceLayout& layout);
SDL_FRect VerticalScrollbarHitRect(const ScrollbarGeometry& geometry);
SDL_FRect TabCloseHitRect(const SDL_FRect& close_visual_rect, const SDL_FRect& tab_rect);
SDL_FRect EmptyTabStripPlaceholderRect(const SDL_FRect& tab_strip);
SDL_FRect WindowControlButtonHitRect(const SDL_FRect& button_rect);
LayoutMode ResolveLayoutMode(float window_width, const LayoutModeInputs& inputs);
SDL_FRect ComputeMenuOverflowPopupRect(const SDL_FRect& chevron_rect, std::size_t item_count);
SDL_FRect MenuOverflowPopupRowRect(const SDL_FRect& popup, std::size_t index);
std::optional<std::size_t> MenuOverflowPopupRowAt(const SDL_FRect& popup,
                                                  std::size_t item_count,
                                                  float x,
                                                  float y);
SDL_FRect BottomPanelContentRect(const WorkspaceLayout& layout);
SDL_FRect ComputeDirtyPromptRect(const SDL_FRect& full);
std::array<SDL_FRect, 3> ComputeDirtyPromptButtonRects(const SDL_FRect& dialog);
SDL_FRect ComputeEditorBannerStripRect(const SDL_FRect& editor_surface);
EditorBannerButtonLayout ComputeEditorBannerButtonRects(const SDL_FRect& strip,
                                                        std::span<const EditorBannerButton> buttons);
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
int RevealedScrollRow(int scroll_row, int visible_rows, int selected_index);
int RevealScrollableListIndex(const ScrollableListLayout& layout, int selected_index);
std::optional<int> ScrollableListIndexAtY(const ScrollableListLayout& layout, float y);
SDL_FRect ScrollableListRowRect(const ScrollableListLayout& layout, int visible_row);
std::optional<std::size_t> ScrollableListItemAtY(const ScrollableListLayout& layout, float y,
                                                 std::size_t item_count);
std::optional<std::size_t> ScrollableListItemAt(const ScrollableListLayout& layout, float x,
                                                float y, std::size_t item_count);
SDL_FRect ScrollableListItemRect(const ScrollableListLayout& layout, std::size_t item_index);
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

float ScrollbarGrabOffset(const ScrollbarGeometry& geometry,
                          float pointer_coordinate,
                          bool vertical);

void BuildCompareScrollbarRunsInto(const compare::ComparePresentationModel& presentation,
                                   const compare::CompareModel& model,
                                   std::vector<CompareScrollbarRun>& runs);
float ComputeChromeButtonWidth(float measured_label_width);
void ComputeVisibleStripLayoutsInto(std::vector<StripSlotLayout>& out,
                                    const std::vector<float>& widths,
                                    float start_x,
                                    float gap,
                                    float max_x,
                                    std::size_t first_index);
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
void BuildChromeTabRenderItemsInto(std::vector<ChromeTabRenderItem>& out,
                                   std::span<const StripSlotLayout> slots,
                                   float tab_y,
                                   float tab_height,
                                   std::span<const std::size_t> model_indices,
                                   std::size_t active_index,
                                   std::span<const std::string> display_titles,
                                   std::span<const std::string> tooltip_labels,
                                   float close_button_size,
                                   float close_button_right_inset);
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
                                             float button_height,
                                             float pane_right);
std::array<SDL_FRect, 4> ComputeMergeResultActionButtonRects(
    float start_x,
    float rows_y,
    float content_bottom,
    const std::optional<SDL_FRect>& conflict_rect,
    const std::array<float, 4>& widths,
    float button_height,
    float button_gap,
    float pane_right);
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
SDL_FRect ComputeQuickOpenOverlaySurfaceRect(const SDL_FRect& editor_area, const SDL_FRect& bounds);
SDL_FRect ComputeSettingsOverlaySurfaceRect(const SDL_FRect& editor_area, const SDL_FRect& bounds);

inline constexpr std::size_t kFindWidgetMaxToggles = 4;

enum class BufferFindToggle : std::size_t {
  MatchCase = 0,
  WholeWord = 1,
  Regex = 2,
  InSelection = 3,
  Count = 4,
};
inline constexpr std::size_t kBufferFindToggleCount =
    static_cast<std::size_t>(BufferFindToggle::Count);
static_assert(kBufferFindToggleCount <= kFindWidgetMaxToggles);
inline constexpr std::size_t kTerminalFindToggleCount = 2;

struct FindWidgetLayout {
  SDL_FRect widget{};
  SDL_FRect search_field{};
  SDL_FRect replace_field{};
  SDL_FRect count_rect{};
  std::array<SDL_FRect, kFindWidgetMaxToggles> toggle_buttons{};
  std::size_t toggle_count = 0;
  SDL_FRect prev_button{};
  SDL_FRect next_button{};
  SDL_FRect close_button{};
  SDL_FRect replace_button{};
  SDL_FRect replace_all_button{};
  bool replace_mode = false;
};

SDL_FRect ComputeFindWidgetRect(const SDL_FRect& editor_area,
                                bool replace_mode,
                                std::optional<SDL_FRect> bounds = std::nullopt);
FindWidgetLayout ComputeFindWidgetLayout(const SDL_FRect& editor_area,
                                         bool replace_mode,
                                         std::size_t toggle_count,
                                         std::optional<SDL_FRect> bounds = std::nullopt);

inline FindWidgetLayout ComputeBufferFindWidgetLayout(const SDL_FRect& editor_area,
                                                      bool replace_mode) {
  FindWidgetLayout fw = ComputeFindWidgetLayout(editor_area, replace_mode, kBufferFindToggleCount);
  if (fw.widget.y + fw.widget.h > editor_area.y + editor_area.h) {
    FindWidgetLayout none;
    none.replace_mode = replace_mode;
    none.toggle_count = fw.toggle_count;
    return none;
  }
  return fw;
}
FindWidgetLayout ComputeTerminalFindWidgetLayout(const SDL_FRect& terminal_body,
                                                 const WorkspaceLayout& layout);

enum class DebugToolbarButton : std::uint8_t {
  ContinuePause = 0,
  StepOver,
  StepInto,
  StepOut,
  ReverseContinue,
  StepBack,
  Restart,
  Stop,
  Count,
};

struct DebugToolbarLayout {
  SDL_FRect widget{};
  std::array<SDL_FRect, static_cast<std::size_t>(DebugToolbarButton::Count)> buttons{};
  std::array<DebugToolbarButton, static_cast<std::size_t>(DebugToolbarButton::Count)> kinds{};
  std::size_t button_count = 0;
};

DebugToolbarLayout ComputeDebugToolbarLayout(const SDL_FRect& editor_area,
                                             std::optional<float> avoid_below_y,
                                             bool include_reverse);

std::string_view DebugToolbarButtonTooltip(DebugToolbarButton button, bool session_stopped);

}
