#include "TestSupport.h"

#include "workspace/WorkspaceShellShared.h"

#include <cmath>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::BottomPanelCommandAreaRect;
using microide::workspace::BottomPanelCommandPromptRect;
using microide::workspace::BottomPanelContentRect;
using microide::workspace::ClampBottomPanelHeight;
using microide::workspace::ClampSidebarWidth;
using microide::workspace::ComputeLayout;
using microide::workspace::ComputeOverlaySurfaceRect;
using microide::workspace::ComputeScrollbarThumb;
using microide::workspace::ComputeVisibleStripLayouts;
using microide::workspace::EnsureVisibleStripIndex;
using microide::workspace::MakeHorizontalScrollbarGeometry;
using microide::workspace::MakeRect;
using microide::workspace::MakeVerticalScrollbarGeometry;
using microide::workspace::ScrollUnitsForPointer;

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

void TestWorkspaceSharedPanelGeometryHelpers() {
  const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 280.0f, 200.0f);
  const auto content = BottomPanelContentRect(layout, true);
  const auto command_area = BottomPanelCommandAreaRect(layout);
  const auto prompt = BottomPanelCommandPromptRect(layout);

  Expect(command_area.y >= content.y + content.h, "command area should sit below panel content");
  Expect(prompt.y >= command_area.y, "command prompt should stay inside the command area");
  Expect(prompt.x > command_area.x, "command prompt should honor horizontal inset");
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
  AddTest(tests, "WorkspaceShared/PanelGeometryHelpers", TestWorkspaceSharedPanelGeometryHelpers);
  AddTest(tests, "WorkspaceShared/ScrollbarEdgeCases", TestWorkspaceSharedScrollbarEdgeCases);
  AddTest(tests, "WorkspaceShared/StripLayoutHelpers", TestWorkspaceSharedStripLayoutHelpers);
  AddTest(tests, "WorkspaceShared/OverlayRectHelpers", TestWorkspaceSharedOverlayRectHelpers);
}

}  // namespace microide::tests
