#include "TestSupport.h"

#include "editor/BreakpointStore.h"
#include "editor/EditorViewModel.h"
#include "editor/TextViewport.h"
#include "editor/WelcomeView.h"
#include "workspace/DebugViewModel.h"
#include "workspace/RenderViewModelBuilder.h"
#include "workspace/SettingsOverlayService.h"
#include "workspace/StatusBarService.h"
#include "workspace/WorkspaceCommandRegistry.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceLayout.h"

#include <filesystem>
#include <string>
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

void TestBuilderMarksExecutionLineOnlyForMatchingFile() {
  WorkspaceContext ctx;
  RenderViewModelBuilder builder(ctx);

  microide::editor::TextViewport viewport;
  viewport.LoadContent("line0\nline1\nline2\nline3\n", "/proj/main.py");
  viewport.SetViewportSize(8, 80);

  microide::workspace::DebugExecutionView exec;
  exec.stopped = true;
  exec.thread_id = 1;
  microide::workspace::DebugStackFrameView frame;
  frame.source_path = std::filesystem::path("/proj/main.py").lexically_normal();
  frame.line = 2;  // 0-based
  exec.frames.push_back(frame);

  microide::editor::EditorViewModel vm;
  builder.BuildEditorViewModelInto(vm, viewport, 8, nullptr,
                                   /*occurrences_highlight_enabled=*/false,
                                   /*occurrences_case_sensitive=*/false,
                                   /*sticky_scroll_enabled=*/false, /*sticky_max_depth=*/3,
                                   /*render_whitespace_enabled=*/false,
                                   /*debug_enabled=*/true, /*breakpoints=*/nullptr, &exec);
  Expect(vm.execution_line_index.has_value() && *vm.execution_line_index == 2,
         "execution line should be marked on the focused frame's line for the matching file");

  // Debugger disabled -> no execution-line mark even with a stopped session.
  builder.BuildEditorViewModelInto(vm, viewport, 8, nullptr, false, false, false, 3, false,
                                   /*debug_enabled=*/false, nullptr, &exec);
  Expect(!vm.execution_line_index.has_value(),
         "execution line should not be marked when the debugger is disabled");

  // A frame in a different file leaves this viewport unmarked.
  exec.frames[0].source_path = std::filesystem::path("/proj/other.py").lexically_normal();
  builder.BuildEditorViewModelInto(vm, viewport, 8, nullptr, false, false, false, 3, false,
                                   /*debug_enabled=*/true, nullptr, &exec);
  Expect(!vm.execution_line_index.has_value(),
         "execution line should not be marked when the focused frame is in another file");
}

void TestBuilderMarksConditionalAndLogpointGutterDots() {
  WorkspaceContext ctx;
  RenderViewModelBuilder builder(ctx);

  microide::editor::TextViewport viewport;
  viewport.LoadContent("line0\nline1\nline2\nline3\n", "/proj/main.py");
  viewport.SetViewportSize(8, 80);

  // Line 0: plain, line 1: conditional, line 2: hit-count, line 3: logpoint.
  microide::editor::BreakpointStore store;
  const std::filesystem::path path("/proj/main.py");
  store.Set(path, 0);
  store.SetCondition(path, 1, "x > 0");
  store.SetHitCondition(path, 2, ">5");
  store.SetLogMessage(path, 3, "hit {x}");

  microide::editor::EditorViewModel vm;
  builder.BuildEditorViewModelInto(vm, viewport, 8, nullptr, false, false, false, 3, false,
                                   /*debug_enabled=*/true, &store, nullptr);
  Expect(vm.breakpoint_gutter_marks.size() == 4,
         "all four breakpoints should surface gutter marks for the matching file");

  const auto mark_for = [&](std::size_t line) -> const microide::editor::BreakpointGutterMark* {
    for (const auto& mark : vm.breakpoint_gutter_marks) {
      if (mark.line_index == line) {
        return &mark;
      }
    }
    return nullptr;
  };
  const auto* plain = mark_for(0);
  const auto* conditional = mark_for(1);
  const auto* hit = mark_for(2);
  const auto* logpoint = mark_for(3);
  Expect(plain != nullptr && !plain->has_condition && !plain->is_logpoint,
         "a plain breakpoint carries neither the conditional nor the logpoint flag");
  Expect(conditional != nullptr && conditional->has_condition && !conditional->is_logpoint,
         "a condition makes the mark read as conditional, not a logpoint");
  Expect(hit != nullptr && hit->has_condition && !hit->is_logpoint,
         "a hit-count condition also reads as conditional");
  Expect(logpoint != nullptr && logpoint->is_logpoint,
         "a log message makes the mark read as a logpoint");
}

void TestBuilderWelcomeViewIsRegistrySourcedWithRecents() {
  WorkspaceContext context;
  RenderViewModelBuilder builder(context);

  // Only existing roots are surfaced, so create real directories on disk. A trailing
  // separator is preserved to exercise the folder-name-from-parent path.
  TemporaryDirectory temp;
  std::filesystem::create_directories(temp.path() / "alpha");
  std::filesystem::create_directories(temp.path() / "beta");
  const std::filesystem::path alpha = temp.path() / "alpha";
  const std::filesystem::path beta_slash = (temp.path() / "beta").string() + "/";
  const std::vector<std::filesystem::path> recents = {alpha, beta_slash};
  const editor::WelcomeViewModel vm = builder.BuildWelcomeView(recents);

  // Recents map to folder name + full path, preserved in order.
  Expect(vm.recent_projects.size() == 2, "every existing recent project should be surfaced");
  Expect(vm.recent_projects[0].name == "alpha" && vm.recent_projects[0].path == alpha,
         "recent name should be the folder name and path should be preserved");
  Expect(vm.recent_projects[1].name == "beta",
         "a trailing slash should still yield the folder name");

  // Shortcuts are sourced from the command registry: the command-palette row must
  // carry the registry's accelerator, and the hint must echo the same chord.
  const microide::workspace::ActionSpec* palette =
      microide::workspace::FindWorkspaceActionSpec(microide::workspace::ActionId::OpenCommandPalette);
  Expect(palette != nullptr && !palette->accelerator.empty(),
         "the command palette action should carry an accelerator");
  bool found_palette_row = false;
  for (const editor::WelcomeShortcut& shortcut : vm.shortcuts) {
    if (shortcut.keys == std::string(palette->accelerator)) {
      found_palette_row = true;
    }
  }
  Expect(found_palette_row, "the welcome shortcuts should be registry-sourced (no drift)");
  Expect(vm.palette_hint.find(std::string(palette->accelerator)) != std::string::npos,
         "the palette hint should reference the real key chord");
}

void TestBuilderWelcomeViewPrunesMissingRecents() {
  WorkspaceContext context;
  RenderViewModelBuilder builder(context);

  TemporaryDirectory temp;
  std::filesystem::create_directories(temp.path() / "present");
  const std::filesystem::path present = temp.path() / "present";
  const std::filesystem::path missing = temp.path() / "deleted-temp-project";

  // A stale root (e.g. a removed temp project) must not appear — every surfaced row has to
  // be openable. The existing root, listed second, should still come through.
  const std::vector<std::filesystem::path> recents = {missing, present};
  const editor::WelcomeViewModel vm = builder.BuildWelcomeView(recents);

  Expect(vm.recent_projects.size() == 1, "non-existent recent roots should be pruned");
  Expect(vm.recent_projects[0].path == present,
         "the surviving recent should be the one that still exists on disk");
}

void TestComputeWelcomeLayoutProducesHitRegions() {
  editor::WelcomeViewModel vm;
  vm.recent_projects = {
      {.name = "a", .path_display = "/a", .path = "/a"},
      {.name = "b", .path_display = "/b", .path = "/b"},
  };
  const SDL_FRect rect{0.0f, 0.0f, 1200.0f, 800.0f};
  const editor::WelcomeLayout layout = editor::ComputeWelcomeLayout(rect, vm, 14.0f);

  std::size_t recent_regions = 0;
  std::size_t open_folder_regions = 0;
  for (const editor::WelcomeHitRegion& region : layout.hit_regions) {
    if (region.kind == editor::WelcomeHitRegion::Kind::RecentProject) {
      ++recent_regions;
      Expect(region.recent_index < vm.recent_projects.size(),
             "recent hit regions index into the model");
    } else {
      ++open_folder_regions;
    }
    // Every interactive region stays within the card.
    Expect(region.rect.x >= layout.card.x - 0.5f &&
               region.rect.x + region.rect.w <= layout.card.x + layout.card.w + 0.5f,
           "hit regions should stay inside the welcome card");
  }
  Expect(recent_regions == 2, "one hit region per recent project");
  Expect(open_folder_regions == 1, "exactly one open-folder affordance");
}

}  // namespace

void RegisterRenderViewModelBuilderTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RenderViewModelBuilder/WelcomeViewIsRegistrySourcedWithRecents",
          TestBuilderWelcomeViewIsRegistrySourcedWithRecents);
  AddTest(tests, "RenderViewModelBuilder/WelcomeViewPrunesMissingRecents",
          TestBuilderWelcomeViewPrunesMissingRecents);
  AddTest(tests, "RenderViewModelBuilder/ComputeWelcomeLayoutProducesHitRegions",
          TestComputeWelcomeLayoutProducesHitRegions);
  AddTest(tests, "RenderViewModelBuilder/ConstructsAllSurfaceViewModels",
          TestBuilderConstructsAllSurfaceViewModels);
  AddTest(tests, "RenderViewModelBuilder/SidebarFallbacksAreViewsIntoStableStorage",
          TestBuildSidebarSurfaceFallbacksAreViewsIntoStableStorage);
  AddTest(tests, "RenderViewModelBuilder/StatusBarSurfacesTooltipFromService",
          TestBuilderStatusBarSurfacesTooltipFromService);
  AddTest(tests, "RenderViewModelBuilder/MarksExecutionLineOnlyForMatchingFile",
          TestBuilderMarksExecutionLineOnlyForMatchingFile);
  AddTest(tests, "RenderViewModelBuilder/MarksConditionalAndLogpointGutterDots",
          TestBuilderMarksConditionalAndLogpointGutterDots);
}

}  // namespace microide::tests
