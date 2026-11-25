#include "TestSupport.h"

#include "editor/BreakpointRender.h"
#include "editor/ExecutionLineRender.h"
#include "editor/GutterMetrics.h"
#include "workspace/debug/DebugPaneMouseCoordinator.h"
#include "workspace/debug/DebugPaneRegistry.h"
#include "workspace/debug/DebugPaneService.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/state/WorkspaceProjectState.h"

#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::BuiltinDebugPaneSurfaceSpecs;
using microide::workspace::Contains;
using microide::workspace::DebugBreakpointRowView;
using microide::workspace::DebugPaneMode;
using microide::workspace::DebugPaneModeRowLayout;
using microide::workspace::DebugPaneMouseCoordinator;
using microide::workspace::DebugPaneRowAtPoint;
using microide::workspace::DebugPaneRowHit;
using microide::workspace::DebugPaneService;
using microide::workspace::FindDebugPaneSurface;
using microide::workspace::MakeRect;
using microide::workspace::ProjectWorkspaceState;
using microide::workspace::WorkspaceLayout;
using microide::workspace::WorkspaceShell;

DebugPaneService MakeService(ProjectWorkspaceState& state) {
  return DebugPaneService(state, DebugPaneService::Operations{
                                     .request_frame = []() {},
                                     .note_layout_inputs_changed = []() {},
                                 });
}

void TestDebugPaneRegistry() {
  const auto specs = BuiltinDebugPaneSurfaceSpecs();
  Expect(specs.size() == 4, "the debug pane has four built-in surfaces");
  Expect(specs[0].mode == DebugPaneMode::Variables, "Variables is the first tab");
  Expect(specs[1].mode == DebugPaneMode::Breakpoints, "Breakpoints is the second tab");
  Expect(specs[2].mode == DebugPaneMode::Watch, "Watch is the third tab");
  Expect(specs[3].mode == DebugPaneMode::CallStack, "Call Stack is the last tab");
  Expect(ProjectWorkspaceState{}.debug_pane.mode == DebugPaneMode::Variables,
         "the pane defaults to the Variables surface");
  const auto* watch = FindDebugPaneSurface("watch");
  Expect(watch != nullptr && watch->mode == DebugPaneMode::Watch,
         "FindDebugPaneSurface resolves by id");
  const auto* bp = FindDebugPaneSurface(DebugPaneMode::Breakpoints);
  Expect(bp != nullptr && bp->id == "breakpoints", "FindDebugPaneSurface resolves by mode");
  Expect(FindDebugPaneSurface("nope") == nullptr, "unknown id resolves to nullptr");
}

void TestDebugPaneServiceShowAndToggle() {
  ProjectWorkspaceState state;
  DebugPaneService service = MakeService(state);

  Expect(!state.debug_pane.visible, "pane starts hidden");

  service.ShowMode(DebugPaneMode::Variables);
  Expect(state.debug_pane.visible && state.debug_pane.mode == DebugPaneMode::Variables,
         "ShowMode makes the pane visible on the requested surface");
  Expect(state.surface.focus == microide::workspace::FocusTarget::DebugPane,
         "showing the pane moves focus to it");

  service.Toggle();
  Expect(!state.debug_pane.visible, "Toggle hides a visible pane");
  Expect(state.surface.focus == microide::workspace::FocusTarget::Editor,
         "hiding the pane returns focus to the editor");

  service.Toggle();
  Expect(state.debug_pane.visible && state.debug_pane.mode == DebugPaneMode::Variables,
         "Toggle re-shows the pane on its last surface");
}

void TestDebugPaneServiceOpenOnStop() {
  ProjectWorkspaceState state;
  DebugPaneService service = MakeService(state);

  service.OpenOnStop();
  Expect(state.debug_pane.visible && state.debug_pane.mode == DebugPaneMode::Variables,
         "the first stop opens the pane on the Variables inspector");

  service.ShowMode(DebugPaneMode::CallStack);
  service.OpenOnStop();
  Expect(state.debug_pane.mode == DebugPaneMode::CallStack,
         "a stop while the pane is already open preserves the active surface");
}

void TestDebugPaneServiceClose() {
  ProjectWorkspaceState state;
  DebugPaneService service = MakeService(state);
  service.ShowMode(DebugPaneMode::CallStack);
  service.Close();
  Expect(!state.debug_pane.visible, "Close hides the pane (session teardown)");
}

void TestDebugPaneRowAtPointMapsCenters() {
  const SDL_FRect content = MakeRect(0.0f, 30.0f, 200.0f, 320.0f);
  const float text_y = 38.0f;
  const float line_height = 16.0f;
  const int visible_rows = 20;
  const std::size_t line_count = 10;
  for (int k = 0; k < static_cast<int>(line_count); ++k) {
    const float y = text_y + static_cast<float>(k) * line_height + line_height * 0.5f;
    const DebugPaneRowHit hit =
        DebugPaneRowAtPoint(content, text_y, line_height, visible_rows, 0, line_count,
                            100.0f, y);
    Expect(hit.in_content, "row center is inside the content rect");
    Expect(hit.row_index == k, "row center maps to its own absolute row");
  }
}

void TestDebugPaneRowAtPointTopBandIsRowZero() {
  const SDL_FRect content = MakeRect(0.0f, 30.0f, 200.0f, 320.0f);
  const DebugPaneRowHit hit =
      DebugPaneRowAtPoint(content, 38.0f, 16.0f, 20,
                          0, 10, 100.0f, 31.0f);
  Expect(hit.in_content && hit.row_index == 0, "the top inset hits row 0, not nothing");
}

void TestDebugPaneRowAtPointMisses() {
  const SDL_FRect content = MakeRect(0.0f, 30.0f, 200.0f, 320.0f);
  const DebugPaneRowHit below =
      DebugPaneRowAtPoint(content, 38.0f, 16.0f, 20,
                          0, 3, 100.0f, 200.0f);
  Expect(below.in_content && below.row_index == -1, "below the last row: in content, no row");
  const DebugPaneRowHit outside =
      DebugPaneRowAtPoint(content, 38.0f, 16.0f, 20,
                          0, 10, 100.0f, 10.0f);
  Expect(!outside.in_content && outside.row_index == -1, "above the content: no hit at all");
}

void TestDebugPaneRowAtPointHonorsScroll() {
  const SDL_FRect content = MakeRect(0.0f, 30.0f, 200.0f, 320.0f);
  const DebugPaneRowHit hit =
      DebugPaneRowAtPoint(content, 38.0f, 16.0f, 20,
                          3, 20, 100.0f, 46.0f);
  Expect(hit.row_index == 3, "first visible band maps to the scrolled-to absolute row");
}

void TestDebugPaneClickFrameNavigates() {
  ProjectWorkspaceState state;
  state.debug_pane.visible = true;
  state.debug_pane.mode = DebugPaneMode::CallStack;
  state.debug_execution.stopped = true;
  microide::workspace::DebugStackFrameView frame;
  frame.id = 7;
  frame.SetSource("main.cpp");
  frame.line = 5;
  state.debug_execution.frames.push_back(frame);

  std::string opened;
  int focused_frame = -1;
  WorkspaceShell::LogSurfaceLayout panel_layout;
  panel_layout.content_rect = MakeRect(0.0f, 30.0f, 200.0f, 370.0f);
  panel_layout.text_x = 12.0f;
  panel_layout.text_y = 38.0f;
  panel_layout.line_height = 16.0f;
  panel_layout.scroll.visible_rows = 20;
  panel_layout.scroll.vertical_scroll = 0;

  microide::workspace::InteractionState interaction;
  DebugPaneMouseCoordinator coordinator(
      state, interaction,
      DebugPaneMouseCoordinator::Operations{
          .compute_debug_pane_list_layout =
              [&](const WorkspaceLayout&, std::size_t) { return panel_layout; },
          .debug_pane_mode_row = [](const SDL_FRect&) { return DebugPaneModeRowLayout{}; },
          .debug_pane_active_row_count = [&]() { return state.debug_execution.PanelRowCount(); },
          .open_file = [&](const std::filesystem::path& p) { opened = p.string(); },
          .active_editor_viewport = []() -> microide::editor::TextViewport* { return nullptr; },
          .on_debug_frame_focus_changed = [&](int id) { focused_frame = id; },
      });

  WorkspaceLayout layout;
  layout.right_pane = MakeRect(0.0f, 0.0f, 200.0f, 400.0f);
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  event.button.button = SDL_BUTTON_LEFT;
  event.button.x = 100.0f;
  event.button.y = 46.0f;
  event.button.clicks = 1;

  Expect(coordinator.HandleButtonDown(event, layout), "the click is consumed by the pane");
  Expect(opened == "main.cpp", "clicking the frame row opens its source file");
  Expect(focused_frame == 7, "clicking the frame row focuses that DAP frame");
}

void TestDebugPaneScrollbarDrags() {
  ProjectWorkspaceState state;
  state.debug_pane.visible = true;
  state.debug_pane.mode = DebugPaneMode::CallStack;
  state.debug_execution.stopped = true;
  for (int i = 0; i < 200; ++i) {
    microide::workspace::DebugStackFrameView frame;
    frame.id = i;
    frame.SetSource("main.cpp");
    frame.line = static_cast<std::size_t>(i);
    state.debug_execution.frames.push_back(frame);
  }
  const std::size_t row_count = state.debug_execution.PanelRowCount();
  Expect(row_count > 20, "the fixture is tall enough to scroll");

  const SDL_FRect content = MakeRect(0.0f, 30.0f, 200.0f, 320.0f);
  WorkspaceShell::LogSurfaceLayout panel_layout;
  panel_layout.content_rect = content;
  panel_layout.text_x = 12.0f;
  panel_layout.text_y = 38.0f;
  panel_layout.line_height = 16.0f;

  int scroll_row = 0;
  auto rebuild = [&]() {
    panel_layout.scroll = microide::workspace::ComputeScrollSurfaceLayout(content, row_count,
                                                                         20,
                                                                         scroll_row);
  };
  rebuild();
  Expect(panel_layout.scroll.vertical_scrollbar.has_value(), "the pane shows a scrollbar");

  int opened_count = 0;
  microide::workspace::InteractionState interaction;
  DebugPaneMouseCoordinator coordinator(
      state, interaction,
      DebugPaneMouseCoordinator::Operations{
          .compute_debug_pane_list_layout =
              [&](const WorkspaceLayout&, std::size_t) { return panel_layout; },
          .debug_pane_mode_row = [](const SDL_FRect&) { return DebugPaneModeRowLayout{}; },
          .debug_pane_active_row_count = [&]() { return row_count; },
          .set_debug_pane_scroll_row =
              [&](int row, std::size_t, int) {
                scroll_row = row;
                rebuild();
              },
          .open_file = [&](const std::filesystem::path&) { ++opened_count; },
          .active_editor_viewport = []() -> microide::editor::TextViewport* { return nullptr; },
          .on_debug_frame_focus_changed = [](int) {},
      });

  WorkspaceLayout layout;
  layout.right_pane = MakeRect(0.0f, 0.0f, 200.0f, 400.0f);

  const SDL_FRect track = panel_layout.scroll.vertical_scrollbar->track;
  SDL_Event press{};
  press.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  press.button.button = SDL_BUTTON_LEFT;
  press.button.clicks = 1;
  press.button.x = track.x + track.w * 0.5f;
  press.button.y = track.y + track.h * 0.75f;
  Expect(coordinator.HandleButtonDown(press, layout), "the scrollbar press is consumed");
  Expect(interaction.drag_target == microide::workspace::DragTarget::DebugPaneScrollbar,
         "pressing the track arms the pane scrollbar drag");
  Expect(scroll_row > 0, "pressing low on the track jumps the view down");
  Expect(opened_count == 0, "a scrollbar grab never activates the row behind it");

  const int after_press = scroll_row;
  SDL_Event drag{};
  drag.type = SDL_EVENT_MOUSE_MOTION;
  drag.motion.state = SDL_BUTTON_LMASK;
  drag.motion.x = track.x + track.w * 0.5f;
  drag.motion.y = track.y + 1.0f;
  Expect(coordinator.HandleDrag(drag, layout), "the drag is consumed");
  Expect(scroll_row == 0, "dragging the thumb to the top scrolls back to row 0");
  Expect(after_press != scroll_row, "the drag actually moved the view");
  Expect(state.surface.focus == microide::workspace::FocusTarget::DebugPane,
         "grabbing the pane scrollbar focuses the pane");
}

void TestDebugPaneRightClickOpensRowMenus() {
  ProjectWorkspaceState state;
  state.debug_pane.visible = true;
  state.debug_pane.mode = DebugPaneMode::Breakpoints;
  state.breakpoint_store.Toggle("main.cpp", 4);

  WorkspaceShell::LogSurfaceLayout panel_layout;
  panel_layout.content_rect = MakeRect(0.0f, 30.0f, 200.0f, 370.0f);
  panel_layout.text_x = 12.0f;
  panel_layout.text_y = 38.0f;
  panel_layout.line_height = 16.0f;
  panel_layout.scroll.visible_rows = 20;
  panel_layout.scroll.vertical_scroll = 0;

  state.debug_breakpoints_panel.Rebuild(state.breakpoint_store,
                                        state.function_breakpoint_store);
  const std::size_t breakpoint_rows = state.debug_breakpoints_panel.RowCount();
  Expect(breakpoint_rows > 0, "the breakpoints panel fixture should have rows");

  std::string breakpoint_menu_path;
  int value_menus_opened = 0;
  int opened_files = 0;
  microide::workspace::InteractionState interaction;
  DebugPaneMouseCoordinator coordinator(
      state, interaction,
      DebugPaneMouseCoordinator::Operations{
          .compute_debug_pane_list_layout =
              [&](const WorkspaceLayout&, std::size_t) { return panel_layout; },
          .debug_pane_mode_row = [](const SDL_FRect&) { return DebugPaneModeRowLayout{}; },
          .debug_pane_active_row_count =
              [&]() -> std::size_t {
                return state.debug_pane.mode == DebugPaneMode::Breakpoints
                           ? state.debug_breakpoints_panel.RowCount()
                           : state.debug_watch.Rows().size();
              },
          .open_file = [&](const std::filesystem::path&) { ++opened_files; },
          .active_editor_viewport = []() -> microide::editor::TextViewport* { return nullptr; },
          .open_debug_value_context_menu = [&](const SDL_FRect&) { ++value_menus_opened; },
          .open_breakpoint_context_menu =
              [&](const std::filesystem::path& path, std::size_t, const SDL_FRect&) {
                breakpoint_menu_path = path.string();
              },
      });

  WorkspaceLayout layout;
  layout.right_pane = MakeRect(0.0f, 0.0f, 200.0f, 400.0f);
  const auto press_at = [&](float y, Uint8 button) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = button;
    event.button.clicks = 1;
    event.button.x = 100.0f;
    event.button.y = y;
    return coordinator.HandleButtonDown(event, layout);
  };

  const auto& rows = state.debug_breakpoints_panel.Rows();
  std::size_t breakpoint_row = rows.size();
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].kind == DebugBreakpointRowView::Kind::Breakpoint && !rows[i].path.empty()) {
      breakpoint_row = i;
      break;
    }
  }
  Expect(breakpoint_row < rows.size(), "the fixture should contain a line breakpoint row");
  const float breakpoint_y =
      panel_layout.text_y + static_cast<float>(breakpoint_row) * panel_layout.line_height + 8.0f;

  Expect(press_at(breakpoint_y, SDL_BUTTON_RIGHT), "the right-click should be consumed");
  Expect(breakpoint_menu_path.find("main.cpp") != std::string::npos,
         "right-clicking a breakpoint row should open the gutter breakpoint menu for its file");
  Expect(opened_files == 0, "right-clicking a breakpoint row must not navigate the editor");
  Expect(state.surface.focus == microide::workspace::FocusTarget::DebugPane,
         "a right-click in the pane should focus the pane");

  state.debug_pane.mode = DebugPaneMode::Watch;
  state.debug_watch.AddExpression("counter");
  state.debug_watch.AddExpression("total");
  Expect(state.debug_watch.Rows().size() >= 2, "the watch fixture should have two rows");

  Expect(press_at(panel_layout.text_y + panel_layout.line_height + 4.0f, SDL_BUTTON_RIGHT),
         "a right-click on a watch row should be consumed");
  Expect(value_menus_opened == 1, "a value row should open the shared value row menu");
  Expect(state.debug_watch.SelectedRow() == 1,
         "the row under the pointer should be selected before the menu opens");
}

void TestDebugPaneRowAtPointLargeScrolledList() {
  const SDL_FRect content = MakeRect(0.0f, 30.0f, 240.0f, 800.0f);
  const float text_y = 38.0f;
  const float line_height = 16.0f;
  const int visible_rows = 48;
  const std::size_t line_count = 5000;
  for (const int scroll : {0, 1, 100, 2500, 4990}) {
    for (int band = 0; band < visible_rows; ++band) {
      const float y = text_y + static_cast<float>(band) * line_height + line_height * 0.5f;
      const DebugPaneRowHit hit = DebugPaneRowAtPoint(content, text_y, line_height, visible_rows,
                                                      scroll, line_count, 100.0f, y);
      const std::size_t absolute = static_cast<std::size_t>(scroll) + static_cast<std::size_t>(band);
      if (absolute < line_count) {
        Expect(hit.in_content && hit.row_index == static_cast<int>(absolute),
               "a visible band maps to its absolute row at any scroll depth");
      } else {
        Expect(hit.in_content && hit.row_index == -1,
               "a band beyond the last populated row resolves to no row");
      }
    }
  }
}

void TestGutterMarkersClearLineNumbers() {
  using microide::editor::BreakpointGutterMarkerRect;
  using microide::editor::ExecutionLineGutterMarkerRect;
  for (const float gutter_x : {0.0f, 40.0f, 123.5f}) {
    for (const float line_height : {12.0f, 16.0f, 24.0f, 40.0f}) {
      const float digits_left = gutter_x + microide::editor::kGutterLineNumberInset;
      const SDL_FRect bp = BreakpointGutterMarkerRect(gutter_x, 100.0f, 56.0f,
                                                      line_height);
      Expect(bp.x >= gutter_x + microide::editor::kGutterMarkerInset - 0.01f,
             "breakpoint dot starts within the marker strip");
      Expect(bp.x + bp.w <= digits_left + 0.01f,
             "breakpoint dot ends before the line-number digits");
      const SDL_FRect exec = ExecutionLineGutterMarkerRect(gutter_x, 100.0f,
                                                           56.0f, line_height);
      Expect(exec.x + exec.w <= digits_left + 0.01f,
             "execution arrow ends before the line-number digits");
    }
  }
}

}

void RegisterDebugPaneTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DebugPane/Registry", TestDebugPaneRegistry);
  AddTest(tests, "DebugPane/ServiceShowAndToggle", TestDebugPaneServiceShowAndToggle);
  AddTest(tests, "DebugPane/ServiceOpenOnStop", TestDebugPaneServiceOpenOnStop);
  AddTest(tests, "DebugPane/ServiceClose", TestDebugPaneServiceClose);
  AddTest(tests, "DebugPane/RowAtPointMapsCenters", TestDebugPaneRowAtPointMapsCenters);
  AddTest(tests, "DebugPane/RowAtPointTopBandIsRowZero", TestDebugPaneRowAtPointTopBandIsRowZero);
  AddTest(tests, "DebugPane/RowAtPointMisses", TestDebugPaneRowAtPointMisses);
  AddTest(tests, "DebugPane/RowAtPointHonorsScroll", TestDebugPaneRowAtPointHonorsScroll);
  AddTest(tests, "DebugPane/RowAtPointLargeScrolledList", TestDebugPaneRowAtPointLargeScrolledList);
  AddTest(tests, "DebugPane/ClickFrameNavigates", TestDebugPaneClickFrameNavigates);
  AddTest(tests, "DebugPane/ScrollbarDrags", TestDebugPaneScrollbarDrags);
  AddTest(tests, "DebugPane/RightClickOpensRowMenus", TestDebugPaneRightClickOpensRowMenus);
  AddTest(tests, "DebugPane/GutterMarkersClearLineNumbers", TestGutterMarkersClearLineNumbers);
}

}
