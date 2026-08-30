#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "compare/CompareModel.h"
#include "compare/ComparePresentationModel.h"
#include "compare/MergeModel.h"
#include "util/InlineVector.h"
#include "workspace/EditorSplitTree.h"

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

// Every value `ComputeLayout` reads, as one struct. The layout is a pure function
// of this and nothing else, which is what lets the shell memoize it by comparing
// INPUTS rather than by trusting a dirty flag (TD-2026-08-30-280): a flag has to
// be raised at every site that changes any of these fields, and one missed site
// turns a hit test stale in the place a wrong rect is least visible; a key cannot
// be wrong, only unequal. Memberwise `==`, so a NaN never matches and simply
// recomputes.
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
  LayoutMode layout_mode = LayoutMode::Regular;
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

// A coalesced run of same-kind changed rows in the compare presentation, ready to be
// mapped onto the overview lane. Geometry + color are applied by the render TU via
// overview::BuildMarkers; this stays a pure, testable walk over the diff model.
struct CompareScrollbarRun {
  compare::CompareRowKind kind = compare::CompareRowKind::Unchanged;
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


// Rects for one editor group's chrome. `breadcrumb` has w==0 when the group has
// no breadcrumb band (any pane that does not start at the top of the editor
// area, which synthesizes only a tab strip above its surface).
struct EditorGroupRects {
  SDL_FRect tab_strip{};
  SDL_FRect breadcrumb{};
  SDL_FRect editor_surface{};
};

// One divider between two adjacent panes of a split branch. `node`/`boundary`
// address it in the tree (stable across a resize, which is what a live drag
// needs); `pair_start`/`pair_extent` are the axis span the two panes SHARE, so a
// drag converts a pointer position into that pair's share without re-walking the
// tree or knowing where the branch sits.
struct EditorSplitDividerRect {
  SDL_FRect rect{};
  std::uint8_t node = 0;
  std::uint8_t boundary = 0;
  bool vertical = true;  // true: separates side-by-side panes
  float pair_start = 0.0f;
  float pair_extent = 0.0f;
};

// Rects for every editor group carved out of the editor column, in visual order
// (== editor group order). For a single group the rects are byte-identical to
// `layout.tab_strip` / `.breadcrumb` / `.editor_surface`.
struct EditorGroupRectsLayout {
  // Heap-free: this is rebuilt on hit-test, cursor-shape, redraw-rect and render
  // paths (three times per mouse-motion event during a selection drag), and it
  // carries at most `kMaxEditorGroups` 48-byte structs — see TD-2026-08-06-145.
  util::InlineVector<EditorGroupRects, kMaxEditorGroups> groups;
  // One per adjacent pane pair, in the same visual order.
  util::InlineVector<EditorSplitDividerRect, kMaxEditorGroups - 1> dividers;
};

inline constexpr float kWorkspaceHeaderHeight = 26.0f;
inline constexpr float kWorkspaceDividerThickness = 1.0f;
inline constexpr float kWorkspaceResizeHandleThickness = 6.0f;
inline constexpr float kWorkspaceScrollbarThickness = 10.0f;
inline constexpr float kWorkspaceScrollbarInset = 2.0f;
inline constexpr float kWorkspaceScrollbarMinThumbLength = 24.0f;
inline constexpr float kWorkspaceMaxSidebarWidth = 520.0f;
inline constexpr float kWorkspaceMaxRightPaneWidth = 520.0f;
// Out-of-the-box pane sizes. These are what a fresh project gets, what a session
// restore falls back to when the persisted value is unusable, and what
// double-clicking a resize divider returns to — one definition so all three agree.
inline constexpr float kWorkspaceDefaultSidebarWidth = 288.0f;
inline constexpr float kWorkspaceDefaultBottomPanelHeight = 156.0f;
inline constexpr float kWorkspaceDefaultRightPaneWidth = 320.0f;
// An even split is the default for two editor groups, and what double-clicking the
// split divider restores.
inline constexpr float kWorkspaceDefaultEditorSplitFraction = 0.5f;
// Compare splits evenly; merge splits into equal thirds. Same three roles as the
// pane sizes above (fresh tab, session-restore fallback, double-click reset), and
// the values were previously written out twice — once in the tab state and once in
// the persisted-state defaults — with nothing tying them together.
inline constexpr float kWorkspaceDefaultCompareDividerFraction = 0.5f;
inline constexpr float kWorkspaceDefaultMergeLeftDividerFraction = 1.0f / 3.0f;
inline constexpr float kWorkspaceDefaultMergeRightDividerFraction = 2.0f / 3.0f;
inline constexpr float kWorkspaceMinEditorAreaWidth = 280.0f;
inline constexpr float kWorkspaceMinEditorAreaHeight = 120.0f;
inline constexpr float kWorkspaceBottomPanelHeaderHeight = 28.0f;
inline constexpr float kWorkspaceOverlayMaxHeight = 360.0f;
inline constexpr float kWorkspaceEditorSplitDividerThickness = 6.0f;
inline constexpr float kWorkspaceSidebarRowHeight = 20.0f;
// Tree-row geometry shared by the file tree, git tree, and plugin tree sidebars
// so render and click hit-testing agree on indentation and the disclosure slot.
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
// Resize dividers: the grab (hit) region equals the cursor-change region. A single
// inflate governs both so they can never drift — see *ResizeHitRect/*ResizeCursorRect.
inline constexpr float kWorkspaceResizeHandleCursorInflate = 1.0f;
inline constexpr float kWorkspaceScrollbarHitInflate = 4.0f;
inline constexpr float kWorkspaceWindowControlButtonHitInflate = 2.0f;
inline constexpr float kWorkspaceLayoutCompactBreakpointDefault = 720.0f;
inline constexpr float kWorkspaceLayoutCompactHysteresis = 12.0f;
inline constexpr float kWorkspaceEditorBannerHeight = 30.0f;

// Hit rects for the non-blocking editor banner. `reload`/`overwrite`/`keep` have
// w == 0 for the informational `ReloadedNotice` banner; `dismiss` (the X) is
// always present.
struct EditorBannerButtonLayout {
  SDL_FRect reload{};
  SDL_FRect overwrite{};
  SDL_FRect keep{};
  SDL_FRect dismiss{};
};

SDL_FRect MakeRect(float x, float y, float w, float h);

// Turns a content rect into the dirty rect that stands for it: one pixel of halo
// on every side, for antialiased glyph edges and 1px underlines/stripes that sit
// just outside the nominal row box.
//
// The three row-range redraw-rect builders (editor, compare, merge) each nudged
// the top up by a pixel and left the height alone, so the LAST pixel row of a
// partially repainted range was never invalidated — a stale sliver of whatever
// had been drawn there survived. The blame overlay made it visible: typing to
// dirty a buffer suppresses blame, but the bottom row of the blame line below the
// caret kept its old text.
inline SDL_FRect DirtyRectWithHalo(const SDL_FRect& content) {
  return SDL_FRect{content.x - 1.0f, content.y - 1.0f, content.w + 2.0f, content.h + 2.0f};
}
WorkspaceLayout ComputeLayout(const WorkspaceLayoutInputs& inputs);
// Positional form of the same function, for call sites that build the inputs
// inline (tests, mostly). Same result, by construction: it only packs the struct.
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
// Carve the editor column into one rect set per leaf of `split`, in visual order.
// A single-leaf tree hands back the layout's own strip/breadcrumb/surface rects
// unchanged; every deeper tree lays its branches out along their axis with the
// per-child weights and reports the dividers between them.
EditorGroupRectsLayout ComputeEditorGroupRects(const WorkspaceLayout& layout,
                                               const EditorSplitTree& split);

// Which way a directional pane verb points. `focus-other-group` cycles; these are
// VS Code's `focusLeftGroup` / `moveActiveEditorGroupLeft` family, which is what a
// grid of more than two panes actually needs (TD-2026-08-18-266).
enum class EditorGroupDirection : std::uint8_t { Left, Right, Up, Down };

// No pane lies that way.
inline constexpr std::size_t kNoEditorGroup = static_cast<std::size_t>(-1);

// The pane adjacent to `from` in `direction`, by the geometry already computed for
// the frame: among the panes wholly on that side, the nearest one, tie-broken by
// how much of the cross axis it shares with `from` and then by centre distance.
// Pure: no state, no tree walk, and the answer is exactly what is on screen.
std::size_t AdjacentEditorGroup(const EditorGroupRectsLayout& rects,
                                std::size_t from,
                                EditorGroupDirection direction);
bool Contains(const SDL_FRect& rect, float x, float y);
// Exact four-float equality for SDL_FRect.
inline bool RectsEqual(const SDL_FRect& lhs, const SDL_FRect& rhs) {
  return lhs.x == rhs.x && lhs.y == rhs.y && lhs.w == rhs.w && lhs.h == rhs.h;
}
// Union of an optional accumulator rect with another rect. The result spans
// both inputs; if the accumulator is nullopt, the result is simply `rhs`.
std::optional<SDL_FRect> UnionOptionalRects(std::optional<SDL_FRect> lhs,
                                            const SDL_FRect& rhs);
float ClampSidebarWidth(float width, float window_width);
// Clamp the right debug pane width so the sidebar + pane together never starve the
// editor below kWorkspaceMinEditorAreaWidth. `sidebar_width` is the currently
// resolved sidebar width (0 when hidden).
float ClampRightPaneWidth(float width, float window_width, float sidebar_width);
float ClampBottomPanelHeight(float height, float window_height);
int BottomPanelVisibleRowsForHeight(float panel_height, float line_height);
// Resolve the vertical coordinate `y` to an absolute bottom-panel log line index, or
// nullopt when `y` is above the first row, below the last visible row, or past the
// content. Floors the row offset so coordinates above `text_y` reject rather than
// snapping to row 0. Single-sourced for the panel click and hover-cursor paths.
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
// Right debug pane resize handle, anchored on the pane's LEFT edge (where it meets
// the editor area), mirroring the sidebar handle on the opposite side.
SDL_FRect RightPaneResizeHandleRect(const WorkspaceLayout& layout);
SDL_FRect SidebarResizeCursorRect(const WorkspaceLayout& layout);
SDL_FRect BottomPanelResizeCursorRect(const WorkspaceLayout& layout);
SDL_FRect RightPaneResizeCursorRect(const WorkspaceLayout& layout);
SDL_FRect SidebarResizeHitRect(const WorkspaceLayout& layout);
SDL_FRect BottomPanelResizeHitRect(const WorkspaceLayout& layout);
SDL_FRect RightPaneResizeHitRect(const WorkspaceLayout& layout);
SDL_FRect VerticalScrollbarHitRect(const ScrollbarGeometry& geometry);
SDL_FRect HorizontalScrollbarHitRect(const ScrollbarGeometry& geometry);
SDL_FRect TabCloseHitRect(const SDL_FRect& close_visual_rect, const SDL_FRect& tab_rect);
// The "Welcome" placeholder tab rect painted (and hit-tested) when no tabs are open.
// Shared so the render, click, and cursor paths cannot drift apart.
SDL_FRect EmptyTabStripPlaceholderRect(const SDL_FRect& tab_strip);
SDL_FRect WindowControlButtonHitRect(const SDL_FRect& button_rect);
LayoutMode ResolveLayoutMode(float window_width, const LayoutModeInputs& inputs);
SDL_FRect ComputeMenuOverflowPopupRect(const SDL_FRect& chevron_rect, std::size_t item_count);
SDL_FRect BottomPanelContentRect(const WorkspaceLayout& layout);
SDL_FRect ComputeDirtyPromptRect(const SDL_FRect& full);
std::array<SDL_FRect, 3> ComputeDirtyPromptButtonRects(const SDL_FRect& dialog);
// The banner strip occupies the top `kWorkspaceEditorBannerHeight` of the editor
// surface; editor content is laid out below it (see ComputeEditorPaneLayouts).
SDL_FRect ComputeEditorBannerStripRect(const SDL_FRect& editor_surface);
EditorBannerButtonLayout ComputeEditorBannerButtonRects(const SDL_FRect& strip, bool has_actions);
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
// Smallest scroll offset that keeps `selected_index` inside the window of
// `visible_rows` rows starting at `scroll_row`: unchanged when the row is already
// on screen, otherwise just far enough to bring it to the near edge. Callers clamp
// against their own max. Split out of RevealScrollableListIndex so the surfaces
// whose scroll model is a ScrollSurfaceLayout rather than a ScrollableListLayout
// (the debug pane) reveal by the same rule instead of restating it.
int RevealedScrollRow(int scroll_row, int visible_rows, int selected_index);
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

// The grab offset for a scrollbar press: grabbing the thumb keeps the point under
// the pointer, pressing the bare track centres the thumb on it. Spelled out four
// times across the compare and merge coordinators (vertical and horizontal each),
// which is four chances to get "centre the thumb" subtly different.
float ScrollbarGrabOffset(const ScrollbarGeometry& geometry,
                          float pointer_coordinate,
                          bool vertical);

std::vector<CompareScrollbarRun> BuildCompareScrollbarRuns(
    const compare::ComparePresentationModel& presentation,
    const compare::CompareModel& model);
// Into-form for the overview-lane cache, which is invalidated by every
// presentation-revision bump — so on the keystroke path in an editable diff the
// returning form allocated and freed this list per keystroke (TD-2026-08-17-261).
void BuildCompareScrollbarRunsInto(const compare::ComparePresentationModel& presentation,
                                   const compare::CompareModel& model,
                                   std::vector<CompareScrollbarRun>& runs);
float ComputeChromeButtonWidth(float measured_label_width);
// Fills `out` rather than returning a fresh vector. Both strip builders run
// their layout several times to settle the overflow reserve, so a returning form
// materialises the same list three or four times per rebuild
// (TD-2026-08-14-222); the returning form below is this plus a temporary.
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
// Same contract as ComputeVisibleStripLayoutsInto, and the reuse matters more
// here: each item owns two strings, so a returning form allocates two per tab per
// pass and frees the identical two when the next pass replaces the vector.
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
SDL_FRect ComputeQuickOpenOverlaySurfaceRect(const SDL_FRect& editor_area);
SDL_FRect ComputeSettingsOverlaySurfaceRect(const SDL_FRect& editor_area);

// Geometry of the floating (non-modal) find / find-replace widget anchored at the
// top-right of the editor area. These sub-rects are the single source of truth
// shared by the renderer, the field hit-test, and button mouse handling so they
// cannot drift apart. Rects for controls that only exist in replace mode are
// zero-sized when replace_mode is false.
// Upper bound on the mode toggles a find widget can carry. Fixed-size so the
// layout stays a trivially copyable value.
inline constexpr std::size_t kFindWidgetMaxToggles = 3;

// The in-file find widget's toggles, in paint / hit-test / tooltip order. The
// first two match the terminal find bar's `Aa` and `ab` exactly, so the two find
// surfaces read the same left to right.
enum class BufferFindToggle : std::size_t {
  MatchCase = 0,
  WholeWord = 1,
  Regex = 2,
  Count = 3,
};
inline constexpr std::size_t kBufferFindToggleCount =
    static_cast<std::size_t>(BufferFindToggle::Count);
static_assert(kBufferFindToggleCount <= kFindWidgetMaxToggles);
// Toggles on the terminal find bar: `Aa` and `ab`, the same first two.
inline constexpr std::size_t kTerminalFindToggleCount = 2;

struct FindWidgetLayout {
  SDL_FRect widget{};
  SDL_FRect search_field{};
  SDL_FRect replace_field{};
  SDL_FRect count_rect{};
  // Mode toggles, laid out left to right between the search field and the match
  // count; only `[0, toggle_count)` are populated and the rest stay zero-sized, so
  // the generic hover/hit-test loops can scan the whole array. Which toggle means
  // what is the caller's business — the layout only reserves the slots.
  std::array<SDL_FRect, kFindWidgetMaxToggles> toggle_buttons{};
  std::size_t toggle_count = 0;
  SDL_FRect prev_button{};
  SDL_FRect next_button{};
  SDL_FRect close_button{};
  SDL_FRect replace_button{};
  SDL_FRect replace_all_button{};
  bool replace_mode = false;
};

SDL_FRect ComputeFindWidgetRect(const SDL_FRect& editor_area, bool replace_mode);
FindWidgetLayout ComputeFindWidgetLayout(const SDL_FRect& editor_area,
                                         bool replace_mode,
                                         std::size_t toggle_count);

// The in-file find widget's layout. Resolve it through this rather than calling
// ComputeFindWidgetLayout directly: paint, hit-test and the tooltip resolver all
// have to agree on the toggle count or the buttons move out from under the mouse.
inline FindWidgetLayout ComputeBufferFindWidgetLayout(const SDL_FRect& editor_area,
                                                      bool replace_mode) {
  return ComputeFindWidgetLayout(editor_area, replace_mode, kBufferFindToggleCount);
}

// Floating, icon-only debug control bar anchored top-right of the editor area
// while a session is active. Like FindWidgetLayout, a single shared layout drives
// both the renderer and the mouse hit-test so they cannot drift apart.
enum class DebugToolbarButton : std::uint8_t {
  ContinuePause = 0,  // Continue when stopped, Pause when running
  StepOver,
  StepInto,
  StepOut,
  ReverseContinue,  // Present only when the adapter advertises supportsStepBack
  StepBack,         //   (same gate; both are skipped for ordinary adapters)
  Restart,
  Stop,
  Count,
};

struct DebugToolbarLayout {
  SDL_FRect widget{};
  // `buttons[i]` is the rect for the i-th *active* button, identified by
  // `kinds[i]`; only `[0, button_count)` are populated. Inactive trailing slots are
  // zero so the generic hover/cursor loops that scan the whole array still work.
  std::array<SDL_FRect, static_cast<std::size_t>(DebugToolbarButton::Count)> buttons{};
  std::array<DebugToolbarButton, static_cast<std::size_t>(DebugToolbarButton::Count)> kinds{};
  std::size_t button_count = 0;
};

// `avoid_below_y`, when set, anchors the bar just below that y so it stacks under
// the find widget when both are visible; otherwise it sits at the editor top.
// `include_reverse` adds the Reverse Continue / Step Back buttons (only when the
// active adapter supports reverse execution); when false the bar is identical to
// the pre-reverse layout.
DebugToolbarLayout ComputeDebugToolbarLayout(const SDL_FRect& editor_area,
                                             std::optional<float> avoid_below_y,
                                             bool include_reverse);

// Tooltip label for a toolbar button, including the two run-state substitutions
// (Continue reads "Pause" while running; a step control reads "Pause to step").
// Defined in DebugToolbarSurface.cpp beside the button specs and consumed by the
// shared hover-tooltip surface, so the label and the button it describes cannot
// drift apart.
std::string_view DebugToolbarButtonTooltip(DebugToolbarButton button, bool session_stopped);

}  // namespace microide::workspace
