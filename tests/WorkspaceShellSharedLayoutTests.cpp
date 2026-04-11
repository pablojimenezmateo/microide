#include "TestSupport.h"

#include "compare/CompareModel.h"
#include "workspace/WorkspaceShellShared.h"

#include <cmath>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::BottomPanelCommandAreaRect;
using microide::workspace::BottomPanelCommandPromptRect;
using microide::workspace::BottomPanelContentRect;
using microide::workspace::BuildChromeTabRenderItems;
using microide::workspace::BuildCompareScrollbarMarkers;
using microide::workspace::ComputeDirtyPromptButtonRects;
using microide::workspace::ComputeDirtyPromptRect;
using microide::workspace::ComputeEditorSplitAxisLayout;
using microide::workspace::BuildMergeScrollbarMarkers;
using microide::workspace::ClampBottomPanelHeight;
using microide::workspace::ClampSidebarWidth;
using microide::workspace::ComputeChromeButtonWidth;
using microide::workspace::ComputeLayout;
using microide::workspace::ComputeMergeResultActionButtonRects;
using microide::workspace::ComputeMergeResultViewportRect;
using microide::workspace::ComputeMergeSourceActionButtonRect;
using microide::workspace::ComputeOverlaySurfaceRect;
using microide::workspace::ComputePromptSurfaceButtonRects;
using microide::workspace::ComputePromptSurfaceInputRect;
using microide::workspace::ComputePromptSurfaceRect;
using microide::workspace::ComputeScrollableListLayout;
using microide::workspace::ComputeScrollbarThumb;
using microide::workspace::ComputeVisibleLineRangeRect;
using microide::workspace::ComputeVisibleStripLayouts;
using microide::workspace::EnsureVisibleStripIndex;
using microide::workspace::HoveredChromeTabTooltipLabel;
using microide::workspace::MakeHorizontalScrollbarGeometry;
using microide::workspace::MakeRect;
using microide::workspace::MakeVerticalScrollbarGeometry;
using microide::workspace::RevealScrollableListIndex;
using microide::workspace::ScrollUnitsForPointer;
using microide::workspace::ScrollableListIndexAtY;
using microide::workspace::ScrollableListRowRect;
using microide::workspace::VisibleLineRangeLayout;

void TestWorkspaceSharedLayoutHelpers() {
  const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 300.0f, 180.0f);
  Expect(layout.menu_bar.h == 25.0f, "layout should preserve menu bar height");
  Expect(layout.sidebar.w == 300.0f, "layout should preserve visible sidebar width");
  Expect(layout.bottom_panel.h == 180.0f, "layout should preserve visible bottom panel height");
  Expect(layout.editor_surface.y == layout.editor_area.y + 27.0f,
         "editor surface should sit below the breadcrumb and divider");

  Expect(ClampSidebarWidth(80.0f, 800.0f) == 160.0f, "sidebar width should clamp to the minimum");
  Expect(ClampSidebarWidth(700.0f, 800.0f) == 519.0f,
         "sidebar width should leave room for the minimum editor width");
  Expect(ClampBottomPanelHeight(50.0f, 240.0f) == 96.0f,
         "bottom panel height should clamp to the available content height");
}

void TestWorkspaceSharedScrollbarHelpers() {
  const auto vertical =
      MakeVerticalScrollbarGeometry(MakeRect(0.0f, 0.0f, 100.0f, 200.0f), 100.0f, 20.0f, 30.0f, false);
  Expect(vertical.has_value(), "vertical scrollbar geometry should exist when content overflows");
  Expect(vertical->track.w == 10.0f, "vertical scrollbar should use the shared thickness");
  Expect(vertical->thumb.h >= 24.0f, "vertical scrollbar thumb should respect minimum length");
  Expect(ScrollUnitsForPointer(*vertical, vertical->thumb.y, 0.0f) >= 29.0f &&
             ScrollUnitsForPointer(*vertical, vertical->thumb.y, 0.0f) <= 31.0f,
         "scroll units should round-trip near the thumb position");

  const auto horizontal = MakeHorizontalScrollbarGeometry(MakeRect(0.0f, 0.0f, 200.0f, 100.0f),
                                                          80.0f, 20.0f, 10.0f, false);
  Expect(horizontal.has_value(), "horizontal scrollbar geometry should exist when content overflows");
  Expect(horizontal->track.h == 10.0f, "horizontal scrollbar should use the shared thickness");
}

void TestWorkspaceSharedScrollbarReserveGeometry() {
  const SDL_FRect area = MakeRect(0.0f, 0.0f, 240.0f, 140.0f);
  const auto vertical =
      MakeVerticalScrollbarGeometry(area, 120.0f, 24.0f, 18.0f, true);
  Expect(vertical.has_value(), "vertical scrollbar geometry should exist with reserved horizontal space");
  Expect(vertical->track.h == 124.0f,
         "vertical scrollbar track should reserve the shared horizontal scrollbar height");

  const auto horizontal =
      MakeHorizontalScrollbarGeometry(area, 160.0f, 32.0f, 24.0f, true);
  Expect(horizontal.has_value(),
         "horizontal scrollbar geometry should exist with reserved vertical space");
  Expect(horizontal->track.w == 224.0f,
         "horizontal scrollbar track should reserve the shared vertical scrollbar width");
}

void TestWorkspaceSharedCompareScrollbarMarkers() {
  const auto make_row = [](microide::compare::CompareRowKind kind) {
    return microide::compare::CompareRow{
        .left_text = "",
        .right_text = "",
        .left_line = 0,
        .right_line = 0,
        .kind = kind,
        .hunk = -1,
        .left_changed_spans = {},
        .right_changed_spans = {},
    };
  };

  microide::compare::CompareModel model;
  model.rows = {
      make_row(microide::compare::CompareRowKind::Unchanged),
      make_row(microide::compare::CompareRowKind::Added),
      make_row(microide::compare::CompareRowKind::Added),
      make_row(microide::compare::CompareRowKind::Modified),
      make_row(microide::compare::CompareRowKind::Deleted),
      make_row(microide::compare::CompareRowKind::Deleted),
      make_row(microide::compare::CompareRowKind::Unchanged),
  };

  const auto markers = BuildCompareScrollbarMarkers(MakeRect(10.0f, 20.0f, 8.0f, 70.0f), model);
  Expect(markers.size() == 3, "compare scrollbar markers should group contiguous changed rows");
  Expect(markers[0].kind == microide::compare::CompareRowKind::Added &&
             markers[0].start_row == 1 && markers[0].end_row == 3,
         "compare scrollbar markers should preserve added-row ranges");
  Expect(markers[1].kind == microide::compare::CompareRowKind::Modified &&
             markers[1].start_row == 3 && markers[1].end_row == 4,
         "compare scrollbar markers should preserve modified-row ranges");
  Expect(markers[2].kind == microide::compare::CompareRowKind::Deleted &&
             markers[2].start_row == 4 && markers[2].end_row == 6,
         "compare scrollbar markers should preserve deleted-row ranges");
  Expect(markers[0].rect.y >= 20.0f && markers[2].rect.y + markers[2].rect.h <= 90.0f,
         "compare scrollbar markers should stay inside the track bounds");
  Expect(markers[1].rect.h >= 2.0f,
         "compare scrollbar markers should stay visible even for single-row changes");
}

void TestWorkspaceSharedMergeScrollbarMarkers() {
  const std::vector<microide::workspace::MergeScrollbarMarkerInput> inputs = {
      {.start_row = 2, .end_row = 5, .choice = microide::compare::MergeChoice::Base, .valid = true},
      {.start_row = 10,
       .end_row = 14,
       .choice = microide::compare::MergeChoice::Incoming,
       .valid = true},
      {.start_row = 18,
       .end_row = 19,
       .choice = microide::compare::MergeChoice::Both,
       .valid = false},
  };

  const auto markers =
      BuildMergeScrollbarMarkers(MakeRect(10.0f, 20.0f, 8.0f, 90.0f), 24, inputs);
  Expect(markers.size() == 3,
         "merge scrollbar markers should preserve one marker per tracked merge span");
  Expect(markers[0].start_row == 2 && markers[0].end_row == 5 &&
             markers[0].choice == microide::compare::MergeChoice::Base && markers[0].valid,
         "merge scrollbar markers should preserve the first tracked span");
  Expect(markers[1].start_row == 10 && markers[1].end_row == 14 &&
             markers[1].choice == microide::compare::MergeChoice::Incoming && markers[1].valid,
         "merge scrollbar markers should preserve the second tracked span");
  Expect(markers[2].start_row == 18 && markers[2].end_row == 19 &&
             markers[2].choice == microide::compare::MergeChoice::Both && !markers[2].valid,
         "merge scrollbar markers should preserve invalid spans too");
  Expect(markers.front().rect.y >= 20.0f &&
             markers.back().rect.y + markers.back().rect.h <= 110.0f,
         "merge scrollbar markers should stay inside the track bounds");
  Expect(markers.back().rect.h >= 2.0f,
         "merge scrollbar markers should stay visible even for near-single-line spans");
}

void TestWorkspaceSharedPanelGeometryHelpers() {
  const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 280.0f, 200.0f);
  const auto content = BottomPanelContentRect(layout, true);
  const auto command_area = BottomPanelCommandAreaRect(layout);
  const auto prompt = BottomPanelCommandPromptRect(layout);

  Expect(command_area.y >= content.y + content.h, "command area should sit below panel content");
  Expect(prompt.y >= command_area.y, "command prompt should stay inside the command area");
  Expect(prompt.x > command_area.x, "command prompt should honor horizontal inset");
}

void TestWorkspaceSharedPromptGeometry() {
  const SDL_FRect full = MakeRect(0.0f, 0.0f, 800.0f, 600.0f);
  const SDL_FRect dirty_prompt = ComputeDirtyPromptRect(full);
  Expect(dirty_prompt.x == 170.0f && dirty_prompt.y == 212.0f && dirty_prompt.w == 460.0f &&
             dirty_prompt.h == 176.0f,
         "dirty prompt rect should stay centered while preserving the shared prompt size");

  const auto dirty_buttons = ComputeDirtyPromptButtonRects(dirty_prompt);
  Expect(dirty_buttons[0].x == 306.0f && dirty_buttons[2].x == 518.0f &&
             dirty_buttons[0].y == 344.0f,
         "dirty prompt button rects should align to the shared footer button layout");

  const SDL_FRect prompt_surface = ComputePromptSurfaceRect(full);
  Expect(prompt_surface.x == 140.0f && prompt_surface.y == 206.0f &&
             prompt_surface.w == 520.0f && prompt_surface.h == 188.0f,
         "prompt surface rect should stay centered while preserving the shared surface size");

  const auto prompt_buttons = ComputePromptSurfaceButtonRects(prompt_surface);
  Expect(prompt_buttons[0].x == 418.0f && prompt_buttons[1].x == 536.0f &&
             prompt_buttons[0].y == 350.0f,
         "prompt surface button rects should align to the shared footer button layout");

  const SDL_FRect input_rect = ComputePromptSurfaceInputRect(prompt_surface);
  Expect(input_rect.x == 156.0f && input_rect.y == 304.0f && input_rect.w == 488.0f &&
             input_rect.h == 24.0f,
         "prompt surface input rect should align to the shared text-input slot");

  const SDL_FRect compact_prompt = ComputePromptSurfaceRect(MakeRect(0.0f, 0.0f, 300.0f, 200.0f));
  Expect(compact_prompt.x == 16.0f && compact_prompt.y == 16.0f && compact_prompt.w == 268.0f &&
             compact_prompt.h == 168.0f,
         "prompt surface rect should clamp to the compact fallback when the window is small");
}

void TestWorkspaceSharedScrollbarEdgeCases() {
  const auto thumb =
      ComputeScrollbarThumb(MakeRect(0.0f, 0.0f, 10.0f, 100.0f), 10.0f, 10.0f, 0.0f, true);
  Expect(!thumb.has_value(), "scrollbar thumb should be absent when all content is visible");

  const auto hidden_vertical =
      MakeVerticalScrollbarGeometry(MakeRect(0.0f, 0.0f, 100.0f, 100.0f), 8.0f, 10.0f, 0.0f, false);
  Expect(!hidden_vertical.has_value(),
         "vertical scrollbar geometry should be absent when content does not overflow");
}

void TestWorkspaceSharedScrollableListLayout() {
  const auto discrete = ComputeScrollableListLayout(
      MakeRect(10.0f, 20.0f, 220.0f, 140.0f), 50.0f, 8, 7, 12.0f, 20.0f, 18.0f);
  Expect(discrete.visible_units == 5.0f && discrete.visible_rows == 5,
         "scrollable list layout should floor discrete visible units to whole rows");
  Expect(discrete.max_scroll == 3 && discrete.scroll_row == 3,
         "scrollable list layout should clamp scroll rows to the computed max");
  Expect(discrete.row_width == 180.0f && discrete.list_rect.h == 110.0f,
         "scrollable list layout should reserve scrollbar width only when overflow exists");
  Expect(discrete.scrollbar.has_value(),
         "scrollable list layout should surface scrollbar geometry when items overflow");

  const SDL_FRect row_rect = ScrollableListRowRect(discrete, 2);
  Expect(row_rect.x == 22.0f && row_rect.y == 90.0f && row_rect.w == 180.0f &&
             row_rect.h == 18.0f,
         "scrollable list row rect should align rows to the shared inset and step");

  const auto visible_index = ScrollableListIndexAtY(discrete, 91.0f);
  Expect(visible_index.has_value() && *visible_index == 5,
         "scrollable list hit testing should translate y positions through the scroll row");
  Expect(!ScrollableListIndexAtY(discrete, 49.0f).has_value() &&
             !ScrollableListIndexAtY(discrete, 150.0f).has_value(),
         "scrollable list hit testing should ignore y positions outside visible rows");

  const auto reveal = ComputeScrollableListLayout(
      MakeRect(0.0f, 0.0f, 200.0f, 100.0f), 20.0f, 10, 2, 10.0f, 20.0f, 18.0f);
  Expect(RevealScrollableListIndex(reveal, 1) == 1,
         "reveal helper should scroll upward when the selection is above the viewport");
  Expect(RevealScrollableListIndex(reveal, 5) == 2,
         "reveal helper should leave the scroll row unchanged when the selection stays visible");
  Expect(RevealScrollableListIndex(reveal, 7) == 4,
         "reveal helper should scroll downward just enough to show the selection");

  const auto fractional = ComputeScrollableListLayout(
      MakeRect(0.0f, 0.0f, 200.0f, 109.0f), 1.0f, 10, 8, 10.0f, 20.0f, 18.0f, 0.0f, 5.0f, true);
  Expect(std::fabs(fractional.visible_units - 5.4f) < 0.01f && fractional.visible_rows == 5,
         "scrollable list layout should preserve fractional visible units when requested");
  Expect(fractional.max_scroll == 5 && fractional.scroll_row == 5,
         "fractional scrollable list layout should clamp against the fractional max scroll");
  Expect(fractional.list_rect.h == 103.0f,
         "scrollable list layout should honor separate scrollbar bottom padding");
}

void TestWorkspaceSharedStripLayoutHelpers() {
  const std::vector<float> widths = {90.0f, 100.0f, 80.0f, 70.0f};
  const auto visible = ComputeVisibleStripLayouts(widths, 12.0f, 6.0f, 215.0f, 0);
  Expect(visible.size() == 2, "strip layout should stop before overflowing the max x bound");
  Expect(visible[0].index == 0 && visible[0].x == 12.0f,
         "strip layout should place the first slot at the provided start x");
  Expect(visible[1].index == 1 && visible[1].x == 108.0f,
         "strip layout should advance by prior width plus the configured gap");

  Expect(EnsureVisibleStripIndex(widths, 12.0f, 6.0f, 215.0f, 0, 2) == 1,
         "visible strip index should scroll just enough to reveal the third item");
  Expect(EnsureVisibleStripIndex(widths, 12.0f, 6.0f, 215.0f, 0, 3) == 2,
         "visible strip index should keep the active tail item in view");
  Expect(EnsureVisibleStripIndex({}, 12.0f, 6.0f, 215.0f, 0, 0) == 0,
         "visible strip index should remain at zero for empty strips");
}

void TestWorkspaceSharedChromeTabRenderItems() {
  const std::vector<microide::workspace::StripSlotLayout> slots = {
      {.index = 0, .x = 12.0f, .width = 90.0f},
      {.index = 2, .x = 108.0f, .width = 120.0f},
  };
  const std::vector<std::size_t> model_indices = {5, 6, 7};
  const std::vector<std::string> display_titles = {"alpha", "beta", "*gamma"};
  const std::vector<std::string> tooltip_labels = {"path/a", "path/b", "path/c"};

  const auto items = BuildChromeTabRenderItems(slots, 24.0f, 22.0f, model_indices, 7,
                                               display_titles, tooltip_labels, 14.0f, 6.0f);
  Expect(items.size() == 2, "chrome tab builder should preserve visible item count");
  Expect(items[0].index == 5 && !items[0].active,
         "chrome tab builder should map slot indices through the provided model indices");
  Expect(items[1].index == 7 && items[1].active,
         "chrome tab builder should mark the requested active tab");
  Expect(items[0].display_title == "alpha" && items[1].display_title == "*gamma",
         "chrome tab builder should preserve display labels");
  Expect(items[0].tooltip_label == "path/a" && items[1].tooltip_label == "path/c",
         "chrome tab builder should preserve tooltip labels");
  Expect(items[0].close_rect.x > items[0].rect.x && items[0].close_rect.y >= items[0].rect.y,
         "chrome tab builder should place the close button inside the tab bounds");
  Expect(HoveredChromeTabTooltipLabel(items, 150.0f, 30.0f) == "path/c",
         "chrome tab hover helper should return the tooltip for the hovered tab");
  Expect(HoveredChromeTabTooltipLabel(items, 4.0f, 4.0f).empty(),
         "chrome tab hover helper should ignore pointers outside every tab");
}

void TestWorkspaceSharedEditorSplitLayout() {
  const auto even_vertical = ComputeEditorSplitAxisLayout(
      MakeRect(10.0f, 20.0f, 300.0f, 120.0f), true, std::vector<float>{1.0f, 1.0f});
  Expect(even_vertical.has_value(), "editor split layout should exist for non-empty child lists");
  Expect(even_vertical->total_extent == 294.0f,
         "editor split layout should reserve shared divider thickness from the axis extent");
  Expect(even_vertical->child_rects.size() == 2 && even_vertical->divider_rects.size() == 1,
         "editor split layout should produce one child rect per pane and one divider per gap");
  Expect(even_vertical->child_rects[0].w == 147.0f && even_vertical->child_rects[1].w == 147.0f,
         "editor split layout should split equal fractions evenly across the remaining extent");
  Expect(even_vertical->divider_rects[0].x == 157.0f && even_vertical->divider_rects[0].w == 6.0f,
         "editor split layout should place the divider after the leading child extent");

  const auto clamped_vertical = ComputeEditorSplitAxisLayout(
      MakeRect(0.0f, 0.0f, 600.0f, 80.0f), true, std::vector<float>{0.9f, 0.05f, 0.05f});
  Expect(clamped_vertical.has_value(), "editor split layout should handle skewed fractions");
  Expect(clamped_vertical->extents[0] <= 228.0f && clamped_vertical->extents[1] >= 179.0f &&
             clamped_vertical->extents[2] >= 179.0f,
         "editor split layout should cap an oversized leading pane and keep later panes near the shared minimum extent");

  const auto even_horizontal = ComputeEditorSplitAxisLayout(
      MakeRect(0.0f, 0.0f, 120.0f, 240.0f), false, std::vector<float>{0.0f, 0.0f, 0.0f});
  Expect(even_horizontal.has_value(), "editor split layout should normalize zero weights");
  Expect(even_horizontal->child_rects[0].h == 76.0f && even_horizontal->child_rects[1].h == 76.0f &&
             even_horizontal->child_rects[2].h == 76.0f,
         "editor split layout should evenly distribute zero-weight panes");
  Expect(even_horizontal->divider_rects[0].y == 76.0f && even_horizontal->divider_rects[1].y == 158.0f,
         "editor split layout should advance divider positions by child extent plus divider thickness");

  Expect(!ComputeEditorSplitAxisLayout(MakeRect(0.0f, 0.0f, 100.0f, 100.0f), true, {})
              .has_value(),
         "editor split layout should be absent for empty split-node children");
}

void TestWorkspaceSharedMergeInteractionGeometry() {
  Expect(ComputeChromeButtonWidth(4.0f) == 64.0f,
         "chrome button width should clamp to the minimum width");
  Expect(ComputeChromeButtonWidth(90.0f) == 108.0f,
         "chrome button width should preserve measured width plus padding");
  Expect(ComputeChromeButtonWidth(240.0f) == 160.0f,
         "chrome button width should clamp to the maximum width");

  const SDL_FRect editor_surface = MakeRect(100.0f, 200.0f, 640.0f, 320.0f);
  const SDL_FRect result_rect =
      ComputeMergeResultViewportRect(editor_surface, 280.0f, 260.0f, 32.0f, 180.0f, true);
  Expect(result_rect.x == 280.0f && result_rect.y == 252.0f && result_rect.w == 212.0f,
         "merge result rect should preserve the shared merge viewport origin and width");
  Expect(result_rect.h == 256.0f,
         "merge result rect should reserve horizontal scrollbar space when present");

  const VisibleLineRangeLayout line_layout = {
      .first_line_y = 300.0f,
      .line_height = 18.0f,
      .scroll_line = 5,
      .visible_rows = 4,
  };
  const std::optional<SDL_FRect> visible_rect =
      ComputeVisibleLineRangeRect(result_rect, line_layout, 6, 8);
  Expect(visible_rect.has_value(),
         "visible line range rect should exist when the requested span is on screen");
  Expect(visible_rect->y == 317.0f && visible_rect->h == 36.0f,
         "visible line range rect should align to the visible line span");
  Expect(!ComputeVisibleLineRangeRect(result_rect, line_layout, 10, 11).has_value(),
         "visible line range rect should be absent for off-screen spans");

  const SDL_FRect accept_rect = ComputeMergeSourceActionButtonRect(
      120.0f, 30.0f, 260.0f, 18.0f, 5, 8, 508.0f, 90.0f, 22.0f);
  Expect(accept_rect.x == 150.0f && accept_rect.y == 316.0f,
         "merge source action rect should anchor to the pane gutter and source line");

  const std::array<float, 4> action_widths = {64.0f, 82.0f, 84.0f, 70.0f};
  const auto action_rects = ComputeMergeResultActionButtonRects(
      320.0f, 260.0f, 508.0f, visible_rect, action_widths, 22.0f, 8.0f);
  Expect(action_rects[0].x == 320.0f && action_rects[0].y == 355.0f,
         "merge result action rects should start below the visible conflict span");
  Expect(action_rects[1].x == 392.0f && action_rects[2].x == 482.0f &&
             action_rects[3].x == 574.0f,
         "merge result action rects should advance by width plus shared gap");

  const auto clamped_action_rects = ComputeMergeResultActionButtonRects(
      320.0f, 260.0f, 380.0f, std::optional<SDL_FRect>{MakeRect(320.0f, 360.0f, 212.0f, 30.0f)},
      action_widths, 22.0f, 8.0f);
  Expect(clamped_action_rects[0].y == 336.0f,
         "merge result action rects should move above the conflict when they would overflow");
}

void TestWorkspaceSharedOverlayRectHelpers() {
  const SDL_FRect roomy = ComputeOverlaySurfaceRect(MakeRect(100.0f, 200.0f, 1200.0f, 800.0f));
  Expect(roomy.w == 696.0f,
         "overlay rect should use the preferred width ratio when within bounds");
  Expect(roomy.h == 352.0f,
         "overlay rect should use the preferred height ratio when within bounds");
  Expect(roomy.x == 352.0f, "overlay rect should stay horizontally centered in the editor area");
  Expect(std::fabs(roomy.y - 298.56f) < 0.01f,
         "overlay rect should bias vertically toward the top of the editor area");

  const SDL_FRect compact = ComputeOverlaySurfaceRect(MakeRect(0.0f, 0.0f, 300.0f, 200.0f));
  Expect(compact.w == 260.0f, "overlay rect should clamp width to the compact fallback width");
  Expect(compact.h == 160.0f, "overlay rect should clamp height to the compact fallback height");
  Expect(compact.x == 20.0f, "overlay rect should remain centered after compact-width clamping");
  Expect(std::fabs(compact.y - 8.8f) < 0.01f,
         "overlay rect should preserve the vertical bias after compact-height clamping");
}

}  // namespace

void RegisterWorkspaceShellSharedLayoutTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShared/LayoutHelpers", TestWorkspaceSharedLayoutHelpers);
  AddTest(tests, "WorkspaceShared/ScrollbarHelpers", TestWorkspaceSharedScrollbarHelpers);
  AddTest(tests, "WorkspaceShared/ScrollbarReserveGeometry",
          TestWorkspaceSharedScrollbarReserveGeometry);
  AddTest(tests, "WorkspaceShared/CompareScrollbarMarkers",
          TestWorkspaceSharedCompareScrollbarMarkers);
  AddTest(tests, "WorkspaceShared/MergeScrollbarMarkers",
          TestWorkspaceSharedMergeScrollbarMarkers);
  AddTest(tests, "WorkspaceShared/PanelGeometryHelpers", TestWorkspaceSharedPanelGeometryHelpers);
  AddTest(tests, "WorkspaceShared/PromptGeometry", TestWorkspaceSharedPromptGeometry);
  AddTest(tests, "WorkspaceShared/ScrollbarEdgeCases", TestWorkspaceSharedScrollbarEdgeCases);
  AddTest(tests, "WorkspaceShared/ScrollableListLayout",
          TestWorkspaceSharedScrollableListLayout);
  AddTest(tests, "WorkspaceShared/StripLayoutHelpers", TestWorkspaceSharedStripLayoutHelpers);
  AddTest(tests, "WorkspaceShared/ChromeTabRenderItems",
          TestWorkspaceSharedChromeTabRenderItems);
  AddTest(tests, "WorkspaceShared/EditorSplitLayout", TestWorkspaceSharedEditorSplitLayout);
  AddTest(tests, "WorkspaceShared/MergeInteractionGeometry",
          TestWorkspaceSharedMergeInteractionGeometry);
  AddTest(tests, "WorkspaceShared/OverlayRectHelpers", TestWorkspaceSharedOverlayRectHelpers);
}

}  // namespace microide::tests
