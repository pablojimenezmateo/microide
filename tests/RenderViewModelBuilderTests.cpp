#include "TestSupport.h"

#include "editor/BreakpointStore.h"
#include "editor/EditorViewModel.h"
#include "editor/TextViewport.h"
#include "editor/WelcomeView.h"
#include "render/TextRenderer.h"
#include "workspace/DebugViewModel.h"
#include "workspace/RecentsService.h"
#include "workspace/RenderViewModelBuilder.h"
#include "workspace/SettingsOverlayService.h"
#include "workspace/StatusBarService.h"
#include "workspace/WorkspaceCommandRegistry.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceLayout.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::ComputeLayout;
using microide::workspace::LayoutMode;
using microide::workspace::LayoutModeInputs;
using microide::workspace::RecentsService;
using microide::render::TextRenderer;
using microide::workspace::RenderViewModelBuilder;
using microide::workspace::SettingsOverlayRow;
using microide::workspace::SettingsOverlayService;
using microide::workspace::SettingsRowViewModel;
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
  TextRenderer settings_text_renderer;
  const auto settings_vm =
      builder.BuildSettingsOverlay(layout, settings_service, settings_text_renderer);
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
  frame.SetSource("/proj/main.py");
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
  exec.frames[0].SetSource("/proj/other.py");
  builder.BuildEditorViewModelInto(vm, viewport, 8, nullptr, false, false, false, 3, false,
                                   /*debug_enabled=*/true, nullptr, &exec);
  Expect(!vm.execution_line_index.has_value(),
         "execution line should not be marked when the focused frame is in another file");
}

void TestBuilderInsetGapsEmptyWithoutPluginPresentation() {
  WorkspaceContext ctx;  // default context: no plugin presentation bundle
  RenderViewModelBuilder builder(ctx);

  microide::editor::TextViewport viewport;
  viewport.LoadContent("line0\nline1\nline2\nline3\n", "/proj/main.py");
  viewport.SetViewportSize(8, 80);

  // Every inline-inset feature flag is on, but no plugin/LSP presentation has been
  // published. The builder must early-out so the editor geometry stays byte-identical
  // to the no-plugin path: no row gaps, no ghost tail. The render frame's master gate
  // additionally skips even reading the plugins.* settings in this case; this test
  // locks in the consumer-side early-out that the gate depends on for correctness.
  const editor::InsetGapFeatureFlags all_on{
      .inline_surfaces = true, .code_lens_above = true, .ghost_text = true};
  microide::editor::EditorViewModel vm;
  builder.BuildEditorViewModelInto(vm, viewport, 8, nullptr, false, false, false, 3, false,
                                   /*debug_enabled=*/false, nullptr, nullptr, all_on,
                                   /*line_height=*/16.0f);

  Expect(ctx.current_project_state.plugin_presentation_if_present() == nullptr,
         "the default workspace context allocates no plugin presentation bundle");
  Expect(vm.row_gaps.empty(),
         "no plugin presentation => no inline-inset row gaps regardless of feature flags");
  Expect(vm.row_gap_contents.empty(),
         "no plugin presentation => no row-gap contents regardless of feature flags");
  Expect(!vm.ghost_text_tail.has_value(),
         "no plugin presentation => no ghost-text tail regardless of feature flags");
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
  WorkspaceContext context;  // empty root => NoProject cold-start variant
  RenderViewModelBuilder builder(context);

  // Only existing roots are surfaced, so create real directories on disk. A trailing
  // separator is preserved to exercise the folder-name-from-parent path. Record beta then
  // alpha so the newest-first MRU surfaces them in [alpha, beta] order.
  TemporaryDirectory temp;
  std::filesystem::create_directories(temp.path() / "alpha");
  std::filesystem::create_directories(temp.path() / "beta");
  const std::filesystem::path alpha = temp.path() / "alpha";
  const std::filesystem::path beta_slash = (temp.path() / "beta").string() + "/";
  RecentsService recents;
  recents.RecordProjectOpen(beta_slash);
  recents.RecordProjectOpen(alpha);
  const editor::WelcomeViewModel vm = builder.BuildWelcomeView(recents);

  Expect(vm.kind == editor::WelcomeKind::NoProject, "no open project => NoProject variant");
  // Recents map to folder name + full path, preserved in order.
  Expect(vm.recent_projects.size() == 2, "every existing recent project should be surfaced");
  Expect(vm.recent_projects[0].name == "alpha" && vm.recent_projects[0].path == alpha,
         "recent name should be the folder name and path should be preserved");
  Expect(vm.recent_projects[1].name == "beta",
         "a trailing slash should still yield the folder name");

  // Shortcuts are sourced from the command registry: the command-palette row must carry
  // the registry's accelerator (the palette is advertised here, once — no footer hint).
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
}

void TestBuilderWelcomeViewPrunesMissingRecents() {
  WorkspaceContext context;
  RenderViewModelBuilder builder(context);

  TemporaryDirectory temp;
  std::filesystem::create_directories(temp.path() / "present");
  const std::filesystem::path present = temp.path() / "present";
  const std::filesystem::path missing = temp.path() / "deleted-temp-project";

  // A stale root (e.g. a removed temp project) must not appear — every surfaced row has to
  // be openable. The existing root should still come through.
  RecentsService recents;
  recents.RecordProjectOpen(present);
  recents.RecordProjectOpen(missing);
  const editor::WelcomeViewModel vm = builder.BuildWelcomeView(recents);

  Expect(vm.recent_projects.size() == 1, "non-existent recent roots should be pruned");
  Expect(vm.recent_projects[0].path == present,
         "the surviving recent should be the one that still exists on disk");
}

void TestBuilderWelcomeViewProjectHomeShowsRecentFiles() {
  // A project is open but its focused group has no tab => ProjectHome variant: the hero is
  // the project name, the list shows this project's recent files, and the no-project
  // affordances (open-folder / recent projects) are absent.
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "myproject";
  std::filesystem::create_directories(root);
  const std::filesystem::path present = root / "main.cpp";
  { std::ofstream(present) << "int main() {}\n"; }
  const std::filesystem::path missing = root / "ghost.cpp";

  WorkspaceContext context;
  context.current_project_state.root = root;
  RenderViewModelBuilder builder(context);

  RecentsService recents;
  recents.RecordFileOpen(present, root);
  recents.RecordFileOpen(missing, root);
  // A file recorded under a different project must not leak into this project's home.
  recents.RecordFileOpen(temp.path() / "other.cpp", temp.path() / "other");
  const editor::WelcomeViewModel vm = builder.BuildWelcomeView(recents);

  Expect(vm.kind == editor::WelcomeKind::ProjectHome, "an open project => ProjectHome variant");
  Expect(vm.title == "myproject", "the hero title should be the project folder name");
  Expect(vm.recent_projects.empty(), "ProjectHome must not surface recent projects");
  Expect(!vm.new_file_label.empty() && !vm.open_file_label.empty() &&
             !vm.find_in_project_label.empty(),
         "ProjectHome should carry the new/open/find action labels");
  Expect(vm.recent_files.size() == 1,
         "only this project's existing recent files should be surfaced (missing + other pruned)");
  Expect(vm.recent_files[0].path == present && vm.recent_files[0].name == "main.cpp",
         "the surviving recent file should be the one that still exists on disk");
}

void TestComputeWelcomeLayoutProducesHitRegions() {
  const SDL_FRect rect{0.0f, 0.0f, 1200.0f, 800.0f};
  const auto within_card = [](const editor::WelcomeLayout& layout,
                              const editor::WelcomeHitRegion& region) {
    return region.rect.x >= layout.card.x - 0.5f &&
           region.rect.x + region.rect.w <= layout.card.x + layout.card.w + 0.5f;
  };

  // NoProject: one open-folder button + one row per recent project.
  {
    editor::WelcomeViewModel vm;
    vm.kind = editor::WelcomeKind::NoProject;
    vm.recent_projects = {
        {.name = "a", .path_display = "/a", .path = "/a"},
        {.name = "b", .path_display = "/b", .path = "/b"},
    };
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
      Expect(within_card(layout, region), "hit regions should stay inside the welcome card");
    }
    Expect(recent_regions == 2, "one hit region per recent project");
    Expect(open_folder_regions == 1, "exactly one open-folder affordance");
  }

  // ProjectHome: three action buttons + one row per recent file.
  {
    editor::WelcomeViewModel vm;
    vm.kind = editor::WelcomeKind::ProjectHome;
    vm.recent_files = {
        {.name = "x", .path_display = "/x", .path = "/x"},
        {.name = "y", .path_display = "/y", .path = "/y"},
    };
    const editor::WelcomeLayout layout = editor::ComputeWelcomeLayout(rect, vm, 14.0f);
    std::size_t recent_file_regions = 0;
    bool saw_new = false, saw_open = false, saw_find = false;
    for (const editor::WelcomeHitRegion& region : layout.hit_regions) {
      switch (region.kind) {
        case editor::WelcomeHitRegion::Kind::RecentFile:
          ++recent_file_regions;
          Expect(region.recent_index < vm.recent_files.size(),
                 "recent-file hit regions index into the model");
          break;
        case editor::WelcomeHitRegion::Kind::NewFile: saw_new = true; break;
        case editor::WelcomeHitRegion::Kind::OpenFile: saw_open = true; break;
        case editor::WelcomeHitRegion::Kind::FindInProject: saw_find = true; break;
        default:
          Expect(false, "ProjectHome should not emit no-project regions");
          break;
      }
      Expect(within_card(layout, region), "hit regions should stay inside the welcome card");
    }
    Expect(recent_file_regions == 2, "one hit region per recent file");
    Expect(saw_new && saw_open && saw_find, "ProjectHome exposes new/open/find buttons");
  }
}

}  // namespace

void TestSettingsOverlayWrapsLongDescriptions() {
  WorkspaceContext context;
  RenderViewModelBuilder builder(context);
  // No-backend TextRenderer: deterministic 8px/char, 14px line-height.
  TextRenderer text_renderer;
  const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 280.0f, 160.0f,
                                    LayoutModeInputs{}, true);

  SettingsOverlayService service;
  service.OpenSettings();
  std::vector<SettingsOverlayRow> rows;
  SettingsOverlayRow short_row;
  short_row.id = "test.short";
  short_row.label = "Short";
  short_row.description = "Tiny.";
  rows.push_back(short_row);
  SettingsOverlayRow long_row;
  long_row.id = "test.long";
  long_row.label = "Long";
  long_row.description =
      "This is a deliberately long help description that must wrap across several "
      "lines so the full text stays readable instead of being clipped with an "
      "ellipsis at the right edge of the settings row.";
  rows.push_back(long_row);
  service.RebuildSettingsRows({}, {}, {}, rows);
  service.SetSelectedCategory(0);

  const auto vm = builder.BuildSettingsOverlay(layout, service, text_renderer);
  Expect(vm.visible, "settings overlay should be visible after OpenSettings");
  Expect(vm.rows.size() >= 2, "both injected rows should be built into the view model");

  const SettingsRowViewModel* short_vm = nullptr;
  const SettingsRowViewModel* long_vm = nullptr;
  for (const SettingsRowViewModel& row : vm.rows) {
    if (row.id == "test.short") {
      short_vm = &row;
    } else if (row.id == "test.long") {
      long_vm = &row;
    }
  }
  Expect(short_vm != nullptr && long_vm != nullptr,
         "both the short and long rows should appear in the view model");
  Expect(long_vm->description_lines.size() > 1,
         "a long description should word-wrap to multiple visible lines");
  Expect(short_vm->description_lines.size() == 1,
         "a short description should occupy a single line");
  Expect(long_vm->row_rect.h > short_vm->row_rect.h,
         "the wrapped row should be taller than a single-line row");

  // Rows stack by their variable heights without overlapping.
  const SettingsRowViewModel& first = vm.rows.front();
  const SettingsRowViewModel& second = vm.rows[1];
  Expect(second.row_rect.y >= first.row_rect.y + first.row_rect.h - 0.5f,
         "consecutive rows must not overlap under variable-height stacking");

  // Wrapped lines are views into the row's own description string (no per-line copies).
  for (std::string_view line : long_vm->description_lines) {
    const bool inside = line.data() >= long_vm->description.data() &&
                        line.data() + line.size() <=
                            long_vm->description.data() + long_vm->description.size();
    Expect(inside, "each wrapped line must be a view into the row description");
  }
}

// The font-picker dropdown windows a long family list; when it overflows the visible
// window the builder must emit scrollbar geometry (drawn by the render pass) and the
// scroll offset must control which slice of families becomes item view models.
void TestSettingsOverlayFontPickerBuildsScrollbarOnOverflow() {
  WorkspaceContext context;
  RenderViewModelBuilder builder(context);
  TextRenderer text_renderer;
  const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 280.0f, 160.0f,
                                    LayoutModeInputs{}, true);

  SettingsOverlayService service;
  service.OpenSettings();
  SettingsOverlayRow font_row;
  font_row.id = "editor.font_family";
  font_row.label = "Font Family";
  font_row.description = "Editor font family.";
  font_row.control_kind = microide::workspace::SettingsControlKind::TextEdit;
  font_row.editable = true;
  font_row.suggests_fonts = true;
  service.RebuildSettingsRows({}, {}, {}, {font_row});
  service.SetSelectedCategory(0);

  const int total = SettingsOverlayService::kPickerVisibleFamilies + 5;
  std::vector<std::string> families;
  for (int i = 0; i < total; ++i) {
    families.push_back("Family " + std::string(1, static_cast<char>('A' + i)));
  }
  service.BeginFontValueEdit("editor.font_family", families);

  const auto vm = builder.BuildSettingsOverlay(layout, service, text_renderer);
  Expect(vm.value_picker.visible, "the font picker should be visible while editing a font row");
  Expect(vm.value_picker.scrollbar.has_value(),
         "an overflowing family list should build a picker scrollbar");
  // Window shows kPickerVisibleFamilies rows + the pinned "Choose file…" footer.
  Expect(vm.value_picker.items.size() ==
             static_cast<std::size_t>(SettingsOverlayService::kPickerVisibleFamilies) + 1,
         "the picker draws one window of families plus the Choose file… footer");
  Expect(vm.value_picker.items.front().text == "Family A",
         "at scroll 0 the window starts at the first family");

  // Scrolling advances the visible slice and keeps the scrollbar present.
  service.SetPickerScroll(3);
  const auto scrolled = builder.BuildSettingsOverlay(layout, service, text_renderer);
  Expect(scrolled.value_picker.scrollbar.has_value(), "scrollbar persists while scrolled");
  Expect(scrolled.value_picker.items.front().text == "Family D",
         "a scroll offset of 3 starts the window at the fourth family");
  Expect(scrolled.value_picker.more_above && scrolled.value_picker.more_below,
         "a mid-list window reports families both above and below");
}

void RegisterRenderViewModelBuilderTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RenderViewModelBuilder/SettingsOverlayFontPickerBuildsScrollbarOnOverflow",
          TestSettingsOverlayFontPickerBuildsScrollbarOnOverflow);
  AddTest(tests, "RenderViewModelBuilder/SettingsOverlayWrapsLongDescriptions",
          TestSettingsOverlayWrapsLongDescriptions);
  AddTest(tests, "RenderViewModelBuilder/WelcomeViewIsRegistrySourcedWithRecents",
          TestBuilderWelcomeViewIsRegistrySourcedWithRecents);
  AddTest(tests, "RenderViewModelBuilder/WelcomeViewPrunesMissingRecents",
          TestBuilderWelcomeViewPrunesMissingRecents);
  AddTest(tests, "RenderViewModelBuilder/WelcomeViewProjectHomeShowsRecentFiles",
          TestBuilderWelcomeViewProjectHomeShowsRecentFiles);
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
  AddTest(tests, "RenderViewModelBuilder/InsetGapsEmptyWithoutPluginPresentation",
          TestBuilderInsetGapsEmptyWithoutPluginPresentation);
}

}  // namespace microide::tests
