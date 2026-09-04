#include "TestSupport.h"

#include "editor/SnippetEngine.h"
#include "editor/TextViewport.h"
#include "util/StringUtil.h"

#include <string>

namespace microide::tests {
namespace {

using microide::editor::SelectionRange;
using microide::editor::SnippetSessionState;
using microide::editor::CommitSnippetSession;
using microide::editor::SnippetOnCaretMoved;
using microide::editor::SnippetNavigateTab;
using microide::editor::TextPosition;
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
  viewport.ClearSelection();
  viewport.MoveCursorTo(0, 1);
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
  viewport.ClearSelection();
  viewport.MoveCursorTo(0, 1);
  Expect(SnippetTryInsertText(viewport, session, "!"), "first mirrored keystroke");
  Expect(SnippetTryInsertText(viewport, session, "?"), "second mirrored keystroke");
  Expect(SnippetTryInsertText(viewport, session, "."), "third mirrored keystroke");
  Expect(viewport.lines()[0] == "x!?. y!?.",
         "successive linked edits must keep mirroring at the correct column in every occurrence");
}

// TD-2026-08-06-159: the batched mirror shift keeps its per-line prefix sums in
// ONE sorted vector rather than a hash map per line, so a snippet whose mirrors
// and other tab stops span several lines is the case that says whether the
// line-boundary handling is right — a lookup that walks past its own line's run
// would apply another line's accumulated shift.
void TestSnippetMirrorShiftAcrossLines() {
  TextViewport viewport;
  viewport.LoadContent("--", "/tmp/snippet-lines.cpp");
  viewport.MoveCursorTo(0, 0);
  SnippetSessionState session;
  viewport.BeginUndoGroup();
  SelectionRange trigger{{0, 0}, {0, 2}};
  // Tab 1 has two mirrors on line 0 and two on line 2; tab 2 sits after a mirror
  // on each of those lines, so both must follow their OWN line's shift.
  Expect(ExpandSnippetAtSelection(viewport, session, trigger,
                                  "$1 $1 ${2:a}\nplain\n$1 $1 ${3:b}$0"),
         "a multi-line snippet with mirrors on two lines should expand");
  Expect(viewport.lines()[0] == "  a" && viewport.lines()[1] == "plain" &&
             viewport.lines()[2] == "  b",
         "the expansion lays out three lines with empty tab-1 occurrences");

  Expect(SnippetTryInsertText(viewport, session, "XY"), "typing into tab 1 mirrors everywhere");
  Expect(viewport.lines()[0] == "XY XY a", "both line-0 mirrors take the text");
  Expect(viewport.lines()[2] == "XY XY b", "both line-2 mirrors take the text");
  Expect(viewport.lines()[1] == "plain", "the untouched line between them is unchanged");

  // Each downstream tab stop moved by its own line's total (two mirrors x 2 chars),
  // not by the sum across both lines.
  Expect(session.ranges_by_tab[2].size() == 1u && session.ranges_by_tab[2][0].start.line == 0u &&
             session.ranges_by_tab[2][0].start.column == 6u,
         "tab 2 follows line 0's shift only");
  Expect(session.ranges_by_tab[3].size() == 1u && session.ranges_by_tab[3][0].start.line == 2u &&
             session.ranges_by_tab[3][0].start.column == 6u,
         "tab 3 follows line 2's shift only");

  // A second keystroke lands at the shifted columns in all four mirrors.
  Expect(SnippetTryInsertText(viewport, session, "Z"), "second mirrored keystroke");
  Expect(viewport.lines()[0] == "XYZ XYZ a" && viewport.lines()[2] == "XYZ XYZ b",
         "successive edits keep mirroring at the correct column on both lines");
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

// Focusing a placeholder selects its default text, and a keystroke REPLACES the
// selection — VS Code's behaviour, and the whole point of a default: `foo(${1:int
// x})` is typed over, not appended to. The engine used to insert at the caret
// (the selection's end) and then re-select the whole field after every keystroke,
// so "int x" became "int xy" and the field stayed selected as you typed.
void TestSnippetTypingReplacesSelectedDefault() {
  TextViewport viewport;
  viewport.LoadContent("", "/tmp/snippet.cpp");
  SnippetSessionState session;
  viewport.BeginUndoGroup();
  SelectionRange trigger{{0, 0}, {0, 0}};
  Expect(ExpandSnippetAtSelection(viewport, session, trigger, "${1:foo} and ${1:foo}$0"),
         "expands");
  Expect(viewport.has_selection(), "the focused placeholder's default is selected");
  Expect(SnippetTryInsertText(viewport, session, "x"), "typing routes through the session");
  Expect(viewport.lines()[0] == "x and x", "the keystroke replaces the default in every mirror");
  Expect(!viewport.has_selection(), "the caret is collapsed after the keystroke");
  Expect(viewport.cursor_column() == 1u, "the caret sits after the typed text");
  Expect(SnippetTryInsertText(viewport, session, "y"), "a second keystroke appends");
  Expect(viewport.lines()[0] == "xy and xy", "the second keystroke extends what was typed");
  Expect(session.ranges_by_tab[1][1].start.column == 7u &&
             session.ranges_by_tab[1][1].end.column == 9u,
         "the second mirror's range follows the replacement and the append");

  // A selection inside the field is replaced too; Backspace and Delete remove it.
  viewport.MoveCursorTo(0, 0, false);
  viewport.MoveCursorTo(0, 2, true);
  Expect(SnippetTryBackspace(viewport, session), "backspace over a field selection");
  Expect(viewport.lines()[0] == " and ", "backspace removes the selected field text everywhere");
  Expect(SnippetTryInsertText(viewport, session, "ab"), "retype");
  viewport.MoveCursorTo(0, 0, false);
  viewport.MoveCursorTo(0, 1, true);
  Expect(SnippetTryDeleteForward(viewport, session), "delete over a field selection");
  Expect(viewport.lines()[0] == "b and b", "delete removes the selected field text everywhere");
  Expect(viewport.cursor_column() == 0u, "the caret lands at the removed span's start");
  Expect(session.active, "the session survives the edits");

  // Backspace with the caret at the field's start is not a field edit.
  Expect(!SnippetTryBackspace(viewport, session),
         "backspace at the field's start falls back to an ordinary backspace");
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

  // Collapse the focus selection onto the field's end so the keystroke appends
  // (a keystroke over the selected default replaces it).
  viewport.ClearSelection();
  viewport.MoveCursorTo(0, 1);
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

  // Caret collapsed at the end of $1 ('index'); backspace removes the trailing 'x'.
  viewport.ClearSelection();
  viewport.MoveCursorTo(0, 5);
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

  // A payload with a newline must be declined by the snippet fast path (no double
  // insert — the host performs the real insert), and the linked-edit session must
  // be ended so it cannot retain ranges computed for the pre-newline document and
  // later navigate/mirror against stale line/column positions.
  Expect(!SnippetTryInsertText(viewport, session, "x\ny"),
         "multi-line insert must decline the mirror fast path");
  Expect(viewport.lines().size() == 1 && viewport.lines()[0] == "i n",
         "declined insert leaves the buffer untouched (host does the real insert)");
  Expect(!session.active,
         "the snippet session must be dropped on a multi-line insert, not left with stale ranges");
}

// Pre-expansion secondary carets are consumed by the snippet, not restored at
// stale offsets on commit. Restoring the saved pre-snippet positions after the
// replacement (and field edits) shifted the buffer would leave carets at wrong
// locations; the sanctioned behavior is to discard them.
void TestSnippetDiscardsPreExpansionSecondaryCarets() {
  TextViewport viewport;
  viewport.LoadContent("word\nsecond", "/tmp/snippet.cpp");
  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{1, 0}});
  Expect(viewport.secondary_carets().size() == 1, "precondition: one secondary caret");

  SnippetSessionState session;
  viewport.BeginUndoGroup();
  SelectionRange trigger{{0, 0}, {0, 4}};  // replace "word"
  Expect(ExpandSnippetAtSelection(viewport, session, trigger, "${1:x}$0"),
         "snippet expands over the trigger");
  CommitSnippetSession(viewport, session);

  Expect(viewport.secondary_carets().empty(),
         "pre-expansion secondary carets are discarded on commit, not restored at stale offsets");
}

// Regression: a bare `$N` tab stop must read ALL consecutive digits, so `$10` is
// tab stop 10 (VSCode) rather than tab stop 1 followed by a literal '0'. Before
// the fix only the braced `${N}` form parsed multi-digit ids; the bare form read
// a single digit and leaked the remaining digits into the expanded text.
void TestSnippetParseBareTabStopReadsAllDigits() {
  const auto parsed = ParseSnippetBody("a$10b");
  Expect(parsed.expanded == "ab",
         "the multi-digit id must not leak digits into the expanded text");
  Expect(parsed.occurrences.size() == 1 && parsed.occurrences.front().tab_stop == 10,
         "a bare $10 is tab stop 10, matching the braced ${10} form and VSCode");

  // A bare final stop `$0` still resolves, and `$2$10` yields two distinct stops
  // (2 and 10), not (2, 1, 0).
  const auto two = ParseSnippetBody("$2$10");
  Expect(two.occurrences.size() == 2 && two.occurrences[0].tab_stop == 2 &&
             two.occurrences[1].tab_stop == 10,
         "adjacent bare tab stops each consume their own full digit run");
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

// Regression: VSCode-style escapes inside a placeholder must insert the literal
// delimiter instead of terminating early. Previously the parser walked to the
// first raw `}`, `,`, or `|`, truncating any default/choice that contained one.
void TestSnippetParseHonorsEscapedDelimiters() {
  // Escaped brace in default text: `${1:a\}b}` -> default "a}b".
  const auto brace = ParseSnippetBody("${1:a\\}b}");
  Expect(brace.occurrences.size() == 1 && brace.expanded == "a}b",
         "an escaped } in default text is literal, not a terminator");

  // Escaped dollar in default text: `${1:price \$5}` -> "price $5".
  const auto dollar = ParseSnippetBody("${1:price \\$5}");
  Expect(dollar.expanded == "price $5", "an escaped $ is literal in default text");

  // Escaped comma inside a choice yields the two options "a,b" and "c".
  const auto choice = ParseSnippetBody("${1:|a\\,b,c|}");
  Expect(choice.occurrences.size() == 1 && choice.occurrences[0].choices.size() == 2 &&
             choice.occurrences[0].choices[0] == "a,b" &&
             choice.occurrences[0].choices[1] == "c",
         "an escaped comma stays inside the choice value");
}

// The batched mirror shift (TD-2026-07-17A-060: O(mirrors * placeholders) ->
// O(mirrors + placeholders) per keystroke) must reproduce the old per-edit
// ShiftPlaceholdersAtOrAfter result exactly at scale. Expand a snippet with many
// mirrors of one tab on a single line, plus a trailing distinct tab stop, then
// type into the linked tab: every mirror must grow in place and the trailing tab
// stop's recorded start must shift by the aggregate of all mirror insertions.
void TestSnippetManyMirrorBatchedShift() {
  TextViewport viewport;
  viewport.LoadContent("--", "/tmp/snippet-many-mirror.cpp");
  viewport.MoveCursorTo(0, 0);
  SnippetSessionState session;
  viewport.BeginUndoGroup();

  constexpr int kMirrors = 200;
  std::string body;
  for (int i = 0; i < kMirrors; ++i) {
    body += "${1:a}";  // each mirror expands to a single 'a' at columns 0..N-1
  }
  body += "${2:z}$0";  // a distinct tab stop right after the mirrors

  SelectionRange trigger{{0, 0}, {0, 2}};
  Expect(ExpandSnippetAtSelection(viewport, session, trigger, body),
         "a snippet with hundreds of linked mirrors should expand");
  Expect(session.ranges_by_tab[1].size() == static_cast<std::size_t>(kMirrors),
         "every mirror occurrence of tab 1 should be recorded");
  Expect(session.ranges_by_tab[2][0].start.column == static_cast<std::size_t>(kMirrors),
         "tab 2 starts right after the N single-char mirrors");

  // Tab 1's first occurrence is focused with its default 'a' selected. Typing
  // "bb" replaces the 'a' in every mirror; each mirror grows by 1 and tab 2
  // shifts by N.
  Expect(SnippetTryInsertText(viewport, session, "bb"),
         "typing into the linked tab should edit every mirror");

  bool mirrors_ok = true;
  for (int i = 0; i < kMirrors; ++i) {
    // Mirror i now occupies [2i, 2i+2] holding "bb".
    const SelectionRange& r = session.ranges_by_tab[1][static_cast<std::size_t>(i)];
    if (r.start.column != static_cast<std::size_t>(2 * i) ||
        r.end.column != static_cast<std::size_t>(2 * i + 2)) {
      mirrors_ok = false;
      break;
    }
  }
  Expect(mirrors_ok, "every mirror must grow in place to [2i, 2i+2] after the batched shift");
  Expect(session.ranges_by_tab[2][0].start.column == static_cast<std::size_t>(2 * kMirrors),
         "tab 2's recorded start must shift by the aggregate of all mirror edits "
         "(N -> 2N), not a stale column");
  Expect(viewport.lines()[0].size() == static_cast<std::size_t>(2 * kMirrors + 1),
         "the line holds N 'bb' mirrors plus the single-char tab 2");
}

}  // namespace

void RegisterEditorSnippetTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorSnippet/TypingReplacesSelectedDefault",
          TestSnippetTypingReplacesSelectedDefault);
  AddTest(tests, "EditorSnippet/ManyMirrorBatchedShift", TestSnippetManyMirrorBatchedShift);
  AddTest(tests, "EditorSnippet/ParseHonorsEscapedDelimiters",
          TestSnippetParseHonorsEscapedDelimiters);
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
  AddTest(tests, "EditorSnippet/MirrorShiftAcrossLines", TestSnippetMirrorShiftAcrossLines);
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
  AddTest(tests, "EditorSnippet/DiscardsPreExpansionSecondaryCarets",
          TestSnippetDiscardsPreExpansionSecondaryCarets);
  AddTest(tests, "EditorSnippet/ParseBareTabStopReadsAllDigits",
          TestSnippetParseBareTabStopReadsAllDigits);
  AddTest(tests, "EditorSnippet/ParseRejectsHugeTabId", TestSnippetParseRejectsHugeTabId);
  AddTest(tests, "EditorSnippet/ParseRejectsTooManyPlaceholders",
          TestSnippetParseRejectsTooManyPlaceholders);
  AddTest(tests, "EditorSnippet/ParseRejectsTooManyChoices",
          TestSnippetParseRejectsTooManyChoices);
}

}  // namespace microide::tests
