#include "TestSupport.h"

#include "compare/CompareModel.h"
#include "compare/ComparePresentationModel.h"
#include "compare/CompareSemanticMetadata.h"
#include "workspace/render/CompareMergeRender.h"
#include "workspace/services/LayoutModeService.h"
#include "workspace/render/OverviewRuler.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/state/WorkspaceInteractionState.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::BottomPanelContentRect;
using microide::workspace::BottomPanelLineIndexAtY;
using microide::workspace::CompareCollapsedContextBlockRect;
using microide::workspace::EmptyTabStripPlaceholderRect;
using microide::workspace::BuildChromeTabRenderItems;
using microide::workspace::BuildCompareScrollbarRuns;
using microide::workspace::ClassifyMergeHoverState;
using microide::workspace::ComputeDirtyPromptButtonRects;
using microide::workspace::ComputeDirtyPromptRect;
using microide::workspace::ClampBottomPanelHeight;
using microide::workspace::ClampSidebarWidth;
using microide::workspace::ComputeChromeButtonWidth;
using microide::workspace::ComputeLayout;
using microide::workspace::ComputeMergeResultActionButtonRects;
using microide::workspace::ComputeMergeResultViewportRect;
using microide::workspace::ComputeMergeSourceActionButtonRect;
using microide::workspace::FindMergeTrackedConflictAtSourceLine;
using microide::workspace::ComputeQuickOpenOverlaySurfaceRect;
using microide::workspace::ComputeSettingsOverlaySurfaceRect;
using microide::workspace::ComputePromptSurfaceButtonRects;
using microide::workspace::ComputePromptSurfaceInputRect;
using microide::workspace::ComputePromptSurfaceRect;
using microide::workspace::ComputeScrollSurfaceLayout;
using microide::workspace::ComputeScrollableListLayout;
using microide::workspace::ComputeScrollbarThumb;
using microide::workspace::ComputeTextGridInteractionLayout;
using microide::workspace::ComputeVisibleLineRangeRect;
using microide::workspace::ComputeVisibleStripLayouts;
using microide::workspace::EnsureVisibleStripIndex;
using microide::workspace::HoveredChromeTabTooltipLabel;
using microide::workspace::HorizontalScrollbarHitRect;
using microide::workspace::LayoutMode;
using microide::workspace::LayoutModeInputs;
using microide::workspace::MakeHorizontalScrollbarGeometry;
using microide::workspace::MakeRect;
using microide::workspace::MakeVerticalScrollbarGeometry;
using microide::workspace::ResolveLayoutMode;
using microide::workspace::kWorkspaceLayoutCompactBreakpointDefault;
using microide::workspace::kWorkspaceLayoutCompactHysteresis;
using microide::workspace::SidebarResizeHandleRect;
using microide::workspace::SidebarResizeHitRect;
using microide::workspace::SidebarResizeCursorRect;
using microide::workspace::TabCloseHitRect;
using microide::workspace::VerticalScrollbarHitRect;
using microide::workspace::BottomPanelResizeHandleRect;
using microide::workspace::BottomPanelResizeHitRect;
using microide::workspace::BottomPanelResizeCursorRect;
using microide::workspace::RectsEqual;
using microide::workspace::MergeHoverInteractionLayout;
using microide::workspace::MergeHoverResultLayout;
using microide::workspace::MergeHoverState;
using microide::workspace::MergeHoverSurfaceLayout;
using microide::workspace::MergeTrackedConflict;
using microide::workspace::RevealScrollableListIndex;
using microide::workspace::ScrollUnitsForPointer;
using microide::workspace::ScrollableListIndexAtY;
using microide::workspace::ScrollableListItemAt;
using microide::workspace::ScrollableListItemAtY;
using microide::workspace::ScrollableListItemRect;
using microide::workspace::ScrollableListRowRect;
using microide::workspace::ClampTextGridLineAtY;
using microide::workspace::TextGridCursorX;
using microide::workspace::TextGridLineY;
using microide::workspace::TextGridVisualColumnAtX;
using microide::workspace::VisibleTextGridLineAtY;
using microide::workspace::VisibleLineRangeLayout;

void TestWorkspaceSharedLayoutHelpers() {
  const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 300.0f, 180.0f);
  Expect(layout.menu_bar.h == 25.0f, "layout should preserve menu bar height");
  Expect(layout.sidebar.w == 300.0f, "layout should preserve visible sidebar width");
  Expect(layout.bottom_panel.h == 180.0f, "layout should preserve visible bottom panel height");
  Expect(layout.editor_surface.y == layout.editor_area.y + 27.0f,
         "editor surface should sit below the breadcrumb and divider");

  Expect(ClampSidebarWidth(140.0f, 800.0f) == 140.0f,
         "sidebar width should allow any width above the viable minimum");
  Expect(ClampSidebarWidth(80.0f, 800.0f) == 120.0f,
         "sidebar width stops at the width its header controls need");
  Expect(ClampSidebarWidth(700.0f, 800.0f) == 519.0f,
         "sidebar width should leave room for the minimum editor width");
  Expect(ClampSidebarWidth(1200.0f, 2560.0f) == 1200.0f,
         "on a wide window the sidebar widens past the old 520 px ceiling");
  Expect(ClampSidebarWidth(3000.0f, 2560.0f) == 2560.0f - 280.0f - 1.0f,
         "and stops only where the editor keeps its minimum width");
  Expect(ClampBottomPanelHeight(50.0f, 240.0f) == 52.0f,
         "bottom panel height should clamp to the available content height");
}

// The sidebar fills the band beside the editor tab strip (it starts under the
// project strip, not under the tab strip), down to the panel like the editor
// column -- that band was empty chrome over the tree. Its resize handle stays
// beside the content so it never takes the first tab's left edge, while a drag's
// damage covers the whole divider line it lights.
void TestWorkspaceSharedSidebarFillsTheTabStripBand() {
  for (const bool project_strip : {false, true}) {
    const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 300.0f, 180.0f, {}, true, false, 0.0f,
                                      project_strip);
    const float under_project_strip = layout.project_tab_strip.y + layout.project_tab_strip.h;
    Expect(layout.sidebar.y == under_project_strip && layout.sidebar.y == layout.tab_strip.y,
           "the sidebar starts level with the editor tab strip");
    Expect(layout.sidebar.y + layout.sidebar.h == layout.editor_area.y + layout.editor_area.h,
           "and ends where the editor column does, at the panel");
    const SDL_FRect handle = microide::workspace::SidebarResizeHandleRect(layout);
    Expect(handle.y == layout.content.y && handle.y + handle.h == layout.sidebar.y + layout.sidebar.h,
           "its resize handle runs beside the content only");
    const auto damage = microide::workspace::DragTargetHighlightRect(
        layout, microide::workspace::DragTarget::SidebarDivider);
    Expect(damage.has_value() && damage->y == layout.sidebar.y &&
               damage->y + damage->h == layout.sidebar.y + layout.sidebar.h,
           "a divider drag repaints the divider's whole height");
  }
}

// The debug pane is the sidebar's mirror on the right and takes the same band
// (VS Code's secondary side bar does; TD-2026-11-01-427 item 4) -- it started
// under the editor tab strip, a strip-high gap of chrome over it. Its handle stays
// beside the content, clear of the strip's overflow chevrons at its right end.
void TestWorkspaceSharedDebugPaneFillsTheTabStripBand() {
  for (const bool project_strip : {false, true}) {
    const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 300.0f, 180.0f, {}, true,
                                      /*right_pane_visible=*/true, 320.0f, project_strip);
    Expect(layout.right_pane.w > 0.0f && layout.right_pane.y == layout.sidebar.y &&
               layout.right_pane.h == layout.sidebar.h,
           "the debug pane spans the sidebar's band");
    Expect(layout.right_pane.x >= layout.tab_strip.x + layout.tab_strip.w,
           "beside the editor tab strip, not over it");
    const SDL_FRect handle = microide::workspace::RightPaneResizeHandleRect(layout);
    Expect(handle.y == layout.content.y && handle.y + handle.h == layout.right_pane.y + layout.right_pane.h,
           "its resize handle runs beside the content only");
    const auto damage = microide::workspace::DragTargetHighlightRect(
        layout, microide::workspace::DragTarget::RightPaneDivider);
    Expect(damage.has_value() && damage->y == layout.right_pane.y &&
               damage->y + damage->h == layout.right_pane.y + layout.right_pane.h,
           "a divider drag repaints the divider's whole height");
  }
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

std::vector<microide::workspace::overview::Marker> CompareMarkersFromRuns(
    const SDL_FRect& lane, const std::vector<microide::workspace::CompareScrollbarRun>& runs,
    std::size_t total_rows) {
  std::vector<microide::workspace::overview::MarkerInput> inputs;
  inputs.reserve(runs.size());
  for (const auto& run : runs) {
    inputs.push_back(microide::workspace::overview::MarkerInput{
        .start_row = run.start_row, .end_row = run.end_row, .color = SDL_Color{255, 0, 0, 255},
        .priority = 0});
  }
  return microide::workspace::overview::BuildMarkers(lane, total_rows, inputs);
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

  const auto presentation = microide::compare::BuildComparePresentationModel(
      model,
      microide::compare::InferCompareSemanticFileMetadata(
          microide::compare::CompareSemanticMetadataInput{
              .path = "f.txt",
              .left_content = "a\nb\nc\nd\ne\nf\ng\n",
              .right_content = "a\nb\nc\nd\ne\nf\ng\n",
              .git_entry = std::nullopt,
              .old_path = {},
          }),
      microide::compare::ComparePresentationOptions{},
      microide::compare::ComparePresentationCollapseState{}, 1);
  const auto runs = BuildCompareScrollbarRuns(presentation, model);
  Expect(runs.size() == 3, "compare scrollbar runs should group contiguous changed rows");
  Expect(runs[0].kind == microide::compare::CompareRowKind::Added && runs[0].start_row == 1 &&
             runs[0].end_row == 3,
         "compare scrollbar runs should preserve added-row ranges");
  Expect(runs[1].kind == microide::compare::CompareRowKind::Modified && runs[1].start_row == 3 &&
             runs[1].end_row == 4,
         "compare scrollbar runs should preserve modified-row ranges");
  Expect(runs[2].kind == microide::compare::CompareRowKind::Deleted && runs[2].start_row == 4 &&
             runs[2].end_row == 6,
         "compare scrollbar runs should preserve deleted-row ranges");

  const auto markers = CompareMarkersFromRuns(MakeRect(10.0f, 20.0f, 8.0f, 70.0f), runs,
                                              presentation.rows.size());
  Expect(markers.size() == 3, "shared builder should emit one marker per changed run");
  Expect(markers[0].rect.y >= 20.0f && markers[2].rect.y + markers[2].rect.h <= 90.0f,
         "compare scrollbar markers should stay inside the track bounds");
  Expect(markers[1].rect.h >= 2.0f,
         "compare scrollbar markers should stay visible even for single-row changes");
}

void TestWorkspaceSharedCompareScrollbarMarkersFollowPresentationRows() {
  const auto build_text = [](std::string_view first_change, std::string_view second_change) {
    std::string text;
    for (int i = 0; i < 24; ++i) {
      text += "prefix " + std::to_string(i) + "\n";
    }
    text += std::string(first_change) + "\n";
    for (int i = 0; i < 30; ++i) {
      text += "middle " + std::to_string(i) + "\n";
    }
    text += std::string(second_change) + "\n";
    for (int i = 0; i < 8; ++i) {
      text += "suffix " + std::to_string(i) + "\n";
    }
    return text;
  };

  const std::string left = build_text("left a", "left b");
  const std::string right = build_text("right a", "right b");
  const auto model = microide::compare::BuildCompareModel(left, right);
  const auto presentation = microide::compare::BuildComparePresentationModel(
      model,
      microide::compare::InferCompareSemanticFileMetadata(
          microide::compare::CompareSemanticMetadataInput{
              .path = "f.txt",
              .left_content = left,
              .right_content = right,
              .git_entry = std::nullopt,
              .old_path = {},
          }),
      microide::compare::ComparePresentationOptions{},
      microide::compare::ComparePresentationCollapseState{}, 1);

  const auto runs = BuildCompareScrollbarRuns(presentation, model);
  Expect(runs.size() == 2,
         "collapsed compare presentations should still emit one run per visible changed run");
  Expect(presentation.rows.size() > 5,
         "collapsed compare fixture should include buffered context rows around changes");
  Expect(runs[0].start_row > 1 && runs[0].end_row > runs[0].start_row,
         "first changed run should be positioned using presentation-row coordinates");
  Expect(runs[1].start_row > runs[0].end_row && runs[1].end_row > runs[1].start_row,
         "second changed run should also be positioned using presentation-row coordinates");

  const auto markers = CompareMarkersFromRuns(MakeRect(10.0f, 20.0f, 8.0f, 100.0f), runs,
                                              presentation.rows.size());
  Expect(markers.size() == 2, "shared builder should emit one marker per visible changed run");
  Expect(markers[1].rect.y < 90.0f,
         "collapsed context should shrink the marker gap instead of preserving raw model spacing");
}

void TestWorkspaceSharedMergeScrollbarMarkers() {
  const SDL_Color color{100, 150, 200, 255};
  const std::vector<microide::workspace::overview::MarkerInput> inputs = {
      {.start_row = 2, .end_row = 5, .color = color, .priority = 0},
      {.start_row = 10, .end_row = 14, .color = color, .priority = 0},
      {.start_row = 18, .end_row = 19, .color = color, .priority = 0},
  };

  const auto markers =
      microide::workspace::overview::BuildMarkers(MakeRect(10.0f, 20.0f, 8.0f, 90.0f), 24, inputs);
  Expect(markers.size() == 3,
         "shared builder should preserve one marker per tracked merge span");
  Expect(markers.front().rect.y >= 20.0f &&
             markers.back().rect.y + markers.back().rect.h <= 110.0f,
         "merge scrollbar markers should stay inside the track bounds");
  Expect(markers.back().rect.h >= 2.0f,
         "merge scrollbar markers should stay visible even for near-single-line spans");
}

void TestWorkspaceSharedOverviewLaneGeometry() {
  namespace overview = microide::workspace::overview;
  const SDL_FRect track = MakeRect(200.0f, 40.0f, 10.0f, 300.0f);
  const SDL_FRect lane = overview::LaneRect(track);
  Expect(lane.x < track.x, "overview lane sits to the left of the scrollbar track");
  Expect(std::abs((lane.x + lane.w + overview::kLaneGap) - track.x) < 0.01f,
         "lane is separated from the track by exactly the lane gap");
  Expect(lane.y == track.y && lane.h == track.h,
         "lane spans the same vertical extent as the track");
  Expect(lane.w == overview::kLaneWidth, "lane uses the configured lane width");

  const SDL_FRect inner = overview::LaneInnerRect(lane);
  Expect(inner.x == lane.x + 1.0f && inner.y == lane.y + 1.0f,
         "inner lane insets by 1px for the border");
  Expect(inner.w == lane.w - 2.0f && inner.h == lane.h - 2.0f,
         "inner lane is 2px smaller than the lane in each dimension");
}

void TestWorkspaceSharedOverviewReducerBoundsAndPriority() {
  namespace overview = microide::workspace::overview;
  const SDL_FRect inner_lane = MakeRect(100.0f, 10.0f, 6.0f, 50.0f);
  std::vector<std::uint32_t> buckets;
  std::vector<SDL_Color> palette;

  // Thousands of single-line inputs across a huge document must collapse to at most
  // one marker per lane pixel (~ceil(height)), never one per input.
  std::vector<overview::MarkerInput> dense;
  const SDL_Color low{10, 10, 10, 255};
  for (int line = 0; line < 4000; line += 2) {
    dense.push_back({.start_row = line, .end_row = line + 1, .color = low, .priority = 1});
  }
  const auto reduced = overview::ReduceMarkers(inner_lane, 4000, dense, buckets, palette);
  Expect(!reduced.empty(), "dense reducer should still emit markers");
  Expect(reduced.size() <= static_cast<std::size_t>(std::ceil(inner_lane.h)) + 1,
         "dense reducer output is bounded by lane height, not input count");
  for (const auto& marker : reduced) {
    Expect(marker.rect.y >= inner_lane.y - 0.01f &&
               marker.rect.y + marker.rect.h <= inner_lane.y + inner_lane.h + 0.01f,
           "reduced markers stay within the lane");
  }

  // Higher priority wins a contested pixel row: a high-priority error over the same
  // row as a low-priority match paints the error color.
  const SDL_Color high{200, 0, 0, 255};
  const std::vector<overview::MarkerInput> contested = {
      {.start_row = 5, .end_row = 6, .color = low, .priority = 1},
      {.start_row = 5, .end_row = 6, .color = high, .priority = 90},
  };
  const auto resolved = overview::ReduceMarkers(inner_lane, 20, contested, buckets, palette);
  Expect(!resolved.empty(), "contested reducer should emit a marker");
  bool saw_high = false;
  for (const auto& marker : resolved) {
    if (marker.color.r == high.r && marker.color.g == high.g && marker.color.b == high.b) {
      saw_high = true;
    }
    Expect(!(marker.color.r == low.r && marker.color.g == low.g && marker.color.b == low.b),
           "the lower-priority color must not win the contested pixel");
  }
  Expect(saw_high, "the higher-priority color should win the contested pixel");
}

void TestWorkspaceSharedOverviewReducerEqualPriorityTieIsInputOrder() {
  namespace overview = microide::workspace::overview;
  const SDL_FRect inner_lane = MakeRect(100.0f, 10.0f, 6.0f, 50.0f);
  std::vector<std::uint32_t> buckets;
  std::vector<SDL_Color> palette;

  const SDL_Color first{10, 20, 30, 255};
  const SDL_Color second{200, 180, 160, 255};
  // Two equal-priority, different-colored inputs collide on one pixel row. The winner
  // must be the FIRST input (source order), deterministically — never the incidental
  // palette-insertion order a full-packed-value compare used to leak.
  const auto color_at_row5 = [&](const SDL_Color& a, const SDL_Color& b) -> SDL_Color {
    const std::vector<overview::MarkerInput> contested = {
        {.start_row = 5, .end_row = 6, .color = a, .priority = 50},
        {.start_row = 5, .end_row = 6, .color = b, .priority = 50},
    };
    const auto reduced = overview::ReduceMarkers(inner_lane, 20, contested, buckets, palette);
    Expect(!reduced.empty(), "equal-priority contest should still emit a marker");
    return reduced.front().color;
  };

  const SDL_Color forward = color_at_row5(first, second);
  Expect(forward.r == first.r && forward.g == first.g && forward.b == first.b,
         "equal priority: the first input wins the contested pixel");
  const SDL_Color reversed = color_at_row5(second, first);
  Expect(reversed.r == second.r && reversed.g == second.g && reversed.b == second.b,
         "equal-priority winner tracks input order, not a fixed palette magnitude");
}

void TestWorkspaceSharedOverviewLaneLeftClamp() {
  namespace overview = microide::workspace::overview;
  const SDL_FRect track = MakeRect(200.0f, 40.0f, 10.0f, 300.0f);

  // Unclamped (default): the lane sits its full footprint left of the track.
  const SDL_FRect unclamped = overview::LaneRect(track);
  Expect(std::abs(unclamped.x - (track.x - overview::kLaneGap - overview::kLaneWidth)) < 0.01f,
         "default LaneRect places the lane a full gap+width left of the track");

  // A min_x to the right of the natural lane x clamps the lane so it never paints left of
  // the owning surface (regression: the extraction dropped this guard).
  const float surface_left = track.x - 4.0f;  // narrower than gap+width (9px)
  const SDL_FRect clamped = overview::LaneRect(track, surface_left);
  Expect(clamped.x == surface_left, "LaneRect clamps the lane's left edge to min_x");
  Expect(clamped.w == overview::kLaneWidth, "clamping keeps the configured lane width");

  // A min_x left of the natural lane x leaves it untouched.
  const SDL_FRect unaffected = overview::LaneRect(track, 0.0f);
  Expect(unaffected.x == unclamped.x, "a min_x left of the lane does not move it");
}

void TestWorkspaceSharedPanelGeometryHelpers() {
  const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 280.0f, 200.0f);
  const auto content = BottomPanelContentRect(layout);

  // The bottom panel content fills below the header with no reserved command-prompt strip
  // (the command surface is now the overlay command palette).
  Expect(content.y > layout.bottom_panel.y, "panel content should sit below the panel header");
  Expect(content.y + content.h <= layout.bottom_panel.y + layout.bottom_panel.h + 0.5f,
         "panel content should stay within the bottom panel");
  Expect(content.w == layout.bottom_panel.w, "panel content should span the panel width");
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
  Expect(prompt_surface.x == 140.0f && prompt_surface.y == 192.0f &&
             prompt_surface.w == 520.0f && prompt_surface.h == 216.0f,
         "prompt surface rect should stay centered while preserving the shared surface size");

  const auto prompt_buttons = ComputePromptSurfaceButtonRects(prompt_surface);
  Expect(prompt_buttons[0].x == 418.0f && prompt_buttons[1].x == 536.0f &&
             prompt_buttons[0].y == 364.0f,
         "prompt surface button rects should align to the shared footer button layout");

  const SDL_FRect input_rect = ComputePromptSurfaceInputRect(prompt_surface);
  Expect(input_rect.x == 156.0f && input_rect.y == 290.0f && input_rect.w == 488.0f &&
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

  // A track SHORTER than the 24px minimum thumb. The thumb length used to be
  // std::clamp(proportional, kMin, track_length), whose bounds invert here --
  // undefined behaviour, and a library precondition no sanitizer checks, so it
  // survived until a _GLIBCXX_ASSERTIONS build aborted on it. Reachable in
  // production from a small window, a deep editor split, or a panel dragged
  // almost shut; it was found by a menu paint test at a 40px-tall window, where
  // the EDITOR underneath produced the short track.
  for (const float track_height : {1.0f, 4.0f, 12.0f, 23.0f, 24.0f, 25.0f}) {
    const auto short_track =
        ComputeScrollbarThumb(MakeRect(0.0f, 0.0f, 10.0f, track_height), 500.0f, 3.0f, 10.0f, true);
    Expect(short_track.has_value(), "an overflowing short track should still produce a thumb");
    Expect(short_track->h > 0.0f, "the thumb must have a positive height");
    Expect(short_track->h <= track_height,
           "the thumb can never be longer than the track it sits in");
    Expect(short_track->y >= 0.0f && short_track->y + short_track->h <= track_height + 0.001f,
           "the thumb must stay inside the track");
  }

  // The same inversion on the horizontal axis, which takes the other branch.
  for (const float track_width : {1.0f, 7.0f, 23.0f}) {
    const auto short_h =
        ComputeScrollbarThumb(MakeRect(0.0f, 0.0f, track_width, 10.0f), 500.0f, 3.0f, 10.0f, false);
    Expect(short_h.has_value(), "an overflowing short horizontal track should produce a thumb");
    Expect(short_h->w > 0.0f && short_h->w <= track_width,
           "the horizontal thumb must fit inside its track");
  }
}

void TestWorkspaceSharedScrollSurfaceLayout() {
  const auto dual_axis = ComputeScrollSurfaceLayout(MakeRect(0.0f, 0.0f, 200.0f, 120.0f), 20, 8,
                                                    15, 50, 20, 40);
  Expect(dual_axis.show_vertical && dual_axis.show_horizontal,
         "scroll surface layout should flag both axes when both dimensions overflow");
  Expect(dual_axis.max_vertical_scroll == 12 && dual_axis.vertical_scroll == 12,
         "scroll surface layout should clamp vertical scroll to the computed max");
  Expect(dual_axis.max_horizontal_scroll == 30 && dual_axis.horizontal_scroll == 30,
         "scroll surface layout should clamp horizontal scroll to the computed max");
  Expect(dual_axis.content_rect.w == 188.0f && dual_axis.content_rect.h == 108.0f,
         "scroll surface layout should reserve shared scrollbar space from the content rect");
  Expect(dual_axis.vertical_scrollbar.has_value() &&
             dual_axis.vertical_scrollbar->track.h == 104.0f,
         "scroll surface layout should reserve horizontal scrollbar height from the vertical track");
  Expect(dual_axis.horizontal_scrollbar.has_value() &&
             dual_axis.horizontal_scrollbar->track.w == 184.0f,
         "scroll surface layout should reserve vertical scrollbar width from the horizontal track");

  const auto vertical_only = ComputeScrollSurfaceLayout(MakeRect(10.0f, 20.0f, 120.0f, 80.0f), 5,
                                                        8, -4, 12, 24, 9);
  Expect(!vertical_only.show_vertical && !vertical_only.show_horizontal,
         "scroll surface layout should hide scrollbars when both dimensions fit");
  Expect(vertical_only.vertical_scroll == 0 && vertical_only.horizontal_scroll == 0,
         "scroll surface layout should clamp both scroll positions back to zero when content fits");
  Expect(vertical_only.content_rect.x == 10.0f && vertical_only.content_rect.w == 120.0f,
         "scroll surface layout should preserve the full content rect when no scrollbars are needed");
}

void TestWorkspaceSharedTextGridInteractionLayout() {
  const auto layout = ComputeTextGridInteractionLayout(
      MakeRect(10.0f, 20.0f, 180.0f, 90.0f), 42.0f, 32.0f, 16.0f, 8.0f, 4, 12, 6, 5, 14);
  Expect(layout.scroll_line == 4 && layout.visible_rows == 5 && layout.visible_columns == 14,
         "text-grid interaction layout should preserve the requested visible window");
  Expect(!VisibleTextGridLineAtY(layout, 31.0f).has_value(),
         "text-grid interaction layout should reject points above the first visible line");
  const auto first_row_line = VisibleTextGridLineAtY(layout, 32.0f);
  Expect(first_row_line.has_value() && *first_row_line == 4,
         "text-grid interaction layout should map the first visible row back to the scroll line");
  const auto mid_row_line = VisibleTextGridLineAtY(layout, 96.0f);
  Expect(mid_row_line.has_value() && *mid_row_line == 8,
         "text-grid interaction layout should translate visible row offsets into line indices");
  Expect(!VisibleTextGridLineAtY(layout, 112.0f).has_value(),
         "text-grid interaction layout should reject points past the visible row window");
  // The last VISIBLE line, which with scroll_line 4 and 5 visible rows is 8 --
  // rows 0..4 map to lines 4..8, and `VisibleTextGridLineAtY` rejects y=112
  // (row 5) two assertions above for exactly that reason. This asserted 9 while
  // its own message said "last visible line": the clamp was against the document
  // end only, so a pointer held below a pane during a selection drag resolved to
  // a row far past the band and one motion event teleported the selection there.
  Expect(ClampTextGridLineAtY(layout, 120.0f) == 8,
         "text-grid interaction layout should clamp result-surface hits below the window to the last visible line");
  Expect(ClampTextGridLineAtY(layout, 4000.0f) == 8,
         "an arbitrarily large overshoot clamps to the same last visible line, not to the document end");
  Expect(TextGridVisualColumnAtX(layout, 42.0f) == 6 &&
             TextGridVisualColumnAtX(layout, 66.0f) == 9,
         "text-grid interaction layout should translate x positions into visual columns from the text origin");
  Expect(TextGridCursorX(layout, 11) == 82.0f && TextGridLineY(layout, 7) == 80.0f,
         "text-grid interaction layout should round-trip caret positions through the shared geometry");

  const auto clamped_scroll = ComputeTextGridInteractionLayout(
      MakeRect(0.0f, 0.0f, 120.0f, 80.0f), 8.0f, 12.0f, 14.0f, 0.0f, 9, 3, 2, 5, 12);
  Expect(clamped_scroll.scroll_line == 0,
         "text-grid interaction layout should clamp scroll line when content is shorter than the visible rows");
  Expect(ClampTextGridLineAtY(clamped_scroll, 200.0f) == 2,
         "text-grid interaction layout should clamp below-content hits to the last model line");
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
  Expect(ScrollableListItemAtY(discrete, 91.0f, 8) == std::optional<std::size_t>(5) &&
             !ScrollableListItemAtY(discrete, 91.0f, 5).has_value(),
         "item hit testing should range-check the row against the item count");
  Expect(ScrollableListItemAt(discrete, 100.0f, 91.0f, 8) == std::optional<std::size_t>(5) &&
             !ScrollableListItemAt(discrete, 15.0f, 91.0f, 8).has_value(),
         "item hit testing by point should exclude the list's horizontal inset");
  const SDL_FRect item_rect = ScrollableListItemRect(discrete, 5);
  Expect(item_rect.x == row_rect.x && item_rect.y == row_rect.y,
         "an item's rect is its visible row's rect");

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

  // Soak 0156becd453c, 2c8a5f0188c1 and seven more git-row signatures: a list
  // whose top sits at or below its container's bottom -- the git file list under
  // a commit draft that fills a short sidebar -- still got one row, laid out
  // below the container and the window, where the control channel reported it
  // and a click there could select it. No room, no row; and none partly out.
  for (const bool fractional_units : {false, true}) {
    for (const float list_y : {120.0f, 100.0f, 90.0f}) {  // below, at, 10px above the bottom
      const auto none = ComputeScrollableListLayout(MakeRect(0.0f, 0.0f, 200.0f, 100.0f), list_y, 10, 0, 10.0f,
                                                    20.0f, 18.0f, 0.0f, 0.0f, fractional_units);
      Expect(none.visible_rows == 0,
             "a list with no room for a row has none (top " + std::to_string(list_y) +
                 (fractional_units ? ", fractional" : "") + "): " + std::to_string(none.visible_rows));
      Expect(!ScrollableListIndexAtY(none, list_y + 1.0f).has_value(), "and nothing there is a row");
    }
  }
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
  using microide::workspace::EditorSplitOrientation;
  using microide::workspace::EditorSplitTree;

  EditorSplitTree tree;
  Expect(tree.leaf_count() == 1 && !tree.is_split() && tree.node(tree.root()).leaf(),
         "a fresh split tree should be a single pane");

  // Splitting the same way twice in a row keeps ONE flat row (VS Code's grid),
  // not a nest of pairs, and every new pane takes half of the one it split.
  Expect(tree.InsertLeaf(0, EditorSplitOrientation::Vertical, false) == 1,
         "splitting the only pane to the right should seat the new pane after it");
  Expect(tree.InsertLeaf(1, EditorSplitOrientation::Vertical, false) == 2 &&
             tree.leaf_count() == 3 && tree.node(tree.root()).children.size() == 3,
         "a same-axis split should extend the row rather than nest");
  Expect(std::fabs(tree.node(tree.root()).weights[0] - 0.5f) < 0.001f &&
             std::fabs(tree.node(tree.root()).weights[1] - 0.25f) < 0.001f,
         "a new pane should take half of the pane it split");

  // A cross-axis split nests, and dropping that pane again flattens the row back.
  Expect(tree.InsertLeaf(0, EditorSplitOrientation::Horizontal, true) == 0 &&
             tree.leaf_count() == 4 && tree.node(tree.root()).children.size() == 3,
         "a cross-axis split should nest inside the pane it splits");
  tree.RemoveLeaf(0);
  Expect(tree.leaf_count() == 3 && tree.node(tree.root()).children.size() == 3,
         "dropping the nested pane should collapse its branch back into the row");

  // Divider resize moves only the pair it separates.
  const float untouched = tree.node(tree.root()).weights[2];
  Expect(tree.ResizeDivider(tree.root(), 0, 0.25f) &&
             std::fabs(tree.node(tree.root()).weights[2] - untouched) < 0.0001f,
         "resizing a divider should leave the panes it does not touch alone");
  Expect(!tree.ResizeDivider(tree.root(), 7, 0.5f),
         "resizing a divider that does not exist should fail");

  // The cap is a hard one, and the tree stays consistent at it.
  while (!tree.full()) {
    Expect(tree.InsertLeaf(tree.leaf_count() - 1, EditorSplitOrientation::Vertical, false) !=
               EditorSplitTree::kNoLeaf,
           "splitting below the cap should succeed");
  }
  Expect(tree.leaf_count() == microide::workspace::kMaxEditorGroups &&
             tree.InsertLeaf(0, EditorSplitOrientation::Vertical, false) == EditorSplitTree::kNoLeaf,
         "the split tree should refuse to grow past the editor group cap");

  // MoveLeaf relocates without ever growing: it is the one structural edit a FULL
  // grid can still take, which is what lets a pane be moved directionally and what
  // lets another pane's last tab be dropped on an edge at the cap.
  Expect(tree.full(), "the cap fixture should still be full here");
  const std::size_t moved_at_cap = tree.MoveLeaf(microide::workspace::kMaxEditorGroups - 1, 0,
                                                 EditorSplitOrientation::Vertical, true);
  Expect(moved_at_cap != EditorSplitTree::kNoLeaf &&
             tree.leaf_count() == microide::workspace::kMaxEditorGroups,
         "moving a pane at the cap should succeed and keep the pane count");
  Expect(!tree.node(tree.root()).leaf() && tree.NodeForLeaf(moved_at_cap) != EditorSplitTree::kNoNode,
         "the moved pane should still be addressable");
  Expect(tree.MoveLeaf(0, 0, EditorSplitOrientation::Vertical, true) == EditorSplitTree::kNoLeaf &&
             tree.MoveLeaf(0, 99, EditorSplitOrientation::Vertical, true) ==
                 EditorSplitTree::kNoLeaf &&
             tree.MoveLeaf(0, 1, EditorSplitOrientation::None, true) == EditorSplitTree::kNoLeaf,
         "a move onto itself, past the end, or with no orientation should be refused");

  // A row of three: moving the last pane to the front reorders the row.
  EditorSplitTree row;
  row.InsertLeaf(0, EditorSplitOrientation::Vertical, false);
  row.InsertLeaf(1, EditorSplitOrientation::Vertical, false);
  Expect(row.leaf_count() == 3 && row.MoveLeaf(2, 0, EditorSplitOrientation::Vertical, true) == 0,
         "moving the trailing pane before the leading one should land it at ordinal 0");
  Expect(row.leaf_count() == 3 && row.node(row.root()).children.size() == 3,
         "the row should stay one flat branch of three across the move");
  Expect(row.MoveLeaf(0, 1, EditorSplitOrientation::Vertical, true) == EditorSplitTree::kNoLeaf ||
             row.leaf_count() == 3,
         "a same-axis move should never change the pane count");

  // Round-trip through the persisted pre-order form, and reject a malformed one.
  EditorSplitTree restored;
  Expect(restored.Load(tree.Flatten()) && restored == tree,
         "a split tree should survive its flat pre-order form");
  microide::workspace::EditorSplitTreeRecord malformed;
  malformed.push_back(microide::workspace::EditorSplitNodeRecord{
      .orientation = EditorSplitOrientation::Vertical, .weights = {}});
  Expect(!restored.Load(malformed) && restored.leaf_count() == 1,
         "a branch with no children should be rejected and leave a single pane");
}

// Every mutation of the split tree stamps a fresh revision, a copy carries its
// source's, and a refused edit leaves it alone. The shell's pane-rect memo is
// keyed on this (TD-2026-08-30-280), so a mutator that forgot to stamp would
// serve the previous grid's rects for the new one -- and nothing else would say.
void TestWorkspaceSharedEditorSplitTreeRevision() {
  using microide::workspace::EditorSplitOrientation;
  using microide::workspace::EditorSplitTree;

  EditorSplitTree tree;
  std::uint64_t seen = tree.revision();
  auto expect_stamped = [&](const char* what) {
    Expect(tree.revision() != seen, std::string(what) + " should stamp a new tree revision");
    seen = tree.revision();
  };

  Expect(tree.InsertLeaf(0, EditorSplitOrientation::Vertical, false) == 1, "first split");
  expect_stamped("InsertLeaf (leaf becomes a branch)");
  Expect(tree.InsertLeaf(1, EditorSplitOrientation::Vertical, false) == 2, "second split");
  expect_stamped("InsertLeaf (sibling in an existing branch)");
  Expect(tree.ResizeDivider(tree.root(), 0, 0.3f), "divider move");
  expect_stamped("ResizeDivider");
  Expect(!tree.ResizeDivider(tree.root(), 7, 0.3f) && tree.revision() == seen,
         "a refused divider move should leave the revision alone");
  Expect(tree.ResetDivider(tree.root(), 0), "divider reset");
  expect_stamped("ResetDivider");

  const EditorSplitTree copy = tree;
  Expect(copy.revision() == tree.revision() && copy == tree,
         "a copy should carry its source's revision, and be equal to it");

  Expect(tree.MoveLeaf(0, 2, EditorSplitOrientation::Vertical, false) != EditorSplitTree::kNoLeaf,
         "move pane");
  expect_stamped("MoveLeaf");
  Expect(copy.revision() != tree.revision(),
         "mutating the source must not move the copy's revision");
  tree.RemoveLeaf(0);
  expect_stamped("RemoveLeaf");

  EditorSplitTree loaded;
  const std::uint64_t before_load = loaded.revision();
  Expect(loaded.Load(copy.Flatten()) && loaded == copy, "Load should round-trip the copy");
  Expect(loaded.revision() != before_load, "Load should stamp a new revision");
  Expect(loaded.revision() != copy.revision(),
         "two trees of identical shape built independently do not share a revision; "
         "equal revisions imply equal shape, never the converse");

  seen = tree.revision();
  tree.ResetToEvenSplit(3, EditorSplitOrientation::Horizontal);
  expect_stamped("ResetToEvenSplit");
  tree.Reset();
  expect_stamped("Reset");
}

// The struct form of `ComputeLayout` is the one the shell memoizes on; the
// positional form must be exactly the same function, and the key's `==` must
// notice every field -- a field the comparison skipped would be an input the memo
// ignores, which is the stale-hit-test failure the memo exists to rule out.
void TestWorkspaceLayoutInputsStructMatchesPositionalForm() {
  using microide::workspace::ComputeLayout;
  using microide::workspace::LayoutMode;
  using microide::workspace::LayoutModeInputs;
  using microide::workspace::WorkspaceLayoutInputs;

  const LayoutModeInputs mode{.user_override = LayoutModeInputs::Override::Auto,
                              .compact_breakpoint_px = 900.0f,
                              .previous_mode = LayoutMode::Regular};
  const WorkspaceLayoutInputs inputs{
      .window_width = 1280.0f,
      .window_height = 720.0f,
      .sidebar_visible = true,
      .bottom_panel_visible = true,
      .sidebar_width = 260.0f,
      .bottom_panel_height = 180.0f,
      .layout_mode = mode,
      .reserve_status_bar = true,
      .right_pane_visible = true,
      .right_pane_width = 300.0f,
      .project_tab_strip_visible = false,
  };
  Expect(ComputeLayout(inputs) ==
             ComputeLayout(1280.0f, 720.0f, true, true, 260.0f, 180.0f, mode, true, true, 300.0f,
                           false),
         "the positional ComputeLayout must be the struct form, field for field");

  // Each field, changed alone, must make the key unequal AND move the layout: a
  // field that does not move the layout would be dead weight in the key, and one
  // the key ignored would be a stale memo.
  const microide::workspace::WorkspaceLayout base = ComputeLayout(inputs);
  auto expect_field = [&](const char* name, WorkspaceLayoutInputs changed) {
    Expect(!(changed == inputs), std::string(name) + " should make the key unequal");
    Expect(ComputeLayout(changed) != base, std::string(name) + " should move the layout");
  };
  WorkspaceLayoutInputs v = inputs;
  v.window_width = 1400.0f;
  expect_field("window_width", v);
  v = inputs;
  v.window_height = 900.0f;
  expect_field("window_height", v);
  v = inputs;
  v.sidebar_visible = false;
  expect_field("sidebar_visible", v);
  v = inputs;
  v.bottom_panel_visible = false;
  expect_field("bottom_panel_visible", v);
  v = inputs;
  v.sidebar_width = 320.0f;
  expect_field("sidebar_width", v);
  v = inputs;
  v.bottom_panel_height = 240.0f;
  expect_field("bottom_panel_height", v);
  v = inputs;
  v.layout_mode.user_override = LayoutModeInputs::Override::Compact;
  expect_field("layout_mode.user_override", v);
  v = inputs;
  v.layout_mode.previous_mode = LayoutMode::Compact;
  v.layout_mode.compact_breakpoint_px = 1300.0f;  // inside the hysteresis band at 1280
  expect_field("layout_mode.previous_mode+breakpoint", v);
  v = inputs;
  v.reserve_status_bar = false;
  expect_field("reserve_status_bar", v);
  v = inputs;
  v.right_pane_visible = false;
  expect_field("right_pane_visible", v);
  v = inputs;
  v.right_pane_width = 360.0f;
  expect_field("right_pane_width", v);
  v = inputs;
  v.project_tab_strip_visible = true;
  expect_field("project_tab_strip_visible", v);
}

void TestWorkspaceEditorGroupRects() {
  using microide::workspace::EditorSplitOrientation;
  using microide::workspace::EditorSplitTree;
  // A reference single-window layout: sidebar hidden, no bottom panel.
  const auto layout = microide::workspace::ComputeLayout(1000.0f, 700.0f, /*sidebar_visible=*/false,
                                                         /*bottom_panel_visible=*/false, 0.0f, 0.0f);

  // Single group reproduces the base layout rects byte-for-byte.
  EditorSplitTree tree;
  const auto one = microide::workspace::ComputeEditorGroupRects(layout, tree);
  Expect(one.groups.size() == 1 && one.dividers.empty(),
         "single editor group should produce one rect and no divider");
  Expect(microide::workspace::RectsEqual(one.groups[0].tab_strip, layout.tab_strip) &&
             microide::workspace::RectsEqual(one.groups[0].breadcrumb, layout.breadcrumb) &&
             microide::workspace::RectsEqual(one.groups[0].editor_surface, layout.editor_surface),
         "single editor group rects should equal the base layout rects");

  // Side-by-side split: surfaces partition the editor-area width with a divider
  // between them; both groups keep their own breadcrumb band.
  EditorSplitTree side_by_side;
  side_by_side.InsertLeaf(0, EditorSplitOrientation::Vertical, false);
  const auto vertical = microide::workspace::ComputeEditorGroupRects(layout, side_by_side);
  Expect(vertical.groups.size() == 2 && vertical.dividers.size() == 1 &&
             vertical.dividers[0].vertical,
         "side-by-side split should produce two groups and a vertical divider");
  Expect(vertical.groups[0].editor_surface.x == layout.editor_surface.x,
         "left group surface should start at the editor area left edge");
  Expect(vertical.groups[1].editor_surface.x ==
             vertical.groups[0].editor_surface.x + vertical.groups[0].editor_surface.w +
                 microide::workspace::kWorkspaceEditorSplitDividerThickness,
         "right group surface should begin after the left surface plus the divider");
  Expect(vertical.groups[0].editor_surface.y == layout.editor_surface.y &&
             vertical.groups[0].editor_surface.h == layout.editor_surface.h,
         "side-by-side surfaces should keep the full editor-surface height");
  Expect(vertical.groups[1].breadcrumb.w > 0.0f,
         "side-by-side second group should keep its own breadcrumb band");
  Expect(vertical.dividers[0].pair_extent ==
             vertical.groups[0].editor_surface.w + vertical.groups[1].editor_surface.w,
         "a divider should span the two panes it can resize");

  // Stacked split: group 0 keeps the top tab strip; group 1 synthesizes a tab
  // strip inside its own region, with no breadcrumb of its own.
  EditorSplitTree stacked;
  stacked.InsertLeaf(0, EditorSplitOrientation::Horizontal, false);
  const auto horizontal = microide::workspace::ComputeEditorGroupRects(layout, stacked);
  Expect(horizontal.groups.size() == 2 && horizontal.dividers.size() == 1 &&
             !horizontal.dividers[0].vertical,
         "stacked split should produce two groups and a horizontal divider");
  Expect(microide::workspace::RectsEqual(horizontal.groups[0].tab_strip, layout.tab_strip),
         "stacked first group should keep the global top tab strip");
  Expect(horizontal.groups[1].breadcrumb.w == 0.0f,
         "stacked second group should have no breadcrumb band");
  Expect(horizontal.groups[1].tab_strip.y >= layout.editor_surface.y &&
             horizontal.groups[1].tab_strip.h > 0.0f,
         "stacked second group should synthesize a tab strip inside the editor surface");
  Expect(horizontal.groups[1].editor_surface.y >=
             horizontal.groups[1].tab_strip.y + horizontal.groups[1].tab_strip.h,
         "stacked second group surface should sit below its synthesized tab strip");

  // Three panes in a row, then the middle one split downward: the rects tile the
  // editor area with one divider per gap, and the nested pair stays inside the
  // column it was carved from.
  EditorSplitTree grid;
  grid.InsertLeaf(0, EditorSplitOrientation::Vertical, false);
  grid.InsertLeaf(1, EditorSplitOrientation::Vertical, false);
  grid.InsertLeaf(1, EditorSplitOrientation::Horizontal, false);
  const auto four = microide::workspace::ComputeEditorGroupRects(layout, grid);
  Expect(four.groups.size() == 4 && four.dividers.size() == 3,
         "four panes should produce four rect sets and three dividers");
  Expect(four.groups[1].editor_surface.x == four.groups[2].editor_surface.x &&
             four.groups[2].editor_surface.y > four.groups[1].editor_surface.y,
         "a pane split downward should stack inside its own column");
  Expect(four.groups[3].editor_surface.x + four.groups[3].editor_surface.w ==
             layout.editor_area.x + layout.editor_area.w,
         "the last column should reach the editor area's right edge");
  Expect(four.groups[2].breadcrumb.w == 0.0f && four.groups[1].breadcrumb.w > 0.0f,
         "only panes at the top of the editor area should carry a breadcrumb");

  // Directional neighbours read straight off those rects: pane 0 is the left
  // column, 1/2 are the top/bottom of the middle column, 3 is the right column.
  using microide::workspace::AdjacentEditorGroup;
  using microide::workspace::EditorGroupDirection;
  using microide::workspace::kNoEditorGroup;
  Expect(AdjacentEditorGroup(four, 0, EditorGroupDirection::Right) == 1,
         "right of the left column is the top pane of the middle column");
  Expect(AdjacentEditorGroup(four, 0, EditorGroupDirection::Left) == kNoEditorGroup &&
             AdjacentEditorGroup(four, 0, EditorGroupDirection::Up) == kNoEditorGroup &&
             AdjacentEditorGroup(four, 0, EditorGroupDirection::Down) == kNoEditorGroup,
         "a full-height leftmost column has no neighbour left, above or below it");
  Expect(AdjacentEditorGroup(four, 1, EditorGroupDirection::Down) == 2 &&
             AdjacentEditorGroup(four, 2, EditorGroupDirection::Up) == 1,
         "the stacked pair should be each other's vertical neighbours");
  Expect(AdjacentEditorGroup(four, 1, EditorGroupDirection::Left) == 0 &&
             AdjacentEditorGroup(four, 2, EditorGroupDirection::Left) == 0,
         "both halves of the middle column see the left column beside them");
  Expect(AdjacentEditorGroup(four, 3, EditorGroupDirection::Right) == kNoEditorGroup,
         "nothing lies right of the rightmost column");
  Expect(AdjacentEditorGroup(four, 3, EditorGroupDirection::Left) == 1,
         "the pane left of the right column is the nearer of the stacked pair");
  Expect(AdjacentEditorGroup(one, 0, EditorGroupDirection::Right) == kNoEditorGroup,
         "a single pane has no neighbour in any direction");
  Expect(AdjacentEditorGroup(four, 9, EditorGroupDirection::Right) == kNoEditorGroup,
         "an out-of-range origin resolves to no neighbour");
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
      120.0f, 30.0f, 260.0f, 18.0f, 5, 8, 508.0f, 90.0f, 22.0f, 320.0f);
  Expect(accept_rect.x == 150.0f && accept_rect.y == 316.0f,
         "merge source action rect should anchor to the pane gutter and source line");

  const std::array<float, 4> action_widths = {64.0f, 82.0f, 84.0f, 70.0f};
  const auto action_rects = ComputeMergeResultActionButtonRects(
      320.0f, 260.0f, 508.0f, visible_rect, action_widths, 22.0f, 8.0f, 700.0f);
  Expect(action_rects[0].x == 320.0f && action_rects[0].y == 355.0f,
         "merge result action rects should start below the visible conflict span");
  Expect(action_rects[1].x == 392.0f && action_rects[2].x == 482.0f &&
             action_rects[3].x == 574.0f,
         "merge result action rects should advance by width plus shared gap");

  const auto clamped_action_rects = ComputeMergeResultActionButtonRects(
      320.0f, 260.0f, 380.0f, std::optional<SDL_FRect>{MakeRect(320.0f, 360.0f, 212.0f, 30.0f)},
      action_widths, 22.0f, 8.0f, 700.0f);
  Expect(clamped_action_rects[0].y == 336.0f,
         "merge result action rects should move above the conflict when they would overflow");
}

// A conflict's buttons stay inside their pane. In a narrow merge pane the
// "Accept Local" button (114 px) ran past the right pane into the window's edge,
// and the result row's buttons into the Local pane (soak a67e96e83bf3,
// 4e21043cd592, 1b97fc7b724e). A source button with no room is not laid out;
// the result row wraps onto further rows inside the Result pane, so all four
// stay reachable in an ordinary window (its pane is narrower than the row at
// 1280x900), and only a button wider than the pane itself is left out. Empty
// rects are skipped by paint, the hover classifier and the QA element source.
void TestWorkspaceSharedMergeConflictButtonsStayInsideTheirPane() {
  const SDL_FRect roomy = ComputeMergeSourceActionButtonRect(
      120.0f, 30.0f, 260.0f, 18.0f, 0, 2, 508.0f, 114.0f, 22.0f, 320.0f);
  Expect(roomy.w == 114.0f, "a source button with room keeps its width");
  const SDL_FRect squeezed = ComputeMergeSourceActionButtonRect(
      541.0f, 30.0f, 260.0f, 18.0f, 0, 2, 508.0f, 114.0f, 22.0f, 660.0f);
  Expect(squeezed.w == 0.0f || squeezed.x + squeezed.w <= 660.0f,
         "a source button never runs past its pane's right edge");
  Expect(squeezed.w == 0.0f, "a source button with no room in its pane is not laid out");

  const std::array<float, 4> widths = {64.0f, 82.0f, 84.0f, 70.0f};
  const SDL_FRect conflict = MakeRect(422.0f, 330.0f, 200.0f, 36.0f);
  const auto rects = ComputeMergeResultActionButtonRects(450.0f, 260.0f, 508.0f, conflict, widths,
                                                         22.0f, 8.0f, 622.0f);
  for (std::size_t i = 0; i < rects.size(); ++i) {
    Expect(rects[i].w == widths[i], "every result button is laid out in a pane as wide as one of them");
    Expect(rects[i].x >= 450.0f && rects[i].x + rects[i].w <= 622.0f,
           "a result button stays inside the Result pane");
    Expect(rects[i].y >= conflict.y + conflict.h && rects[i].y + rects[i].h <= 508.0f - 4.0f,
           "the wrapped rows sit below the conflict, above the content's bottom");
    for (std::size_t j = 0; j < i; ++j) {
      const bool apart = rects[i].x >= rects[j].x + rects[j].w || rects[j].x >= rects[i].x + rects[i].w ||
                         rects[i].y >= rects[j].y + rects[j].h || rects[j].y >= rects[i].y + rects[i].h;
      Expect(apart, "wrapped result buttons do not overlap one another");
    }
  }
  Expect(rects[1].y == rects[0].y && rects[2].y > rects[0].y, "the row wraps where the next button does not fit");
  // Near the content's bottom the whole block moves above the conflict.
  const auto lifted = ComputeMergeResultActionButtonRects(450.0f, 260.0f, 380.0f, conflict, widths, 22.0f,
                                                          8.0f, 622.0f);
  Expect(lifted[3].y + lifted[3].h <= conflict.y && lifted[0].y >= 260.0f,
         "a wrapped block that would overflow the content moves above the conflict");
  // A pane narrower than a button leaves that button out.
  const auto narrow = ComputeMergeResultActionButtonRects(450.0f, 260.0f, 508.0f, conflict, widths, 22.0f,
                                                          8.0f, 520.0f);
  Expect(narrow[0].w == 64.0f && narrow[1].w == 0.0f && narrow[2].w == 0.0f && narrow[3].w == 70.0f &&
             narrow[3].x + narrow[3].w <= 520.0f && narrow[3].y > narrow[0].y,
         "a result button wider than the Result pane is not laid out; the rest still wrap inside it");
}

void TestWorkspaceSharedMergeHoverClassifier() {
  const MergeHoverSurfaceLayout surface = {
      .gutter_width = 32.0f,
      .left_x = 120.0f,
      .center_x = 320.0f,
      .right_x = 560.0f,
      .rows_y = 260.0f,
      .line_height = 18.0f,
      .left_width = 200.0f,
      .center_width = 240.0f,
      .right_width = 180.0f,
  };
  const SDL_FRect editor_surface = MakeRect(100.0f, 200.0f, 640.0f, 320.0f);
  const SDL_FRect result_rect = ComputeMergeResultViewportRect(editor_surface, surface.center_x,
                                                               surface.rows_y, surface.gutter_width,
                                                               180.0f, true);
  const MergeHoverInteractionLayout interaction = {
      .content_bottom = 508.0f,
      .incoming =
          ComputeTextGridInteractionLayout(MakeRect(surface.left_x, surface.rows_y, 180.0f, 72.0f),
                                           surface.left_x + surface.gutter_width, surface.rows_y,
                                           surface.line_height, 8.0f, 5, 20, 0, 4, 20),
      .current =
          ComputeTextGridInteractionLayout(MakeRect(surface.right_x, surface.rows_y, 180.0f, 72.0f),
                                           surface.right_x + surface.gutter_width, surface.rows_y,
                                           surface.line_height, 8.0f, 5, 20, 0, 4, 20),
      .result =
          MergeHoverResultLayout{
              .rect = result_rect,
              .lines =
                  VisibleLineRangeLayout{
                      .first_line_y = 300.0f,
                      .line_height = 18.0f,
                      .scroll_line = 5,
                      .visible_rows = 4,
                  },
              .text = ComputeTextGridInteractionLayout(result_rect, 352.0f, 300.0f,
                                                       surface.line_height, 8.0f, 5, 20, 0, 4, 20),
          },
      .incoming_accept_button_width = 90.0f,
      .current_accept_button_width = 92.0f,
      .result_action_widths = {40.0f, 44.0f, 44.0f, 40.0f},  // all four fit the Result pane
      .button_height = 22.0f,
      .button_gap = 8.0f,
  };
  const std::vector<MergeTrackedConflict> conflicts = {
      MergeTrackedConflict{
          .hunk_index = 0,
          .incoming_start_line = 6,
          .incoming_end_line = 8,
          .current_start_line = 6,
          .current_end_line = 8,
          .start_line = 6,
          .end_line = 8,
          .last_choice = microide::compare::MergeChoice::Base,
          .valid = true,
      },
  };

  const SDL_FRect incoming_accept_rect = ComputeMergeSourceActionButtonRect(
      surface.left_x, surface.gutter_width, surface.rows_y, surface.line_height,
      static_cast<int>(interaction.result.text.scroll_line), conflicts[0].incoming_end_line,
      interaction.content_bottom, interaction.incoming_accept_button_width, interaction.button_height,
      surface.left_x + surface.left_width);
  const auto result_action_rects = ComputeMergeResultActionButtonRects(
      surface.center_x + surface.gutter_width, surface.rows_y, interaction.content_bottom,
      ComputeVisibleLineRangeRect(result_rect, interaction.result.lines, conflicts[0].start_line,
                                  conflicts[0].end_line),
      interaction.result_action_widths, interaction.button_height, interaction.button_gap,
      surface.center_x + surface.center_width);

  const auto incoming_accept_hover =
      ClassifyMergeHoverState(surface, interaction, conflicts,
                              incoming_accept_rect.x + incoming_accept_rect.w * 0.5f,
                              incoming_accept_rect.y + incoming_accept_rect.h * 0.5f);
  Expect(incoming_accept_hover.has_value(),
         "merge hover classifier should return a state for the incoming accept button");
  Expect(incoming_accept_hover->kind == MergeHoverState::Kind::IncomingAccept,
         "merge hover classifier should prefer the incoming accept button over source content");

  const auto current_conflict_hover = ClassifyMergeHoverState(
      surface, interaction, conflicts, surface.right_x + surface.gutter_width + 8.0f,
      surface.rows_y + 27.0f);
  Expect(current_conflict_hover.has_value(),
         "merge hover classifier should return a state for current-source conflict content");
  Expect(current_conflict_hover->kind == MergeHoverState::Kind::CurrentConflict &&
             current_conflict_hover->preview_choice == microide::compare::MergeChoice::Current,
         "merge hover classifier should classify current-source conflict content");

  const auto result_action_hover =
      ClassifyMergeHoverState(surface, interaction, conflicts,
                              result_action_rects[3].x + result_action_rects[3].w * 0.5f,
                              result_action_rects[3].y + result_action_rects[3].h * 0.5f);
  Expect(result_action_hover.has_value(),
         "merge hover classifier should return a state for result action buttons");
  Expect(result_action_hover->kind == MergeHoverState::Kind::ResultAction &&
             result_action_hover->preview_choice == microide::compare::MergeChoice::Both,
         "merge hover classifier should prefer explicit result actions over result conflict content");

  const auto result_conflict_hover =
      ClassifyMergeHoverState(surface, interaction, conflicts, interaction.result.text.text_x + 8.0f,
                              interaction.result.lines.first_line_y + interaction.result.lines.line_height +
                                  9.0f);
  Expect(result_conflict_hover.has_value(),
         "merge hover classifier should return a state for result conflict content");
  Expect(result_conflict_hover->kind == MergeHoverState::Kind::ResultConflict,
         "merge hover classifier should classify result conflict content when no action button is hit");

  Expect(!ClassifyMergeHoverState(surface, interaction, conflicts, 20.0f, 20.0f).has_value(),
         "merge hover classifier should ignore pointers outside the merge panes");
}

// B3: zero-length source spans (start == end) describe pure insertion/deletion
// conflicts where one side contributes no source lines. They must still own their
// anchor row for source hover/tint/accept hit-testing, exactly as the result-side
// lookup normalizes zero-length ranges.
void TestWorkspaceSharedMergeZeroLengthSourceHitTest() {
  // Pure insertion on the incoming side: incoming span is zero-length at line 4,
  // current side inserts two lines [4, 6).
  const std::vector<MergeTrackedConflict> insertion = {
      MergeTrackedConflict{
          .hunk_index = 0,
          .incoming_start_line = 4,
          .incoming_end_line = 4,  // zero-length -> pure insertion, no incoming source lines
          .current_start_line = 4,
          .current_end_line = 6,
          .start_line = 4,
          .end_line = 6,
          .valid = true,
      },
  };
  Expect(FindMergeTrackedConflictAtSourceLine(insertion, 4, /*incoming=*/true) == std::size_t{0},
         "zero-length incoming source span should still match its anchor row");
  Expect(!FindMergeTrackedConflictAtSourceLine(insertion, 5, /*incoming=*/true).has_value(),
         "a normalized zero-length span should only own the single anchor row");
  Expect(FindMergeTrackedConflictAtSourceLine(insertion, 4, /*incoming=*/false) == std::size_t{0},
         "the non-empty current source span should match across its two rows");
  Expect(FindMergeTrackedConflictAtSourceLine(insertion, 5, /*incoming=*/false) == std::size_t{0},
         "the non-empty current source span should match its second row");

  // Pure deletion on the current side: current span is zero-length at line 7.
  const std::vector<MergeTrackedConflict> deletion = {
      MergeTrackedConflict{
          .hunk_index = 0,
          .incoming_start_line = 7,
          .incoming_end_line = 9,
          .current_start_line = 7,
          .current_end_line = 7,  // zero-length -> pure deletion, no current source lines
          .start_line = 7,
          .end_line = 9,
          .valid = true,
      },
  };
  Expect(FindMergeTrackedConflictAtSourceLine(deletion, 7, /*incoming=*/false) == std::size_t{0},
         "zero-length current source span should still match its anchor row");
  Expect(!FindMergeTrackedConflictAtSourceLine(deletion, 8, /*incoming=*/false).has_value(),
         "a normalized zero-length current span should only own the single anchor row");
  Expect(FindMergeTrackedConflictAtSourceLine(deletion, 8, /*incoming=*/true) == std::size_t{0},
         "the non-empty incoming source span should still match its rows");
}

// B4: source-pane accept buttons must anchor to the SOURCE pane's own scroll, not
// the result pane's clamped scroll. When a source pane is longer than the result,
// the result scroll clamps lower, so a shared scroll would drift the button off its
// rendered source row.
void TestWorkspaceSharedMergeSourceButtonUsesSourceScroll() {
  const MergeHoverSurfaceLayout surface = {
      .gutter_width = 32.0f,
      .left_x = 120.0f,
      .center_x = 320.0f,
      .right_x = 560.0f,
      .rows_y = 260.0f,
      .line_height = 18.0f,
      .left_width = 200.0f,
      .center_width = 240.0f,
      .right_width = 180.0f,
  };
  const SDL_FRect editor_surface = MakeRect(100.0f, 200.0f, 640.0f, 320.0f);
  const SDL_FRect result_rect = ComputeMergeResultViewportRect(editor_surface, surface.center_x,
                                                               surface.rows_y, surface.gutter_width,
                                                               180.0f, true);
  // Source panes scrolled to line 8; the (shorter) result pane clamps its scroll to
  // line 3. A conflict whose incoming source ends at line 12 renders at a source-pane
  // relative row of (12 - 8), so the accept button must anchor there, not at (12 - 3).
  const std::size_t source_scroll = 8;
  const std::size_t result_scroll = 3;
  const MergeHoverInteractionLayout interaction = {
      .content_bottom = 900.0f,
      .incoming = ComputeTextGridInteractionLayout(
          MakeRect(surface.left_x, surface.rows_y, 180.0f, 360.0f),
          surface.left_x + surface.gutter_width, surface.rows_y, surface.line_height, 8.0f,
          source_scroll, 40, 0, 20, 20),
      .current = ComputeTextGridInteractionLayout(
          MakeRect(surface.right_x, surface.rows_y, 180.0f, 360.0f),
          surface.right_x + surface.gutter_width, surface.rows_y, surface.line_height, 8.0f,
          source_scroll, 40, 0, 20, 20),
      .result =
          MergeHoverResultLayout{
              .rect = result_rect,
              .lines =
                  VisibleLineRangeLayout{
                      .first_line_y = surface.rows_y,
                      .line_height = surface.line_height,
                      .scroll_line = result_scroll,
                      .visible_rows = 20,
                  },
              .text = ComputeTextGridInteractionLayout(result_rect, 352.0f, surface.rows_y,
                                                       surface.line_height, 8.0f, result_scroll, 20,
                                                       0, 20, 20),
          },
      .incoming_accept_button_width = 90.0f,
      .current_accept_button_width = 92.0f,
      .result_action_widths = {64.0f, 82.0f, 84.0f, 70.0f},
      .button_height = 22.0f,
      .button_gap = 8.0f,
  };
  const std::vector<MergeTrackedConflict> conflicts = {
      MergeTrackedConflict{
          .hunk_index = 0,
          .incoming_start_line = 10,
          .incoming_end_line = 12,
          .current_start_line = 10,
          .current_end_line = 12,
          .start_line = 10,
          .end_line = 12,
          .valid = true,
      },
  };

  // The rendered source accept button uses the SOURCE scroll (source_scroll).
  const SDL_FRect source_anchored = ComputeMergeSourceActionButtonRect(
      surface.left_x, surface.gutter_width, surface.rows_y, surface.line_height,
      static_cast<int>(source_scroll), conflicts[0].incoming_end_line, interaction.content_bottom,
      interaction.incoming_accept_button_width, interaction.button_height,
      surface.left_x + surface.left_width);
  // The old buggy geometry would have used the result scroll and landed on a
  // different row; make sure the two actually differ so this test is meaningful.
  const SDL_FRect result_anchored = ComputeMergeSourceActionButtonRect(
      surface.left_x, surface.gutter_width, surface.rows_y, surface.line_height,
      static_cast<int>(result_scroll), conflicts[0].incoming_end_line, interaction.content_bottom,
      interaction.incoming_accept_button_width, interaction.button_height,
      surface.left_x + surface.left_width);
  Expect(source_anchored.y != result_anchored.y,
         "source and result scroll must produce different button rows for this fixture");

  // The classifier must hit-test at the source-anchored position (not the result one).
  const auto hover = ClassifyMergeHoverState(surface, interaction, conflicts,
                                             source_anchored.x + source_anchored.w * 0.5f,
                                             source_anchored.y + source_anchored.h * 0.5f);
  Expect(hover.has_value() && hover->kind == MergeHoverState::Kind::IncomingAccept,
         "merge hover classifier should hit the source accept button at its source-scrolled row");
  const auto miss = ClassifyMergeHoverState(surface, interaction, conflicts,
                                            result_anchored.x + result_anchored.w * 0.5f,
                                            result_anchored.y + result_anchored.h * 0.5f);
  Expect(!miss.has_value() || miss->kind != MergeHoverState::Kind::IncomingAccept,
         "the stale result-scrolled row must no longer register as the accept button");
}

void TestWorkspaceSharedOverlayRectHelpers() {
  const SDL_FRect roomy = ComputeQuickOpenOverlaySurfaceRect(MakeRect(100.0f, 200.0f, 1200.0f, 800.0f),
                                                               MakeRect(0.0f, 0.0f, 4000.0f, 4000.0f));
  Expect(std::fabs(roomy.w - 792.0f) < 0.01f,
         "overlay rect should use the preferred width ratio when within bounds");
  Expect(std::fabs(roomy.h - 360.0f) < 0.01f,
         "overlay rect should clamp height to the shared overlay maximum");
  Expect(std::fabs(roomy.x - 304.0f) < 0.01f,
         "overlay rect should stay horizontally centered in the editor area");
  Expect(std::fabs(roomy.y - 279.2f) < 0.01f,
         "overlay rect should bias vertically toward the top of the editor area");

  const SDL_FRect compact = ComputeQuickOpenOverlaySurfaceRect(MakeRect(0.0f, 0.0f, 300.0f, 200.0f),
                                                                 MakeRect(-100.0f, -100.0f, 4000.0f, 4000.0f));
  Expect(std::fabs(compact.w - 260.0f) < 0.01f,
         "overlay rect should clamp width to the compact fallback width");
  Expect(std::fabs(compact.h - 160.0f) < 0.01f,
         "overlay rect should clamp height to the compact fallback height");
  Expect(std::fabs(compact.x - 20.0f) < 0.01f,
         "overlay rect should remain centered after compact-width clamping");
  Expect(std::fabs(compact.y - 7.2f) < 0.01f,
         "overlay rect should preserve the vertical bias after compact-height clamping");

  // QA resize-overlays: at 640x480 with the sidebar shown the editor area is 351 px
  // wide, the Settings card's minimum is 420, and centring it on the editor area
  // ran its right third off the window. A card wider than its anchor stays inside
  // the window instead.
  const SDL_FRect window = MakeRect(0.0f, 0.0f, 640.0f, 480.0f);
  const SDL_FRect narrow_editor = MakeRect(289.0f, 58.0f, 351.0f, 240.0f);
  const SDL_FRect settings = ComputeSettingsOverlaySurfaceRect(narrow_editor, window);
  Expect(settings.x >= window.x && settings.x + settings.w <= window.x + window.w,
         "the Settings card stays inside a narrow window (" + std::to_string(settings.x) + " + " +
             std::to_string(settings.w) + ")");
  Expect(settings.y >= window.y && settings.y + settings.h <= window.y + window.h,
         "and inside its height");
  const SDL_FRect tiny = ComputeSettingsOverlaySurfaceRect(narrow_editor, MakeRect(0.0f, 0.0f, 360.0f, 200.0f));
  Expect(tiny.w <= 360.0f && tiny.x >= 0.0f && tiny.x + tiny.w <= 360.0f,
         "a window narrower than the card's minimum shrinks the card to the window");
}

void TestWorkspaceSharedHitTargets() {
  const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 300.0f, 180.0f);

  // The resize grab pad equals the cursor-change region exactly (no over-extension).
  const SDL_FRect sidebar_visual = SidebarResizeHandleRect(layout);
  const SDL_FRect sidebar_hit = SidebarResizeHitRect(layout);
  Expect(RectsEqual(sidebar_hit, SidebarResizeCursorRect(layout)),
         "sidebar resize hit rect must equal the cursor rect");
  Expect(sidebar_hit.x <= sidebar_visual.x && sidebar_hit.y == sidebar_visual.y,
         "sidebar hit rect should inflate horizontally only");

  const SDL_FRect panel_visual = BottomPanelResizeHandleRect(layout);
  const SDL_FRect panel_hit = BottomPanelResizeHitRect(layout);
  Expect(RectsEqual(panel_hit, BottomPanelResizeCursorRect(layout)),
         "bottom panel resize hit rect must equal the cursor rect");
  Expect(panel_hit.y <= panel_visual.y && panel_hit.x == panel_visual.x,
         "panel hit rect should inflate vertically only");

  const auto vertical = MakeVerticalScrollbarGeometry(MakeRect(0.0f, 0.0f, 100.0f, 200.0f),
                                                     100.0f, 20.0f, 30.0f, false);
  Expect(vertical.has_value(), "vertical scrollbar geometry should exist for hit-rect test");
  const SDL_FRect vert_hit = VerticalScrollbarHitRect(*vertical);
  Expect(vert_hit.w >= 18.0f,
         "vertical scrollbar hit rect should meet the documented 18px cross-axis size");

  const auto horizontal = MakeHorizontalScrollbarGeometry(MakeRect(0.0f, 0.0f, 200.0f, 100.0f),
                                                          80.0f, 20.0f, 10.0f, false);
  Expect(horizontal.has_value(), "horizontal scrollbar geometry should exist for hit-rect test");
  const SDL_FRect horiz_hit = HorizontalScrollbarHitRect(*horizontal);
  Expect(horiz_hit.h >= 18.0f,
         "horizontal scrollbar hit rect should meet the documented 18px cross-axis size");

  const SDL_FRect tab_rect = MakeRect(100.0f, 30.0f, 160.0f, 24.0f);
  const SDL_FRect close_visual = MakeRect(240.0f, 35.0f, 14.0f, 14.0f);
  const SDL_FRect close_hit = TabCloseHitRect(close_visual, tab_rect);
  Expect(close_hit.w >= 16.0f && close_hit.h >= 18.0f,
         "tab close hit rect should expand toward 20x20 within the tab bounds");
  Expect(close_hit.x >= tab_rect.x &&
             close_hit.x + close_hit.w <= tab_rect.x + tab_rect.w + 0.01f,
         "tab close hit rect must stay inside the tab rectangle");
}

void TestWorkspaceHitTargetClickRouting() {
  const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 300.0f, 180.0f);

  const SDL_FRect sidebar_visual = SidebarResizeHandleRect(layout);
  const SDL_FRect sidebar_hit = SidebarResizeHitRect(layout);

  // The hit pad equals the cursor rect, which inflates the visual seam by 1px each
  // side: a click 1px outside the visual divider still lands on the seam, but a click
  // deep in the editor must not.
  const float just_outside_left = sidebar_visual.x - 1.0f;
  const float just_outside_right = sidebar_visual.x + sidebar_visual.w + 0.5f;
  const float mid_y = sidebar_visual.y + sidebar_visual.h * 0.5f;
  Expect(microide::workspace::Contains(sidebar_hit, just_outside_left, mid_y),
         "click 1px left of the visual divider must land inside the hit rect");
  Expect(microide::workspace::Contains(sidebar_hit, just_outside_right, mid_y),
         "click just right of the visual divider must land inside the hit rect");

  const float editor_text_x = layout.editor_surface.x + 80.0f;
  Expect(!microide::workspace::Contains(sidebar_hit, editor_text_x, mid_y),
         "click well inside the editor must NOT land on the sidebar resize hit pad");

  const SDL_FRect panel_visual = BottomPanelResizeHandleRect(layout);
  const SDL_FRect panel_hit = BottomPanelResizeHitRect(layout);
  const float just_above = panel_visual.y - 1.0f;
  const float just_below = panel_visual.y + panel_visual.h + 0.5f;
  const float mid_x = panel_visual.x + panel_visual.w * 0.5f;
  Expect(microide::workspace::Contains(panel_hit, mid_x, just_above),
         "click 1px above the bottom-panel divider must land inside the hit rect");
  Expect(microide::workspace::Contains(panel_hit, mid_x, just_below),
         "click just below the bottom-panel divider must land inside the hit rect");
}

void TestWorkspaceLayoutModeResolution() {
  LayoutModeInputs inputs;
  inputs.compact_breakpoint_px = kWorkspaceLayoutCompactBreakpointDefault;
  inputs.previous_mode = LayoutMode::Regular;

  Expect(ResolveLayoutMode(900.0f, inputs) == LayoutMode::Regular,
         "wide windows resolve to Regular");
  Expect(ResolveLayoutMode(600.0f, inputs) == LayoutMode::Compact,
         "narrow windows resolve to Compact");

  inputs.previous_mode = LayoutMode::Compact;
  Expect(ResolveLayoutMode(kWorkspaceLayoutCompactBreakpointDefault, inputs) == LayoutMode::Compact,
         "hysteresis keeps Compact at the breakpoint when starting Compact");
  Expect(ResolveLayoutMode(kWorkspaceLayoutCompactBreakpointDefault +
                               kWorkspaceLayoutCompactHysteresis + 3.0f,
                           inputs) == LayoutMode::Regular,
         "hysteresis flips to Regular only past breakpoint + 12");

  inputs.user_override = LayoutModeInputs::Override::Regular;
  Expect(ResolveLayoutMode(640.0f, inputs) == LayoutMode::Regular,
         "Regular override defeats narrow auto");
  inputs.user_override = LayoutModeInputs::Override::Compact;
  Expect(ResolveLayoutMode(1920.0f, inputs) == LayoutMode::Compact,
         "Compact override defeats wide auto");
}

void TestLayoutModeServiceFlipFlop() {
  microide::workspace::LayoutModeService service;
  service.SetCompactBreakpointPx(kWorkspaceLayoutCompactBreakpointDefault);

  // Wide window: should resolve Regular.
  service.SetCurrentMode(ResolveLayoutMode(900.0f, service.SnapshotInputs()));
  Expect(service.CurrentMode() == LayoutMode::Regular,
         "service should hold Regular for a wide window");

  // Narrow window: flip to Compact.
  service.SetCurrentMode(ResolveLayoutMode(600.0f, service.SnapshotInputs()));
  Expect(service.CurrentMode() == LayoutMode::Compact,
         "service should flip to Compact below the breakpoint");

  // Hover at breakpoint: hysteresis keeps Compact.
  service.SetCurrentMode(ResolveLayoutMode(kWorkspaceLayoutCompactBreakpointDefault,
                                           service.SnapshotInputs()));
  Expect(service.CurrentMode() == LayoutMode::Compact,
         "service hysteresis should hold Compact at the breakpoint");

  // Cross hysteresis upward: flip to Regular.
  service.SetCurrentMode(ResolveLayoutMode(kWorkspaceLayoutCompactBreakpointDefault +
                                               kWorkspaceLayoutCompactHysteresis + 3.0f,
                                           service.SnapshotInputs()));
  Expect(service.CurrentMode() == LayoutMode::Regular,
         "service should flip to Regular only past breakpoint + hysteresis");

  // Override defeats auto.
  service.SetUserOverride(LayoutModeInputs::Override::Compact);
  service.SetCurrentMode(ResolveLayoutMode(1920.0f, service.SnapshotInputs()));
  Expect(service.CurrentMode() == LayoutMode::Compact,
         "user override should defeat the wide-window auto resolution");
}

void TestWorkspaceSharedEmptyTabStripPlaceholderRect() {
  const SDL_FRect strip = MakeRect(12.0f, 30.0f, 800.0f, 26.0f);
  const SDL_FRect rect = EmptyTabStripPlaceholderRect(strip);
  Expect(rect.x == strip.x, "placeholder shares the strip left edge");
  Expect(rect.y == strip.y + 2.0f, "placeholder drops 2px below the strip top");
  Expect(rect.w == 220.0f, "placeholder uses the fixed welcome-tab width");
  Expect(rect.h == 24.0f, "placeholder height is strip height minus 2px");
  const SDL_FRect thin = EmptyTabStripPlaceholderRect(MakeRect(0.0f, 0.0f, 100.0f, 10.0f));
  Expect(thin.h == 22.0f, "placeholder height clamps to the 22px minimum");
}

void TestWorkspaceSharedBottomPanelLineIndexAtY() {
  const float text_y = 100.0f;
  const float line_height = 18.0f;
  const int visible_rows = 5;
  const std::size_t line_count = 40;

  // Above the first row rejects (floor, not snap-to-row-0).
  Expect(!BottomPanelLineIndexAtY(text_y, line_height, visible_rows, 10, text_y - 1.0f, line_count)
              .has_value(),
         "y above the first row maps to no line");
  // First visible row resolves to the scroll offset; later rows add to it.
  const auto first =
      BottomPanelLineIndexAtY(text_y, line_height, visible_rows, 10, text_y + 1.0f, line_count);
  Expect(first.has_value() && *first == 10u, "first visible row is the scroll offset");
  const auto third = BottomPanelLineIndexAtY(text_y, line_height, visible_rows, 10,
                                             text_y + 2.0f * line_height + 1.0f, line_count);
  Expect(third.has_value() && *third == 12u, "row offset adds to the scroll offset");
  // Below the last visible row rejects.
  Expect(!BottomPanelLineIndexAtY(text_y, line_height, visible_rows, 10,
                                  text_y + 5.0f * line_height + 1.0f, line_count)
              .has_value(),
         "y past the last visible row maps to no line");
  // Absolute index past the content rejects even within the visible band.
  Expect(!BottomPanelLineIndexAtY(text_y, line_height, visible_rows, 38,
                                  text_y + 4.0f * line_height + 1.0f, line_count)
              .has_value(),
         "absolute index past line_count maps to no line");
  // Non-positive line height rejects rather than dividing by zero.
  Expect(!BottomPanelLineIndexAtY(text_y, 0.0f, visible_rows, 10, text_y + 1.0f, line_count)
              .has_value(),
         "non-positive line height maps to no line");
}

void TestWorkspaceSharedCompareCollapsedContextBlockRect() {
  const SDL_FRect editor_surface = MakeRect(100.0f, 50.0f, 600.0f, 400.0f);
  const float rows_y = 80.0f;
  const float line_height = 16.0f;

  const SDL_FRect no_bar = CompareCollapsedContextBlockRect(editor_surface, rows_y, line_height,
                                                            /*show_vertical_scrollbar=*/false, 2);
  Expect(no_bar.x == editor_surface.x + 4.0f, "block is inset 4px from the surface left");
  Expect(no_bar.w == editor_surface.w - 8.0f, "block width drops the 4px inset on each side");
  Expect(no_bar.y == rows_y + 2.0f * line_height - 1.0f, "block row y matches the painted row");
  Expect(no_bar.h == line_height, "block height is one row");

  // With a vertical scrollbar the block must also exclude the scrollbar reserve so the
  // right-aligned action buttons stay clickable where they are painted. The pre-dedup
  // hit-test passed the full editor-surface width, so its action rects drifted right of
  // the painted buttons by exactly the scrollbar reserve plus the block inset.
  const SDL_FRect with_bar = CompareCollapsedContextBlockRect(editor_surface, rows_y, line_height,
                                                              /*show_vertical_scrollbar=*/true, 2);
  const float reserve = microide::workspace::kWorkspaceDiffScrollbarReserve;
  Expect(with_bar.w == editor_surface.w - reserve - 8.0f,
         "scrollbar reserve is excluded from the block width");
  const float painted_right = with_bar.x + with_bar.w;
  const float buggy_full_row_right = editor_surface.x + editor_surface.w;
  Expect(painted_right < buggy_full_row_right,
         "block right edge is left of the full-surface edge the buggy hit-test used");
  Expect(std::abs((buggy_full_row_right - painted_right) - (reserve + 4.0f)) < 0.001f,
         "the old hit-test offset equals the scrollbar reserve plus the block inset");
}

void TestWorkspaceSharedRightPaneLayout() {
  using microide::workspace::ClampRightPaneWidth;
  using microide::workspace::RightPaneResizeHandleRect;

  // Pane hidden by default: editor area spans the full width past the sidebar.
  const auto hidden = ComputeLayout(1280.0f, 720.0f, true, true, 300.0f, 180.0f);
  Expect(hidden.right_pane.w == 0.0f, "right pane has zero width when not requested");

  // Pane visible: it is carved off the right edge of the editor area, which shrinks
  // by the pane width plus a divider.
  const auto shown = ComputeLayout(1280.0f, 720.0f, true, true, 300.0f, 180.0f,
                                   microide::workspace::LayoutModeInputs{}, false, true, 288.0f);
  Expect(shown.right_pane.w == 288.0f, "visible right pane preserves its width");
  Expect(std::fabs((shown.right_pane.x) -
                   (shown.editor_area.x + shown.editor_area.w + 1.0f)) < 0.001f,
         "right pane sits to the right of the editor area past a 1px divider");
  Expect(shown.right_pane.x + shown.right_pane.w <= 1280.0f + 0.001f,
         "right pane stays within the window");
  Expect(shown.editor_area.w < hidden.editor_area.w,
         "showing the right pane shrinks the editor area");

  // Compact mode suppresses the pane to protect the minimum editor width.
  microide::workspace::LayoutModeInputs compact;
  compact.user_override = microide::workspace::LayoutModeInputs::Override::Compact;
  const auto compact_layout =
      ComputeLayout(1280.0f, 720.0f, true, true, 300.0f, 180.0f, compact, false, true, 288.0f);
  Expect(compact_layout.right_pane.w == 0.0f, "compact mode hides the right pane");

  // Clamp keeps the editor above its minimum with both the sidebar and pane open.
  Expect(ClampRightPaneWidth(120.0f, 800.0f, 200.0f) == 120.0f,
         "right-pane width above the viable minimum is accepted");
  const float clamped = ClampRightPaneWidth(700.0f, 800.0f, 200.0f);
  Expect(800.0f - 200.0f - 1.0f - clamped - 1.0f >=
             microide::workspace::kWorkspaceMinEditorAreaWidth - 1.0f,
         "right-pane clamp preserves the minimum editor width alongside the sidebar");

  Expect(RightPaneResizeHandleRect(hidden).w == 0.0f,
         "no resize handle when the pane is hidden");
  Expect(RightPaneResizeHandleRect(shown).w > 0.0f,
         "resize handle exists when the pane is visible");
}

void TestWorkspaceSharedProjectTabStripVisibility() {
  // Strip shown by default: it reserves 32px below the 25px menu bar, so the editor
  // tab strip and content start below both bands.
  const auto shown = ComputeLayout(1280.0f, 720.0f, true, true, 300.0f, 180.0f);
  Expect(shown.project_tab_strip.h == 32.0f, "project tab strip reserves 32px when visible");
  Expect(shown.tab_strip.y == 25.0f + 32.0f,
         "editor tab strip sits below the menu bar and the project tab strip");

  // Hidden: the strip collapses to zero height (keeping its y at the menu-bar bottom),
  // and everything below reclaims the 32px.
  const auto hidden = ComputeLayout(1280.0f, 720.0f, true, true, 300.0f, 180.0f,
                                    microide::workspace::LayoutModeInputs{}, false, false, 0.0f,
                                    /*project_tab_strip_visible=*/false);
  Expect(hidden.project_tab_strip.h == 0.0f, "hidden project tab strip has zero height");
  Expect(hidden.project_tab_strip.y == 25.0f,
         "hidden strip keeps its y at the menu-bar bottom so hit-tests reject naturally");
  Expect(hidden.tab_strip.y == 25.0f,
         "editor tab strip moves up to the menu bar when the project strip is hidden");
  Expect(std::abs((shown.content.y - hidden.content.y) - 32.0f) < 0.001f,
         "hiding the strip lifts the content region up by the strip height");
  Expect(hidden.content.h == shown.content.h + 32.0f,
         "content gains the reclaimed strip height");
  Expect(hidden.tab_strip.h == shown.tab_strip.h,
         "the editor tab strip height is unaffected by hiding the project strip");
}

// The find widget stays inside the area it belongs to. It was anchored 12 px
// below the area's top with a 280 px minimum width and no check of either
// dimension, so a bottom panel squeezed to its tab strip put the terminal's
// find bar past the window's bottom edge (soak 23535aaf3151, cce216e505a7), and
// an area narrower than 280 px pushed it off the area's left.
void TestFindWidgetStaysInsideAShortOrNarrowArea() {
  using microide::workspace::ComputeFindWidgetLayout;
  const auto inside = [](const SDL_FRect& r, const SDL_FRect& area) {
    return r.x >= area.x && r.x + r.w <= area.x + area.w + 0.5f &&
           r.y + r.h <= area.y + area.h + 0.5f;
  };
  // The terminal's find bar floats over the panel's body, never over its tab row:
  // lifted onto the row it covered the panel tabs and the new-terminal button
  // (soak 0ead4679e247, e04d9f4e8b95). A body that cannot hold it -- the panel
  // squeezed to little more than its tab row -- lays out no bar at all rather
  // than one past the window's bottom (soak 23535aaf3151).
  microide::workspace::WorkspaceLayout squeezed_panel;
  squeezed_panel.bottom_panel = SDL_FRect{0.0f, 748.0f, 1067.0f, 52.0f};
  const SDL_FRect short_body = microide::workspace::BottomPanelContentRect(squeezed_panel);
  const auto hidden_fw = microide::workspace::ComputeTerminalFindWidgetLayout(short_body, squeezed_panel);
  bool any_area = false;
  for (const SDL_FRect& r : {hidden_fw.widget, hidden_fw.search_field, hidden_fw.prev_button,
                             hidden_fw.next_button, hidden_fw.close_button, hidden_fw.count_rect,
                             hidden_fw.toggle_buttons[0], hidden_fw.toggle_buttons[1]}) {
    any_area = any_area || (r.w > 0.0f && r.h > 0.0f);
  }
  Expect(!any_area, "a panel body too short for the terminal's find bar lays out none of it");
  // A body a little taller than the bar: lifted inside the body, below the tab row.
  microide::workspace::WorkspaceLayout low_panel;
  low_panel.bottom_panel = SDL_FRect{0.0f, 700.0f, 1067.0f, 100.0f};
  const SDL_FRect low_body = microide::workspace::BottomPanelContentRect(low_panel);
  const auto terminal_fw = microide::workspace::ComputeTerminalFindWidgetLayout(low_body, low_panel);
  Expect(terminal_fw.widget.h > 0.0f && inside(terminal_fw.widget, low_body) &&
             terminal_fw.widget.y >= low_body.y && terminal_fw.next_button.y >= low_body.y,
         "the terminal's find bar stays inside the panel's body, below its tab row");
  // An editor column collapsed under a maximized panel: a surface of no height at
  // the window's top. The widget may not be lifted above it, out of the window.
  const SDL_FRect collapsed{240.0f, 24.0f, 700.0f, 0.0f};
  for (const bool replace : {false, true}) {
    const auto fw = ComputeFindWidgetLayout(collapsed, replace, 2);
    Expect(fw.widget.y >= collapsed.y && fw.search_field.y >= collapsed.y,
           "a find widget is never lifted above the region it floats over");
  }
  // A roomy area keeps the widget where it always was: 12 px below the top.
  const SDL_FRect roomy{100.0f, 50.0f, 900.0f, 600.0f};
  const auto fw = ComputeFindWidgetLayout(roomy, false, 2);
  Expect(fw.widget.y == 62.0f, "a roomy area keeps the widget's top margin");
  // An area narrower than the widget's minimum width.
  const SDL_FRect narrow{40.0f, 50.0f, 200.0f, 400.0f};
  const auto narrow_fw = ComputeFindWidgetLayout(narrow, false, 2);
  Expect(inside(narrow_fw.widget, narrow) && narrow_fw.close_button.x >= narrow.x,
         "a find widget must not run off the left of a narrow area");
}

}  // namespace

// The editor banner shows whole or not at all: squeezed to an editor area with
// no height (the panel maximized), its buttons were centred on a strip of none
// and landed over the menu bar's window controls (QA monkey, qa-asan seed 4).
void TestEditorBannerKeepsInsideTheEditorArea() {
  static constexpr workspace::EditorBannerButton kButtons[] = {{workspace::EditorBannerAction::Keep, "Keep", 54.0f, false}};
  const SDL_FRect roomy{100.0f, 60.0f, 800.0f, 400.0f};
  const SDL_FRect strip = workspace::ComputeEditorBannerStripRect(roomy);
  const workspace::EditorBannerButtonLayout fits = workspace::ComputeEditorBannerButtonRects(strip, kButtons);
  Expect(fits.buttons.size() == 1 && fits.rects[0].y >= strip.y && fits.rects[0].y + fits.rects[0].h <= strip.y + strip.h,
         "a banner with room keeps its buttons inside its strip");
  for (const float height : {0.0f, 10.0f}) {
    const SDL_FRect squeezed{100.0f, 60.0f, 800.0f, height};
    const SDL_FRect none = workspace::ComputeEditorBannerStripRect(squeezed);
    const workspace::EditorBannerButtonLayout gone = workspace::ComputeEditorBannerButtonRects(none, kButtons);
    Expect(none.h == 0.0f && gone.buttons.empty() && gone.dismiss.w == 0.0f,
           "an editor area " + std::to_string(height) + " high shows no banner and no buttons");
  }
}

// The banner's buttons, laid right to left from its dismiss button, had no left
// bound: in a pane narrower than the row, the leading ones landed over the
// sidebar (soak 6b622ef8a9ea, a1673d0162dc, 4eda8acffd2f: banner/Overwrite on
// git/sync). The trailing buttons that fit are kept; the rest are not laid out.
void TestEditorBannerButtonsStayInsideANarrowStrip() {
  static constexpr workspace::EditorBannerButton kButtons[] = {
      {workspace::EditorBannerAction::Keep, "Reload", 64.0f, false},
      {workspace::EditorBannerAction::Keep, "Compare", 72.0f, false},
      {workspace::EditorBannerAction::Keep, "Overwrite", 78.0f, false}};
  const SDL_FRect strip{280.0f, 88.0f, 150.0f, 26.0f};
  const workspace::EditorBannerButtonLayout layout = workspace::ComputeEditorBannerButtonRects(strip, kButtons);
  Expect(!layout.buttons.empty() && layout.buttons.size() < 3, "a narrow strip keeps only the buttons that fit");
  for (std::size_t i = 0; i < layout.buttons.size(); ++i) {
    Expect(layout.rects[i].x >= strip.x, "a banner button must not start left of its strip");
  }
  Expect(layout.buttons.back().label == "Overwrite", "the trailing buttons are the ones kept");
  const SDL_FRect wide{280.0f, 88.0f, 900.0f, 26.0f};
  Expect(workspace::ComputeEditorBannerButtonRects(wide, kButtons).buttons.size() == 3,
         "a wide strip keeps every button");
}

// Soak 538a455016ef (1024x700 at a scale of 3.25: 316x216 units): the dirty
// prompt narrowed to the window and its three fixed-width buttons did not --
// Save hung past the dialog's left edge, outside the dialog's repaint, so Tab
// moving the choice off it left its focus ring on screen. The prompt surface's
// footer had the same arithmetic. Every footer button stays inside its dialog's
// padding at any window width.
void TestDialogFooterButtonsStayInsideANarrowDialog() {
  const auto inside = [](const SDL_FRect& button, const SDL_FRect& dialog) {
    return button.w >= 0.0f && button.x >= dialog.x + 16.0f - 0.01f &&
           button.x + button.w <= dialog.x + dialog.w - 16.0f + 0.01f;
  };
  for (float width = 120.0f; width <= 800.0f; width += 7.0f) {
    const SDL_FRect full = MakeRect(0.0f, 0.0f, width, 216.0f);
    const SDL_FRect dirty = ComputeDirtyPromptRect(full);
    for (const SDL_FRect& button : ComputeDirtyPromptButtonRects(dirty)) {
      Expect(inside(button, dirty), "a dirty prompt button should stay inside its dialog at width " +
                                        std::to_string(static_cast<int>(width)));
    }
    const SDL_FRect surface = ComputePromptSurfaceRect(full);
    for (int count = 1; count <= 3; ++count) {
      for (const SDL_FRect& button : ComputePromptSurfaceButtonRects(surface, count)) {
        Expect(inside(button, surface), "a prompt button should stay inside its surface at width " +
                                            std::to_string(static_cast<int>(width)) + " with " +
                                            std::to_string(count) + " buttons");
      }
    }
  }
}

void RegisterWorkspaceShellSharedLayoutTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShared/DialogFooterButtonsStayInsideANarrowDialog",
          TestDialogFooterButtonsStayInsideANarrowDialog);
  AddTest(tests, "WorkspaceShared/EditorBannerButtonsStayInsideANarrowStrip",
          TestEditorBannerButtonsStayInsideANarrowStrip);
  AddTest(tests, "WorkspaceShared/MergeConflictButtonsStayInsideTheirPane",
          TestWorkspaceSharedMergeConflictButtonsStayInsideTheirPane);
  AddTest(tests, "WorkspaceShared/FindWidgetStaysInsideAShortOrNarrowArea",
          TestFindWidgetStaysInsideAShortOrNarrowArea);
  AddTest(tests, "WorkspaceShared/EditorBannerKeepsInsideTheEditorArea", TestEditorBannerKeepsInsideTheEditorArea);
  AddTest(tests, "WorkspaceShared/EditorSplitTreeRevision",
          TestWorkspaceSharedEditorSplitTreeRevision);
  AddTest(tests, "WorkspaceShared/LayoutInputsStructMatchesPositionalForm",
          TestWorkspaceLayoutInputsStructMatchesPositionalForm);
  AddTest(tests, "WorkspaceShared/LayoutHelpers", TestWorkspaceSharedLayoutHelpers);
  AddTest(tests, "WorkspaceShared/SidebarFillsTheTabStripBand", TestWorkspaceSharedSidebarFillsTheTabStripBand);
  AddTest(tests, "WorkspaceShared/DebugPaneFillsTheTabStripBand", TestWorkspaceSharedDebugPaneFillsTheTabStripBand);
  AddTest(tests, "WorkspaceShared/ScrollbarHelpers", TestWorkspaceSharedScrollbarHelpers);
  AddTest(tests, "WorkspaceShared/ScrollbarReserveGeometry",
          TestWorkspaceSharedScrollbarReserveGeometry);
  AddTest(tests, "WorkspaceShared/CompareScrollbarMarkers",
          TestWorkspaceSharedCompareScrollbarMarkers);
  AddTest(tests, "WorkspaceShared/CompareScrollbarMarkersFollowPresentationRows",
          TestWorkspaceSharedCompareScrollbarMarkersFollowPresentationRows);
  AddTest(tests, "WorkspaceShared/MergeScrollbarMarkers",
          TestWorkspaceSharedMergeScrollbarMarkers);
  AddTest(tests, "WorkspaceShared/OverviewLaneGeometry",
          TestWorkspaceSharedOverviewLaneGeometry);
  AddTest(tests, "WorkspaceShared/OverviewReducerBoundsAndPriority",
          TestWorkspaceSharedOverviewReducerBoundsAndPriority);
  AddTest(tests, "WorkspaceShared/OverviewReducerEqualPriorityTieIsInputOrder",
          TestWorkspaceSharedOverviewReducerEqualPriorityTieIsInputOrder);
  AddTest(tests, "WorkspaceShared/OverviewLaneLeftClamp",
          TestWorkspaceSharedOverviewLaneLeftClamp);
  AddTest(tests, "WorkspaceShared/PanelGeometryHelpers", TestWorkspaceSharedPanelGeometryHelpers);
  AddTest(tests, "WorkspaceShared/PromptGeometry", TestWorkspaceSharedPromptGeometry);
  AddTest(tests, "WorkspaceShared/ScrollbarEdgeCases", TestWorkspaceSharedScrollbarEdgeCases);
  AddTest(tests, "WorkspaceShared/ScrollSurfaceLayout", TestWorkspaceSharedScrollSurfaceLayout);
  AddTest(tests, "WorkspaceShared/TextGridInteractionLayout",
          TestWorkspaceSharedTextGridInteractionLayout);
  AddTest(tests, "WorkspaceShared/ScrollableListLayout",
          TestWorkspaceSharedScrollableListLayout);
  AddTest(tests, "WorkspaceShared/StripLayoutHelpers", TestWorkspaceSharedStripLayoutHelpers);
  AddTest(tests, "WorkspaceShared/ChromeTabRenderItems",
          TestWorkspaceSharedChromeTabRenderItems);
  AddTest(tests, "WorkspaceShared/EditorSplitLayout", TestWorkspaceSharedEditorSplitLayout);
  AddTest(tests, "WorkspaceShared/EditorGroupRects", TestWorkspaceEditorGroupRects);
  AddTest(tests, "WorkspaceShared/MergeHoverClassifier",
          TestWorkspaceSharedMergeHoverClassifier);
  AddTest(tests, "WorkspaceShared/MergeInteractionGeometry",
          TestWorkspaceSharedMergeInteractionGeometry);
  AddTest(tests, "WorkspaceShared/MergeZeroLengthSourceHitTest",
          TestWorkspaceSharedMergeZeroLengthSourceHitTest);
  AddTest(tests, "WorkspaceShared/MergeSourceButtonUsesSourceScroll",
          TestWorkspaceSharedMergeSourceButtonUsesSourceScroll);
  AddTest(tests, "WorkspaceShared/OverlayRectHelpers", TestWorkspaceSharedOverlayRectHelpers);
  AddTest(tests, "WorkspaceShared/HitTargets", TestWorkspaceSharedHitTargets);
  AddTest(tests, "WorkspaceShared/HitTargetClickRouting", TestWorkspaceHitTargetClickRouting);
  AddTest(tests, "WorkspaceShared/LayoutModeResolution", TestWorkspaceLayoutModeResolution);
  AddTest(tests, "WorkspaceShared/LayoutModeServiceFlipFlop", TestLayoutModeServiceFlipFlop);
  AddTest(tests, "WorkspaceShared/EmptyTabStripPlaceholderRect",
          TestWorkspaceSharedEmptyTabStripPlaceholderRect);
  AddTest(tests, "WorkspaceShared/BottomPanelLineIndexAtY",
          TestWorkspaceSharedBottomPanelLineIndexAtY);
  AddTest(tests, "WorkspaceShared/CompareCollapsedContextBlockRect",
          TestWorkspaceSharedCompareCollapsedContextBlockRect);
  AddTest(tests, "WorkspaceShared/RightPaneLayout", TestWorkspaceSharedRightPaneLayout);
  AddTest(tests, "WorkspaceShared/ProjectTabStripVisibility",
          TestWorkspaceSharedProjectTabStripVisibility);
}

}  // namespace microide::tests
