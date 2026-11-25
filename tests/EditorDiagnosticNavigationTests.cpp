#include "TestSupport.h"

#include "editor/DiagnosticsStore.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"
#include "WorkspaceShellEventHelpers.h"

#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;
using microide::editor::Diagnostic;
using microide::editor::DiagnosticSeverity;
using microide::workspace::WorkspaceShell;

struct Caret {
  std::size_t line = 0;
  std::size_t column = 0;
  bool operator==(const Caret& other) const {
    return line == other.line && column == other.column;
  }
};

const std::vector<Caret>& ExpectedOrder() {
  static const std::vector<Caret> kOrder = {{2, 4}, {5, 0}, {5, 9}, {11, 2}, {20, 7}};
  return kOrder;
}

struct DiagnosticFixture {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  std::filesystem::path source;

  DiagnosticFixture() {
    const std::filesystem::path root = temp_dir.path() / "project";
    std::filesystem::create_directories(root);
    source = root / "main.cpp";
    std::string text;
    for (int i = 0; i < 30; ++i) {
      text += "int line_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    }
    WriteFile(source, text);
    WorkspaceShellTestAccess::SetProjectRoot(shell, root);
    WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 800);
    WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);

    const std::vector<Caret> published = {{11, 2}, {2, 4}, {20, 7}, {5, 9}, {5, 0}};
    std::vector<Diagnostic> diagnostics;
    for (std::size_t i = 0; i < published.size(); ++i) {
      Diagnostic d;
      d.range.start = {published[i].line, published[i].column};
      d.range.end = {published[i].line, published[i].column + 3};
      d.severity = (i % 3 == 0)   ? DiagnosticSeverity::Error
                   : (i % 3 == 1) ? DiagnosticSeverity::Warning
                                  : DiagnosticSeverity::Info;
      d.message = "problem " + std::to_string(i);
      diagnostics.push_back(std::move(d));
    }
    Expect(WorkspaceShellTestAccess::SetDiagnosticsForTest(shell, source, std::move(diagnostics)),
           "seeding diagnostics should take");
  }

  void PlaceCaret(std::size_t line, std::size_t column) {
    auto* viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
    Expect(viewport != nullptr, "the fixture must have an active viewport");
    viewport->JumpCursorTo(line, column, false);
  }

  Caret caret() {
    auto* viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
    return {viewport->cursor_line(), viewport->cursor_column()};
  }

  void Step(int delta) {
    WorkspaceShellTestAccess::ExecuteAction(
        shell,
        delta > 0 ? WorkspaceShell::ActionId::GoToNextDiagnostic
                  : WorkspaceShell::ActionId::GoToPreviousDiagnostic,
        {});
  }
};

void TestNextVisitsEveryDiagnosticInDocumentOrder() {
  DiagnosticFixture fixture;
  fixture.PlaceCaret(0, 0);
  for (const Caret& expected : ExpectedOrder()) {
    fixture.Step(1);
    Expect(fixture.caret() == expected,
           "next should visit diagnostics in document order regardless of publish order");
  }
  fixture.Step(1);
  Expect(fixture.caret() == ExpectedOrder().front(),
         "next from the last diagnostic should wrap to the first");
}

void TestPreviousVisitsEveryDiagnosticInReverse() {
  DiagnosticFixture fixture;
  fixture.PlaceCaret(29, 0);
  for (auto it = ExpectedOrder().rbegin(); it != ExpectedOrder().rend(); ++it) {
    fixture.Step(-1);
    Expect(fixture.caret() == *it, "previous should visit diagnostics in reverse document order");
  }
  fixture.Step(-1);
  Expect(fixture.caret() == ExpectedOrder().back(),
         "previous from the first diagnostic should wrap to the last");
}

void TestADiagnosticAtTheCaretIsSteppedPast() {
  DiagnosticFixture fixture;
  const std::vector<Caret>& order = ExpectedOrder();
  for (std::size_t i = 0; i < order.size(); ++i) {
    fixture.PlaceCaret(order[i].line, order[i].column);
    fixture.Step(1);
    const Caret expected = order[(i + 1) % order.size()];
    Expect(fixture.caret() == expected, "next from a diagnostic must move to the following one");

    fixture.PlaceCaret(order[i].line, order[i].column);
    fixture.Step(-1);
    const Caret expected_back = order[(i + order.size() - 1) % order.size()];
    Expect(fixture.caret() == expected_back,
           "previous from a diagnostic must move to the preceding one");
  }
}

void TestNextAndPreviousAreInversesOnADiagnostic() {
  DiagnosticFixture fixture;
  for (const Caret& start : ExpectedOrder()) {
    fixture.PlaceCaret(start.line, start.column);
    fixture.Step(1);
    fixture.Step(-1);
    Expect(fixture.caret() == start, "next then previous should return to the starting diagnostic");
  }
}

void TestSteppingTheWholeCycleReturnsToStart() {
  DiagnosticFixture fixture;
  const std::size_t count = ExpectedOrder().size();
  fixture.PlaceCaret(ExpectedOrder().front().line, ExpectedOrder().front().column);
  const Caret start = fixture.caret();
  for (std::size_t i = 0; i < count; ++i) {
    fixture.Step(1);
  }
  Expect(fixture.caret() == start, "stepping once per diagnostic should return to the start");
}

void TestStepFailsWithNoDiagnostics() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::filesystem::create_directories(root);
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int a = 0;\nint b = 1;\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 800);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);

  auto* viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
  Expect(viewport != nullptr, "the fixture must have an active viewport");
  viewport->JumpCursorTo(1, 2, false);
  const std::size_t line_before = viewport->cursor_line();
  const std::size_t column_before = viewport->cursor_column();

  WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::GoToNextDiagnostic, {});
  WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::GoToPreviousDiagnostic,
                                          {});
  Expect(viewport->cursor_line() == line_before && viewport->cursor_column() == column_before,
         "stepping with no diagnostics published must leave the caret exactly where it was");
}

void TestSingleDiagnosticIsStableUnderRepeatedSteps() {
  DiagnosticFixture fixture;
  Diagnostic only;
  only.range.start = {7, 3};
  only.range.end = {7, 8};
  only.severity = DiagnosticSeverity::Error;
  only.message = "the only problem";
  Expect(WorkspaceShellTestAccess::SetDiagnosticsForTest(fixture.shell, fixture.source, {only}),
         "replacing the diagnostics should take");

  fixture.PlaceCaret(0, 0);
  for (const int delta : {1, 1, -1, -1, 1}) {
    fixture.Step(delta);
    Expect(fixture.caret() == (Caret{7, 3}),
           "with a single diagnostic every step lands on it");
  }
}

void TestF8StepsThroughProblemsAcrossFiles() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::filesystem::create_directories(root);
  const std::filesystem::path a = root / "a.cpp";
  const std::filesystem::path b = root / "b.cpp";
  WriteFile(a, "int a0;\nint a1;\nint a2;\n");
  WriteFile(b, "int b0;\nint b1;\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 800);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, a);
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  const auto problem = [](std::size_t line) {
    Diagnostic d;
    d.range.start = {line, 4};
    d.range.end = {line, 6};
    d.severity = DiagnosticSeverity::Error;
    d.message = "problem on line " + std::to_string(line);
    return d;
  };
  Expect(WorkspaceShellTestAccess::SetDiagnosticsForTest(shell, a, {problem(1)}), "a.cpp's problem");
  Expect(WorkspaceShellTestAccess::SetDiagnosticsForTest(shell, b, {problem(0), problem(1)}),
         "b.cpp's problems");
  const auto where = [&]() -> std::string {
    auto* viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
    return viewport == nullptr ? "none"
                               : viewport->path().filename().string() + ":" +
                                     std::to_string(viewport->cursor_line());
  };
  const auto press = [&](SDL_Keymod modifiers) {
    (void)shell.HandleEvent(KeyDownEvent(SDLK_F8, modifiers));
  };
  press(SDL_KMOD_NONE);
  Expect(where() == "a.cpp:1", "F8 should go to a.cpp's problem, got " + where());
  press(SDL_KMOD_NONE);
  Expect(where() == "b.cpp:0", "F8 again should open b.cpp at its first problem, got " + where());
  press(SDL_KMOD_NONE);
  Expect(where() == "b.cpp:1", "then b.cpp's second, got " + where());
  press(SDL_KMOD_NONE);
  Expect(where() == "a.cpp:1", "and wrap to the first file, got " + where());
  press(SDL_KMOD_SHIFT);
  Expect(where() == "b.cpp:1", "Shift+F8 should wrap back to the last problem, got " + where());
}

}

void RegisterEditorDiagnosticNavigationTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorDiagnosticNavigation/F8StepsThroughProblemsAcrossFiles",
          TestF8StepsThroughProblemsAcrossFiles);
  AddTest(tests, "EditorDiagnosticNavigation/NextVisitsEveryDiagnosticInDocumentOrder",
          TestNextVisitsEveryDiagnosticInDocumentOrder);
  AddTest(tests, "EditorDiagnosticNavigation/PreviousVisitsEveryDiagnosticInReverse",
          TestPreviousVisitsEveryDiagnosticInReverse);
  AddTest(tests, "EditorDiagnosticNavigation/ADiagnosticAtTheCaretIsSteppedPast",
          TestADiagnosticAtTheCaretIsSteppedPast);
  AddTest(tests, "EditorDiagnosticNavigation/NextAndPreviousAreInversesOnADiagnostic",
          TestNextAndPreviousAreInversesOnADiagnostic);
  AddTest(tests, "EditorDiagnosticNavigation/SteppingTheWholeCycleReturnsToStart",
          TestSteppingTheWholeCycleReturnsToStart);
  AddTest(tests, "EditorDiagnosticNavigation/StepFailsWithNoDiagnostics",
          TestStepFailsWithNoDiagnostics);
  AddTest(tests, "EditorDiagnosticNavigation/SingleDiagnosticIsStableUnderRepeatedSteps",
          TestSingleDiagnosticIsStableUnderRepeatedSteps);
}

}
