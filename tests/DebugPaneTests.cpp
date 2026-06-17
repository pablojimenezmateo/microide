#include "TestSupport.h"

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
  Expect(state.debug_pane.visible && state.debug_pane.mode == DebugPaneMode::CallStack,
         "the first stop opens the pane on Call Stack");

  // The user switches to Variables, then a later stop must not yank them back.
  service.ShowVariables();
  service.OpenOnStop();
  Expect(state.debug_pane.mode == DebugPaneMode::Variables,
         "a stop while the pane is already open preserves the active surface");
}

void TestDebugPaneServiceClose() {
  ProjectWorkspaceState state;
  DebugPaneService service = MakeService(state);
  service.ShowCallStack();
  service.Close();
  Expect(!state.debug_pane.visible, "Close hides the pane (session teardown)");
}

}  // namespace

void RegisterDebugPaneTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DebugPane/Registry", TestDebugPaneRegistry);
  AddTest(tests, "DebugPane/ServiceShowAndToggle", TestDebugPaneServiceShowAndToggle);
  AddTest(tests, "DebugPane/ServiceOpenOnStop", TestDebugPaneServiceOpenOnStop);
  AddTest(tests, "DebugPane/ServiceClose", TestDebugPaneServiceClose);
}

}  // namespace microide::tests
