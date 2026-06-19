#include "TestSupport.h"

#include "editor/BreakpointRender.h"
#include "editor/ExecutionLineRender.h"
#include "editor/GutterMetrics.h"
#include "workspace/DebugPaneRegistry.h"
#include "workspace/DebugPaneService.h"
#include "workspace/WorkspaceProjectState.h"

#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::BuiltinDebugPaneSurfaceSpecs;
using microide::workspace::DebugPaneMode;
using microide::workspace::DebugPaneService;
using microide::workspace::FindDebugPaneSurface;
using microide::workspace::ProjectWorkspaceState;

DebugPaneService MakeService(ProjectWorkspaceState& state) {
  return DebugPaneService(state, DebugPaneService::Operations{
                                     .request_redraw = []() {},
                                     .mark_layout_dirty = []() {},
                                 });
}

void TestDebugPaneRegistry() {
  const auto specs = BuiltinDebugPaneSurfaceSpecs();
  Expect(specs.size() == 4, "the debug pane has four built-in surfaces");
  // Display order is Variables / Breakpoints / Watch / Call Stack — Variables
  // first since it's the most-used surface while stepping.
  Expect(specs[0].mode == DebugPaneMode::Variables, "Variables is the first tab");
  Expect(specs[1].mode == DebugPaneMode::Breakpoints, "Breakpoints is the second tab");
  Expect(specs[2].mode == DebugPaneMode::Watch, "Watch is the third tab");
  Expect(specs[3].mode == DebugPaneMode::CallStack, "Call Stack is the last tab");
  // The default pane surface matches the first tab so the pane opens on Variables.
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

  service.ShowVariables();
  Expect(state.debug_pane.visible && state.debug_pane.mode == DebugPaneMode::Variables,
         "ShowVariables makes the pane visible on the Variables surface");
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

  // The user switches to Call Stack, then a later stop must not yank them back.
  service.ShowCallStack();
  service.OpenOnStop();
  Expect(state.debug_pane.mode == DebugPaneMode::CallStack,
         "a stop while the pane is already open preserves the active surface");
}

void TestDebugPaneServiceClose() {
  ProjectWorkspaceState state;
  DebugPaneService service = MakeService(state);
  service.ShowCallStack();
  service.Close();
  Expect(!state.debug_pane.visible, "Close hides the pane (session teardown)");
}

// Gutter markers must stay within the reserved marker strip and never overlap the
// line-number digits, which begin at gutter_x + kGutterLineNumberInset. (Regression
// for the breakpoint dot / execution arrow drawing over the line numbers.)
void TestGutterMarkersClearLineNumbers() {
  using microide::editor::BreakpointGutterMarkerRect;
  using microide::editor::ExecutionLineGutterMarkerRect;
  for (const float gutter_x : {0.0f, 40.0f, 123.5f}) {
    for (const float line_height : {12.0f, 16.0f, 24.0f, 40.0f}) {
      const float digits_left = gutter_x + microide::editor::kGutterLineNumberInset;
      const SDL_FRect bp = BreakpointGutterMarkerRect(gutter_x, /*y=*/100.0f, /*gutter_width=*/56.0f,
                                                      line_height);
      Expect(bp.x >= gutter_x + microide::editor::kGutterMarkerInset - 0.01f,
             "breakpoint dot starts within the marker strip");
      Expect(bp.x + bp.w <= digits_left + 0.01f,
             "breakpoint dot ends before the line-number digits");
      const SDL_FRect exec = ExecutionLineGutterMarkerRect(gutter_x, /*y=*/100.0f,
                                                           /*gutter_width=*/56.0f, line_height);
      Expect(exec.x + exec.w <= digits_left + 0.01f,
             "execution arrow ends before the line-number digits");
    }
  }
}

}  // namespace

void RegisterDebugPaneTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DebugPane/Registry", TestDebugPaneRegistry);
  AddTest(tests, "DebugPane/ServiceShowAndToggle", TestDebugPaneServiceShowAndToggle);
  AddTest(tests, "DebugPane/ServiceOpenOnStop", TestDebugPaneServiceOpenOnStop);
  AddTest(tests, "DebugPane/ServiceClose", TestDebugPaneServiceClose);
  AddTest(tests, "DebugPane/GutterMarkersClearLineNumbers", TestGutterMarkersClearLineNumbers);
}

}  // namespace microide::tests
