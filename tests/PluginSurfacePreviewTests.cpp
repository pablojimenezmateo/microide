#include "TestSupport.h"

#include "editor/PluginSurfaceStore.h"
#include "workspace/PluginSurfacePreview.h"
#include "workspace/WorkspaceShellTestAccess.h"

#include <string>

#include "WorkspaceShellEventHelpers.h"

// Bottom-panel plugin surface preview interaction (TD-2026-07-16-60/61): host-
// owned wheel/scrollbar scrolling of the preview content, and hit-region command
// dispatch through the validated command runner.
namespace microide::tests {
namespace {

using microide::editor::RasterHandle;
using microide::editor::SurfaceContent;
using microide::editor::SurfaceHitRegion;
using microide::workspace::FindPluginSurfacePreviewHitRegion;
using microide::workspace::kPluginSurfacePreviewPadding;
using microide::workspace::kWorkspaceBottomPanelHeaderHeight;
using microide::workspace::MaxPluginSurfacePreviewScroll;
using microide::workspace::PanelContentKind;
using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

SurfaceContent TallSurface(float intrinsic_height) {
  SurfaceContent content;
  content.body = RasterHandle{.content_hash = 7, .width = 200,
                              .height = static_cast<int>(intrinsic_height)};
  content.intrinsic_width = 200.0f;
  content.intrinsic_height = intrinsic_height;
  return content;
}

// Publishes a tall preview surface and points the bottom panel at it.
void ShowPreviewPanel(WorkspaceShell& shell, SurfaceContent content) {
  auto& state = WorkspaceShellTestAccess::CurrentProjectState(shell);
  state.EnsurePluginPresentation().surfaces.ReplaceForOwnerSurface("plug", "chart",
                                                                   std::move(content));
  state.panel.content = PanelContentKind::PluginSurface;
  state.panel.surface_owner = "plug";
  state.panel.surface_id = "chart";
  state.panel.surface_scroll_y = 0;
}

void TestHitRegionMappingIsScrollAndPaddingAware() {
  SurfaceContent content = TallSurface(2000.0f);
  content.hit_regions.push_back(
      SurfaceHitRegion{.rect = {10.0f, 100.0f, 80.0f, 20.0f}, .command = "first"});
  content.hit_regions.push_back(
      SurfaceHitRegion{.rect = {10.0f, 100.0f, 80.0f, 20.0f}, .command = "second"});
  content.hit_regions.push_back(
      SurfaceHitRegion{.rect = {0.0f, 0.0f, -8.0f, 4.0f}, .command = "degenerate"});

  const SDL_FRect body{100.0f, 400.0f, 300.0f, 150.0f};
  const float pad = kPluginSurfacePreviewPadding;

  // Unscrolled: content y=100 sits below the 150px body — no hit at the top.
  Expect(FindPluginSurfacePreviewHitRegion(content, body, 0.0f, body.x + pad + 15.0f,
                                           body.y + pad + 5.0f) == nullptr,
         "a point above the region should not hit");
  // Scrolled so content y=100 maps into view: hit resolves, last-published wins.
  const float scroll = 90.0f;
  const SurfaceHitRegion* hit = FindPluginSurfacePreviewHitRegion(
      content, body, scroll, body.x + pad + 15.0f, body.y + pad + (105.0f - scroll));
  Expect(hit != nullptr && hit->command == "second",
         "the topmost (last-published) overlapping region should win");
  // Outside the region horizontally: no hit; the degenerate region never matches.
  Expect(FindPluginSurfacePreviewHitRegion(content, body, scroll, body.x + pad + 95.0f,
                                           body.y + pad + (105.0f - scroll)) == nullptr,
         "a point right of the region should not hit");

  Expect(MaxPluginSurfacePreviewScroll(content, 150.0f) ==
             static_cast<int>(2000.0f + 2.0f * pad - 150.0f),
         "max scroll should be the padded content overflow");
  Expect(MaxPluginSurfacePreviewScroll(content, 3000.0f) == 0,
         "a fitting surface should not scroll");
}

void TestWheelScrollsAndClampsPreview() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 900, 700);
  ShowPreviewPanel(shell, TallSurface(5000.0f));
  auto& state = WorkspaceShellTestAccess::CurrentProjectState(shell);

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  Expect(layout.bottom_panel.h > 0.0f,
         "a plugin surface preview should reserve the bottom panel in the layout");
  const float body_height = layout.bottom_panel.h - kWorkspaceBottomPanelHeaderHeight;
  const float in_x = layout.bottom_panel.x + 40.0f;
  const float in_y = layout.bottom_panel.y + kWorkspaceBottomPanelHeaderHeight + 30.0f;

  Expect(SendMouseWheel(shell, in_x, in_y, -3),
         "wheel over the preview should be handled");
  const int after_first = state.panel.surface_scroll_y;
  Expect(after_first > 0, "wheel down should scroll the preview content");

  // A huge scroll clamps to the padded content overflow, and wheel-up returns to 0.
  Expect(SendMouseWheel(shell, in_x, in_y, -100000),
         "a large wheel should still be handled");
  const SurfaceContent* content =
      state.plugin_presentation_if_present()->surfaces.Find("plug", "chart");
  Expect(content != nullptr, "the preview surface should stay published");
  Expect(state.panel.surface_scroll_y ==
             MaxPluginSurfacePreviewScroll(*content, body_height),
         "wheel scroll should clamp to the content overflow");
  Expect(SendMouseWheel(shell, in_x, in_y, 100000),
         "wheel up should be handled");
  Expect(state.panel.surface_scroll_y == 0, "wheel up should clamp back to zero");
}

void TestHitRegionClickDispatchesCommand() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 900, 700);
  SurfaceContent content = TallSurface(5000.0f);
  content.hit_regions.push_back(
      SurfaceHitRegion{.rect = {10.0f, 1500.0f, 120.0f, 24.0f}, .command = "sidebar-toggle"});
  ShowPreviewPanel(shell, std::move(content));
  auto& state = WorkspaceShellTestAccess::CurrentProjectState(shell);

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const float body_y = layout.bottom_panel.y + kWorkspaceBottomPanelHeaderHeight;
  const float pad = kPluginSurfacePreviewPadding;

  // Scroll the region into view, then click inside it: its command dispatches
  // through the normal command path (observable as the sidebar toggling).
  state.panel.surface_scroll_y = 1490;
  const bool sidebar_before = state.sidebar.visible;
  const float click_x = layout.bottom_panel.x + pad + 20.0f;
  const float click_y = body_y + pad + (1510.0f - 1490.0f);
  Expect(SendMouseDown(shell, click_x, click_y, SDL_BUTTON_LEFT),
         "a click on the preview should be handled");
  Expect(state.sidebar.visible != sidebar_before,
         "a hit-region click should run its command");
  Expect(state.surface.focus == microide::workspace::FocusTarget::Panel,
         "a preview click should focus the panel");
  SendMouseUp(shell, click_x, click_y, SDL_BUTTON_LEFT);

  // A click outside every region focuses the panel but dispatches nothing.
  const bool sidebar_mid = state.sidebar.visible;
  Expect(SendMouseDown(shell, click_x, body_y + pad + 200.0f, SDL_BUTTON_LEFT),
         "a click off-region should still be handled by the panel");
  Expect(state.sidebar.visible == sidebar_mid,
         "a click outside every hit region should not run a command");
  SendMouseUp(shell, click_x, body_y + pad + 200.0f, SDL_BUTTON_LEFT);

  // An unscrolled click at the same panel point maps to different content
  // coordinates and must miss the (scrolled-away) region.
  state.panel.surface_scroll_y = 0;
  Expect(SendMouseDown(shell, click_x, click_y, SDL_BUTTON_LEFT),
         "an unscrolled click should be handled");
  Expect(state.sidebar.visible == sidebar_mid,
         "hit-region mapping must account for the scroll offset");
  SendMouseUp(shell, click_x, click_y, SDL_BUTTON_LEFT);
}

void TestScrollbarDragScrollsPreview() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 900, 700);
  ShowPreviewPanel(shell, TallSurface(5000.0f));
  auto& state = WorkspaceShellTestAccess::CurrentProjectState(shell);

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const SDL_FRect body = microide::workspace::BottomPanelContentRect(layout);
  const SurfaceContent* content =
      state.plugin_presentation_if_present()->surfaces.Find("plug", "chart");
  const auto geometry = microide::workspace::MakeVerticalScrollbarGeometry(
      body, microide::workspace::PluginSurfacePreviewContentHeight(*content), body.h, 0.0f);
  Expect(geometry.has_value(), "an overflowing preview should have a scrollbar");

  // Grab the track below the thumb: the thumb centers on the pointer and the
  // scroll lands strictly past the top.
  const float track_x = geometry->track.x + geometry->track.w * 0.5f;
  const float grab_y = geometry->track.y + geometry->track.h * 0.75f;
  Expect(SendMouseDown(shell, track_x, grab_y, SDL_BUTTON_LEFT),
         "a scrollbar-track press should be handled");
  const int after_press = state.panel.surface_scroll_y;
  Expect(after_press > 0, "pressing the lower track should jump the scroll down");

  // Dragging to the very bottom of the track clamps to max scroll; releasing
  // ends the drag.
  Expect(SendMouseMotion(shell, track_x, geometry->track.y + geometry->track.h,
                         SDL_BUTTON_LMASK),
         "a scrollbar drag should be handled");
  Expect(state.panel.surface_scroll_y == MaxPluginSurfacePreviewScroll(*content, body.h),
         "dragging to the track bottom should clamp to max scroll");
  Expect(SendMouseUp(shell, track_x, geometry->track.y + geometry->track.h, SDL_BUTTON_LEFT),
         "releasing the scrollbar should be handled");
}

}  // namespace

void RegisterPluginSurfacePreviewTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PluginSurfacePreview/HitRegionMappingIsScrollAndPaddingAware",
          TestHitRegionMappingIsScrollAndPaddingAware);
  AddTest(tests, "PluginSurfacePreview/WheelScrollsAndClampsPreview",
          TestWheelScrollsAndClampsPreview);
  AddTest(tests, "PluginSurfacePreview/HitRegionClickDispatchesCommand",
          TestHitRegionClickDispatchesCommand);
  AddTest(tests, "PluginSurfacePreview/ScrollbarDragScrollsPreview",
          TestScrollbarDragScrollsPreview);
}

}  // namespace microide::tests
