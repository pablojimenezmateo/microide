#include "TestSupport.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "editor/TextViewport.h"
#include "editor/WordBoundary.h"

namespace microide::tests {
namespace {

using microide::editor::TextViewport;

std::string DocumentText(const TextViewport& viewport) {
  std::string out;
  for (std::size_t line = 0; line < viewport.line_count(); ++line) {
    if (line > 0) {
      out.push_back('\n');
    }
    out.append(viewport.lines().LineView(line));
  }
  return out;
}

void ExpectCaret(const TextViewport& viewport,
                 std::size_t line,
                 std::size_t column,
                 std::string_view context) {
  Expect(viewport.cursor_line() == line && viewport.cursor_column() == column,
         std::string(context) + ": caret at " + std::to_string(viewport.cursor_line()) + ":" +
             std::to_string(viewport.cursor_column()) + ", expected " + std::to_string(line) + ":" +
             std::to_string(column));
}

void TestWordBoundaryTreatsSeparatorRunsAsWords() {
  constexpr std::string_view kText = "foo === bar";
  Expect(editor::WordBoundaryRight(kText, 0) == 3, "word-right stops at the end of foo");
  Expect(editor::WordBoundaryRight(kText, 3) == 7, "word-right stops at the end of the === run");
  Expect(editor::WordBoundaryRight(kText, 7) == 11, "word-right stops at the end of bar");
  Expect(editor::WordBoundaryRight(kText, 11) == 11, "word-right saturates at the end");

  Expect(editor::WordBoundaryLeft(kText, 11) == 8, "word-left stops at the start of bar");
  Expect(editor::WordBoundaryLeft(kText, 8) == 4, "word-left stops at the start of the === run");
  Expect(editor::WordBoundaryLeft(kText, 4) == 0, "word-left stops at the start of foo");
  Expect(editor::WordBoundaryLeft(kText, 0) == 0, "word-left saturates at the start");
}

void TestWordBoundaryKeepsMultibyteWordsWhole() {
  constexpr std::string_view kText = "café ñoño";
  Expect(editor::WordBoundaryRight(kText, 0) == 5, "word-right clears the whole of café");
  Expect(editor::WordBoundaryRight(kText, 5) == 12, "word-right clears the whole of ñoño");
  Expect(editor::WordBoundaryLeft(kText, 12) == 6, "word-left lands on the start of ñoño");
  Expect(editor::WordBoundaryLeft(kText, 6) == 0, "word-left lands on the start of café");
}

void TestDeleteWordBoundaryTakesAWhitespaceRunOnItsOwn() {
  constexpr std::string_view kIndented = "    value";
  Expect(editor::DeleteWordBoundaryLeft(kIndented, 4) == 0,
         "delete-word-left removes the indent run alone");
  Expect(editor::WordBoundaryLeft(kIndented, 4) == 0,
         "motion also stops at 0 here -- there is no word before the indent");

  constexpr std::string_view kGap = "foo    bar";
  Expect(editor::DeleteWordBoundaryLeft(kGap, 7) == 3,
         "delete-word-left removes only the gap, leaving foo");
  Expect(editor::WordBoundaryLeft(kGap, 7) == 0,
         "motion, by contrast, walks past the gap onto foo");

  constexpr std::string_view kSingle = "foo bar";
  Expect(editor::DeleteWordBoundaryLeft(kSingle, 4) == 0,
         "a lone space does not trigger the whitespace-only delete");

  Expect(editor::DeleteWordBoundaryRight(kGap, 3) == 7,
         "delete-word-right removes only the gap");
  Expect(editor::DeleteWordBoundaryRight(kSingle, 3) == 7,
         "a lone space is deleted together with the following word");
}

void TestWordBoundarySnapsAMidScalarIndex() {
  constexpr std::string_view kText = "café bar";
  Expect(editor::WordBoundaryRight(kText, 4) == 5,
         "a right scan from inside é finishes the word it is inside");
  Expect(editor::WordBoundaryLeft(kText, 4) == 0,
         "a left scan from inside é lands on the word start");
  Expect(editor::DeleteWordBoundaryRight(kText, 4) == 5,
         "a delete-right span from inside é ends on a character boundary");
  Expect(editor::DeleteWordBoundaryLeft(kText, 4) == 0,
         "a delete-left span from inside é starts on a character boundary");
}

void TestViewportSelectsMultibyteWordsWhole() {
  TextViewport viewport;
  viewport.SetViewportSize(10, 80);
  viewport.LoadContent("int café = résumé;\n", "/tmp/word.txt");

  viewport.MoveCursorTo(0, 4);
  viewport.SelectWordAtCursor();
  Expect(viewport.SelectedText() == "café",
         "double-click on café should select all of it, not just caf");

  const auto range = viewport.WordRangeAt(editor::TextPosition{0, 7});
  Expect(range.has_value(), "a position inside the multi-byte tail is still inside the word");
  Expect(range->start.column == 4 && range->end.column == 9,
         "the run should span the whole of café");

  const auto mid_scalar = viewport.WordRangeAt(editor::TextPosition{0, 8});
  Expect(mid_scalar.has_value() && mid_scalar->start.column == 4 && mid_scalar->end.column == 9,
         "a caret inside a multi-byte character still resolves to its word");
}

void TestViewportWordMotionCrossesLines() {
  TextViewport viewport;
  viewport.SetViewportSize(10, 80);
  viewport.LoadContent("alpha beta\ngamma\n", "/tmp/word.txt");

  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorWord(1);
  ExpectCaret(viewport, 0, 5, "word-right past alpha");
  viewport.MoveCursorWord(1);
  ExpectCaret(viewport, 0, 10, "word-right past beta");
  viewport.MoveCursorWord(1);
  ExpectCaret(viewport, 1, 0, "word-right crosses onto the next line");
  viewport.MoveCursorWord(1);
  ExpectCaret(viewport, 1, 5, "word-right past gamma");

  viewport.MoveCursorWord(-1);
  ExpectCaret(viewport, 1, 0, "word-left back to the start of gamma");
  viewport.MoveCursorWord(-1);
  ExpectCaret(viewport, 0, 10, "word-left crosses back onto the previous line");
  viewport.MoveCursorWord(-1);
  ExpectCaret(viewport, 0, 6, "word-left to the start of beta");
  viewport.MoveCursorWord(-1);
  ExpectCaret(viewport, 0, 0, "word-left to the start of alpha");
  viewport.MoveCursorWord(-1);
  ExpectCaret(viewport, 0, 0, "word-left saturates at the document start");
}

void TestViewportWordMotionSelectionSemantics() {
  TextViewport viewport;
  viewport.SetViewportSize(10, 80);
  viewport.LoadContent("alpha beta gamma\n", "/tmp/word.txt");

  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorWord(1, true);
  const auto extended = viewport.selection_range();
  Expect(extended.has_value(), "shift+ctrl+right should start a selection");
  Expect(extended->start.column == 0 && extended->end.column == 5,
         "shift+ctrl+right should select the first word");
  viewport.MoveCursorWord(1, true);
  const auto grown = viewport.selection_range();
  Expect(grown.has_value() && grown->end.column == 10,
         "a second shift+ctrl+right should grow the selection by a word");

  viewport.MoveCursorWord(-1);
  Expect(!viewport.has_selection(), "a plain word step should drop the selection");
  ExpectCaret(viewport, 0, 6, "plain ctrl+left moves from the caret, not the anchor");
}

void TestViewportDeleteWordBackwardAndForward() {
  TextViewport viewport;
  viewport.SetViewportSize(10, 80);
  viewport.LoadContent("alpha beta\ngamma\n", "/tmp/word.txt");

  viewport.MoveCursorTo(0, 10);
  viewport.DeleteWord(-1);
  Expect(DocumentText(viewport) == "alpha \ngamma\n", "ctrl+backspace should remove beta");
  ExpectCaret(viewport, 0, 6, "caret lands where the word started");

  viewport.MoveCursorTo(1, 0);
  viewport.DeleteWord(-1);
  Expect(DocumentText(viewport) == "alpha gamma\n", "ctrl+backspace at column 0 joins the lines");

  viewport.MoveCursorTo(0, 0);
  viewport.DeleteWord(1);
  Expect(DocumentText(viewport) == " gamma\n", "ctrl+delete should remove the leading word");

  Expect(viewport.Undo(), "undo should report a change");
  Expect(DocumentText(viewport) == "alpha gamma\n", "one undo restores the whole word");
}

void TestViewportWordDeleteRemovesSelectionInstead() {
  TextViewport viewport;
  viewport.SetViewportSize(10, 80);
  viewport.LoadContent("alpha beta gamma\n", "/tmp/word.txt");

  viewport.MoveCursorTo(0, 6);
  viewport.MoveCursorTo(0, 10, true);
  viewport.DeleteWord(-1);
  Expect(DocumentText(viewport) == "alpha  gamma\n",
         "a word delete over a selection removes exactly the selection");
}

void TestViewportWordVerbsApplyToEveryCaret() {
  TextViewport viewport;
  viewport.SetViewportSize(10, 80);
  viewport.LoadContent("alpha one\nbravo two\n", "/tmp/word.txt");

  viewport.MoveCursorTo(0, 9);
  viewport.AddSecondaryCaret(1, 9);
  Expect(viewport.has_multiple_carets(), "expected two carets");

  viewport.MoveCursorWord(-1);
  ExpectCaret(viewport, 0, 6, "primary caret stepped a word left");
  Expect(viewport.has_multiple_carets(), "the secondary caret survives a word step");

  TextViewport deleter;
  deleter.SetViewportSize(10, 80);
  deleter.LoadContent("alpha one\nbravo two\n", "/tmp/word.txt");
  deleter.MoveCursorTo(0, 9);
  deleter.AddSecondaryCaret(1, 9);
  deleter.DeleteWord(-1);
  Expect(DocumentText(deleter) == "alpha \nbravo \n",
         "ctrl+backspace should remove a word at every caret");
}

}

namespace {

int VsCodeClass(char c) {
  if (c == ' ' || c == '\t') return 0;
  return std::string_view("`~!@#$%^&*()-=+[{]}\\|;:'\",.<>/?").find(c) != std::string_view::npos ? 1 : 2;
}

std::size_t VsCodeWordStartLeft(std::string_view line, std::size_t caret) {
  std::size_t i = caret;
  while (i > 0 && VsCodeClass(line[i - 1]) == 0) --i;
  if (i == 0) return 0;
  const int run = VsCodeClass(line[i - 1]);
  while (i > 0 && VsCodeClass(line[i - 1]) == run) --i;
  return i;
}

std::size_t VsCodeWordEndRight(std::string_view line, std::size_t caret) {
  std::size_t i = caret;
  while (i < line.size() && VsCodeClass(line[i]) == 0) ++i;
  if (i >= line.size()) return line.size();
  const int run = VsCodeClass(line[i]);
  while (i < line.size() && VsCodeClass(line[i]) == run) ++i;
  return i;
}

std::size_t VsCodeDeleteWordLeft(std::string_view line, std::size_t caret) {
  std::size_t blanks = 0;
  while (caret - blanks > 0 && VsCodeClass(line[caret - blanks - 1]) == 0) ++blanks;
  if (blanks >= 2) return caret - blanks;
  return VsCodeWordStartLeft(line, caret);
}

struct VsCodeWord {
  std::size_t start = 0;
  std::size_t end = 0;
  int cls = 0;
  bool found = false;
};

VsCodeWord VsCodeRunAround(std::string_view line, std::size_t seed) {
  const int run = VsCodeClass(line[seed]);
  VsCodeWord word{seed, seed, run, true};
  while (word.start > 0 && VsCodeClass(line[word.start - 1]) == run) --word.start;
  while (word.end < line.size() && VsCodeClass(line[word.end]) == run) ++word.end;
  return word;
}

VsCodeWord VsCodePrevWord(std::string_view line, std::size_t caret) {
  std::size_t i = caret;
  while (i > 0 && VsCodeClass(line[i - 1]) == 0) --i;
  if (i == 0) return VsCodeWord{};
  return VsCodeRunAround(line, i - 1);
}

VsCodeWord VsCodeNextWord(std::string_view line, std::size_t caret) {
  std::size_t i = caret;
  while (i < line.size() && VsCodeClass(line[i]) == 0) ++i;
  if (i >= line.size()) return VsCodeWord{};
  return VsCodeRunAround(line, i);
}

std::pair<std::size_t, std::size_t> VsCodeWordSelection(std::string_view line, std::size_t caret) {
  const VsCodeWord prev = VsCodePrevWord(line, caret);
  const VsCodeWord next = VsCodeNextWord(line, caret);
  if (prev.found && prev.cls == 2 && prev.start <= caret && caret <= prev.end) {
    return {prev.start, prev.end};
  }
  if (next.found && next.cls == 2 && next.start <= caret && caret <= next.end) {
    return {next.start, next.end};
  }
  std::size_t start = prev.found ? prev.end : 0;
  std::size_t end = next.found ? next.start : line.size();
  if (start > end) std::swap(start, end);
  return {start, end};
}

std::pair<std::size_t, std::size_t> VsCodeWordAtPosition(std::string_view line,
                                                         std::size_t caret) {
  const VsCodeWord prev = VsCodePrevWord(line, caret);
  const VsCodeWord next = VsCodeNextWord(line, caret);
  if (prev.found && prev.cls == 2 && prev.start <= caret && caret <= prev.end) {
    return {prev.start, prev.end};
  }
  if (next.found && next.cls == 2 && next.start <= caret && caret <= next.end) {
    return {next.start, next.end};
  }
  return {0, 0};
}

}

void TestEditorWordSelectionAgreesWithVsCodeWordRule() {
  std::uint64_t state = 0x2545f4914f6cdd1dULL;
  const auto next = [&](std::size_t bound) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return static_cast<std::size_t>(state % bound);
  };
  static constexpr std::string_view kAlphabet = "ab_9 \t=+.(-)/\"'x";
  for (int round = 0; round < 400; ++round) {
    std::string line;
    const std::size_t length = next(14);
    for (std::size_t i = 0; i < length; ++i) {
      line.push_back(kAlphabet[next(kAlphabet.size())]);
    }
    for (std::size_t caret = 0; caret <= line.size(); ++caret) {
      const auto [want_sel_start, want_sel_end] = VsCodeWordSelection(line, caret);
      const editor::WordSpan got_sel = editor::WordSelectionRunAt(line, caret);
      Expect(got_sel.start == want_sel_start && got_sel.end == want_sel_end,
             "double-click selection disagrees on <" + line + "> at " + std::to_string(caret) +
                 ": got [" + std::to_string(got_sel.start) + "," + std::to_string(got_sel.end) +
                 ") want [" + std::to_string(want_sel_start) + "," +
                 std::to_string(want_sel_end) + ")");

      const auto [want_word_start, want_word_end] = VsCodeWordAtPosition(line, caret);
      const editor::WordSpan got_word = editor::IdentifierRunTouching(line, caret);
      const std::size_t got_word_start = got_word.empty() ? 0 : got_word.start;
      const std::size_t got_word_end = got_word.empty() ? 0 : got_word.end;
      Expect(got_word_start == want_word_start && got_word_end == want_word_end,
             "word-at-position disagrees on <" + line + "> at " + std::to_string(caret) +
                 ": got [" + std::to_string(got_word_start) + "," +
                 std::to_string(got_word_end) + ") want [" + std::to_string(want_word_start) +
                 "," + std::to_string(want_word_end) + ")");
    }
  }
}

void TestEditorWordMotionAgreesWithVsCodeRuleOnRandomAsciiLines() {
  std::uint64_t state = 0x51ed270b4c6d3a19ULL;
  const auto next = [&](std::size_t bound) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return static_cast<std::size_t>(state % bound);
  };
  static constexpr std::string_view kAlphabet = "ab_9 \t=+.(-)/\"'x";
  for (int round = 0; round < 400; ++round) {
    std::string line;
    const std::size_t length = next(14);
    for (std::size_t i = 0; i < length; ++i) {
      line.push_back(kAlphabet[next(kAlphabet.size())]);
    }
    for (std::size_t caret = 0; caret <= line.size(); ++caret) {
      const std::size_t left = microide::editor::WordBoundaryLeft(line, caret);
      const std::size_t right = microide::editor::WordBoundaryRight(line, caret);
      const std::size_t del = microide::editor::DeleteWordBoundaryLeft(line, caret);
      if (left != VsCodeWordStartLeft(line, caret) || right != VsCodeWordEndRight(line, caret) ||
          del != VsCodeDeleteWordLeft(line, caret)) {
        Expect(false, "word motion disagrees with the VS Code rule on <" + line + "> at " +
                          std::to_string(caret) + ": left " + std::to_string(left) + "/" +
                          std::to_string(VsCodeWordStartLeft(line, caret)) + ", right " +
                          std::to_string(right) + "/" + std::to_string(VsCodeWordEndRight(line, caret)) +
                          ", delete-left " + std::to_string(del) + "/" +
                          std::to_string(VsCodeDeleteWordLeft(line, caret)));
        return;
      }
    }
  }
}

void RegisterEditorWordMotionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorWordMotion/AgreesWithVsCodeRuleOnRandomAsciiLines",
          TestEditorWordMotionAgreesWithVsCodeRuleOnRandomAsciiLines);
  AddTest(tests, "EditorWordMotion/SelectionAgreesWithVsCodeWordRule",
          TestEditorWordSelectionAgreesWithVsCodeWordRule);
  AddTest(tests, "EditorWordMotion/BoundaryTreatsSeparatorRunsAsWords",
          TestWordBoundaryTreatsSeparatorRunsAsWords);
  AddTest(tests, "EditorWordMotion/BoundaryKeepsMultibyteWordsWhole",
          TestWordBoundaryKeepsMultibyteWordsWhole);
  AddTest(tests, "EditorWordMotion/DeleteBoundaryTakesWhitespaceRunAlone",
          TestDeleteWordBoundaryTakesAWhitespaceRunOnItsOwn);
  AddTest(tests, "EditorWordMotion/BoundarySnapsAMidScalarIndex",
          TestWordBoundarySnapsAMidScalarIndex);
  AddTest(tests, "EditorWordMotion/ViewportSelectsMultibyteWordsWhole",
          TestViewportSelectsMultibyteWordsWhole);
  AddTest(tests, "EditorWordMotion/ViewportMotionCrossesLines",
          TestViewportWordMotionCrossesLines);
  AddTest(tests, "EditorWordMotion/ViewportMotionSelectionSemantics",
          TestViewportWordMotionSelectionSemantics);
  AddTest(tests, "EditorWordMotion/ViewportDeleteWordBackwardAndForward",
          TestViewportDeleteWordBackwardAndForward);
  AddTest(tests, "EditorWordMotion/ViewportWordDeleteRemovesSelectionInstead",
          TestViewportWordDeleteRemovesSelectionInstead);
  AddTest(tests, "EditorWordMotion/ViewportWordVerbsApplyToEveryCaret",
          TestViewportWordVerbsApplyToEveryCaret);
}

}
