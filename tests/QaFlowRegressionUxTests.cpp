// Regressions the GUI QA ux flows (tools/qa/flows_ux_*.py) found, pinned in
// process: real events through HandleEvent, checked against the same queries the
// harness reads. The redraw-diff regressions stay in QaFlowRegressionTests.cpp;
// this file holds the behavioural ones -- what a press, a key or a drag DOES.
#include "TestSupport.h"
#include "TestSupportSdl.h"

#include <filesystem>
#include <string>
#include <string_view>

#include "WorkspaceShellEventHelpers.h"
#include "support/RetainedRedraw.h"
#include "support/SoftwareCanvas.h"
#include "util/JsonValue.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using TestAccess = microide::workspace::WorkspaceShell::TestAccess;

struct UxFixture {
  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;

  UxFixture(std::string_view file_name, std::string_view content) {
    EnsureDummySdlVideoInitialized();
    root = temp_dir.path() / "project";
    file = root / std::string(file_name);
    WriteFile(file, std::string(content));
    Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the ux fixture should open its project");
    TestAccess::SetWindowSize(shell, kWidth, kHeight);
    TestAccess::OpenSingleEditorTab(shell, file);
    TestAccess::ShowTreeSidebar(shell);
    TestAccess::SetFocusEditor(shell);
    TestAccess::NoteLayoutInputsChanged(shell);
    shell.Render(canvas.renderer(), kWidth, kHeight);
    (void)shell.ConsumePendingRenderInvalidation();
  }

  // The window point of the middle of the line-number column on 1-based `line`.
  SDL_FPoint LineNumberPoint(int line) {
    const util::JsonValue ui = TestAccess::ControlUi(shell);
    const util::JsonValue& pane = ui["editors"].AsArray()[0];
    const float x = static_cast<float>(pane["gutterX"].AsDouble() + pane["gutterWidth"].AsDouble() * 0.4);
    for (const util::JsonValue& row : pane["rows"].AsArray()) {
      if (row["line"].AsInt() == line) {
        return SDL_FPoint{x, static_cast<float>(row["y"].AsDouble() + pane["lineHeight"].AsDouble() / 2.0)};
      }
    }
    Expect(false, "line " + std::to_string(line) + " should be on screen");
    return SDL_FPoint{x, 0.0f};
  }

  // The primary selection as 0-based (anchor line, anchor column, caret line, caret column).
  std::string Selection() {
    const editor::TextViewport& viewport = TestAccess::ActiveEditor(shell);
    const auto range = viewport.selection_range();
    std::string anchor = range.has_value() ? std::to_string(range->start.line) + ":" +
                                                 std::to_string(range->start.column)
                                           : std::string("none");
    return anchor + " -> " + std::to_string(viewport.cursor_line()) + ":" +
           std::to_string(viewport.cursor_column());
  }

  TemporaryDirectory temp_dir;
  std::filesystem::path root;
  std::filesystem::path file;
  WorkspaceShell shell;
  SoftwareCanvas canvas{kWidth, kHeight};
};

// editor-mouse: a click on a line number placed a bare caret, and a drag down the
// numbers stopped at the start of the line under the pointer. VS Code selects the
// clicked line whole, and a drag over the numbers takes every line it crosses.
void TestALineNumberPressSelectsWholeLines() {
  UxFixture fixture("lines.txt", "one\ntwo\nthree\nfour\nfive\n");
  const SDL_FPoint two = fixture.LineNumberPoint(2);
  (void)DispatchEvents(fixture.shell, {MouseMotionEvent(two.x, two.y),
                                       MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, two.x, two.y),
                                       MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, two.x, two.y)});
  Expect(fixture.Selection() == "1:0 -> 2:0",
         "a click on line 2's number should select line 2 whole, got " + fixture.Selection());

  const SDL_FPoint four = fixture.LineNumberPoint(4);
  SDL_Event held = MouseMotionEvent(four.x, four.y);
  held.motion.state = SDL_BUTTON_LMASK;
  (void)DispatchEvents(fixture.shell, {MouseMotionEvent(two.x, two.y),
                                       MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, two.x, two.y),
                                       held,
                                       MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, four.x, four.y)});
  Expect(fixture.Selection() == "1:0 -> 4:0",
         "a drag down the numbers from 2 to 4 should select lines 2-4 whole, got " + fixture.Selection());
}


}  // namespace

void RegisterQaFlowRegressionUxTests(std::vector<TestCase>& tests) {
  AddTest(tests, "QaFlowRegressionUx/ALineNumberPressSelectsWholeLines", TestALineNumberPressSelectsWholeLines);
}

}  // namespace microide::tests
