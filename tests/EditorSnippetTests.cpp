#include "TestSupport.h"

#include "editor/SnippetEngine.h"
#include "editor/TextViewport.h"

#include <string>

namespace microide::tests {
namespace {

using microide::editor::SelectionRange;
using microide::editor::SnippetSessionState;
using microide::editor::SnippetOnCaretMoved;
using microide::editor::SnippetNavigateTab;
using microide::editor::SnippetTryInsertText;
using microide::editor::ExpandSnippetAtSelection;
using microide::editor::ParseSnippetBody;
using microide::editor::TextViewport;

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

}  // namespace

void RegisterEditorSnippetTests(std::vector<TestCase>& tests) {
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
}

}  // namespace microide::tests
