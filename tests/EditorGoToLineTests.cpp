#include "TestSupport.h"

#include "workspace/actions/WorkspaceActionRequests.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <filesystem>
#include <string>
#include <vector>

// Go to line, both halves of it.
//
// BuildLineNavigationRequest parses `line[:column]` and ExecuteLineNavigation
// applies it, and neither had a test. Four behaviours are worth pinning because
// each is easy to get wrong by assumption:
//
//  - `line:col` names a 1-based CHARACTER column (VSCode's Ctrl+G), not a byte
//    offset, so multibyte text before the target must not land the caret short.
//  - `goto` is ABSOLUTE and rejects a non-positive line. That is a fixed bug,
//    not an accident: TD-2026-07-16-68 records that a negative used to fall
//    through to "from end" mode, so `goto -1` went to EOF and a typo navigated
//    to the end of the file. The guard lives at the call site.
//  - `jump` is RELATIVE -- a signed delta from the caret's line -- and is the
//    only command for which line 0 parses (allow_zero_line), where it means
//    "stay put".
//  - a line past the end clamps to the last line rather than failing.
//
// The from-end branch still inside ExecuteLineNavigation is unreachable through
// the single call site (goto rejects non-positive, jump is relative). It is left
// alone deliberately: without it a negative arriving there would compute a huge
// size_t and clamp to EOF, which is the exact bug TD-68 fixed. Defence, not dead
// weight.

namespace microide::tests {
namespace {

using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;
using microide::workspace::BuildLineNavigationRequest;
using microide::workspace::WorkspaceShell;

struct GoToFixture {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;

  GoToFixture() {
    const std::filesystem::path root = temp_dir.path() / "project";
    std::filesystem::create_directories(root);
    const std::filesystem::path source = root / "main.cpp";
    // Line 2 (1-based 3) carries multibyte characters before the target column,
    // so a byte-offset reading of `3:5` would land short of the right character.
    WriteFile(source,
              "first line\n"
              "second line\n"
              "ünïcødé here\n"
              "fourth line\n"
              "fifth line\n");
    WorkspaceShellTestAccess::SetProjectRoot(shell, root);
    WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 800);
    WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  }

  microide::editor::TextViewport& viewport() {
    auto* v = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
    Expect(v != nullptr, "the go-to fixture must have an active viewport");
    return *v;
  }

  bool Goto(const std::string& spec) {
    return WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::Goto, {spec});
  }
  void Jump(const std::string& spec) {
    WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::Jump, {spec});
  }
  std::size_t Line() { return viewport().cursor_line(); }
  std::size_t Column() { return viewport().cursor_column(); }
};

void TestGoToLineIsOneBased() {
  GoToFixture fixture;
  fixture.Goto("1");
  Expect(fixture.Line() == 0, "line 1 is the first line");
  fixture.Goto("4");
  Expect(fixture.Line() == 3, "line 4 is the fourth line, 0-based 3");
}

void TestGoToColumnCountsCharactersNotBytes() {
  // The assertion this file exists for. "ünïcødé here" has multibyte characters
  // before column 5, so the byte offset of the 5th CHARACTER is larger than 4.
  GoToFixture fixture;
  fixture.Goto("3:5");
  Expect(fixture.Line() == 2, "the caret should be on the unicode line");
  Expect(fixture.Column() > 4,
         "a 1-based CHARACTER column must resolve past the multibyte prefix, so the "
         "byte column is greater than the naive 4");

  // The same column on a pure-ASCII line is exactly the byte offset, which is
  // what makes the comparison above meaningful rather than accidental.
  fixture.Goto("1:5");
  Expect(fixture.Line() == 0 && fixture.Column() == 4,
         "on an ASCII line the 5th character is byte 4");
}

void TestGoToBeyondTheEndClampsToTheLastLine() {
  GoToFixture fixture;
  fixture.Goto("9999");
  Expect(fixture.Line() == fixture.viewport().line_count() - 1,
         "a line past the end clamps to the last line rather than failing");
}

void TestGotoRejectsNonPositiveLines() {
  // TD-2026-07-16-68: `goto` is absolute, so a non-positive line is a typo, not
  // an address. It must be refused rather than quietly navigating to EOF.
  GoToFixture fixture;
  fixture.Goto("3");
  const std::size_t settled = fixture.Line();
  Expect(settled == 2, "the fixture should be parked on line 3 before the bad input");

  fixture.Goto("-1");
  Expect(fixture.Line() == settled, "goto -1 must not move the caret");
  fixture.Goto("0");
  Expect(fixture.Line() == settled, "goto 0 must not move the caret either");
}

void TestJumpIsARelativeDelta() {
  // `jump` is the signed-delta command: +N down, -N up, 0 stays.
  GoToFixture fixture;
  fixture.Goto("3");
  Expect(fixture.Line() == 2, "start from line 3");

  fixture.Jump("2");
  Expect(fixture.Line() == 4, "jump 2 moves two lines down, not to line 2");
  fixture.Jump("-3");
  Expect(fixture.Line() == 1, "jump -3 moves three lines up");
  fixture.Jump("0");
  Expect(fixture.Line() == 1, "jump 0 stays put");

  // Deltas past either end clamp instead of underflowing: the arithmetic runs
  // in long long precisely so a large negative cannot wrap.
  fixture.Jump("-9999");
  Expect(fixture.Line() == 0, "a delta past the start clamps to the first line");
  fixture.Jump("9999");
  Expect(fixture.Line() == fixture.viewport().line_count() - 1,
         "a delta past the end clamps to the last line");
}

void TestZeroLineIsRejectedForGotoAndAcceptedForJump() {
  // allow_zero_line is the only difference between the two commands' parsing.
  Expect(!BuildLineNavigationRequest({"0"}, /*allow_zero_line=*/false).has_value(),
         "goto must reject line 0, which is not a 1-based line");
  Expect(BuildLineNavigationRequest({"0"}, /*allow_zero_line=*/true).has_value(),
         "jump accepts 0, which its from-end arithmetic gives a meaning");
}

void TestMalformedSpecsAreRejected() {
  const char* bad[] = {"", "abc", ":5", "12:", "12:-5", "12:5:7", "1 2", "+"};
  for (const char* spec : bad) {
    Expect(!BuildLineNavigationRequest({spec}, /*allow_zero_line=*/false).has_value(),
           "a malformed line spec must be rejected rather than partly parsed");
  }
  Expect(!BuildLineNavigationRequest({}, /*allow_zero_line=*/false).has_value(),
         "no argument at all is not a location");

  // And the forms that must parse, so the rejections above are not vacuous.
  const auto plain = BuildLineNavigationRequest({"12"}, /*allow_zero_line=*/false);
  Expect(plain.has_value() && plain->requested_line == 12 && plain->column == 0,
         "a bare line number parses with no column");
  const auto with_column = BuildLineNavigationRequest({"12:5"}, /*allow_zero_line=*/false);
  Expect(with_column.has_value() && with_column->requested_line == 12 &&
             with_column->column == 5,
         "line:column parses both halves");
  const auto negative = BuildLineNavigationRequest({"-3"}, /*allow_zero_line=*/false);
  Expect(negative.has_value() && negative->requested_line == -3,
         "a negative line parses: it is from-end addressing, not an error");
}

}  // namespace

void RegisterEditorGoToLineTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorGoToLine/LineIsOneBased", TestGoToLineIsOneBased);
  AddTest(tests, "EditorGoToLine/ColumnCountsCharactersNotBytes",
          TestGoToColumnCountsCharactersNotBytes);
  AddTest(tests, "EditorGoToLine/BeyondTheEndClampsToTheLastLine",
          TestGoToBeyondTheEndClampsToTheLastLine);
  AddTest(tests, "EditorGoToLine/GotoRejectsNonPositiveLines",
          TestGotoRejectsNonPositiveLines);
  AddTest(tests, "EditorGoToLine/JumpIsARelativeDelta", TestJumpIsARelativeDelta);
  AddTest(tests, "EditorGoToLine/ZeroLineRejectedForGotoAcceptedForJump",
          TestZeroLineIsRejectedForGotoAndAcceptedForJump);
  AddTest(tests, "EditorGoToLine/MalformedSpecsAreRejected", TestMalformedSpecsAreRejected);
}

}  // namespace microide::tests
