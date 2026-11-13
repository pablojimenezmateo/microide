#include "TestSupport.h"

#include "workspace/PaneDrop.h"
#include "workspace/SplitTreeLayout.h"
#include "workspace/WorkspaceLayout.h"

#include <cmath>
#include <cstdint>
#include <span>
#include <string>

namespace microide::tests {
namespace {

using microide::workspace::AdjacentSplitRegion;
using microide::workspace::ComputeLayout;
using microide::workspace::ComputeSplitRegions;
using microide::workspace::kMaxSplitLeaves;
using microide::workspace::kNoSplitRegion;
using microide::workspace::kSplitDividerThickness;
using microide::workspace::PaneDirection;
using microide::workspace::PaneDropAt;
using microide::workspace::PaneDropZone;
using microide::workspace::SplitForPaneDropZone;
using microide::workspace::SplitOrientation;
using microide::workspace::SplitRegionIndexAt;
using microide::workspace::SplitRegionsLayout;
using microide::workspace::SplitTree;
using microide::workspace::WorkspaceLayout;
using microide::workspace::WorkspaceLayoutInputs;

bool Overlap(const SDL_FRect& a, const SDL_FRect& b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// The regions and dividers tile the area exactly: nothing overlaps, nothing sits
// outside, and together they cover every pixel of it.
void ExpectTiles(const SplitTree& tree, const SDL_FRect& area, const std::string& context) {
  const SplitRegionsLayout layout = ComputeSplitRegions(tree, area);
  Expect(layout.panes.size() == tree.leaf_count(), context + ": one region per leaf");
  Expect(layout.dividers.size() + 1 == layout.panes.size(), context + ": one divider per adjacent pair");
  float covered = 0.0f;
  util::InlineVector<SDL_FRect, kMaxSplitLeaves * 2> all;
  for (const SDL_FRect& pane : layout.panes) {
    all.push_back(pane);
  }
  for (const auto& divider : layout.dividers) {
    all.push_back(divider.rect);
    Expect(divider.pair_extent >= 0.0f, context + ": a pair spans a non-negative extent");
    Expect((divider.vertical ? divider.rect.w : divider.rect.h) == kSplitDividerThickness,
           context + ": a divider is one band thick across its axis");
  }
  for (std::size_t i = 0; i < all.size(); ++i) {
    const SDL_FRect& r = all[i];
    Expect(r.w >= 0.0f && r.h >= 0.0f, context + ": no negative extent");
    Expect(r.x >= area.x - 1e-3f && r.y >= area.y - 1e-3f && r.x + r.w <= area.x + area.w + 1e-3f &&
               r.y + r.h <= area.y + area.h + 1e-3f,
           context + ": inside the area");
    for (std::size_t j = i + 1; j < all.size(); ++j) {
      Expect(!Overlap(r, all[j]), context + ": nothing overlaps");
    }
    covered += r.w * r.h;
  }
  Expect(std::abs(covered - area.w * area.h) < 1.0f, context + ": the area is covered exactly");
}

void TestSplitRegionsTileTheAreaForRandomTrees() {
  const SDL_FRect area{40.0f, 300.0f, 1013.0f, 611.0f};
  SplitTree one;
  const SplitRegionsLayout single = ComputeSplitRegions(one, area);
  Expect(single.panes.size() == 1 && single.dividers.empty() && single.panes[0].x == area.x &&
             single.panes[0].w == area.w && single.panes[0].h == area.h,
         "one leaf is the area itself");

  std::uint64_t state = 0x2545F4914F6CDD1Dull;
  const auto next = [&state](std::uint64_t bound) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return bound == 0 ? 0 : static_cast<std::size_t>(state % bound);
  };
  for (int step = 0; step < 400; ++step) {
    SplitTree tree;
    const std::size_t leaves = 1 + next(kMaxSplitLeaves);
    while (tree.leaf_count() < leaves) {
      (void)tree.InsertLeaf(next(tree.leaf_count()),
                            next(2) == 0 ? SplitOrientation::Vertical : SplitOrientation::Horizontal,
                            next(2) == 0);
    }
    for (std::size_t moves = next(3); moves > 0 && tree.is_split(); --moves) {
      const auto& root = tree.node(tree.root());
      (void)tree.ResizeDivider(tree.root(), next(root.children.size() - 1),
                               0.1f + static_cast<float>(next(800)) / 1000.0f);
    }
    const SDL_FRect random_area{static_cast<float>(next(50)), static_cast<float>(next(50)),
                                static_cast<float>(400 + next(1600)), static_cast<float>(300 + next(900))};
    ExpectTiles(tree, random_area, "random step " + std::to_string(step));
  }
}

// A row split right then the right pane split down: the grid a terminal tab gets
// from Split Terminal + Split Terminal Down.
SplitTree LeftAndRightStack() {
  SplitTree tree;
  (void)tree.InsertLeaf(0, SplitOrientation::Vertical, false);
  (void)tree.InsertLeaf(1, SplitOrientation::Horizontal, false);
  return tree;
}

void TestSplitRegionsHitTestAndDividerAddressing() {
  const SDL_FRect area{0.0f, 0.0f, 1000.0f, 600.0f};
  const SplitTree tree = LeftAndRightStack();
  const SplitRegionsLayout layout = ComputeSplitRegions(tree, area);
  Expect(layout.panes.size() == 3 && layout.dividers.size() == 2, "three panes, two dividers");
  Expect(SplitRegionIndexAt(layout, 10.0f, 10.0f) == 0, "top left is the left pane");
  Expect(SplitRegionIndexAt(layout, 990.0f, 10.0f) == 1, "top right is the upper right pane");
  Expect(SplitRegionIndexAt(layout, 990.0f, 590.0f) == 2, "bottom right is the lower right pane");
  Expect(SplitRegionIndexAt(layout, layout.dividers[0].rect.x + 1.0f, 10.0f) == kNoSplitRegion,
         "a divider belongs to no pane");
  Expect(SplitRegionIndexAt(layout, -1.0f, 10.0f) == kNoSplitRegion, "outside belongs to no pane");

  bool saw_vertical = false;
  bool saw_horizontal = false;
  for (const auto& divider : layout.dividers) {
    saw_vertical = saw_vertical || divider.vertical;
    saw_horizontal = saw_horizontal || !divider.vertical;
    // The share at the divider's own centre is where it already sits.
    const float cx = divider.rect.x + divider.rect.w * 0.5f;
    const float cy = divider.rect.y + divider.rect.h * 0.5f;
    Expect(divider.ShareAt(cx, cy) > 0.45f && divider.ShareAt(cx, cy) < 0.55f,
           "a divider at an even split reads as half its pair");
  }
  Expect(saw_vertical && saw_horizontal, "the grid reports a divider along each axis");
}

void TestAdjacentSplitRegionFollowsTheScreen() {
  const SDL_FRect area{0.0f, 0.0f, 1000.0f, 600.0f};
  const SplitRegionsLayout layout = ComputeSplitRegions(LeftAndRightStack(), area);
  const std::span<const SDL_FRect> panes(layout.panes.data(), layout.panes.size());
  Expect(AdjacentSplitRegion(panes, 2, PaneDirection::Up) == 1, "up from the lower right is the upper right");
  Expect(AdjacentSplitRegion(panes, 2, PaneDirection::Left) == 0, "left from the lower right is the left pane");
  Expect(AdjacentSplitRegion(panes, 0, PaneDirection::Right) != kNoSplitRegion,
         "right from the left pane is one of the stacked panes");
  Expect(AdjacentSplitRegion(panes, 1, PaneDirection::Down) == 2, "down from the upper right is the lower right");
  Expect(AdjacentSplitRegion(panes, 0, PaneDirection::Left) == kNoSplitRegion, "nothing left of the left pane");
  Expect(AdjacentSplitRegion(panes, 1, PaneDirection::Up) == kNoSplitRegion, "nothing above the upper right");
}

void TestPaneDropZonesAndTheirSplits() {
  const SDL_FRect pane{100.0f, 100.0f, 500.0f, 300.0f};
  Expect(PaneDropAt(pane, 105.0f, 250.0f, true).zone == PaneDropZone::Left, "left fifth");
  Expect(PaneDropAt(pane, 595.0f, 250.0f, true).zone == PaneDropZone::Right, "right fifth");
  Expect(PaneDropAt(pane, 350.0f, 105.0f, true).zone == PaneDropZone::Top, "top fifth");
  Expect(PaneDropAt(pane, 350.0f, 395.0f, true).zone == PaneDropZone::Bottom, "bottom fifth");
  Expect(PaneDropAt(pane, 350.0f, 250.0f, true).zone == PaneDropZone::Center, "the middle is the pane");
  Expect(PaneDropAt(pane, 350.0f, 250.0f, true, /*can_center=*/false).zone == PaneDropZone::None,
         "without a centre the middle offers nothing");
  Expect(PaneDropAt(pane, 105.0f, 250.0f, false, false).zone == PaneDropZone::None,
         "no split and no centre offers nothing anywhere");

  const auto left = SplitForPaneDropZone(PaneDropZone::Left);
  const auto bottom = SplitForPaneDropZone(PaneDropZone::Bottom);
  Expect(left.edge && left.orientation == SplitOrientation::Vertical && left.before,
         "left is a side-by-side split on the leading side");
  Expect(bottom.edge && bottom.orientation == SplitOrientation::Horizontal && !bottom.before,
         "bottom is a stacked split on the trailing side");
  Expect(!SplitForPaneDropZone(PaneDropZone::Center).edge && !SplitForPaneDropZone(PaneDropZone::None).edge,
         "the centre and none are not splits");
}

// Immersive mode: the panel owns everything below the menu bar and project strip.
void TestMaximizedPanelLayoutLeavesOnlyMenuAndProjectStrip() {
  WorkspaceLayoutInputs inputs;
  inputs.window_width = 1440.0f;
  inputs.window_height = 900.0f;
  inputs.sidebar_visible = true;
  inputs.sidebar_width = 288.0f;
  inputs.bottom_panel_visible = true;
  inputs.bottom_panel_height = 156.0f;
  inputs.reserve_status_bar = true;
  inputs.right_pane_visible = true;
  inputs.right_pane_width = 320.0f;
  inputs.project_tab_strip_visible = true;

  const WorkspaceLayout regular = ComputeLayout(inputs);
  inputs.bottom_panel_maximized = true;
  const WorkspaceLayout maximized = ComputeLayout(inputs);

  Expect(maximized.panel_maximized && !regular.panel_maximized, "the layout records the mode");
  Expect(maximized.menu_bar.h == regular.menu_bar.h && maximized.project_tab_strip.h ==
                                                            regular.project_tab_strip.h,
         "the menu bar and the project strip keep their bands");
  const float chrome_bottom = maximized.project_tab_strip.y + maximized.project_tab_strip.h;
  Expect(std::abs(maximized.bottom_panel.y - chrome_bottom) < 1e-3f,
         "the panel starts right under the project strip");
  Expect(std::abs(maximized.bottom_panel.y + maximized.bottom_panel.h - inputs.window_height) <
             1e-3f,
         "the panel reaches the bottom of the window");
  Expect(maximized.bottom_panel.w == inputs.window_width, "the panel spans the window width");
  Expect(maximized.tab_strip.h == 0.0f && maximized.editor_area.h == 0.0f &&
             maximized.editor_surface.h == 0.0f && maximized.breadcrumb.h == 0.0f,
         "the editor column collapses to zero height");
  Expect(maximized.sidebar.h == 0.0f && maximized.right_pane.h == 0.0f,
         "the side panes collapse to zero height");
  Expect(maximized.status_bar.h == 0.0f && regular.status_bar.h > 0.0f,
         "the status bar is dropped too — only the menu and project strip remain");
  Expect(maximized.overlay_anchor.y == maximized.bottom_panel.y &&
             maximized.overlay_anchor.h == maximized.bottom_panel.h,
         "centred overlays anchor to the panel while it is maximized");
  Expect(regular.overlay_anchor.x == regular.editor_area.x &&
             regular.overlay_anchor.h == regular.editor_area.h,
         "centred overlays anchor to the editor area otherwise");
  Expect(microide::workspace::BottomPanelResizeHandleRect(maximized).h == 0.0f,
         "a maximized panel has no resize handle");
  Expect(microide::workspace::BottomPanelResizeHandleRect(regular).h > 0.0f,
         "the regular panel keeps its resize handle");

  // Hidden panel: maximized is irrelevant, the layout is the regular hidden one.
  inputs.bottom_panel_visible = false;
  const WorkspaceLayout hidden = ComputeLayout(inputs);
  Expect(!hidden.panel_maximized && hidden.editor_area.h > regular.editor_area.h,
         "a hidden panel is never laid out maximized");

  // With the project strip hidden the panel starts right under the menu bar.
  inputs.bottom_panel_visible = true;
  inputs.project_tab_strip_visible = false;
  const WorkspaceLayout no_strip = ComputeLayout(inputs);
  Expect(std::abs(no_strip.bottom_panel.y - no_strip.menu_bar.h) < 1e-3f,
         "without a project strip the panel starts under the menu bar");
}

}  // namespace

void RegisterSplitTreeLayoutTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SplitTreeLayout/RegionsTileTheAreaForRandomTrees", TestSplitRegionsTileTheAreaForRandomTrees);
  AddTest(tests, "SplitTreeLayout/HitTestAndDividerAddressing", TestSplitRegionsHitTestAndDividerAddressing);
  AddTest(tests, "SplitTreeLayout/AdjacentRegionFollowsTheScreen", TestAdjacentSplitRegionFollowsTheScreen);
  AddTest(tests, "SplitTreeLayout/PaneDropZonesAndTheirSplits", TestPaneDropZonesAndTheirSplits);
  AddTest(tests, "WorkspaceLayout/MaximizedPanelLeavesOnlyMenuAndProjectStrip",
          TestMaximizedPanelLayoutLeavesOnlyMenuAndProjectStrip);
}

}  // namespace microide::tests
