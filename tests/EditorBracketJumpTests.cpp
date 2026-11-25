#include "TestSupport.h"

#include "editor/BracketScanner.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;
using microide::workspace::WorkspaceShell;
using ActionId = WorkspaceShell::ActionId;

struct Caret {
  std::size_t line = 0;
  std::size_t column = 0;

  bool operator==(const Caret& other) const {
    return line == other.line && column == other.column;
  }
};

struct BracketFixture {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  std::filesystem::path source;

  explicit BracketFixture(const std::string& text) {
    const std::filesystem::path root = temp_dir.path() / "project";
    std::filesystem::create_directories(root);
    source = root / "main.cpp";
    WriteFile(source, text);
    WorkspaceShellTestAccess::SetProjectRoot(shell, root);
    WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 800);
    WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  }

  microide::editor::TextViewport& viewport() {
    auto* v = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
    Expect(v != nullptr, "the bracket fixture must have an active viewport");
    return *v;
  }

  void PlaceCaret(std::size_t line, std::size_t column) {
    viewport().JumpCursorTo(line, column, false);
  }

  Caret caret() { return {viewport().cursor_line(), viewport().cursor_column()}; }

  bool Jump() {
    return WorkspaceShellTestAccess::ExecuteAction(shell, ActionId::JumpToMatchingBracket, {});
  }
};

void TestJumpMovesOutOfAnEmptyPair() {
  for (const std::string& pair : {std::string("()"), std::string("[]"), std::string("{}")}) {
    BracketFixture fixture("int main" + pair + " {}\n");
    fixture.PlaceCaret(0, 9);
    Expect(fixture.caret() == Caret{0, 9}, "the fixture should start between the brackets");
    fixture.Jump();
    Expect(fixture.caret() == (Caret{0, 8}),
           "jumping from inside an empty pair should land on the opening bracket");
  }
}

void TestJumpMovesWhereverAMatchExists() {
  const std::string text =
      "void outer(int a, int b) {\n"
      "  if (a > b) { return f(a, g(b), h()); }\n"
      "  while (a) { a--; }\n"
      "  int empty[] = {};\n"
      "}\n";
  const std::vector<std::string> lines = {
      "void outer(int a, int b) {",
      "  if (a > b) { return f(a, g(b), h()); }",
      "  while (a) { a--; }",
      "  int empty[] = {};",
      "}",
  };

  BracketFixture fixture(text);
  int matches = 0;
  int empty_pair_cases = 0;
  for (std::size_t line = 0; line < lines.size(); ++line) {
    for (std::size_t column = 0; column <= lines[line].size(); ++column) {
      std::vector<std::string_view> views;
      views.reserve(lines.size());
      for (const std::string& l : lines) {
        views.emplace_back(l);
      }
      const auto match =
          microide::editor::FindBracketMatchInLines(views, line, column, 2000);
      if (!match.has_value()) {
        continue;
      }
      ++matches;
      if (match->open_line == match->close_line &&
          match->close_column == match->open_column + 1) {
        ++empty_pair_cases;
      }

      fixture.PlaceCaret(line, column);
      const Caret before = fixture.caret();
      fixture.Jump();
      const Caret after = fixture.caret();

      Expect(!(after == before),
             "a bracket jump with a match available must move the caret");
      const bool at_open = after == (Caret{match->open_line, match->open_column});
      const bool at_close = after == (Caret{match->close_line, match->close_column});
      Expect(at_open || at_close,
             "a bracket jump must land on one end of the matched pair");
    }
  }
  Expect(matches >= 30, "the fixture must offer plenty of matches, or this proves nothing");
  Expect(empty_pair_cases >= 4,
         "the sweep must include empty bracket pairs, or it cannot see the bug it is for");
}

void TestJumpFromABracketIsAnInvolution() {
  const std::string line = "f(a, [b, {c}], d)";
  BracketFixture fixture(line + "\n");
  int checked = 0;
  for (std::size_t column = 0; column < line.size(); ++column) {
    const char c = line[column];
    if (c != '(' && c != ')' && c != '[' && c != ']' && c != '{' && c != '}') {
      continue;
    }
    fixture.PlaceCaret(0, column);
    const Caret start = fixture.caret();
    fixture.Jump();
    const Caret midpoint = fixture.caret();
    Expect(!(midpoint == start), "the first jump should move off the bracket");
    fixture.Jump();
    Expect(fixture.caret() == start,
           "jumping twice from a bracket character should return to it");
    ++checked;
  }
  Expect(checked == 6, "the fixture line has six bracket characters to check");
}

void TestJumpAcrossLinesAndWithNoMatch() {
  const std::string text =
      "void f(\n"
      "    int a,\n"
      "    int b) {\n"
      "  return;\n"
      "}\n";
  BracketFixture fixture(text);

  fixture.PlaceCaret(0, 6);
  fixture.Jump();
  Expect(fixture.caret() == (Caret{2, 9}),
         "a multi-line pair should jump to the closing bracket on its own line");
  fixture.Jump();
  Expect(fixture.caret() == (Caret{0, 6}), "and back again");

  fixture.PlaceCaret(3, 4);
  const Caret before = fixture.caret();
  fixture.Jump();
  Expect(fixture.caret() == before,
         "a jump with no adjacent bracket must leave the caret alone");
}

void TestJumpLeavesUnbalancedBracketsAlone() {
  {
    BracketFixture opener_only("int a = f(1, 2;\n");
    opener_only.PlaceCaret(0, 9);
    const Caret before = opener_only.caret();
    opener_only.Jump();
    Expect(opener_only.caret() == before, "an unmatched opener must not move the caret");
  }
  {
    BracketFixture closer_only("int b = 3);\n");
    closer_only.PlaceCaret(0, 9);
    const Caret before = closer_only.caret();
    closer_only.Jump();
    Expect(closer_only.caret() == before, "an unmatched closer must not move the caret");
  }
}

}

void RegisterEditorBracketJumpTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorBracketJump/MovesOutOfAnEmptyPair", TestJumpMovesOutOfAnEmptyPair);
  AddTest(tests, "EditorBracketJump/MovesWhereverAMatchExists",
          TestJumpMovesWhereverAMatchExists);
  AddTest(tests, "EditorBracketJump/FromABracketIsAnInvolution",
          TestJumpFromABracketIsAnInvolution);
  AddTest(tests, "EditorBracketJump/AcrossLinesAndWithNoMatch",
          TestJumpAcrossLinesAndWithNoMatch);
  AddTest(tests, "EditorBracketJump/LeavesUnbalancedBracketsAlone",
          TestJumpLeavesUnbalancedBracketsAlone);
}

}
