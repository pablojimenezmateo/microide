#include "TestSupport.h"

#include "editor/TextDragDrop.h"
#include "editor/TextViewport.h"

#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::SelectionRange;
using microide::editor::TextPosition;
using microide::editor::TextViewport;
namespace drag = microide::editor::text_drag_drop;

TextViewport MakeViewport(std::string_view content) {
  TextViewport viewport;
  viewport.LoadContent(content, "/tmp/drag-drop.txt");
  return viewport;
}

std::string DocumentText(const TextViewport& viewport) {
  std::string text;
  for (std::size_t i = 0; i < viewport.lines().size(); ++i) {
    if (i > 0) {
      text += '\n';
    }
    text += viewport.lines().LineView(i);
  }
  return text;
}

SelectionRange Range(std::size_t sl, std::size_t sc, std::size_t el, std::size_t ec) {
  return SelectionRange{TextPosition{sl, sc}, TextPosition{el, ec}};
}

void TestDropAdjustmentForRemovedRange() {
  const SelectionRange source = Range(1, 2, 3, 4);

  const TextPosition before = drag::AdjustDropForRemovedRange(source, TextPosition{0, 7});
  Expect(before.line == 0 && before.column == 7, "a drop before the source is unaffected");

  const TextPosition below = drag::AdjustDropForRemovedRange(source, TextPosition{9, 3});
  Expect(below.line == 7 && below.column == 3,
         "a drop on a later line rises by the source's line span, keeping its column");

  const TextPosition same_line = drag::AdjustDropForRemovedRange(source, TextPosition{3, 10});
  Expect(same_line.line == 1 && same_line.column == 2 + (10 - 4),
         "a drop on the source's last line lands at the splice join plus its offset");

  const SelectionRange one_line = Range(2, 3, 2, 8);
  const TextPosition after = drag::AdjustDropForRemovedRange(one_line, TextPosition{2, 12});
  Expect(after.line == 2 && after.column == 3 + (12 - 8),
         "a single-line move shifts a later column on the same line by the removed width");
}

void TestPositionAfterInsertedText() {
  const TextPosition flat = drag::PositionAfterInsertedText(TextPosition{4, 6}, "abc");
  Expect(flat.line == 4 && flat.column == 9, "text with no newline advances the column only");

  const TextPosition multi = drag::PositionAfterInsertedText(TextPosition{4, 6}, "ab\ncd\nefg");
  Expect(multi.line == 6 && multi.column == 3,
         "multi-line text ends on the last inserted line, at the last segment's width");

  const TextPosition trailing = drag::PositionAfterInsertedText(TextPosition{0, 0}, "ab\n");
  Expect(trailing.line == 1 && trailing.column == 0,
         "text ending in a newline ends at the start of the next line");
}

void TestMoveWithinALineForward() {
  TextViewport viewport = MakeViewport("alpha bravo charlie\n");
  const SelectionRange source = Range(0, 0, 0, 6);
  const auto moved = drag::Apply(viewport, source, TextPosition{0, 19}, false);
  Expect(moved.has_value(), "a forward move inside one line applies");
  Expect(viewport.lines().LineView(0) == "bravo charliealpha ",
         "the text moves to the drop point, adjusted for its own removal");
  Expect(moved->start.column == 13 && moved->end.column == 19,
         "the returned range covers the moved text where it landed");
}

void TestMoveBackwardIsNotShifted() {
  TextViewport viewport = MakeViewport("alpha bravo charlie\n");
  const SelectionRange source = Range(0, 12, 0, 19);
  const auto moved = drag::Apply(viewport, source, TextPosition{0, 0}, false);
  Expect(moved.has_value(), "a backward move applies");
  Expect(viewport.lines().LineView(0) == "charliealpha bravo ",
         "a drop before the source is not shifted by the removal");
}

void TestMoveAcrossLines() {
  TextViewport viewport = MakeViewport("one\ntwo\nthree\nfour\n");
  const SelectionRange source = Range(1, 0, 2, 0);
  const auto moved = drag::Apply(viewport, source, TextPosition{0, 0}, false);
  Expect(moved.has_value(), "a multi-line move applies");
  Expect(DocumentText(viewport) == "two\none\nthree\nfour\n",
         "the moved lines land at the drop point and leave no gap behind");
}

void TestCopyLeavesTheSourceInPlace() {
  TextViewport viewport = MakeViewport("alpha bravo\n");
  const SelectionRange source = Range(0, 0, 0, 5);
  const auto copied = drag::Apply(viewport, source, TextPosition{0, 11}, true);
  Expect(copied.has_value(), "a copy applies");
  Expect(viewport.lines().LineView(0) == "alpha bravoalpha",
         "a copy inserts at the drop point WITHOUT removing the source");
  Expect(copied->start.column == 11 && copied->end.column == 16,
         "the returned range covers the inserted copy");
}

void TestDropInsideTheSourceIsANoOp() {
  TextViewport viewport = MakeViewport("alpha bravo\n");
  const SelectionRange source = Range(0, 0, 0, 5);
  for (std::size_t column = 0; column <= 5; ++column) {
    const auto result = drag::Apply(viewport, source, TextPosition{0, column}, false);
    Expect(!result.has_value(), "a drop inside the source is refused");
    Expect(viewport.lines().LineView(0) == "alpha bravo", "the document is untouched");
  }
}

void TestMoveUndoesInOneStep() {
  TextViewport viewport = MakeViewport("alpha bravo charlie\n");
  const std::string before = DocumentText(viewport);
  const auto moved = drag::Apply(viewport, Range(0, 0, 0, 6), TextPosition{0, 19}, false);
  Expect(moved.has_value(), "the move applies");
  Expect(DocumentText(viewport) != before, "the document changed");

  Expect(viewport.Undo(), "one undo is available");
  Expect(DocumentText(viewport) == before,
         "a single undo restores the document — the move is one entry, not two");
}

void TestReversedSourceRangeIsNormalized() {
  TextViewport viewport = MakeViewport("alpha bravo\n");
  const auto moved = drag::Apply(viewport, Range(0, 5, 0, 0), TextPosition{0, 11}, false);
  Expect(moved.has_value(), "a reversed source range still applies");
  Expect(viewport.lines().LineView(0) == " bravoalpha",
         "the reversed range moves the same text as the forward one");
}

std::size_t OffsetOf(std::string_view text, TextPosition position) {
  std::size_t offset = 0;
  for (std::size_t line = 0; line < position.line; ++line) {
    offset = text.find('\n', offset);
    if (offset == std::string_view::npos) return text.size();
    ++offset;
  }
  return std::min(offset + position.column, text.size());
}

void TestDragDropMatchesAFlatStringOracle() {
  static constexpr std::string_view kDocument = "ab\ncde\n\nfg";
  std::vector<TextPosition> positions;
  {
    std::size_t line = 0;
    std::size_t column = 0;
    for (std::size_t i = 0; i <= kDocument.size(); ++i) {
      if (i == kDocument.size() || kDocument[i] == '\n') {
        positions.push_back(TextPosition{line, column});
        if (i == kDocument.size()) break;
        ++line;
        column = 0;
        continue;
      }
      positions.push_back(TextPosition{line, column});
      ++column;
    }
  }

  for (const TextPosition& start : positions) {
    for (const TextPosition& end : positions) {
      if (drag::PositionBefore(end, start)) continue;
      const std::size_t start_offset = OffsetOf(kDocument, start);
      const std::size_t end_offset = OffsetOf(kDocument, end);
      if (start_offset == end_offset) continue;
      for (const TextPosition& drop : positions) {
        for (const bool copy : {false, true}) {
          const std::size_t drop_offset = OffsetOf(kDocument, drop);
          const std::string context =
              "source " + std::to_string(start_offset) + ".." + std::to_string(end_offset) +
              " drop " + std::to_string(drop_offset) + (copy ? " copy" : " move");

          TextViewport viewport = MakeViewport(kDocument);
          const auto moved = drag::Apply(viewport, SelectionRange{start, end}, drop, copy);

          if (drop_offset >= start_offset && drop_offset <= end_offset) {
            Expect(!moved.has_value(), "a drop inside the source is refused: " + context);
            Expect(DocumentText(viewport) == kDocument,
                   "a refused drop leaves the document alone: " + context);
            continue;
          }
          const std::string text(kDocument.substr(start_offset, end_offset - start_offset));
          std::string expected(kDocument);
          std::size_t target_offset = drop_offset;
          if (!copy) {
            expected.erase(start_offset, end_offset - start_offset);
            if (drop_offset > end_offset) target_offset -= end_offset - start_offset;
          }
          expected.insert(target_offset, text);

          Expect(moved.has_value(), "a drop outside the source applies: " + context);
          Expect(DocumentText(viewport) == expected,
                 "document matches the oracle: " + context + ", got <" + DocumentText(viewport) +
                     "> want <" + expected + ">");
          if (moved.has_value()) {
            Expect(OffsetOf(expected, moved->start) == target_offset &&
                       OffsetOf(expected, moved->end) == target_offset + text.size(),
                   "the reported range covers the moved text: " + context);
          }

          Expect(viewport.Undo(), "the move undoes: " + context);
          Expect(DocumentText(viewport) == kDocument,
                 "undo restores the document exactly: " + context + ", got <" +
                     DocumentText(viewport) + ">");
          Expect(viewport.Redo(), "the move redoes: " + context);
          Expect(DocumentText(viewport) == expected,
                 "redo reproduces the move: " + context);
        }
      }
    }
  }
}

}

void RegisterTextDragDropTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TextDragDrop/DropAdjustmentForRemovedRange", TestDropAdjustmentForRemovedRange);
  AddTest(tests, "TextDragDrop/PositionAfterInsertedText", TestPositionAfterInsertedText);
  AddTest(tests, "TextDragDrop/MoveWithinALineForward", TestMoveWithinALineForward);
  AddTest(tests, "TextDragDrop/MoveBackwardIsNotShifted", TestMoveBackwardIsNotShifted);
  AddTest(tests, "TextDragDrop/MoveAcrossLines", TestMoveAcrossLines);
  AddTest(tests, "TextDragDrop/CopyLeavesTheSourceInPlace", TestCopyLeavesTheSourceInPlace);
  AddTest(tests, "TextDragDrop/DropInsideTheSourceIsANoOp", TestDropInsideTheSourceIsANoOp);
  AddTest(tests, "TextDragDrop/MoveUndoesInOneStep", TestMoveUndoesInOneStep);
  AddTest(tests, "TextDragDrop/ReversedSourceRangeIsNormalized", TestReversedSourceRangeIsNormalized);
  AddTest(tests, "TextDragDrop/MatchesAFlatStringOracle", TestDragDropMatchesAFlatStringOracle);
}

}
