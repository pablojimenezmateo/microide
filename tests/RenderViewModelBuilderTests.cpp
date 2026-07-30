#include "TestSupport.h"

#include "editor/BreakpointStore.h"
#include "editor/EditorViewModel.h"
#include "editor/TextViewport.h"
#include "editor/WelcomeView.h"
#include "render/TextRenderer.h"
#include "workspace/DebugViewModel.h"
#include "workspace/RecentsService.h"
#include "workspace/RenderViewModelBuilder.h"
#include "workspace/WorkspaceShellRenderPrimitives.h"
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

  TextRenderer overlay_text_renderer;
  microide::workspace::OverlaySurfaceViewModel overlay_vm;
  builder.BuildOverlaySurfaceInto(overlay_vm, layout, layout.editor_area, overlay_text_renderer);
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

// TD-2026-07-17-084: the overlay view model is fully owned/precomputed — labels
// composed and truncated in the builder, geometry included, no live
// OverlayState/ProjectWorkspaceState pointers for the render TU to chase.
void TestBuildOverlaySurfaceOwnsPickerRowsAndChrome() {
  WorkspaceContext context;
  auto& palette = context.current_project_state.overlay.workflow.command_palette;
  palette.items.push_back(microide::workspace::CommandPaletteItem{
      .primary_label = "Open File",
      .secondary_label = "Ctrl+O",
      .search_text = "open file ctrl+o",
  });
  palette.items.push_back(microide::workspace::CommandPaletteItem{
      .primary_label = "Save All",
      .secondary_label = {},
      .search_text = "save all",
  });
  palette.matches = {0, 1};
  palette.selected_index = 1;
  palette.summary_line = "2 of 2";
  context.current_project_state.overlay.visible = true;
  context.current_project_state.overlay.mode = microide::workspace::OverlayMode::CommandPalette;

  RenderViewModelBuilder builder(context);
  TextRenderer text_renderer;  // no-backend: deterministic 8px/char
  const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 280.0f, 160.0f,
                                    LayoutModeInputs{}, true);
  microide::workspace::OverlaySurfaceViewModel vm;
  builder.BuildOverlaySurfaceInto(vm, layout, layout.editor_area, text_renderer);

  Expect(vm.visible, "overlay VM should be visible");
  Expect(vm.title == "Commands", "palette title should be prebuilt");
  Expect(vm.has_query_field &&
             vm.query_surface == microide::workspace::TextInputSurface::CommandPalette,
         "palette VM should carry its query surface");
  Expect(vm.query_display_text == "> ",
         "an empty unfocused query shows the composed '> ' fallback");
  Expect(vm.summary_line == "2 of 2", "summary should view the state-owned string");
  Expect(!vm.hint.empty() && vm.hint_x > 0.0f, "picker hint should be prebuilt with its x");
  Expect(vm.total_rows == 2 && vm.rows.size() == 2, "both matches fit the visible window");
  Expect(vm.selected_row == 1, "selected row index should be forwarded");
  Expect(vm.rows[0].primary == "Open File" && vm.rows[1].primary == "Save All",
         "row primaries should be prebuilt");
  // A fitting, state-stable label must be a zero-copy view into the state string.
  Expect(vm.rows[0].primary.data() == palette.items[0].primary_label.data(),
         "a fitting primary label should be a zero-copy view into state");
  Expect(vm.rows[0].secondary == "Ctrl+O" && vm.rows[0].secondary_width > 0.0f,
         "the accelerator column should be prebuilt with its measured width");
  Expect(vm.rows[1].secondary.empty() && vm.rows[1].secondary_width == 0.0f,
         "a row without an accelerator has no secondary column");

  // The stored scroll row is clamped through the list layout.
  context.current_project_state.overlay.scroll_row = 10000;
  builder.BuildOverlaySurfaceInto(vm, layout, layout.editor_area, text_renderer);
  Expect(vm.scroll_row == vm.list_layout.max_scroll,
         "an out-of-range stored scroll row should be clamped by the builder");
}

void TestBuildOverlaySurfaceComposesProjectSearchAndCompletion() {
  WorkspaceContext context;
  auto& search = context.current_project_state.overlay.workflow.project_search;
  project::ProjectSearchResult result;
  result.relative_path = "src/a.cpp";
  result.relative_path_string = "src/a.cpp";
  result.line = 1;    // 0-based -> shown as 2
  result.column = 4;  // 0-based -> shown as 5
  result.preview = "hello world";
  search.results.push_back(result);
  search.selected_index = 0;
  search.index_incomplete = true;
  context.current_project_state.overlay.visible = true;
  context.current_project_state.overlay.mode = microide::workspace::OverlayMode::ProjectSearch;

  RenderViewModelBuilder builder(context);
  TextRenderer text_renderer;
  const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 280.0f, 160.0f,
                                    LayoutModeInputs{}, true);
  microide::workspace::OverlaySurfaceViewModel vm;
  builder.BuildOverlaySurfaceInto(vm, layout, layout.editor_area, text_renderer);

  Expect(vm.title == "Project Search", "project search title should be prebuilt");
  Expect(!vm.note.empty() && vm.note_x > 0.0f,
         "an incomplete index should surface the right-aligned note");
  Expect(vm.rows.size() == 1 && vm.rows[0].primary == "src/a.cpp:2:5  hello world",
         "the result row label (path:line:col + preview) should be composed in the builder");
  Expect(vm.summary_line == "1 / 1 results", "the selection summary should be composed");

  // Completion popup: caret-anchored, label+detail joined, error in deleted tint.
  auto& completion = context.current_project_state.overlay.workflow.completion;
  completion.items.push_back(microide::workspace::CompletionSessionItem{
      .label = "push_back",
      .detail = "void(T&&)",
  });
  completion.error = "server offline";
  context.current_project_state.overlay.mode = microide::workspace::OverlayMode::Completion;
  builder.BuildOverlaySurfaceInto(vm, layout, layout.editor_area, text_renderer);
  Expect(vm.caret_anchored, "completion popup renders caret-anchored");
  Expect(vm.rows.size() == 1 && vm.rows[0].primary == "push_back  void(T&&)",
         "completion label+detail should be joined in the builder");
  Expect(vm.error_line == "server offline" && vm.error_at_title_row,
         "the completion error draws at the title row in the deleted tint");
}

void TestBuildOverlaySurfaceFindWidgetSubmodel() {
  WorkspaceContext context;
  auto& buffer_search = context.current_project_state.overlay.workflow.buffer_search;
  buffer_search.query.SetText("needle");
  buffer_search.regex = true;
  buffer_search.whole_word = true;
  buffer_search.matches.push_back(microide::editor::SelectionRange{});
  buffer_search.matches.push_back(microide::editor::SelectionRange{});
  buffer_search.selected_index = 1;
  context.current_project_state.overlay.visible = true;
  context.current_project_state.overlay.mode = microide::workspace::OverlayMode::BufferReplace;

  RenderViewModelBuilder builder(context);
  TextRenderer text_renderer;
  const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 280.0f, 160.0f,
                                    LayoutModeInputs{}, true);
  microide::workspace::OverlaySurfaceViewModel vm;
  builder.BuildOverlaySurfaceInto(vm, layout, layout.editor_area, text_renderer);

  const auto& fw = vm.find_widget;
  Expect(fw.replace_mode, "BufferReplace builds the replace-mode widget");
  // Aa / ab / .*, the same order and first two glyphs as the terminal find bar.
  Expect(fw.toggles[0].label == "Aa" && !fw.toggles[0].active,
         "the in-file widget's first toggle should be match case");
  Expect(fw.toggles[1].label == "ab" && fw.toggles[1].active,
         "the in-file widget's second toggle should be whole word");
  Expect(fw.toggles[2].label == ".*" && fw.toggles[2].active,
         "the in-file widget's third toggle should be regex mode");
  Expect(fw.fw.toggle_count == microide::workspace::kBufferFindToggleCount,
         "the in-file widget should lay out one slot per toggle");
  Expect(fw.has_matches && fw.has_query, "widget flags should be forwarded");
  Expect(fw.count_text == "2/2", "the selected/total counter should be composed");
  Expect(fw.search_display_text == "needle",
         "the unfocused search field shows the raw query (zero-copy view)");
  Expect(fw.search_display_text.data() == buffer_search.query.text().data(),
         "the unfocused field text must be a view into state, not a copy");
  Expect(fw.fw.widget.w > 0.0f, "the widget geometry should be computed in the builder");
}

// The debug pane view model forwards narrow per-mode model pointers, never the
// broad project state (TD-2026-07-16-26 family).
void TestBuildDebugPaneSurfaceWiresNarrowModelPointers() {
  WorkspaceContext context;
  context.current_project_state.debug_pane.visible = true;
  context.current_project_state.debug_pane.mode = microide::workspace::DebugPaneMode::Variables;
  RenderViewModelBuilder builder(context);
  const auto vm = builder.BuildDebugPaneSurface();
  Expect(vm.variables == &context.current_project_state.debug_variables,
         "Variables mode wires the variables model");
  Expect(vm.execution == nullptr && vm.watch == nullptr && vm.breakpoints == nullptr,
         "inactive modes stay null");

  context.current_project_state.debug_pane.mode = microide::workspace::DebugPaneMode::CallStack;
  const auto stack_vm = builder.BuildDebugPaneSurface();
  Expect(stack_vm.execution == &context.current_project_state.debug_execution,
         "CallStack mode wires the execution view");
  Expect(stack_vm.variables == nullptr, "the variables model is unwired outside its mode");
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

// The search sidebar's status line used to append a five-segment key cheat-sheet
// ("26 matches | / query | = replace | r rerun | R replace all | c count all").
// At the default 288px sidebar that is roughly twice the available width, so the
// line always rendered cut mid-word and the count -- the part carrying
// information -- was the only thing that survived. The keys now live in
// Help/About beside the git sidebar's. Pin every state to the real width budget.
void TestProjectSearchSidebarStatusFitsSidebarWidth() {
  using microide::workspace::ProjectSearchEditField;
  using microide::workspace::kWorkspaceDefaultSidebarWidth;

  WorkspaceContext context;
  RenderViewModelBuilder builder(context);
  TextRenderer text_renderer;
  context.current_project_state.sidebar.view_id = "search";
  auto& search = context.current_project_state.overlay.workflow.project_search;

  // Sidebar text column: card width less the inset the search panel draws at on
  // both sides (WorkspaceShellRenderSidebar's kSidebarInset).
  constexpr float kSidebarInset = 10.0f;
  const float text_width = kWorkspaceDefaultSidebarWidth - kSidebarInset * 2.0f;

  const auto status_for = [&]() {
    return builder.BuildSidebarSurface().project_search_status_text;
  };
  const auto expect_fits = [&](std::string_view what) {
    const std::string_view status = status_for();
    Expect(text_renderer.MeasureWidth(status) <= text_width,
           what.data());
    Expect(status.find(" rerun") == std::string_view::npos &&
               status.find("count all") == std::string_view::npos &&
               status.find("replace all") == std::string_view::npos,
           "the search status line must not carry the key cheat-sheet any more");
  };

  expect_fits("the idle search status must fit the default sidebar width");

  search.query.SetText("task");
  search.results.assign(26, microide::project::ProjectSearchResult{});
  expect_fits("a match-count status must fit the default sidebar width");

  search.truncated = true;
  search.total_matches = 4096;
  expect_fits("a capped-result status must fit the default sidebar width");

  search.truncated = false;
  search.results.clear();
  expect_fits("a no-matches status must fit the default sidebar width");

  search.editing = true;
  search.edit_field = ProjectSearchEditField::Query;
  expect_fits("the editing hint must fit the default sidebar width");
  search.edit_field = ProjectSearchEditField::Replace;
  expect_fits("the replace-editing hint must fit the default sidebar width");

  search.editing = false;
  search.error = "regex compile failed at offset 12";
  expect_fits("a search-error status must fit the default sidebar width");
}

// Narrow-rail empty states ("No breakpoints — click the editor gutter to add
// one.") are written to be actionable, which is about twice what a ~270px
// sidebar or debug pane fits. They used to be drawn flat -- clipped at the panel
// edge in the debug pane, single-line-truncated in the sidebar -- so the half
// that told the user what to do was exactly the half that disappeared.
void TestWrappedPlaceholderWrapsInsteadOfTruncating() {
  using microide::workspace::detail::ForEachWrappedLabelLine;

  TextRenderer text_renderer;
  constexpr std::string_view kHint = "No breakpoints — click the editor gutter to add one.";
  const float rail_width = 268.0f;  // 288px rail less the 10px inset on each side

  std::vector<std::string> lines;
  const auto collect = [&](std::size_t, std::string_view line) { lines.emplace_back(line); };

  const std::size_t count =
      ForEachWrappedLabelLine(text_renderer, kHint, rail_width, 3, collect);
  Expect(count == lines.size(), "the emit count should match the lines emitted");
  Expect(lines.size() > 1, "a hint wider than the rail should wrap rather than truncate");
  for (const std::string& line : lines) {
    Expect(text_renderer.MeasureWidth(line) <= rail_width,
           "every wrapped line must fit the rail width");
  }
  // The actionable tail survives, which is the whole point.
  Expect(lines.back().find("add one.") != std::string::npos,
         "wrapping must keep the tail of the hint that says what to do");

  // Degenerate inputs stay safe.
  Expect(ForEachWrappedLabelLine(text_renderer, "", rail_width, 3, collect) == 0,
         "empty text emits no lines");
  Expect(ForEachWrappedLabelLine(text_renderer, kHint, 0.0f, 3, collect) == 0,
         "a zero-width rail emits no lines");
  Expect(ForEachWrappedLabelLine(text_renderer, kHint, rail_width, 0, collect) == 0,
         "a zero line budget emits no lines");

  // A one-line budget falls back to ellipsized truncation rather than overflowing.
  lines.clear();
  Expect(ForEachWrappedLabelLine(text_renderer, kHint, rail_width, 1, collect) == 1,
         "a one-line budget emits exactly one line");
  Expect(text_renderer.MeasureWidth(lines.front()) <= rail_width,
         "the single line must still fit the rail width");
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

// The language server's documentHighlight answer replaces the built-in word scan
// while it is valid for the caret. The point of the feature is that it is NOT a
// spelling match: `value` in an unrelated scope must stay unpainted, which no
// textual scan can express.
void TestBuilderPrefersSemanticOccurrencesOverWordScan() {
  WorkspaceContext ctx;
  RenderViewModelBuilder builder(ctx);

  microide::editor::TextViewport viewport;
  viewport.LoadContent("value = 1\nother = value\nvalue = 2\n", "/proj/main.py");
  viewport.SetViewportSize(8, 80);
  viewport.MoveCursorTo(0, 2);  // inside the first `value`

  const auto build = [&](microide::editor::EditorViewModel& vm) {
    builder.BuildEditorViewModelInto(vm, viewport, 8, nullptr,
                                     /*occurrences_highlight_enabled=*/true,
                                     /*occurrences_case_sensitive=*/true,
                                     /*sticky_scroll_enabled=*/false, /*sticky_max_depth=*/3,
                                     /*render_whitespace_enabled=*/false);
  };

  // Baseline: with no semantic set the word scan paints every spelling match.
  microide::editor::EditorViewModel textual;
  build(textual);
  Expect(textual.occurrence_ranges.size() == 3,
         "the word scan paints all three spellings of `value`");

  // A server answer that deliberately omits line 2 (a different symbol that happens
  // to share the name) and marks line 0 as a write.
  auto& semantic = ctx.current_project_state.semantic_occurrences;
  semantic.path = viewport.path();
  semantic.content_revision = viewport.content_revision();
  semantic.ranges = {
      microide::editor::OccurrenceRange{.line_index = 0,
                                        .start_column = 0,
                                        .end_column = 5,
                                        .is_primary_seed = true,
                                        .kind = microide::editor::OccurrenceKind::Write},
      microide::editor::OccurrenceRange{.line_index = 1,
                                        .start_column = 8,
                                        .end_column = 13,
                                        .kind = microide::editor::OccurrenceKind::Read},
  };

  microide::editor::EditorViewModel vm;
  build(vm);
  Expect(vm.occurrence_ranges.size() == 2,
         "the semantic set replaces the word scan rather than merging with it");
  Expect(vm.occurrence_ranges[0].kind == microide::editor::OccurrenceKind::Write &&
             vm.occurrence_ranges[1].kind == microide::editor::OccurrenceKind::Read,
         "read/write kinds survive into the view model");

  // Move the caret off every highlighted range: the set no longer describes what the
  // caret points at, so the word scan takes back over instead of painting a stale
  // symbol's uses.
  viewport.MoveCursorTo(1, 2);  // inside `other`
  microide::editor::EditorViewModel off_symbol;
  build(off_symbol);
  Expect(off_symbol.occurrence_ranges.size() != 2 || off_symbol.occurrence_ranges.empty() ||
             off_symbol.occurrence_ranges[0].line_index != 0 ||
             off_symbol.occurrence_ranges[0].end_column != 5,
         "a caret outside every semantic range falls back off the stale set");

  // An edit invalidates the absolute positions, so the stale set must not be used
  // even with the caret back on the original symbol.
  viewport.MoveCursorTo(0, 2);
  viewport.InsertText("x");
  viewport.MoveCursorTo(0, 2);
  microide::editor::EditorViewModel after_edit;
  build(after_edit);
  Expect(ctx.current_project_state.semantic_occurrences.content_revision !=
             viewport.content_revision(),
         "the edit moved the buffer past the stored set's revision");
  for (const auto& range : after_edit.occurrence_ranges) {
    Expect(range.kind == microide::editor::OccurrenceKind::Text,
           "after an edit the ranges come from the word scan, which only emits Text");
  }
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

// TD-2026-07-17A-007: the render TU consumes precomputed control fields — the
// truncated/placeholder value string and the caret offset — instead of building
// "(default)" and truncating/measuring per paint.
void TestSettingsOverlayControlValueIsPrecomputed() {
  WorkspaceContext context;
  RenderViewModelBuilder builder(context);
  TextRenderer text_renderer;
  const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 280.0f, 160.0f,
                                    LayoutModeInputs{}, true);

  SettingsOverlayService service;
  service.OpenSettings();

  SettingsOverlayRow empty_text_row;
  empty_text_row.id = "editor.font_family";
  empty_text_row.label = "Font Family";
  empty_text_row.control_kind = microide::workspace::SettingsControlKind::TextEdit;
  empty_text_row.editable = true;

  SettingsOverlayRow segmented_row;
  segmented_row.id = "editor.line_endings";
  segmented_row.label = "Line Endings";
  segmented_row.value = "lf";
  segmented_row.value_display = "LF";
  segmented_row.control_kind = microide::workspace::SettingsControlKind::Segmented;

  service.RebuildSettingsRows({}, {}, {}, {empty_text_row, segmented_row});
  service.SetSelectedCategory(0);

  const auto vm = builder.BuildSettingsOverlay(layout, service, text_renderer);
  const SettingsRowViewModel* text_vm = nullptr;
  const SettingsRowViewModel* seg_vm = nullptr;
  for (const SettingsRowViewModel& row : vm.rows) {
    if (row.id == "editor.font_family") {
      text_vm = &row;
    } else if (row.id == "editor.line_endings") {
      seg_vm = &row;
    }
  }
  Expect(text_vm != nullptr && seg_vm != nullptr, "both settings rows should be built");
  Expect(text_vm->control.value_is_placeholder,
         "an empty non-editing TextEdit reports the default placeholder");
  Expect(text_vm->control.shown_value == "(default)",
         "the placeholder string is precomputed in the view model");
  Expect(!seg_vm->control.value_is_placeholder,
         "a segmented control with a value is not a placeholder");
  Expect(seg_vm->control.shown_value == "LF",
         "the segmented value is precomputed (fits, so untruncated)");
}

// The left-rail category list can hold more sections than fit the pane height (this
// is what previously clipped the last-derived "LSP" category off-screen). The builder
// must expose a category scroll model — a positive max scroll, a left-rail scrollbar,
// and a scroll offset that shifts off-top categories above the pane so the render pass
// skips them — so the tail categories become reachable.
void TestSettingsOverlayCategoryRailScrolls() {
  WorkspaceContext context;
  RenderViewModelBuilder builder(context);
  TextRenderer text_renderer;
  const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 280.0f, 160.0f,
                                    LayoutModeInputs{}, true);

  SettingsOverlayService service;
  service.OpenSettings();
  // Inject far more distinct top-level groups than the rail can show at once.
  std::vector<SettingsOverlayRow> rows;
  constexpr int kCategoryCount = 40;
  for (int i = 0; i < kCategoryCount; ++i) {
    SettingsOverlayRow row;
    const std::string suffix = i < 10 ? "0" + std::to_string(i) : std::to_string(i);
    row.id = "cat" + suffix + ".toggle";
    row.label = "Toggle " + suffix;
    row.group = "Cat" + suffix;  // each distinct group becomes its own category
    rows.push_back(row);
  }
  service.RebuildSettingsRows({}, {}, {}, rows);
  service.SetSelectedCategory(0);

  const auto unscrolled = builder.BuildSettingsOverlay(layout, service, text_renderer);
  Expect(static_cast<int>(unscrolled.categories.size()) == kCategoryCount,
         "every injected group should derive its own category");
  Expect(unscrolled.category_max_scroll > 0,
         "more categories than fit the rail should produce a positive max scroll");
  Expect(unscrolled.category_visible_rows < kCategoryCount,
         "not every category fits the rail at once in this fixture");
  Expect(unscrolled.category_scrollbar.has_value(),
         "an overflowing category rail should expose a scrollbar");
  Expect(unscrolled.category_scroll_row == 0,
         "the rail starts unscrolled");
  // The last category sits below the pane bottom before scrolling.
  const float pane_bottom = unscrolled.left_pane_rect.y + unscrolled.left_pane_rect.h;
  Expect(unscrolled.categories.back().rect.y >= pane_bottom - 0.5f,
         "the tail category is off-screen before scrolling (the old clip bug)");

  // Scroll to the bottom: the last category must now land inside the pane, and the
  // first must shift above the pane top (render skips negatives).
  service.SetCategoryScrollRow(unscrolled.category_max_scroll);
  const auto scrolled = builder.BuildSettingsOverlay(layout, service, text_renderer);
  Expect(scrolled.category_scroll_row == unscrolled.category_max_scroll,
         "the builder honors the requested category scroll");
  const float scrolled_pane_bottom = scrolled.left_pane_rect.y + scrolled.left_pane_rect.h;
  const microide::workspace::SettingsCategoryViewModel& last = scrolled.categories.back();
  Expect(last.rect.y >= scrolled.left_pane_rect.y - 0.5f &&
             last.rect.y + last.rect.h <= scrolled_pane_bottom + 0.5f,
         "the tail category becomes fully visible after scrolling to the bottom");
  Expect(scrolled.categories.front().rect.y < scrolled.left_pane_rect.y - 0.5f,
         "the first category scrolls above the pane top");
}

// Every section renders a fixed header band (title + subtitle), and multi-subsection
// categories get a sub-header on the first row of each subsection so the flat row list
// is visually grouped (VSCode-style). The master row (bare group) has no sub-header.
void TestSettingsOverlaySectionHeaderAndSubsections() {
  WorkspaceContext context;
  RenderViewModelBuilder builder(context);
  TextRenderer text_renderer;
  const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 280.0f, 160.0f,
                                    LayoutModeInputs{}, true);

  SettingsOverlayService service;
  service.OpenSettings();
  std::vector<SettingsOverlayRow> rows;
  SettingsOverlayRow master;
  master.id = "lsp.enabled";
  master.label = "Enable Language Server";
  master.group = "LSP";  // bare top-level group -> no sub-header
  rows.push_back(master);
  SettingsOverlayRow feat_a;
  feat_a.id = "lsp.hover.enabled";
  feat_a.label = "Hover";
  feat_a.group = "LSP → Features";
  rows.push_back(feat_a);
  SettingsOverlayRow feat_b;
  feat_b.id = "lsp.completion.enabled";
  feat_b.label = "Completion";
  feat_b.group = "LSP → Features";
  rows.push_back(feat_b);
  service.RebuildSettingsRows({}, {}, {}, rows);

  // Select the derived LSP category.
  const auto& categories = service.Categories();
  const auto lsp_it = std::find(categories.begin(), categories.end(), "LSP");
  Expect(lsp_it != categories.end(), "the LSP group should derive an LSP category");
  service.SetSelectedCategory(static_cast<int>(std::distance(categories.begin(), lsp_it)));

  const auto vm = builder.BuildSettingsOverlay(layout, service, text_renderer);
  Expect(vm.section_title == "LSP", "the header band shows the selected category title");
  Expect(!vm.section_subtitle.empty(), "known sections carry a one-line subtitle");
  Expect(vm.section_header_rect.h > 0.0f && vm.right_pane_rect.y >= vm.section_header_rect.y +
                                                                        vm.section_header_rect.h -
                                                                        0.5f,
         "value rows start below the fixed header band");

  const SettingsRowViewModel* master_vm = nullptr;
  const SettingsRowViewModel* hover_vm = nullptr;
  const SettingsRowViewModel* completion_vm = nullptr;
  for (const SettingsRowViewModel& row : vm.rows) {
    if (row.id == "lsp.enabled") master_vm = &row;
    else if (row.id == "lsp.hover.enabled") hover_vm = &row;
    else if (row.id == "lsp.completion.enabled") completion_vm = &row;
  }
  Expect(master_vm != nullptr && hover_vm != nullptr && completion_vm != nullptr,
         "all injected LSP rows should be built");
  Expect(master_vm->group_subheader.empty(),
         "the bare-group master row gets no subsection sub-header");
  Expect(hover_vm->group_subheader == "Features",
         "the first row of the Features subsection carries its sub-header");
  Expect(completion_vm->group_subheader.empty(),
         "later rows of the same subsection do not repeat the sub-header");
  // The sub-header reserves a strip above the row, so the first Features row sits lower
  // than a same-height master row would without one.
  Expect(hover_vm->row_rect.y > master_vm->row_rect.y + master_vm->row_rect.h - 0.5f,
         "the Features sub-header pushes its first row down below the master row");
}

// Query filtering matches label and id/detail case-insensitively, with no per-row
// allocation churn (TD-2026-07-17A-006): the row filter now routes through the
// allocation-free util::ContainsCaseInsensitiveAscii instead of lowering the query,
// label, and detail into fresh strings for every row on every keystroke.
void TestSettingsOverlayQueryFilterIsCaseInsensitive() {
  SettingsOverlayService service;
  service.OpenSettings();

  std::vector<SettingsOverlayRow> rows;
  SettingsOverlayRow theme;
  theme.id = "editor.colorScheme";
  theme.label = "Color Theme";
  rows.push_back(theme);
  SettingsOverlayRow tabs;
  tabs.id = "editor.tabSize";
  tabs.label = "Tab Width";
  rows.push_back(tabs);

  // Uppercase query matches a mixed-case LABEL ("THEME" ⊂ "Color Theme").
  service.SetQuery("THEME");
  service.RebuildSettingsRows({}, {}, {}, rows);
  Expect(service.VisibleRowCount() == 1, "uppercase query should match the mixed-case label");

  // Query matches only the row ID/detail, case-insensitively ("tabsize" ⊂ "editor.tabSize").
  service.SetQuery("tabsize");
  service.RebuildSettingsRows({}, {}, {}, rows);
  Expect(service.VisibleRowCount() == 1, "lowercase query should match the camelCase row id");

  // Empty query keeps every row; a non-matching query hides them all.
  service.SetQuery("");
  service.RebuildSettingsRows({}, {}, {}, rows);
  Expect(service.VisibleRowCount() == 2, "empty query should keep every row");
  service.SetQuery("zzz-nomatch");
  service.RebuildSettingsRows({}, {}, {}, rows);
  Expect(service.VisibleRowCount() == 0, "a non-matching query should hide every row");
}

// RowAtVisibleIndex / RowCountInCategory resolve rows through per-category index vectors
// built once per RebuildSettingsRows, so the render loop that walks a category row-by-row
// is O(rows) not O(rows^2) (TD-2026-07-17A-019). The cache must be correct across multiple
// categories AND rebuilt on every re-filter (no stale indices from a prior query).
void TestSettingsOverlayCategoryIndexIsRebuiltPerFilter() {
  SettingsOverlayService service;
  service.OpenSettings();

  std::vector<SettingsOverlayRow> rows;
  SettingsOverlayRow a;
  a.id = "editor.a";
  a.label = "Editor Alpha";
  a.group = "Editor";
  rows.push_back(a);
  SettingsOverlayRow b;
  b.id = "editor.b";
  b.label = "Editor Beta";
  b.group = "Editor";
  rows.push_back(b);
  SettingsOverlayRow c;
  c.id = "lsp.c";
  c.label = "Lsp Gamma";
  c.group = "LSP";
  rows.push_back(c);

  service.SetQuery("");
  service.RebuildSettingsRows({}, {}, {}, rows);
  // Categories are first-seen order over the filtered rows: Editor (2 rows), LSP (1 row).
  Expect(service.RowCountInCategory(0) == 2, "the first category should hold both Editor rows");
  Expect(service.RowCountInCategory(1) == 1, "the second category should hold the single LSP row");
  const SettingsOverlayRow* r00 = service.RowAtVisibleIndex(0, 0);
  const SettingsOverlayRow* r01 = service.RowAtVisibleIndex(0, 1);
  const SettingsOverlayRow* r10 = service.RowAtVisibleIndex(1, 0);
  Expect(r00 != nullptr && r00->id == "editor.a", "category 0 row 0 is editor.a");
  Expect(r01 != nullptr && r01->id == "editor.b", "category 0 row 1 is editor.b");
  Expect(r10 != nullptr && r10->id == "lsp.c", "category 1 row 0 is lsp.c");
  Expect(service.RowAtVisibleIndex(0, 2) == nullptr, "out-of-range row index returns null");
  Expect(service.RowAtVisibleIndex(2, 0) == nullptr, "out-of-range category returns null");

  // Re-filter to only the LSP row: the index cache must rebuild, not report the stale
  // two-category layout.
  service.SetQuery("lsp.c");
  service.RebuildSettingsRows({}, {}, {}, rows);
  Expect(service.RowCountInCategory(0) == 1, "after filtering, category 0 holds only the LSP row");
  Expect(service.RowCountInCategory(1) == 0, "the second category no longer exists after filtering");
  const SettingsOverlayRow* filtered = service.RowAtVisibleIndex(0, 0);
  Expect(filtered != nullptr && filtered->id == "lsp.c",
         "the rebuilt index resolves the surviving LSP row");
}

void RegisterRenderViewModelBuilderTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RenderViewModelBuilder/SettingsOverlayCategoryIndexIsRebuiltPerFilter",
          TestSettingsOverlayCategoryIndexIsRebuiltPerFilter);
  AddTest(tests, "RenderViewModelBuilder/SettingsOverlayQueryFilterIsCaseInsensitive",
          TestSettingsOverlayQueryFilterIsCaseInsensitive);
  AddTest(tests, "RenderViewModelBuilder/SettingsOverlayCategoryRailScrolls",
          TestSettingsOverlayCategoryRailScrolls);
  AddTest(tests, "RenderViewModelBuilder/SettingsOverlaySectionHeaderAndSubsections",
          TestSettingsOverlaySectionHeaderAndSubsections);
  AddTest(tests, "RenderViewModelBuilder/SettingsOverlayFontPickerBuildsScrollbarOnOverflow",
          TestSettingsOverlayFontPickerBuildsScrollbarOnOverflow);
  AddTest(tests, "RenderViewModelBuilder/SettingsOverlayControlValueIsPrecomputed",
          TestSettingsOverlayControlValueIsPrecomputed);
  AddTest(tests, "RenderViewModelBuilder/SettingsOverlayWrapsLongDescriptions",
          TestSettingsOverlayWrapsLongDescriptions);
  AddTest(tests, "RenderViewModelBuilder/PrefersSemanticOccurrencesOverWordScan",
          TestBuilderPrefersSemanticOccurrencesOverWordScan);
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
  AddTest(tests, "RenderViewModelBuilder/OverlayOwnsPickerRowsAndChrome",
          TestBuildOverlaySurfaceOwnsPickerRowsAndChrome);
  AddTest(tests, "RenderViewModelBuilder/OverlayComposesProjectSearchAndCompletion",
          TestBuildOverlaySurfaceComposesProjectSearchAndCompletion);
  AddTest(tests, "RenderViewModelBuilder/OverlayFindWidgetSubmodel",
          TestBuildOverlaySurfaceFindWidgetSubmodel);
  AddTest(tests, "RenderViewModelBuilder/DebugPaneWiresNarrowModelPointers",
          TestBuildDebugPaneSurfaceWiresNarrowModelPointers);
  AddTest(tests, "RenderViewModelBuilder/SidebarFallbacksAreViewsIntoStableStorage",
          TestBuildSidebarSurfaceFallbacksAreViewsIntoStableStorage);
  AddTest(tests, "RenderViewModelBuilder/ProjectSearchSidebarStatusFitsSidebarWidth",
          TestProjectSearchSidebarStatusFitsSidebarWidth);
  AddTest(tests, "RenderViewModelBuilder/WrappedPlaceholderWrapsInsteadOfTruncating",
          TestWrappedPlaceholderWrapsInsteadOfTruncating);
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
