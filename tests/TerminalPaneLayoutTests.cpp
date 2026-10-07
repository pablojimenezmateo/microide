#include "TestSupport.h"

#include "workspace/TerminalPaneLayout.h"
#include "workspace/WorkspaceLayout.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace microide::tests {
namespace {

using microide::workspace::ComputeLayout;
using microide::workspace::ComputeTerminalPaneRects;
using microide::workspace::InsertTerminalPaneWeight;
using microide::workspace::kMaxTerminalPanes;
using microide::workspace::kTerminalPaneDividerThickness;
using microide::workspace::kTerminalPaneMinShare;
using microide::workspace::RemoveTerminalPaneWeight;
using microide::workspace::ResetTerminalPaneDivider;
using microide::workspace::ResizeTerminalPaneDivider;
using microide::workspace::TerminalPaneIndexAt;
using microide::workspace::TerminalPaneRectsLayout;
using microide::workspace::TerminalPaneWeights;
using microide::workspace::WorkspaceLayout;
using microide::workspace::WorkspaceLayoutInputs;

float Sum(const TerminalPaneWeights& weights) {
  float total = 0.0f;
  for (float w : weights) {
    total += w;
  }
  return total;
}

void ExpectNormalised(const TerminalPaneWeights& weights, const std::string& context) {
  Expect(!weights.empty(), context + ": weights never empty while a pane exists");
  Expect(std::abs(Sum(weights) - 1.0f) < 1e-4f, context + ": weights sum to 1");
  for (float w : weights) {
    Expect(w > 0.0f && w <= 1.0f, context + ": every pane keeps a positive share");
  }
}

// The body is partitioned exactly: panes and dividers tile it with no gap and no
// overlap, whatever the weights, and the last pane absorbs the rounding.
void ExpectTiles(const SDL_FRect& body, const TerminalPaneWeights& weights,
                 const std::string& context) {
  const TerminalPaneRectsLayout rects =
      ComputeTerminalPaneRects(body, std::span<const float>(weights.data(), weights.size()));
  Expect(rects.panes.size() == weights.size(), context + ": one rect per weight");
  Expect(rects.dividers.size() + 1 == rects.panes.size(), context + ": one divider per pair");
  float x = body.x;
  for (std::size_t i = 0; i < rects.panes.size(); ++i) {
    const SDL_FRect& pane = rects.panes[i];
    Expect(std::abs(pane.x - x) < 1e-3f, context + ": pane starts where the previous ended");
    Expect(pane.y == body.y && pane.h == body.h, context + ": panes share the body's height");
    Expect(pane.w >= 0.0f, context + ": pane width never negative");
    x = pane.x + pane.w;
    if (i + 1 < rects.panes.size()) {
      const auto& divider = rects.dividers[i];
      Expect(divider.boundary == i, context + ": divider addresses its left pane");
      Expect(std::abs(divider.rect.x - x) < 1e-3f, context + ": divider follows the pane");
      Expect(divider.rect.w == kTerminalPaneDividerThickness, context + ": divider width");
      Expect(std::abs(divider.pair_start - pane.x) < 1e-3f, context + ": pair starts at left pane");
      x = divider.rect.x + divider.rect.w;
    }
  }
  Expect(std::abs(x - (body.x + body.w)) < 1e-3f, context + ": the tiling ends at the body edge");
  for (const auto& divider : rects.dividers) {
    const SDL_FRect& right = rects.panes[divider.boundary + 1];
    Expect(std::abs(divider.pair_extent - (right.x + right.w - divider.pair_start)) < 1e-3f,
           context + ": pair extent spans both panes");
  }
}

void TestTerminalPaneWeightsSplitHalvesTheSplitPane() {
  TerminalPaneWeights weights;
  InsertTerminalPaneWeight(weights, 0);
  ExpectNormalised(weights, "first pane");
  Expect(weights.size() == 1 && weights[0] == 1.0f, "a lone pane owns the body");

  // Split the only pane: both halves equal.
  InsertTerminalPaneWeight(weights, 1);
  ExpectNormalised(weights, "two panes");
  Expect(std::abs(weights[0] - 0.5f) < 1e-5f && std::abs(weights[1] - 0.5f) < 1e-5f,
         "splitting halves the split pane");

  // Split the right pane: the LEFT one keeps its half, the right half splits.
  InsertTerminalPaneWeight(weights, 2);
  ExpectNormalised(weights, "three panes");
  Expect(std::abs(weights[0] - 0.5f) < 1e-5f, "a pane not being split keeps its share");
  Expect(std::abs(weights[1] - 0.25f) < 1e-5f && std::abs(weights[2] - 0.25f) < 1e-5f,
         "the split pane's share is halved between it and the new pane");

  // Inserting at the front splits the first pane.
  InsertTerminalPaneWeight(weights, 0);
  ExpectNormalised(weights, "front insert");
  Expect(std::abs(weights[0] - 0.25f) < 1e-5f && std::abs(weights[1] - 0.25f) < 1e-5f,
         "a front insert halves the first pane");

  // The cap holds.
  while (weights.size() < kMaxTerminalPanes) {
    InsertTerminalPaneWeight(weights, weights.size());
  }
  const TerminalPaneWeights full = weights;
  InsertTerminalPaneWeight(weights, 2);
  Expect(weights.size() == kMaxTerminalPanes, "a full row takes no more panes");
  for (std::size_t i = 0; i < full.size(); ++i) {
    Expect(weights[i] == full[i], "a refused insert leaves the weights untouched");
  }
}

void TestTerminalPaneWeightsRemoveGivesShareToTheLeftNeighbour() {
  TerminalPaneWeights weights{0.5f, 0.25f, 0.25f};
  RemoveTerminalPaneWeight(weights, 2);
  ExpectNormalised(weights, "remove last");
  Expect(std::abs(weights[1] - 0.5f) < 1e-5f, "the left neighbour inherits the removed share");

  RemoveTerminalPaneWeight(weights, 0);
  ExpectNormalised(weights, "remove first");
  Expect(weights.size() == 1 && std::abs(weights[0] - 1.0f) < 1e-5f,
         "removing the first pane hands its share to the new first pane");

  RemoveTerminalPaneWeight(weights, 5);
  Expect(weights.size() == 1, "an out-of-range remove is a no-op");
}

void TestTerminalPaneDividerResizeMovesOnlyThePair() {
  TerminalPaneWeights weights{0.25f, 0.25f, 0.5f};
  Expect(ResizeTerminalPaneDivider(weights, 0, 0.8f), "a live divider resizes");
  ExpectNormalised(weights, "resized");
  Expect(std::abs(weights[0] - 0.4f) < 1e-5f && std::abs(weights[1] - 0.1f) < 1e-5f,
         "the pair redistributes its combined half");
  Expect(std::abs(weights[2] - 0.5f) < 1e-5f, "a pane outside the pair does not move");

  Expect(ResizeTerminalPaneDivider(weights, 0, 0.0f), "a clamped resize still answers true");
  Expect(std::abs(weights[0] - 0.5f * kTerminalPaneMinShare) < 1e-5f,
         "a pane cannot be dragged below its minimum share");
  Expect(!ResizeTerminalPaneDivider(weights, 2, 0.5f), "there is no divider after the last pane");
  Expect(!ResizeTerminalPaneDivider(weights, 0, std::nanf("")), "NaN is refused");

  Expect(ResetTerminalPaneDivider(weights, 0), "reset answers like a resize");
  Expect(std::abs(weights[0] - weights[1]) < 1e-5f, "reset evens the pair");
}

void TestTerminalPaneRectsTileTheBodyForEveryWeightMix() {
  const SDL_FRect body{40.0f, 300.0f, 1013.0f, 211.0f};
  TerminalPaneWeights one{1.0f};
  ExpectTiles(body, one, "single pane");
  const TerminalPaneRectsLayout single =
      ComputeTerminalPaneRects(body, std::span<const float>(one.data(), one.size()));
  Expect(single.panes[0].x == body.x && single.panes[0].w == body.w,
         "one pane is the body itself");

  TerminalPaneWeights uneven{0.1f, 0.6f, 0.3f};
  ExpectTiles(body, uneven, "uneven thirds");
  TerminalPaneWeights degenerate{0.0f, 0.0f};
  ExpectTiles(body, degenerate, "all-zero weights fall back to even");
  const TerminalPaneRectsLayout even =
      ComputeTerminalPaneRects(body, std::span<const float>(degenerate.data(), degenerate.size()));
  Expect(std::abs(even.panes[0].w - even.panes[1].w) <= 1.0f, "zero weights split evenly");

  // Random weight mixes at the cap, odd body widths: still an exact tiling.
  std::uint64_t state = 0x9E3779B97F4A7C15ull;
  const auto next = [&state]() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };
  for (int step = 0; step < 500; ++step) {
    TerminalPaneWeights weights;
    const std::size_t count = 1 + static_cast<std::size_t>(next() % kMaxTerminalPanes);
    for (std::size_t i = 0; i < count; ++i) {
      weights.push_back(0.05f + static_cast<float>(next() % 1000) / 1000.0f);
    }
    const SDL_FRect random_body{static_cast<float>(next() % 50), 0.0f,
                                static_cast<float>(100 + next() % 1900), 150.0f};
    ExpectTiles(random_body, weights, "random step " + std::to_string(step));
  }

  // Hit testing: a point in a pane finds it; a point on a divider finds nothing.
  const TerminalPaneRectsLayout rects =
      ComputeTerminalPaneRects(body, std::span<const float>(uneven.data(), uneven.size()));
  Expect(TerminalPaneIndexAt(rects, rects.panes[1].x + 1.0f, body.y + 1.0f) == 1,
         "a point inside the second pane resolves to it");
  Expect(TerminalPaneIndexAt(rects, rects.dividers[0].rect.x + 1.0f, body.y + 1.0f) ==
             rects.panes.size(),
         "a point on a divider belongs to no pane");
  Expect(TerminalPaneIndexAt(rects, body.x - 1.0f, body.y + 1.0f) == rects.panes.size(),
         "a point outside the body belongs to no pane");
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

void RegisterTerminalPaneLayoutTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TerminalPaneLayout/SplitHalvesTheSplitPane",
          TestTerminalPaneWeightsSplitHalvesTheSplitPane);
  AddTest(tests, "TerminalPaneLayout/RemoveGivesShareToTheLeftNeighbour",
          TestTerminalPaneWeightsRemoveGivesShareToTheLeftNeighbour);
  AddTest(tests, "TerminalPaneLayout/DividerResizeMovesOnlyThePair",
          TestTerminalPaneDividerResizeMovesOnlyThePair);
  AddTest(tests, "TerminalPaneLayout/RectsTileTheBodyForEveryWeightMix",
          TestTerminalPaneRectsTileTheBodyForEveryWeightMix);
  AddTest(tests, "WorkspaceLayout/MaximizedPanelLeavesOnlyMenuAndProjectStrip",
          TestMaximizedPanelLayoutLeavesOnlyMenuAndProjectStrip);
}

}  // namespace microide::tests
