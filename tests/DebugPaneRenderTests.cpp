#include "TestSupport.h"

#include "support/SoftwareCanvas.h"

#include "workspace/debug/DapProtocol.h"
#include "workspace/debug/DebugVariablesModel.h"
#include "workspace/debug/DebugViewModel.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <string>
#include <vector>

// The debug pane's PAINT path. DebugPaneTests covers the registry, the service,
// row hit-testing, scroll clamping and the mouse coordinator — all of which run
// without a renderer, which is exactly why DebugPaneRender.cpp sat at 0.56% line
// coverage while the pane itself looked well tested. Nothing here had ever drawn
// a pixel.
//
// Each surface gets painted with content, not empty: an empty Variables tree and
// a populated one take different branches (the twisty, the indent ladder, the
// value column, the "load more" row), and the same is true of every other mode.
// The frames are also painted while scrolled and while a row is selected, since
// the selected-row fill and the clipped-row path are separate branches again.

namespace microide::tests {
namespace {

using microide::workspace::DebugPaneMode;
using microide::workspace::DebugStackFrameView;
using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;
namespace dap = microide::workspace::dap_protocol;

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 800;

std::filesystem::path MakeProject(const std::filesystem::path& root) {
  std::filesystem::create_directories(root);
  const std::filesystem::path source = root / "main.cpp";
  std::string text;
  for (int i = 0; i < 200; ++i) {
    text += "int line_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  WriteFile(source, text);
  return source;
}

// A shell with the debugger enabled, a project open, and the pane visible.
void OpenDebugShell(WorkspaceShell& shell, const std::filesystem::path& root,
                    const std::filesystem::path& source) {
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, kWindowWidth, kWindowHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "debug.enabled", "true"),
         "the debug pane only builds a view model when debug.enabled is on");
  WorkspaceShellTestAccess::SetDebugPaneVisible(shell, true);
}

// Deep enough that the list scrolls and that a frame's secondary line is long
// enough to need truncation.
void SeedCallStack(WorkspaceShell& shell, const std::filesystem::path& source,
                   std::size_t frame_count) {
  auto& execution = WorkspaceShellTestAccess::DebugExecution(shell);
  execution.stopped = true;
  execution.thread_id = 1;
  execution.stop_reason = "breakpoint";
  execution.frames.clear();
  for (std::size_t i = 0; i < frame_count; ++i) {
    DebugStackFrameView frame;
    frame.id = static_cast<int>(i) + 1;
    frame.SetSource(source.string());
    frame.line = i;
    frame.display_primary = "frame_function_with_a_long_name_" + std::to_string(i);
    frame.display_secondary =
        source.string() + ":" + std::to_string(i + 1) + " (a very long trailing detail)";
    execution.frames.push_back(std::move(frame));
  }
  execution.focused_frame_index = 0;
  // Multi-thread and multi-session selectors are separate render branches; a
  // single entry leaves them hidden, so seed two of each.
  execution.threads = {{1, "Thread 1: main"}, {2, "Thread 2: worker"}};
  execution.focused_thread_id = 1;
  execution.sessions = {{1, "launch (paused)", false}, {2, "server", true}};
  execution.focused_session_id = 1;
}

void SeedVariables(WorkspaceShell& shell, int child_count) {
  auto& variables = WorkspaceShellTestAccess::DebugVariables(shell);
  variables.BeginFrame(1);
  std::vector<dap::DapScope> scopes;
  dap::DapScope locals;
  locals.name = "Locals";
  locals.variables_reference = 1000;
  scopes.push_back(locals);
  dap::DapScope registers;
  registers.name = "Registers";
  registers.variables_reference = 2000;
  scopes.push_back(registers);
  variables.ApplyScopes(scopes);

  std::vector<dap::DapVariable> children;
  for (int i = 0; i < child_count; ++i) {
    dap::DapVariable variable;
    variable.name = "local_" + std::to_string(i);
    variable.value = "0x" + std::to_string(i * 1234) + " <a long enough value to be clipped>";
    variable.type = "int";
    // Every third child is itself a container, which is what draws the twisty.
    variable.variables_reference = (i % 3 == 0) ? 3000 + i : 0;
    children.push_back(std::move(variable));
  }
  variables.ApplyVariables(1000, children, 0);
}

// Painting must not depend on a window: every assertion here is that the frame
// came out, plus the opacity contract shared with the other surface tests.
void PaintAndExpectOpaque(WorkspaceShell& shell, SoftwareCanvas& canvas, const char* what) {
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  // The pane occupies the right edge; sample inside it rather than at the window
  // centre, which is the editor.
  const SDL_Color inside_pane = canvas.PixelAt(kWindowWidth - 40, kWindowHeight / 2);
  Expect(inside_pane.a == 255, what);
  // Vacuity guard. RenderDebugPaneSurface early-returns on an empty cached view
  // model (debugger off) and on an invisible one, and either way the frame still
  // paints and still comes out opaque — so the pixel assertion above passes
  // without the pane ever having drawn. This is what makes it evidence.
  Expect(WorkspaceShellTestAccess::LastPaintedDebugPaneWasVisible(shell),
         "the frame must actually reach the debug pane surface");
}

void TestDebugPanePaintsEverySurfaceWithContent() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = MakeProject(root);
  SoftwareCanvas canvas(kWindowWidth, kWindowHeight);

  WorkspaceShell shell;
  OpenDebugShell(shell, root, source);
  SeedCallStack(shell, source, 40);
  SeedVariables(shell, 40);
  for (int i = 0; i < 30; ++i) {
    WorkspaceShellTestAccess::AddDebugWatchExpressionForTest(shell,
                                                             "expr_" + std::to_string(i));
  }
  for (std::size_t line = 0; line < 30; ++line) {
    WorkspaceShellTestAccess::ToggleBreakpointForTest(shell, source, line);
  }

  Expect(WorkspaceShellTestAccess::DebugBreakpointRowCount(shell) > 0,
         "the breakpoints surface must have rows, or its paint proves nothing");
  Expect(WorkspaceShellTestAccess::DebugWatchRowCount(shell) > 0,
         "the watch surface must have rows, or its paint proves nothing");
  Expect(!WorkspaceShellTestAccess::DebugVariables(shell).Empty(),
         "the variables surface must have rows, or its paint proves nothing");

  const DebugPaneMode modes[] = {DebugPaneMode::CallStack, DebugPaneMode::Variables,
                                 DebugPaneMode::Watch, DebugPaneMode::Breakpoints};
  for (const DebugPaneMode mode : modes) {
    WorkspaceShellTestAccess::SetDebugPaneMode(shell, mode);
    PaintAndExpectOpaque(shell, canvas, "a painted debug pane surface should be opaque");
  }
}

void TestDebugPanePaintsEmptySurfaces() {
  // The empty state is its own branch on all four surfaces (the placeholder text,
  // and an early return past the row loop). A pane that only ever painted with
  // content would leave it unexercised.
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = MakeProject(root);
  SoftwareCanvas canvas(kWindowWidth, kWindowHeight);

  WorkspaceShell shell;
  OpenDebugShell(shell, root, source);

  const DebugPaneMode modes[] = {DebugPaneMode::CallStack, DebugPaneMode::Variables,
                                 DebugPaneMode::Watch, DebugPaneMode::Breakpoints};
  for (const DebugPaneMode mode : modes) {
    WorkspaceShellTestAccess::SetDebugPaneMode(shell, mode);
    PaintAndExpectOpaque(shell, canvas, "an empty debug pane surface should still be opaque");
  }
}

void TestDebugPanePaintsWhileScrolledAndSelected() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = MakeProject(root);
  SoftwareCanvas canvas(kWindowWidth, kWindowHeight);

  WorkspaceShell shell;
  OpenDebugShell(shell, root, source);
  SeedCallStack(shell, source, 60);
  SeedVariables(shell, 60);
  for (int i = 0; i < 60; ++i) {
    WorkspaceShellTestAccess::AddDebugWatchExpressionForTest(shell,
                                                             "expr_" + std::to_string(i));
  }
  for (std::size_t line = 0; line < 60; ++line) {
    WorkspaceShellTestAccess::ToggleBreakpointForTest(shell, source, line);
  }

  const int visible_rows = WorkspaceShellTestAccess::DebugPaneVisibleRows(shell);
  Expect(visible_rows > 0, "the pane must have a measurable row budget");
  Expect(visible_rows < 60,
         "the fixture must overflow the pane, or the scrolled branch is never taken");

  // Call Stack: focus a frame past the fold and scroll to it.
  WorkspaceShellTestAccess::SetDebugPaneMode(shell, DebugPaneMode::CallStack);
  WorkspaceShellTestAccess::DebugExecution(shell).focused_frame_index = 50;
  WorkspaceShellTestAccess::FocusDebugPaneCallStack(shell);
  PaintAndExpectOpaque(shell, canvas, "a scrolled call stack should paint");

  // Variables: move the selection deep, which scrolls the tree.
  WorkspaceShellTestAccess::SetDebugPaneMode(shell, DebugPaneMode::Variables);
  WorkspaceShellTestAccess::DebugVariables(shell).SetSelectedRow(40);
  PaintAndExpectOpaque(shell, canvas, "a scrolled variables tree should paint");

  // Watch: same, through the pane's own keyboard path so the scroll follows.
  WorkspaceShellTestAccess::FocusDebugPaneWatch(shell);
  WorkspaceShellTestAccess::DebugWatch(shell).SetSelectedRow(50);
  PaintAndExpectOpaque(shell, canvas, "a scrolled watch list should paint");

  // Breakpoints: the selected-row fill is a distinct branch from the others.
  WorkspaceShellTestAccess::FocusDebugPaneBreakpoints(shell);
  PaintAndExpectOpaque(shell, canvas, "a focused breakpoints list should paint");
}

void TestDebugPanePaintsAtDegenerateWidths() {
  // A pane dragged to almost nothing, and one wider than the window. Both are
  // reachable by dragging the splitter, and both are where a row-rect computation
  // goes negative.
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = MakeProject(root);
  SoftwareCanvas canvas(kWindowWidth, kWindowHeight);

  WorkspaceShell shell;
  OpenDebugShell(shell, root, source);
  SeedCallStack(shell, source, 20);
  SeedVariables(shell, 20);

  for (const float width : {1.0f, 12.0f, 60.0f, 2000.0f}) {
    WorkspaceShellTestAccess::SetDebugPaneWidth(shell, width);
    for (const DebugPaneMode mode : {DebugPaneMode::CallStack, DebugPaneMode::Variables,
                                     DebugPaneMode::Watch, DebugPaneMode::Breakpoints}) {
      WorkspaceShellTestAccess::SetDebugPaneMode(shell, mode);
      WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
    }
  }
}

}  // namespace

void RegisterDebugPaneRenderTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DebugPaneRender/PaintsEverySurfaceWithContent",
          TestDebugPanePaintsEverySurfaceWithContent);
  AddTest(tests, "DebugPaneRender/PaintsEmptySurfaces", TestDebugPanePaintsEmptySurfaces);
  AddTest(tests, "DebugPaneRender/PaintsWhileScrolledAndSelected",
          TestDebugPanePaintsWhileScrolledAndSelected);
  AddTest(tests, "DebugPaneRender/PaintsAtDegenerateWidths",
          TestDebugPanePaintsAtDegenerateWidths);
}

}  // namespace microide::tests
