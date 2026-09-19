#include "TestSupport.h"

#include "editor/BracketScanner.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <filesystem>
#include <string>
#include <vector>

// Ctrl+Shift+\ -- jump to the matching bracket.
//
// FindBracketMatch itself is well covered (EditorEssentialsTests), but the JUMP
// had no test at any level: neither the ActionId nor its command name appeared
// anywhere in tests/. What was untested was the one decision the handler makes
// on top of the scanner -- which end of the pair to jump to -- and it was wrong
// on the commonest shape there is.
//
// The handler re-derived "is the caret at the opener?" as
//   cursor_column == open_column || cursor_column == open_column + 1
// rather than reading `caret_at_opener`, which the scanner already returns. The
// two agree everywhere except an EMPTY pair, where close_column IS
// open_column + 1: in `()` with the caret between the brackets the hand-rolled
// test said "at the opener" and jumped to the closer, which is the column the
// caret was already on. The command did nothing on `()`, `[]` and `{}`.

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

// A shell with `text` open in the editor, positioned at (line, column).
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
    // A real viewport size: a null renderer leaves the viewport one line tall,
    // which makes any caret move scroll and confuses row-based assertions.
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
  // The regression. Before the fix every one of these left the caret where it
  // was, so the keystroke appeared to do nothing on the commonest bracket shape.
  for (const std::string& pair : {std::string("()"), std::string("[]"), std::string("{}")}) {
    BracketFixture fixture("int main" + pair + " {}\n");
    fixture.PlaceCaret(0, 9);  // between the two brackets of `pair`
    Expect(fixture.caret() == Caret{0, 9}, "the fixture should start between the brackets");
    fixture.Jump();
    Expect(fixture.caret() == (Caret{0, 8}),
           "jumping from inside an empty pair should land on the opening bracket");
  }
}

void TestJumpMovesWhereverAMatchExists() {
  // The general property, and the one the empty-pair bug violated: when the
  // scanner reports a match, the jump must MOVE the caret. Landing back on the
  // column you started from is never a correct answer, because the two ends of a
  // pair are always distinct positions.
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
      // It must land on one of the two ends of the pair it matched.
      const bool at_open = after == (Caret{match->open_line, match->open_column});
      const bool at_close = after == (Caret{match->close_line, match->close_column});
      Expect(at_open || at_close,
             "a bracket jump must land on one end of the matched pair");
    }
  }
  // The fixture yields 40 matched positions and 8 empty pairs as written. Both
  // floors sit well below that rather than on it, so they bind if the fixture
  // degrades without failing on an off-by-one.
  Expect(matches >= 30, "the fixture must offer plenty of matches, or this proves nothing");
  // Vacuity guard for the regression specifically: the property above is only
  // interesting if the sweep actually visits empty pairs, which are the shape
  // the old code got wrong. `h()` and `{}` supply them.
  Expect(empty_pair_cases >= 4,
         "the sweep must include empty bracket pairs, or it cannot see the bug it is for");
}

void TestJumpFromABracketIsAnInvolution() {
  // Standing on a bracket and jumping twice must return to that bracket. From an
  // ADJACENT column it need not -- the first jump normalises onto a bracket
  // position -- so this is asserted only from the bracket characters themselves.
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

  // Multi-line pair: the `(` on line 0 matches the `)` on line 2.
  fixture.PlaceCaret(0, 6);
  fixture.Jump();
  Expect(fixture.caret() == (Caret{2, 9}),
         "a multi-line pair should jump to the closing bracket on its own line");
  fixture.Jump();
  Expect(fixture.caret() == (Caret{0, 6}), "and back again");

  // No bracket adjacent: the caret must not move at all.
  fixture.PlaceCaret(3, 4);
  const Caret before = fixture.caret();
  fixture.Jump();
  Expect(fixture.caret() == before,
         "a jump with no adjacent bracket must leave the caret alone");
}

void TestJumpLeavesUnbalancedBracketsAlone() {
  // An opener with no closer and a closer with no opener both report no match,
  // so both must be no-ops rather than jumping somewhere arbitrary.
  //
  // Two separate documents, deliberately. The first attempt put both on adjacent
  // lines of one buffer -- and the scanner spans lines, so the stray `(` matched
  // the stray `)` and the fixture was balanced after all. An "unmatched" bracket
  // is only unmatched with respect to the WHOLE scan window.
  {
    BracketFixture opener_only("int a = f(1, 2;\n");
    opener_only.PlaceCaret(0, 9);  // the `(` with no closer anywhere
    const Caret before = opener_only.caret();
    opener_only.Jump();
    Expect(opener_only.caret() == before, "an unmatched opener must not move the caret");
  }
  {
    BracketFixture closer_only("int b = 3);\n");
    closer_only.PlaceCaret(0, 9);  // the `)` with no opener anywhere
    const Caret before = closer_only.caret();
    closer_only.Jump();
    Expect(closer_only.caret() == before, "an unmatched closer must not move the caret");
  }
}

}  // namespace

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

}  // namespace microide::tests
