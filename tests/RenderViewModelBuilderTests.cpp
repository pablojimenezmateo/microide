#include "TestSupport.h"

#include "workspace/RenderViewModelBuilder.h"
#include "workspace/SettingsOverlayService.h"
#include "workspace/StatusBarService.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceLayout.h"

#include <string_view>
#include <type_traits>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::ComputeLayout;
using microide::workspace::LayoutMode;
using microide::workspace::LayoutModeInputs;
using microide::workspace::RenderViewModelBuilder;
using microide::workspace::SettingsOverlayService;
using microide::workspace::StatusBarService;
using microide::workspace::WorkspaceContext;

void TestBuilderConstructsAllSurfaceViewModels() {
  WorkspaceContext context;
  RenderViewModelBuilder builder(context);
  const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 280.0f, 160.0f,
                                    LayoutModeInputs{}, true);

  // Smoke-build every host-owned surface view model. The assertions here are
  // intentionally light — the goal is to lock in that each Build* entry point
  // is constructible against a default workspace context without dereferencing
  // active editor state, so future field additions stay covered.
  const auto frame_vm = builder.BuildFrameSurface(layout);
  Expect(!frame_vm.compare_surface.has_value(),
         "BuildFrameSurface should not synthesize a compare surface for the default context");

  const auto overlay_vm = builder.BuildOverlaySurface();
  Expect(!overlay_vm.visible, "default workspace context should not surface an overlay");

  const auto text_input_vm = builder.BuildTextInputSurface();
  Expect(text_input_vm.current_surface == microide::workspace::TextInputSurface::None,
         "default workspace context should report no active text-input surface");

  const auto sidebar_vm = builder.BuildSidebarSurface();
  Expect(sidebar_vm.project_state == &context.current_project_state,
         "BuildSidebarSurface should wire the project-state pointer for the render path");

  const auto bottom_vm = builder.BuildBottomPanelSurface();
  Expect(bottom_vm.height >= 0.0f,
         "BuildBottomPanelSurface should report a non-negative height");

  const auto hover_popup_vm = builder.BuildHoverPopup(/*has_active_target=*/false);
  Expect(!hover_popup_vm.has_active_target,
         "BuildHoverPopup should propagate active-target flag");

  const auto hover_targets_vm = builder.BuildHoverTargets();
  Expect(hover_targets_vm.diagnostics_store != nullptr,
         "BuildHoverTargets should reference the workspace diagnostics store");

  SettingsOverlayService settings_service;
  const auto settings_vm = builder.BuildSettingsOverlay(layout, settings_service);
  Expect(!settings_vm.visible,
         "BuildSettingsOverlay should report invisible when the service is closed");
}

void TestBuildSidebarSurfaceFallbacksAreViewsIntoStableStorage() {
  // 2026-05-15 perf deep-dive round 2 Finding 1: BuildSidebarSurface is called multiple times per
  // frame. The fallback fields must be std::string_view backed by either compile-time constants or
  // live state, never per-call std::string allocations.
  static_assert(std::is_same_v<decltype(microide::workspace::SidebarSurfaceViewModel::
                                            query_fallback_text),
                                std::string_view>,
                "SidebarSurfaceViewModel::query_fallback_text must be std::string_view");
  static_assert(std::is_same_v<decltype(microide::workspace::SidebarSurfaceViewModel::
                                            replace_fallback_text),
                                std::string_view>,
                "SidebarSurfaceViewModel::replace_fallback_text must be std::string_view");

  WorkspaceContext context;
  RenderViewModelBuilder builder(context);

  // Empty query: fallback should point at the static "Search in project" placeholder. Two builds
  // back-to-back must return the same data pointer to prove no per-call allocation.
  const auto first = builder.BuildSidebarSurface();
  const auto second = builder.BuildSidebarSurface();
  Expect(first.query_fallback_text == "Search in project",
         "empty query should fall back to the default placeholder");
  Expect(first.replace_fallback_text == "Replace in project",
         "empty replace should fall back to the default placeholder");
  Expect(first.query_fallback_text.data() == second.query_fallback_text.data(),
         "back-to-back BuildSidebarSurface calls must reuse the same fallback storage "
         "(no per-call std::string allocation)");
  Expect(first.replace_fallback_text.data() == second.replace_fallback_text.data(),
         "back-to-back BuildSidebarSurface calls must reuse the same fallback storage "
         "for the replace placeholder");

  // OverlaySurfaceViewModel.buffer_search_query_text similarly. Must be a view.
  static_assert(std::is_same_v<decltype(microide::workspace::OverlaySurfaceViewModel::
                                            buffer_search_query_text),
                                std::string_view>,
                "OverlaySurfaceViewModel::buffer_search_query_text must be std::string_view");
}

void TestBuilderStatusBarSurfacesTooltipFromService() {
  WorkspaceContext context;
  StatusBarService service;
  service.SetSegment(microide::workspace::StatusBarSegmentId::Project,
                     microide::workspace::StatusBarSegmentValue{
                         .text = "microide",
                         .tooltip = "Open Source Control (clean)",
                         .clickable = true,
                         .visible = true,
                     });

  const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 280.0f, 160.0f,
                                    LayoutModeInputs{}, true);
  const auto vm = RenderViewModelBuilder(context).BuildStatusBar(layout, service);

  Expect(vm.visible, "status bar VM should be visible when layout reserves the strip");
  Expect(vm.left_segments.size() == 1,
         "status bar VM should expose the project segment on the left");
  Expect(vm.left_segments.front().text == "microide",
         "status bar VM should forward segment text to the render path");
  Expect(vm.left_segments.front().tooltip == "Open Source Control (clean)",
         "status bar VM should forward the segment tooltip so the render path can paint it");
}

}  // namespace

void RegisterRenderViewModelBuilderTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RenderViewModelBuilder/ConstructsAllSurfaceViewModels",
          TestBuilderConstructsAllSurfaceViewModels);
  AddTest(tests, "RenderViewModelBuilder/SidebarFallbacksAreViewsIntoStableStorage",
          TestBuildSidebarSurfaceFallbacksAreViewsIntoStableStorage);
  AddTest(tests, "RenderViewModelBuilder/StatusBarSurfacesTooltipFromService",
          TestBuilderStatusBarSurfacesTooltipFromService);
}

}  // namespace microide::tests
