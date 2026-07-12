#include "TestSupport.h"

#include "editor/SnippetEngine.h"
#include "editor/TextViewport.h"
#include "util/StringUtil.h"

#include <string>

namespace microide::tests {
namespace {

using microide::editor::SelectionRange;
using microide::editor::SnippetSessionState;
using microide::editor::SnippetOnCaretMoved;
using microide::editor::SnippetNavigateTab;
using microide::editor::SnippetTryInsertText;
using microide::editor::SnippetTryBackspace;
using microide::editor::SnippetTryDeleteForward;
using microide::editor::ExpandSnippetAtSelection;
using microide::editor::ParseSnippetBody;
using microide::editor::TextViewport;
using microide::util::IsValidUtf8;

void TestSnippetSimpleExpansion() {
  TextViewport viewport;
  viewport.LoadContent("x", "/tmp/snippet.cpp");
  viewport.MoveCursorTo(0, 1);
  SnippetSessionState session;
  viewport.BeginUndoGroup();
  SelectionRange trigger{{0, 1}, {0, 1}};
  Expect(ExpandSnippetAtSelection(viewport, session, trigger, "(${1:world})$0"),
         "snippet expansion should apply");
  Expect(viewport.lines()[0] == "x(world)",
         "zero-width trigger at end should insert snippet after existing text");
  Expect(session.active, "placeholder session should be active before final stop");
}

void TestSnippetMultiOccurrenceLinkedTab() {
  TextViewport viewport;
  viewport.LoadContent("--", "/tmp/snippet.cpp");
  viewport.MoveCursorTo(0, 0);
  SnippetSessionState session;
  viewport.BeginUndoGroup();
  SelectionRange trigger{{0, 0}, {0, 2}};
  Expect(ExpandSnippetAtSelection(viewport, session, trigger, "${1:x} ${1:y}$0"),
         "multi-tab1 expansion");
  Expect(viewport.lines()[0] == "x y", "both placeholders expand in place of trigger");
  Expect(session.ranges_by_tab[1].size() == 2u, "tab 1 should track both ranges");
  Expect(SnippetTryInsertText(viewport, session, "!"),
         "insert should mirror across linked placeholders");
  Expect(viewport.lines()[0] == "x! y!",
         "linked edit mirrors at caret column inside each placeholder");
}

void TestSnippetMultiOccurrenceLinkedTabMultiKeystroke() {
  TextViewport viewport;
  viewport.LoadContent("--", "/tmp/snippet.cpp");
  viewport.MoveCursorTo(0, 0);
  SnippetSessionState session;
  viewport.BeginUndoGroup();
  SelectionRange trigger{{0, 0}, {0, 2}};
  Expect(ExpandSnippetAtSelection(viewport, session, trigger, "${1:x} ${1:y}$0"),
         "multi-tab1 expansion");
  Expect(viewport.lines()[0] == "x y", "both placeholders expand in place of trigger");

  // Three successive mirrored keystrokes. After the first edit the right-hand
  // placeholder shifts right; if its recorded range is not advanced, the second
  // and third edits land at the wrong column.
  Expect(SnippetTryInsertText(viewport, session, "!"), "first mirrored keystroke");
  Expect(SnippetTryInsertText(viewport, session, "?"), "second mirrored keystroke");
  Expect(SnippetTryInsertText(viewport, session, "."), "third mirrored keystroke");
  Expect(viewport.lines()[0] == "x!?. y!?.",
         "successive linked edits must keep mirroring at the correct column in every occurrence");
}

void TestSnippetChoiceTabCycles() {
  TextViewport viewport;
  viewport.LoadContent("z", "/tmp/snippet.cpp");
  viewport.MoveCursorTo(0, 0);
  SnippetSessionState session;
  viewport.BeginUndoGroup();
  SelectionRange trigger{{0, 0}, {0, 1}};
  Expect(ExpandSnippetAtSelection(viewport, session, trigger, "${1:|aa,bb|}$0"), "choice snippet");
  Expect(viewport.lines()[0] == "aa", "first choice replaces trigger range");
  Expect(SnippetNavigateTab(viewport, session, false), "tab should cycle choice or advance");
  Expect(viewport.lines()[0] == "bb", "second choice should replace");
}

void TestSnippetExitWhenCaretLeavesPlaceholder() {
  TextViewport viewport;
  viewport.LoadContent("z", "/tmp/snippet.cpp");
  viewport.MoveCursorTo(0, 0);
  SnippetSessionState session;
  viewport.BeginUndoGroup();
  SelectionRange trigger{{0, 0}, {0, 0}};
  Expect(ExpandSnippetAtSelection(viewport, session, trigger, "${1:in}$0"), "expand");
  Expect(session.active, "session active while in placeholder");
  viewport.MoveCursorTo(0, 3);
  SnippetOnCaretMoved(viewport, session);
  Expect(!session.active, "moving caret out should commit session");
  Expect(!viewport.UndoGroupActive(), "commit should end undo group");
}

void TestSnippetExpansionSingleUndoStep() {
  TextViewport viewport;
  viewport.LoadContent("orig", "/tmp/snippet.cpp");
  viewport.MoveCursorTo(0, 0);
  SnippetSessionState session;
  viewport.BeginUndoGroup();
  SelectionRange trigger{{0, 0}, {0, 0}};
  Expect(ExpandSnippetAtSelection(viewport, session, trigger, "${1:a}$0"), "expand");
  Expect(viewport.Undo(), "first undo should revert grouped snippet edit");
  Expect(viewport.lines()[0] == "orig", "buffer restored");
  session.Reset(nullptr);
}

void TestSnippetEngineIgnoresInsertWhenSessionInactive() {
  TextViewport viewport;
  viewport.LoadContent("ok", "/tmp/snippet.cpp");
  SnippetSessionState session;
  Expect(!SnippetTryInsertText(viewport, session, "x"),
         "host should not route text through snippet path when session inactive (simulates "
         "workspace toggle / no session)");
}

void TestSnippetParseFallbackLeavesDollarLiteral() {
  const auto parsed = ParseSnippetBody("$}");
  Expect(parsed.expanded == "$}", "non-placeholder `$` sequences stay literal");
  Expect(parsed.occurrences.empty(), "no tab stops for invalid placeholder");
}

// Editing one tab stop must shift the recorded ranges of OTHER tab stops that sit
// later on the same line. Regression for the bug where only the *current* tab's
// ranges were shifted, so a second tab stop's stored column went stale and
// FocusTabStop later jumped to the wrong position.
void TestSnippetCrossTabShiftOnInsert() {
  TextViewport viewport;
  viewport.LoadContent("--", "/tmp/snippet.cpp");
  viewport.MoveCursorTo(0, 0);
  SnippetSessionState session;
  viewport.BeginUndoGroup();
  SelectionRange trigger{{0, 0}, {0, 2}};
  Expect(ExpandSnippetAtSelection(viewport, session, trigger, "${1:i} ${2:n}$0"),
         "two distinct tab stops on one line should expand");
  Expect(viewport.lines()[0] == "i n", "placeholders expand to 'i n'");
  Expect(session.ranges_by_tab[2].size() == 1u &&
             session.ranges_by_tab[2][0].start.column == 2u,
         "tab 2 starts at column 2 before tab 1 is edited");

  Expect(SnippetTryInsertText(viewport, session, "ndex"), "grow $1 from 'i' to 'index'");
  Expect(viewport.lines()[0] == "index n", "typing into $1 grows it in place: 'index n'");
  Expect(session.ranges_by_tab[2][0].start.column == 6u,
         "editing tab 1 must shift tab 2's recorded start (2 -> 6), so FocusTabStop "
         "later lands on the moved placeholder, not a stale column");
}

void TestSnippetCrossTabShiftOnBackspace() {
  TextViewport viewport;
  viewport.LoadContent("--", "/tmp/snippet.cpp");
  viewport.MoveCursorTo(0, 0);
  SnippetSessionState session;
  viewport.BeginUndoGroup();
  SelectionRange trigger{{0, 0}, {0, 2}};
  Expect(ExpandSnippetAtSelection(viewport, session, trigger, "${1:index} ${2:n}$0"),
         "two tab stops on one line should expand");
  Expect(viewport.lines()[0] == "index n", "placeholders expand to 'index n'");
  Expect(session.ranges_by_tab[2][0].start.column == 6u, "tab 2 starts at column 6");

  // Caret sits at the end of $1 ('index'); backspace removes the trailing 'x'.
  Expect(SnippetTryBackspace(viewport, session), "backspace inside $1 should delete one char");
  Expect(viewport.lines()[0] == "inde n", "backspace shrinks $1 to 'inde'");
  Expect(session.ranges_by_tab[2][0].start.column == 5u,
         "deleting in tab 1 must shift tab 2's recorded start (6 -> 5), not leave it stale");
}

void TestSnippetCrossTabShiftOnChoice() {
  TextViewport viewport;
  viewport.LoadContent("--", "/tmp/snippet.cpp");
  viewport.MoveCursorTo(0, 0);
  SnippetSessionState session;
  viewport.BeginUndoGroup();
  SelectionRange trigger{{0, 0}, {0, 2}};
  Expect(ExpandSnippetAtSelection(viewport, session, trigger, "${1:|short,longer|} ${2:x}$0"),
         "a choice tab stop plus a second tab stop should expand");
  Expect(viewport.lines()[0] == "short x", "first choice fills $1: 'short x'");
  Expect(session.ranges_by_tab[2][0].start.column == 6u, "tab 2 starts at column 6");

  // Cycling the choice grows $1 from 'short' (5) to 'longer' (6).
  Expect(SnippetNavigateTab(viewport, session, false), "cycling the choice should apply 'longer'");
  Expect(viewport.lines()[0] == "longer x", "second choice grows $1: 'longer x'");
  Expect(session.ranges_by_tab[2][0].start.column == 7u,
         "a choice that changes $1's length must shift tab 2's recorded start (6 -> 7)");
}

void TestSnippetLoneCarriageReturnBodyPositions() {
  TextViewport viewport;
  viewport.LoadContent("--", "/tmp/snippet.cpp");
  viewport.MoveCursorTo(0, 0);
  SnippetSessionState session;
  viewport.BeginUndoGroup();
  SelectionRange trigger{{0, 0}, {0, 2}};
  Expect(ExpandSnippetAtSelection(viewport, session, trigger, "${1:a}\r${2:b}$0"),
         "snippet body with a lone CR should expand");
  // ReplaceRange normalizes the lone CR to a newline, so the body spans two lines.
  Expect(viewport.lines().size() == 2 && viewport.lines()[0] == "a" && viewport.lines()[1] == "b",
         "a lone CR should split the inserted text into two lines");
  Expect(session.ranges_by_tab[2].size() == 1u &&
             session.ranges_by_tab[2][0].start.line == 1u &&
             session.ranges_by_tab[2][0].start.column == 0u,
         "tab 2 must map onto the normalized second line, not a stale column on line 0");
}

// Backspace inside a mirrored placeholder that holds a multi-byte code point (é)
// must delete the WHOLE code point in every mirror, never a single byte. The old
// code deleted exactly one byte and decremented end.column by 1, splitting the
// two-byte 'é' and corrupting the buffer / desyncing the mirrors.
void TestSnippetUtf8BackspaceDeletesWholeCodepoint() {
  TextViewport viewport;
  viewport.LoadContent("--", "/tmp/snippet.cpp");
  viewport.MoveCursorTo(0, 0);
  SnippetSessionState session;
  viewport.BeginUndoGroup();
  SelectionRange trigger{{0, 0}, {0, 2}};
  Expect(ExpandSnippetAtSelection(viewport, session, trigger, "${1:\xC3\xA9}-${1:\xC3\xA9}$0"),
         "mirrored placeholder containing 'é' should expand");
  Expect(viewport.lines()[0] == "\xC3\xA9-\xC3\xA9", "expands to 'é-é'");
  Expect(IsValidUtf8(viewport.lines()[0]), "expanded buffer is valid UTF-8");
  Expect(session.ranges_by_tab[1].size() == 2u, "two mirrored placeholders");

  // Caret sits at the end of the first 'é' (byte column 2). Backspace must remove
  // both bytes of the code point across BOTH mirrors.
  Expect(SnippetTryBackspace(viewport, session), "backspace inside mirrored 'é'");
  Expect(viewport.lines()[0] == "-",
         "backspace deletes the whole 'é' from every mirror, leaving just '-'");
  Expect(IsValidUtf8(viewport.lines()[0]), "buffer stays valid UTF-8 after backspace");
  Expect(session.ranges_by_tab[1][0].start.column == session.ranges_by_tab[1][0].end.column &&
             session.ranges_by_tab[1][1].start.column == session.ranges_by_tab[1][1].end.column,
         "both mirrors collapse to empty and stay column-consistent (no half-byte residue)");
}

// Delete-forward on a multi-byte code point at the caret must likewise remove the
// whole code point in every mirror, leaving trailing placeholder text intact.
void TestSnippetUtf8DeleteForwardDeletesWholeCodepoint() {
  TextViewport viewport;
  viewport.LoadContent("--", "/tmp/snippet.cpp");
  viewport.MoveCursorTo(0, 0);
  SnippetSessionState session;
  viewport.BeginUndoGroup();
  SelectionRange trigger{{0, 0}, {0, 2}};
  Expect(ExpandSnippetAtSelection(viewport, session, trigger, "${1:\xC3\xA9z}-${1:\xC3\xA9z}$0"),
         "mirrored placeholder 'éz' should expand");
  Expect(viewport.lines()[0] == "\xC3\xA9z-\xC3\xA9z", "expands to 'éz-éz'");

  // Put the caret at the start of the first placeholder so delete-forward targets
  // the leading 'é'.
  viewport.MoveCursorTo(0, 0);
  Expect(SnippetTryDeleteForward(viewport, session), "delete-forward on leading 'é'");
  Expect(viewport.lines()[0] == "z-z",
         "delete-forward removes the whole 'é' from every mirror, keeping the trailing 'z'");
  Expect(IsValidUtf8(viewport.lines()[0]), "buffer stays valid UTF-8 after delete-forward");
  Expect(session.ranges_by_tab[1][0].end.column - session.ranges_by_tab[1][0].start.column == 1u,
         "first mirror now holds the single-byte 'z'");
  Expect(session.ranges_by_tab[1][1].end.column - session.ranges_by_tab[1][1].start.column == 1u,
         "second mirror stays consistent and holds 'z'");
}

// A multi-line paste inside a placeholder must not use the single-line mirror fast
// path: the engine declines (returns false) so the host does a normal insert and
// following tab stops keep their line/column ranges rather than going stale.
void TestSnippetMultiLineInsertDeclinesFastPath() {
  TextViewport viewport;
  viewport.LoadContent("--", "/tmp/snippet.cpp");
  viewport.MoveCursorTo(0, 0);
  SnippetSessionState session;
  viewport.BeginUndoGroup();
  SelectionRange trigger{{0, 0}, {0, 2}};
  Expect(ExpandSnippetAtSelection(viewport, session, trigger, "${1:i} ${2:n}$0"),
         "two tab stops on one line should expand");
  Expect(viewport.lines()[0] == "i n", "placeholders expand to 'i n'");
  Expect(session.ranges_by_tab[2][0].start.column == 2u, "tab 2 starts at column 2");

  // A payload with a newline must be rejected by the snippet fast path, unchanged
  // buffer, unchanged recorded ranges — no double insert, no stale tab stops.
  Expect(!SnippetTryInsertText(viewport, session, "x\ny"),
         "multi-line insert must decline the mirror fast path");
  Expect(viewport.lines().size() == 1 && viewport.lines()[0] == "i n",
         "declined insert leaves the buffer untouched (host does the real insert)");
  Expect(session.ranges_by_tab[2][0].start.column == 2u,
         "tab 2's recorded range is not corrupted by the rejected multi-line insert");
}

// A snippet body with an enormous tab-stop id must not signed-overflow the id
// accumulator; the parse fails cleanly to an empty result.
void TestSnippetParseRejectsHugeTabId() {
  const auto parsed = ParseSnippetBody("${999999999:x}");
  Expect(parsed.occurrences.empty() && parsed.expanded.empty(),
         "an out-of-range tab-stop id is rejected without overflow");
}

// Thousands of placeholders exceed the occurrence cap and are rejected cleanly.
void TestSnippetParseRejectsTooManyPlaceholders() {
  std::string body;
  for (int i = 0; i < 5000; ++i) {
    body += "$1";
  }
  const auto parsed = ParseSnippetBody(body);
  Expect(parsed.occurrences.empty() && parsed.expanded.empty(),
         "a body past the placeholder cap is rejected without unbounded growth");
}

// Hundreds of choices in one placeholder exceed the per-placeholder choice cap.
void TestSnippetParseRejectsTooManyChoices() {
  std::string body = "${1:|";
  for (int i = 0; i < 400; ++i) {
    body += "a,";
  }
  body += "z|}";
  const auto parsed = ParseSnippetBody(body);
  Expect(parsed.occurrences.empty() && parsed.expanded.empty(),
         "a placeholder past the choice cap is rejected without unbounded growth");
}

// Regression: a choice value containing a newline is single-line-unsafe (cycling
// to it via ApplyChoiceForTab records an off-line range and orphans the wrapped
// text on the next cycle). The parser must reject such a snippet outright so the
// corrupting multi-line choice never enters a session.
void TestSnippetParseRejectsNewlineInChoice() {
  const auto lf = ParseSnippetBody("${1:|a,b\nc|}$0");
  Expect(lf.occurrences.empty() && lf.expanded.empty(),
         "a choice containing a newline must be rejected");
  const auto cr = ParseSnippetBody("${1:|a,b\rc|}$0");
  Expect(cr.occurrences.empty() && cr.expanded.empty(),
         "a choice containing a carriage return must be rejected");
  // A normal single-line choice still parses.
  const auto ok = ParseSnippetBody("${1:|aa,bb|}$0");
  Expect(!ok.occurrences.empty(), "a single-line choice must still parse");
}

}  // namespace

void RegisterEditorSnippetTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorSnippet/ParseRejectsNewlineInChoice",
          TestSnippetParseRejectsNewlineInChoice);
  AddTest(tests, "EditorSnippet/CrossTabShiftOnInsert", TestSnippetCrossTabShiftOnInsert);
  AddTest(tests, "EditorSnippet/CrossTabShiftOnBackspace", TestSnippetCrossTabShiftOnBackspace);
  AddTest(tests, "EditorSnippet/CrossTabShiftOnChoice", TestSnippetCrossTabShiftOnChoice);
  AddTest(tests, "EditorSnippet/LoneCarriageReturnBodyPositions",
          TestSnippetLoneCarriageReturnBodyPositions);
  AddTest(tests, "EditorSnippet/SimpleExpansion", TestSnippetSimpleExpansion);
  AddTest(tests, "EditorSnippet/MultiOccurrenceLinkedTab", TestSnippetMultiOccurrenceLinkedTab);
  AddTest(tests, "EditorSnippet/MultiOccurrenceLinkedTabMultiKeystroke",
          TestSnippetMultiOccurrenceLinkedTabMultiKeystroke);
  AddTest(tests, "EditorSnippet/ChoiceTabCycles", TestSnippetChoiceTabCycles);
  AddTest(tests, "EditorSnippet/ExitWhenCaretLeavesPlaceholder",
          TestSnippetExitWhenCaretLeavesPlaceholder);
  AddTest(tests, "EditorSnippet/ExpansionSingleUndoStep", TestSnippetExpansionSingleUndoStep);
  AddTest(tests, "EditorSnippet/InsertIgnoredWhenSessionInactive",
          TestSnippetEngineIgnoresInsertWhenSessionInactive);
  AddTest(tests, "EditorSnippet/ParseFallbackDollarLiteral", TestSnippetParseFallbackLeavesDollarLiteral);
  AddTest(tests, "EditorSnippet/Utf8BackspaceDeletesWholeCodepoint",
          TestSnippetUtf8BackspaceDeletesWholeCodepoint);
  AddTest(tests, "EditorSnippet/Utf8DeleteForwardDeletesWholeCodepoint",
          TestSnippetUtf8DeleteForwardDeletesWholeCodepoint);
  AddTest(tests, "EditorSnippet/MultiLineInsertDeclinesFastPath",
          TestSnippetMultiLineInsertDeclinesFastPath);
  AddTest(tests, "EditorSnippet/ParseRejectsHugeTabId", TestSnippetParseRejectsHugeTabId);
  AddTest(tests, "EditorSnippet/ParseRejectsTooManyPlaceholders",
          TestSnippetParseRejectsTooManyPlaceholders);
  AddTest(tests, "EditorSnippet/ParseRejectsTooManyChoices",
          TestSnippetParseRejectsTooManyChoices);
}

}  // namespace microide::tests
