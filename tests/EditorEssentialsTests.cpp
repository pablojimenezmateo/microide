#include "TestSupport.h"

#include "editor/BracketScanner.h"
#include "editor/FoldingModel.h"
#include "editor/IndentDetect.h"
#include "editor/IndentGuides.h"
#include "editor/LanguageContractView.h"
#include "editor/ShapingActions.h"
#include "editor/TextViewport.h"
#include "plugin/PluginHost.h"
#include "workspace/WorkspaceFoldingRefresh.h"
#include "workspace/WorkspaceIndentDetectApply.h"
#include "workspace/WorkspaceLanguageContract.h"
#include "workspace/WorkspaceSaveNormalization.h"
#include "workspace/WorkspaceTabState.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::TextPosition;
using microide::editor::TextViewport;

void TestBracketScannerForwardMatch() {
  TextViewport viewport;
  viewport.LoadContent("if (a) {\n  return 1;\n}\n", "/tmp/sample.cpp");
  viewport.MoveCursorTo(0, 7);  // caret right after '{' opener (line 0, col 7 = '{')
  auto match = microide::editor::FindBracketMatch(viewport, viewport.cursor_line(),
                                                  viewport.cursor_column());
  Expect(match.has_value(), "expected a bracket match for '{' on line 0");
  Expect(match->open_line == 0 && match->open_column == 7,
         "open should be at line 0 col 7");
  Expect(match->close_line == 2 && match->close_column == 0,
         "close should be at line 2 col 0");
}

void TestBracketScannerBackwardMatch() {
  TextViewport viewport;
  viewport.LoadContent("{\n  x;\n}\n", "/tmp/sample.cpp");
  viewport.MoveCursorTo(2, 1);  // caret right after '}'
  auto match = microide::editor::FindBracketMatch(viewport, 2, 0);
  Expect(match.has_value(), "expected a backward match for '}'");
  Expect(match->open_line == 0 && match->open_column == 0,
         "backward match should land on line 0 col 0");
}

void TestBracketScannerNoMatchOnUnbalanced() {
  TextViewport viewport;
  viewport.LoadContent("{ no closer\n", "/tmp/sample.cpp");
  auto match = microide::editor::FindBracketMatch(viewport, 0, 0);
  Expect(!match.has_value(), "unbalanced opener should not match");
}

void TestShapingMoveLineDown() {
  TextViewport viewport;
  viewport.LoadContent("a\nb\nc\n", "/tmp/sample.txt");
  viewport.MoveCursorTo(0, 0);
  Expect(microide::editor::MoveLineDown(viewport),
         "MoveLineDown on a non-last line should change buffer");
  Expect(viewport.lines().size() >= 3, "lines preserved");
  Expect(viewport.lines()[0] == "b" && viewport.lines()[1] == "a" &&
             viewport.lines()[2] == "c",
         "first two lines should swap on MoveLineDown");
}

void TestShapingMoveLineUpAtTopIsNoop() {
  TextViewport viewport;
  viewport.LoadContent("a\nb\n", "/tmp/sample.txt");
  viewport.MoveCursorTo(0, 0);
  Expect(!microide::editor::MoveLineUp(viewport),
         "MoveLineUp at top should be a no-op and return false");
}

void TestShapingDuplicateLine() {
  TextViewport viewport;
  viewport.LoadContent("hello\n", "/tmp/sample.txt");
  viewport.MoveCursorTo(0, 0);
  Expect(microide::editor::DuplicateSelection(viewport),
         "DuplicateSelection should produce a change");
  Expect(viewport.lines().size() >= 2, "duplicate should add a line");
  Expect(viewport.lines()[0] == "hello" && viewport.lines()[1] == "hello",
         "first two lines should both be 'hello'");
}

void TestShapingIndentSelection() {
  TextViewport viewport;
  viewport.LoadContent("a\nb\n", "/tmp/sample.txt");
  viewport.SetIndentWidth(2);
  viewport.SetSoftTabs(true);
  viewport.MoveCursorTo(0, 0);
  // Build a 2-line selection by setting selection through anchor + cursor
  viewport.MoveCursorTo(0, 0, false);
  viewport.MoveCursorTo(1, 1, true);
  Expect(microide::editor::IndentSelection(viewport),
         "IndentSelection should change the buffer");
  Expect(viewport.lines()[0].rfind("  ", 0) == 0,
         "first line should now start with two-space indent");
  Expect(viewport.lines()[1].rfind("  ", 0) == 0,
         "second line should now start with two-space indent");
}

void TestShapingToggleLineComment() {
  TextViewport viewport;
  viewport.LoadContent("foo\nbar\n", "/tmp/sample.cpp");
  viewport.MoveCursorTo(0, 0);
  Expect(microide::editor::ToggleLineComment(viewport, "//"),
         "first toggle should comment a non-blank line");
  Expect(viewport.lines()[0].rfind("// ", 0) == 0,
         "line should start with the // marker after toggle");
  // Toggle again to uncomment.
  viewport.MoveCursorTo(0, 0);
  Expect(microide::editor::ToggleLineComment(viewport, "//"),
         "second toggle should uncomment");
  Expect(viewport.lines()[0] == "foo",
         "second toggle should restore the original text");
}

void TestShapingSortLinesAscending() {
  TextViewport viewport;
  viewport.LoadContent("c\nb\na\n", "/tmp/sample.txt");
  viewport.MoveCursorTo(0, 0, false);
  viewport.MoveCursorTo(2, 1, true);
  Expect(microide::editor::SortLines(viewport, /*ascending=*/true),
         "SortLines on a multi-line range should change buffer");
  Expect(viewport.lines()[0] == "a" && viewport.lines()[1] == "b" &&
             viewport.lines()[2] == "c",
         "sort ascending should reorder lines");
}

void TestTextViewportSaveAppliesNormalization() {
  // Setup: write a file with trailing whitespace and no final newline.
  TemporaryDirectory tmp;
  std::filesystem::path path = tmp.path() / "sample.txt";
  WriteFile(path, "foo  \nbar\t\nbaz");
  TextViewport viewport;
  Expect(viewport.OpenFile(path), "OpenFile should succeed");
  viewport.SetSaveTrimTrailingWhitespace(true);
  viewport.SetSaveEnsureFinalNewline(true);
  Expect(viewport.Save(), "Save should succeed");
  std::string after = ReadFile(path);
  Expect(after == "foo\nbar\nbaz\n",
         "saved file should be trimmed and end with a single newline");
}

void TestSaveNormalizationTrim() {
  std::vector<std::string> lines = {"foo  ", "bar\t", "baz"};
  Expect(microide::workspace::TrimTrailingWhitespace(lines),
         "trim should report change");
  Expect(lines[0] == "foo" && lines[1] == "bar" && lines[2] == "baz",
         "all trailing whitespace removed");
}

void TestSaveNormalizationEnsureFinalNewline() {
  std::vector<std::string> lines = {"foo"};
  Expect(microide::workspace::EnsureSingleFinalNewline(lines),
         "single content line without final newline should be normalized");
  Expect(lines.size() == 2 && lines[1].empty(),
         "buffer should end with one empty trailing line");

  // Multiple trailing newlines collapse to one.
  lines = {"foo", "", "", ""};
  Expect(microide::workspace::EnsureSingleFinalNewline(lines),
         "multiple trailing newlines should be collapsed");
  Expect(lines.size() == 2 && lines[0] == "foo" && lines[1].empty(),
         "trailing newlines should reduce to a single one");

  // Already correct: no change.
  lines = {"foo", ""};
  Expect(!microide::workspace::EnsureSingleFinalNewline(lines),
         "already-normalized buffer should be a no-op");
}

void TestIndentDetectSpacesMajority() {
  std::vector<std::string> lines = {
      "x = 1",
      "  if x:",
      "    y = 2",
      "    z = 3",
      "  else:",
      "    return",
  };
  auto detected = microide::editor::DetectIndent(lines);
  Expect(detected.detected, "indent should be detected");
  Expect(detected.soft_tabs, "majority spaces should yield soft_tabs=true");
  Expect(detected.indent_width == 2,
         "indent step should be detected as 2");
}

void TestIndentDetectTabsMajority() {
  std::vector<std::string> lines = {"\tfoo", "\tbar", "\tbaz"};
  auto detected = microide::editor::DetectIndent(lines);
  Expect(detected.detected, "tabs should be detected");
  Expect(!detected.soft_tabs, "majority tabs should yield soft_tabs=false");
}

void TestApplyDetectedIndentOnOpenUpdatesViewport() {
  editor::TextViewport viewport;
  viewport.LoadContent(
      "root\n"
      "    child\n"
      "        grand\n",
      "/tmp/sample.cpp");
  viewport.SetSoftTabs(false);
  viewport.SetIndentWidth(8);
  viewport.SetTabSize(8);

  const auto get_on = [](std::string_view id) -> std::optional<std::string> {
    if (id == "editor.indent.detect_on_open") {
      return std::string{"true"};
    }
    return std::nullopt;
  };
  microide::workspace::ApplyDetectedIndentAfterPreferences(viewport, get_on);
  Expect(viewport.soft_tabs(), "four-space sample should switch to soft tabs");
  Expect(viewport.indent_width() == 4 && viewport.tab_size() == 4,
         "indent and tab size should follow detection");
}

void TestApplyDetectedIndentOnOpenSkippedWhenDisabled() {
  editor::TextViewport viewport;
  viewport.LoadContent("\tone\n\t\ttwo\n", "/tmp/sample.cpp");
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(3);
  viewport.SetTabSize(3);

  const auto get_off = [](std::string_view id) -> std::optional<std::string> {
    if (id == "editor.indent.detect_on_open") {
      return std::string{"false"};
    }
    return std::nullopt;
  };
  microide::workspace::ApplyDetectedIndentAfterPreferences(viewport, get_off);
  Expect(viewport.soft_tabs() && viewport.indent_width() == 3,
         "disabled detection should leave prior viewport settings");
}

void TestApplyDetectedIndentOnOpenSkippedWhenPathEmpty() {
  editor::TextViewport viewport;
  viewport.LoadContent("    lone\n");
  viewport.SetIndentWidth(2);
  const auto get_on = [](std::string_view) -> std::optional<std::string> {
    return std::string{"true"};
  };
  microide::workspace::ApplyDetectedIndentAfterPreferences(viewport, get_on);
  Expect(viewport.indent_width() == 2, "untitled buffer should skip detection");
}

void TestLanguageContractDefaults() {
  microide::workspace::WorkspaceLanguageContract registry;
  const auto* cpp = registry.Find("cpp");
  Expect(cpp != nullptr, "cpp contract should exist by default");
  Expect(cpp->line_comment == "//",
         "cpp line comment should be '//'");
  Expect(!cpp->bracket_pairs.empty(),
         "cpp bracket pairs should be populated");

  const auto* py = registry.Find("python");
  Expect(py != nullptr, "python contract should exist by default");
  Expect(py->line_comment == "#",
         "python line comment should be '#'");
  Expect(py->indent_after_open_patterns.size() > 0,
         "python should have indent-after-open patterns including ':'");
}

void TestLanguageContractMissingLanguage() {
  microide::workspace::WorkspaceLanguageContract registry;
  Expect(registry.Find("totally-fictional") == nullptr,
         "unknown language id should resolve to nullptr");
  auto view = registry.ResolveView("totally-fictional");
  Expect(view.contract == nullptr,
         "unknown language id view should have null contract");
}

void TestIndentGuidesEmitsRunsAtNestedDepths() {
  using microide::editor::ComputeIndentGuides;
  using microide::editor::IndentGuideRun;

  std::vector<std::string> lines = {
      "",                  // 0
      "if (x) {",          // 1
      "    a();",          // 2 — depth 1
      "    if (y) {",      // 3 — depth 1
      "        b();",      // 4 — depth 2
      "    }",             // 5 — depth 1
      "}",                 // 6
  };
  std::vector<std::size_t> visible_rows{0, 1, 2, 3, 4, 5, 6};
  std::vector<IndentGuideRun> runs;
  ComputeIndentGuides(lines, visible_rows, /*tab_size=*/4, /*indent_width=*/4,
                      /*caret_line=*/SIZE_MAX, /*caret_leading=*/0, &runs);

  const auto count_at = [&](std::size_t column, std::size_t row) {
    return std::count_if(runs.begin(), runs.end(), [&](const IndentGuideRun& r) {
      return r.column == column && r.start_row == row;
    });
  };
  Expect(count_at(4, 2) == 1, "depth-1 line should produce a guide at column 4");
  Expect(count_at(4, 4) == 1 && count_at(8, 4) == 1,
         "depth-2 line should produce guides at columns 4 and 8");
  Expect(count_at(4, 6) == 0, "closing brace at column 0 should produce no guides");
}

void TestIndentGuidesMarksActiveAtParentColumn() {
  using microide::editor::ComputeIndentGuides;
  using microide::editor::IndentGuideRun;

  std::vector<std::string> lines = {
      "if (x) {",          // 0
      "    if (y) {",      // 1
      "        b();",      // 2 — caret here, depth 2 → parent at column 4
      "    }",             // 3
      "}",                 // 4
  };
  std::vector<std::size_t> visible_rows{0, 1, 2, 3, 4};
  std::vector<IndentGuideRun> runs;
  ComputeIndentGuides(lines, visible_rows, /*tab_size=*/4, /*indent_width=*/4,
                      /*caret_line=*/2, /*caret_leading=*/8, &runs);

  const auto active_for = [&](std::size_t column, std::size_t row) {
    const auto it = std::find_if(runs.begin(), runs.end(), [&](const IndentGuideRun& r) {
      return r.column == column && r.start_row == row;
    });
    return it != runs.end() && it->active;
  };
  Expect(active_for(4, 2),
         "the caret's parent indent column should be flagged active on the caret's line");
  Expect(!active_for(8, 2),
         "the caret's own indent column should not be marked active on the caret's line");
}

void TestLanguageContractAppliesUserOverrides() {
  microide::workspace::WorkspaceLanguageContract registry;
  microide::plugin::PluginHost host;
  const auto getter = [](std::string_view id) -> std::optional<std::string> {
    if (id == "editor.brackets.user_pairs") return std::string("<|>");
    if (id == "editor.brackets.user_disabled") return std::string("[|]");
    if (id == "editor.comments.user_line") return std::string(";;");
    if (id == "editor.indents.user_open_patterns") return std::string("=>,then");
    return std::nullopt;
  };
  registry.Refresh(host, getter);

  const auto* cpp = registry.Find("cpp");
  Expect(cpp != nullptr, "cpp contract should still exist after override-driven refresh");

  const bool added_pair = std::any_of(
      cpp->bracket_pairs.begin(), cpp->bracket_pairs.end(),
      [](const auto& p) { return p.open == "<" && p.close == ">"; });
  Expect(added_pair, "user_pairs override should append <|> to the bracket set");

  const bool removed_pair = std::any_of(
      cpp->bracket_pairs.begin(), cpp->bracket_pairs.end(),
      [](const auto& p) { return p.open == "[" && p.close == "]"; });
  Expect(!removed_pair, "user_disabled override should remove [|] from the bracket set");

  Expect(cpp->line_comment == ";;",
         "user_line override should replace the line-comment marker globally");

  const bool added_pattern = std::find(cpp->indent_after_open_patterns.begin(),
                                       cpp->indent_after_open_patterns.end(),
                                       "=>") != cpp->indent_after_open_patterns.end();
  Expect(added_pattern, "user_open_patterns override should add custom indent suffixes");
}

microide::editor::LanguageContractView MakeCStyleContractView() {
  microide::editor::LanguageContractView view;
  view.auto_close_pairs = {
      {"(", ")"},
      {"[", "]"},
      {"{", "}"},
      {"\"", "\""},
      {"'", "'"},
  };
  view.surround_pairs = view.auto_close_pairs;
  view.indent_after_open_patterns = {"{", "(", "[", ":"};
  view.dedent_on_close_chars = {"}", ")", "]"};
  view.line_comment = "//";
  view.auto_close_enabled = true;
  view.surround_enabled = true;
  view.smart_indent_enabled = true;
  return view;
}

void TestAutoClosePairInsertsClose() {
  TextViewport viewport;
  viewport.LoadContent("\n", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.MoveCursorTo(0, 0);
  viewport.InsertCharacter('(');
  Expect(viewport.lines()[0] == "()",
         "typing '(' should auto-insert ')'");
  Expect(viewport.cursor_column() == 1,
         "caret should sit between open and close");
}

void TestAutoCloseSkipOverClose() {
  TextViewport viewport;
  viewport.LoadContent("\n", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.MoveCursorTo(0, 0);
  viewport.InsertCharacter('(');
  viewport.InsertCharacter(')');
  Expect(viewport.lines()[0] == "()",
         "typing the matching close should advance over the existing close");
  Expect(viewport.cursor_column() == 2,
         "caret should be after the close character");
}

void TestAutoCloseDisabledByDefault() {
  TextViewport viewport;
  viewport.LoadContent("\n", "/tmp/sample.cpp");
  viewport.MoveCursorTo(0, 0);
  viewport.InsertCharacter('(');
  Expect(viewport.lines()[0] == "(",
         "without contract view, auto-close must not fire");
  Expect(viewport.cursor_column() == 1,
         "caret should advance past the inserted '('");
}

void TestAutoCloseViaInsertTextSingleCharacter() {
  TextViewport viewport;
  viewport.LoadContent("\n", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.MoveCursorTo(0, 0);
  viewport.InsertText("(");
  Expect(viewport.lines()[0] == "()",
         "single-character InsertText should use the same pair path as InsertCharacter");
  Expect(viewport.cursor_column() == 1,
         "InsertText pair path should leave caret between open and close");
}

void TestSurroundSelection() {
  TextViewport viewport;
  viewport.LoadContent("hello\n", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 5, /*extend_selection=*/true);
  viewport.InsertCharacter('"');
  Expect(viewport.lines()[0] == "\"hello\"",
         "surround should wrap the selection with the matching pair");
  Expect(viewport.has_selection(),
         "surround should preserve the inner selection");
  Expect(viewport.cursor_column() == 6,
         "caret should sit at the end of the inner text");
}

void TestSurroundDisabledFallsBackToLiteralInsert() {
  TextViewport viewport;
  auto view = MakeCStyleContractView();
  view.surround_enabled = false;
  viewport.SetLanguageContractView(std::move(view));
  viewport.LoadContent("hello\n", "/tmp/sample.cpp");
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 5, /*extend_selection=*/true);
  viewport.InsertCharacter('"');
  Expect(viewport.lines()[0] == "\"",
         "with surround disabled, typing quote over selection should do literal replacement");
}

void TestAutoCloseInhibitedInsideString() {
  TextViewport viewport;
  viewport.LoadContent("\"abc\"\n", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.MoveCursorTo(0, 2);  // inside string literal
  viewport.InsertCharacter('"');
  Expect(viewport.lines()[0] == "\"a\"bc\"",
         "auto-close should be inhibited inside string scopes");
}

void TestAutoCloseInhibitedInsideComment() {
  TextViewport viewport;
  viewport.LoadContent("// comment\n", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.MoveCursorTo(0, viewport.lines()[0].size());
  viewport.InsertCharacter('(');
  Expect(viewport.lines()[0] == "// comment(",
         "auto-close should be inhibited inside comment scopes");
}

void TestAutoCloseMultiCaretInsertsPairs() {
  TextViewport viewport;
  viewport.LoadContent("\n\n", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.MoveCursorTo(0, 0);
  viewport.AddSecondaryCaret(1, 0);
  viewport.InsertCharacter('(');
  Expect(viewport.lines()[0] == "()" && viewport.lines()[1] == "()",
         "multi-caret pair insert should apply to every caret");
  const auto carets = viewport.secondary_carets();
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 1,
         "primary caret should land between inserted pair");
  Expect(carets.size() == 1 && carets[0].line == 1 && carets[0].column == 1,
         "secondary caret should also land between inserted pair");
}

void TestAutoCloseMultiCaretSkipOverClose() {
  TextViewport viewport;
  viewport.LoadContent("()\n()\n", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.MoveCursorTo(0, 1);
  viewport.AddSecondaryCaret(1, 1);
  viewport.InsertCharacter(')');
  Expect(viewport.lines()[0] == "()" && viewport.lines()[1] == "()",
         "skip-over should not duplicate close characters for multi-caret");
  const auto carets = viewport.secondary_carets();
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 2,
         "primary caret should advance over existing close");
  Expect(carets.size() == 1 && carets[0].line == 1 && carets[0].column == 2,
         "secondary caret should advance over existing close");
}

void TestSmartIndentAfterOpenBrace() {
  TextViewport viewport;
  viewport.LoadContent("if (x) {\n", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(2);
  viewport.MoveCursorTo(0, 8);  // end of line right after '{'
  viewport.InsertNewline();
  // Split-braces should NOT trigger here because there is no closer at caret;
  // smart-indent path inserts a single newline plus indent.
  Expect(viewport.lines().size() >= 2,
         "newline must produce a new line below");
  Expect(viewport.lines()[1] == "  ",
         "smart indent should insert one indent unit after '{'");
}

void TestSmartIndentDisabledFallsBackToBaseIndent() {
  TextViewport viewport;
  auto view = MakeCStyleContractView();
  view.smart_indent_enabled = false;
  viewport.SetLanguageContractView(std::move(view));
  viewport.LoadContent("  if (x) {\n", "/tmp/sample.cpp");
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(2);
  viewport.MoveCursorTo(0, viewport.lines()[0].size());
  viewport.InsertNewline();
  Expect(viewport.lines().size() >= 2, "newline should add another line");
  Expect(viewport.lines()[1] == "  ",
         "smart-indent disabled should preserve only base leading whitespace");
}

void TestSplitBracesInsertsThreeLines() {
  TextViewport viewport;
  viewport.LoadContent("foo() {}", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(2);
  viewport.MoveCursorTo(0, 7);  // between '{' and '}'
  viewport.InsertNewline();
  Expect(viewport.lines().size() == 3,
         "split-braces should produce three lines");
  Expect(viewport.lines()[0] == "foo() {", "first line keeps the opener");
  Expect(viewport.lines()[1] == "  ", "middle line is the indented body");
  Expect(viewport.lines()[2] == "}", "third line carries the closer");
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 2,
         "caret should land on the indented middle line");
}

void TestDedentOnCloseBrace() {
  TextViewport viewport;
  viewport.LoadContent("if (x) {\n    \n", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(2);
  viewport.MoveCursorTo(1, 4);
  // Type '}' on an indent-only line; dedent-on-close should drop one indent
  // unit BEFORE the auto-close path runs.
  viewport.InsertCharacter('}');
  Expect(viewport.lines()[1] == "  }" || viewport.lines()[1] == "  }}",
         "dedent-on-close should reduce indent before inserting the close");
}

void TestSmartIndentAfterArbitraryPattern() {
  TextViewport viewport;
  auto view = MakeCStyleContractView();
  view.indent_after_open_patterns = {":"};
  view.dedent_on_close_chars = {};
  viewport.LoadContent("if cond:", "/tmp/sample.py");
  viewport.SetLanguageContractView(std::move(view));
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(4);
  viewport.MoveCursorTo(0, viewport.lines()[0].size());
  viewport.InsertNewline();
  Expect(viewport.lines().size() >= 2,
         "newline should add a new line after arbitrary indent pattern");
  Expect(viewport.lines()[1] == "    ",
         "smart indent should add one indent unit after ':' opener");
}

void TestDedentOnCloseSkippedOnNonIndentOnlyLine() {
  TextViewport viewport;
  viewport.LoadContent("if (x) {\n    foo()\n", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(2);
  // Position caret at the end of "    foo()" (line index 1).
  viewport.MoveCursorTo(1, viewport.lines()[1].size());
  viewport.InsertCharacter('}');
  Expect(viewport.lines()[1].rfind("    foo()", 0) == 0,
         "dedent-on-close must not trigger on lines with non-whitespace content");
}

microide::workspace::TabEntry::EditorTabState MakeFoldingTab() {
  microide::workspace::TabEntry::EditorTabState tab;
  microide::workspace::TabEntry::EditorTabState::EditorViewState view;
  view.leaf_id = 1;
  tab.views.push_back(std::move(view));
  tab.active_leaf_id = 1;
  tab.next_leaf_id = 2;
  return tab;
}

microide::workspace::LanguageContract MakeCStyleFoldContract() {
  microide::workspace::LanguageContract contract;
  contract.language_id = "cpp";
  contract.bracket_pairs.push_back({"{", "}"});
  contract.bracket_pairs.push_back({"(", ")"});
  contract.bracket_pairs.push_back({"[", "]"});
  return contract;
}

void TestFoldingRefreshComputesBracketRanges() {
  auto tab = MakeFoldingTab();
  auto& viewport = tab.views.front().viewport;
  viewport.LoadContent("void f() {\n  body;\n}\n", "/tmp/sample.cpp");
  const auto contract = MakeCStyleFoldContract();
  microide::workspace::EnsureFoldingModelFresh(tab, viewport, &contract,
                                               /*tab_size=*/4,
                                               /*fold_enabled=*/true);
  Expect(!tab.folding_model.ranges().empty(),
         "fold model should have at least one range after refresh");
  Expect(tab.folding_model.ranges().front().opener_line == 0,
         "first fold opener must be on line 0");
  Expect(tab.folding_model.ranges().front().closer_line == 2,
         "first fold closer must be on line 2");
}

void TestFoldingRefreshFingerprintReusedAcrossCalls() {
  auto tab = MakeFoldingTab();
  auto& viewport = tab.views.front().viewport;
  viewport.LoadContent("void f() {\n  body;\n}\n", "/tmp/sample.cpp");
  const auto contract = MakeCStyleFoldContract();
  microide::workspace::EnsureFoldingModelFresh(tab, viewport, &contract, 4, true);
  const auto& fp_first = tab.folding_model.fingerprint();
  // A subsequent call with the same fingerprint must not change the stored
  // fingerprint or the ranges; the model exposes IsFresh as the canonical
  // freshness check, so we verify that.
  Expect(tab.folding_model.IsFresh(fp_first),
         "model must report itself fresh after a successful compute");
  microide::workspace::EnsureFoldingModelFresh(tab, viewport, &contract, 4, true);
  Expect(tab.folding_model.IsFresh(fp_first),
         "model must remain fresh on a repeat refresh with the same inputs");
}

void TestFoldingRefreshDisabledExpandsAndClears() {
  auto tab = MakeFoldingTab();
  auto& viewport = tab.views.front().viewport;
  viewport.LoadContent("void f() {\n  body;\n}\nvoid g() {\n  body;\n}\n", "/tmp/sample.cpp");
  const auto contract = MakeCStyleFoldContract();
  microide::workspace::EnsureFoldingModelFresh(tab, viewport, &contract, 4, true);
  Expect(tab.folding_model.ranges().size() >= 1,
         "fold model should have ranges after enable");
  Expect(tab.folding_model.Collapse(0), "expected to collapse fold at line 0");
  Expect(tab.folding_model.IsCollapsedAtOpener(0),
         "fold at line 0 should be collapsed before disable");
  microide::workspace::EnsureFoldingModelFresh(tab, viewport, &contract, 4,
                                               /*fold_enabled=*/false);
  Expect(tab.folding_model.ranges().empty(),
         "disabling fold must clear stored ranges");
  Expect(tab.folding_model.collapsed_flags().empty(),
         "disabling fold must drop collapsed state so no rows are hidden");
}

void TestFoldingRefreshLanguageChangeRebuilds() {
  auto tab = MakeFoldingTab();
  auto& viewport = tab.views.front().viewport;
  viewport.LoadContent("void f() {\n  body;\n}\n", "/tmp/sample.cpp");
  const auto cpp = MakeCStyleFoldContract();
  microide::workspace::EnsureFoldingModelFresh(tab, viewport, &cpp, 4, true);
  const auto cpp_fp = tab.folding_model.fingerprint();
  microide::workspace::LanguageContract other = cpp;
  other.language_id = "rust";
  microide::workspace::EnsureFoldingModelFresh(tab, viewport, &other, 4, true);
  Expect(!tab.folding_model.IsFresh(cpp_fp),
         "language id change must invalidate the prior fingerprint");
  Expect(tab.folding_model.fingerprint().language_id == "rust",
         "post-refresh fingerprint must reflect the new language id");
}

}  // namespace

void RegisterEditorEssentialsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorEssentials/BracketScanner/ForwardMatch",
          TestBracketScannerForwardMatch);
  AddTest(tests, "EditorEssentials/BracketScanner/BackwardMatch",
          TestBracketScannerBackwardMatch);
  AddTest(tests, "EditorEssentials/BracketScanner/NoMatchOnUnbalanced",
          TestBracketScannerNoMatchOnUnbalanced);
  AddTest(tests, "EditorEssentials/Shaping/MoveLineDown",
          TestShapingMoveLineDown);
  AddTest(tests, "EditorEssentials/Shaping/MoveLineUpAtTop",
          TestShapingMoveLineUpAtTopIsNoop);
  AddTest(tests, "EditorEssentials/Shaping/DuplicateLine",
          TestShapingDuplicateLine);
  AddTest(tests, "EditorEssentials/Shaping/IndentSelection",
          TestShapingIndentSelection);
  AddTest(tests, "EditorEssentials/Shaping/ToggleLineComment",
          TestShapingToggleLineComment);
  AddTest(tests, "EditorEssentials/Shaping/SortLinesAscending",
          TestShapingSortLinesAscending);
  AddTest(tests, "EditorEssentials/Save/TrimTrailingWhitespace",
          TestSaveNormalizationTrim);
  AddTest(tests, "EditorEssentials/Save/TextViewportAppliesNormalization",
          TestTextViewportSaveAppliesNormalization);
  AddTest(tests, "EditorEssentials/Save/EnsureFinalNewline",
          TestSaveNormalizationEnsureFinalNewline);
  AddTest(tests, "EditorEssentials/IndentDetect/SpacesMajority",
          TestIndentDetectSpacesMajority);
  AddTest(tests, "EditorEssentials/IndentDetect/TabsMajority",
          TestIndentDetectTabsMajority);
  AddTest(tests, "EditorEssentials/IndentDetect/ApplyOnOpenUpdatesViewport",
          TestApplyDetectedIndentOnOpenUpdatesViewport);
  AddTest(tests, "EditorEssentials/IndentDetect/ApplyOnOpenSkippedWhenDisabled",
          TestApplyDetectedIndentOnOpenSkippedWhenDisabled);
  AddTest(tests, "EditorEssentials/IndentDetect/ApplyOnOpenSkippedWhenPathEmpty",
          TestApplyDetectedIndentOnOpenSkippedWhenPathEmpty);
  AddTest(tests, "EditorEssentials/LanguageContract/Defaults",
          TestLanguageContractDefaults);
  AddTest(tests, "EditorEssentials/LanguageContract/Missing",
          TestLanguageContractMissingLanguage);
  AddTest(tests, "EditorEssentials/LanguageContract/AppliesUserOverrides",
          TestLanguageContractAppliesUserOverrides);
  AddTest(tests, "EditorEssentials/IndentGuides/EmitsRunsAtNestedDepths",
          TestIndentGuidesEmitsRunsAtNestedDepths);
  AddTest(tests, "EditorEssentials/IndentGuides/MarksActiveAtParentColumn",
          TestIndentGuidesMarksActiveAtParentColumn);
  AddTest(tests, "EditorEssentials/AutoClose/InsertsClose",
          TestAutoClosePairInsertsClose);
  AddTest(tests, "EditorEssentials/AutoClose/SkipOverClose",
          TestAutoCloseSkipOverClose);
  AddTest(tests, "EditorEssentials/AutoClose/DisabledByDefault",
          TestAutoCloseDisabledByDefault);
  AddTest(tests, "EditorEssentials/AutoClose/InsertTextSingleCharUsesPairPath",
          TestAutoCloseViaInsertTextSingleCharacter);
  AddTest(tests, "EditorEssentials/AutoClose/InhibitInsideString",
          TestAutoCloseInhibitedInsideString);
  AddTest(tests, "EditorEssentials/AutoClose/InhibitInsideComment",
          TestAutoCloseInhibitedInsideComment);
  AddTest(tests, "EditorEssentials/AutoClose/MultiCaretInsertsPairs",
          TestAutoCloseMultiCaretInsertsPairs);
  AddTest(tests, "EditorEssentials/AutoClose/MultiCaretSkipOverClose",
          TestAutoCloseMultiCaretSkipOverClose);
  AddTest(tests, "EditorEssentials/Surround/SingleLineSelection",
          TestSurroundSelection);
  AddTest(tests, "EditorEssentials/Surround/DisabledFallsBackLiteral",
          TestSurroundDisabledFallsBackToLiteralInsert);
  AddTest(tests, "EditorEssentials/SmartIndent/AfterOpenBrace",
          TestSmartIndentAfterOpenBrace);
  AddTest(tests, "EditorEssentials/SmartIndent/AfterArbitraryPattern",
          TestSmartIndentAfterArbitraryPattern);
  AddTest(tests, "EditorEssentials/DedentOnClose/SkippedOnNonIndentOnlyLine",
          TestDedentOnCloseSkippedOnNonIndentOnlyLine);
  AddTest(tests, "EditorEssentials/SmartIndent/DisabledFallsBackToBaseIndent",
          TestSmartIndentDisabledFallsBackToBaseIndent);
  AddTest(tests, "EditorEssentials/SplitBraces/InsertsThreeLines",
          TestSplitBracesInsertsThreeLines);
  AddTest(tests, "EditorEssentials/DedentOnClose/Brace",
          TestDedentOnCloseBrace);
  AddTest(tests, "EditorEssentials/Folding/RefreshComputesBracketRanges",
          TestFoldingRefreshComputesBracketRanges);
  AddTest(tests, "EditorEssentials/Folding/RefreshFingerprintReusedAcrossCalls",
          TestFoldingRefreshFingerprintReusedAcrossCalls);
  AddTest(tests, "EditorEssentials/Folding/RefreshDisabledExpandsAndClears",
          TestFoldingRefreshDisabledExpandsAndClears);
  AddTest(tests, "EditorEssentials/Folding/RefreshLanguageChangeRebuilds",
          TestFoldingRefreshLanguageChangeRebuilds);
}

}  // namespace microide::tests
