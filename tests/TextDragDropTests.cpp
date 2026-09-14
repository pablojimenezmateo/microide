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

// Joined with '\n' BETWEEN lines, not after each: LoadContent("...\n") keeps a
// trailing empty line, so a per-line append would report a newline the buffer
// does not have.
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

// The arithmetic the entry called out: "the delete shifts the insert point when
// the drop is after the source". It shifts by a different amount depending on
// whether the drop shares the source's LAST line, which is the case a
// line-count-only adjustment gets wrong.
void TestDropAdjustmentForRemovedRange() {
  const SelectionRange source = Range(1, 2, 3, 4);

  const TextPosition before = drag::AdjustDropForRemovedRange(source, TextPosition{0, 7});
  Expect(before.line == 0 && before.column == 7, "a drop before the source is unaffected");

  const TextPosition below = drag::AdjustDropForRemovedRange(source, TextPosition{9, 3});
  Expect(below.line == 7 && below.column == 3,
         "a drop on a later line rises by the source's line span, keeping its column");

  // On the source's last line, past its end: that line's tail is spliced onto the
  // source's first line, so the drop lands at the join plus its offset into it.
  const TextPosition same_line = drag::AdjustDropForRemovedRange(source, TextPosition{3, 10});
  Expect(same_line.line == 1 && same_line.column == 2 + (10 - 4),
         "a drop on the source's last line lands at the splice join plus its offset");

  // A single-line source: no lines vanish, only columns shift.
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
  const SelectionRange source = Range(0, 0, 0, 6);  // "alpha "
  const auto moved = drag::Apply(viewport, source, TextPosition{0, 19}, /*copy=*/false);
  Expect(moved.has_value(), "a forward move inside one line applies");
  Expect(viewport.lines().LineView(0) == "bravo charliealpha ",
         "the text moves to the drop point, adjusted for its own removal");
  Expect(moved->start.column == 13 && moved->end.column == 19,
         "the returned range covers the moved text where it landed");
}

void TestMoveBackwardIsNotShifted() {
  TextViewport viewport = MakeViewport("alpha bravo charlie\n");
  const SelectionRange source = Range(0, 12, 0, 19);  // "charlie"
  const auto moved = drag::Apply(viewport, source, TextPosition{0, 0}, /*copy=*/false);
  Expect(moved.has_value(), "a backward move applies");
  Expect(viewport.lines().LineView(0) == "charliealpha bravo ",
         "a drop before the source is not shifted by the removal");
}

void TestMoveAcrossLines() {
  TextViewport viewport = MakeViewport("one\ntwo\nthree\nfour\n");
  // Move the whole "two\n" line to the top.
  const SelectionRange source = Range(1, 0, 2, 0);
  const auto moved = drag::Apply(viewport, source, TextPosition{0, 0}, /*copy=*/false);
  Expect(moved.has_value(), "a multi-line move applies");
  Expect(DocumentText(viewport) == "two\none\nthree\nfour\n",
         "the moved lines land at the drop point and leave no gap behind");
}

void TestCopyLeavesTheSourceInPlace() {
  TextViewport viewport = MakeViewport("alpha bravo\n");
  const SelectionRange source = Range(0, 0, 0, 5);  // "alpha"
  const auto copied = drag::Apply(viewport, source, TextPosition{0, 11}, /*copy=*/true);
  Expect(copied.has_value(), "a copy applies");
  Expect(viewport.lines().LineView(0) == "alpha bravoalpha",
         "a copy inserts at the drop point WITHOUT removing the source");
  Expect(copied->start.column == 11 && copied->end.column == 16,
         "the returned range covers the inserted copy");
}

void TestDropInsideTheSourceIsANoOp() {
  TextViewport viewport = MakeViewport("alpha bravo\n");
  const SelectionRange source = Range(0, 0, 0, 5);
  // Dropping onto itself must not delete-and-reinsert at a point that no longer
  // exists; it must do nothing at all.
  for (std::size_t column = 0; column <= 5; ++column) {
    const auto result = drag::Apply(viewport, source, TextPosition{0, column}, /*copy=*/false);
    Expect(!result.has_value(), "a drop inside the source is refused");
    Expect(viewport.lines().LineView(0) == "alpha bravo", "the document is untouched");
  }
}

// The entry's third requirement: "one undo entry covering a delete and an insert
// at two places". Without the group, Ctrl+Z leaves the text in neither place.
void TestMoveUndoesInOneStep() {
  TextViewport viewport = MakeViewport("alpha bravo charlie\n");
  const std::string before = DocumentText(viewport);
  const auto moved = drag::Apply(viewport, Range(0, 0, 0, 6), TextPosition{0, 19}, /*copy=*/false);
  Expect(moved.has_value(), "the move applies");
  Expect(DocumentText(viewport) != before, "the document changed");

  Expect(viewport.Undo(), "one undo is available");
  Expect(DocumentText(viewport) == before,
         "a single undo restores the document — the move is one entry, not two");
}

void TestReversedSourceRangeIsNormalized() {
  TextViewport viewport = MakeViewport("alpha bravo\n");
  // A drag that selects right-to-left hands over end < start.
  const auto moved = drag::Apply(viewport, Range(0, 5, 0, 0), TextPosition{0, 11}, /*copy=*/false);
  Expect(moved.has_value(), "a reversed source range still applies");
  Expect(viewport.lines().LineView(0) == " bravoalpha",
         "the reversed range moves the same text as the forward one");
}

// Every (source, drop) pair over a small document, move and copy, against a flat
// string oracle: offsets instead of (line, column), so the adjustment the entry
// calls out -- and the two ways it differs on the source's last line -- is
// checked by construction rather than at the three points a literal test picks.
// The oracle also gives the round trip: Undo must restore the document exactly
// and Redo must reproduce the move.
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
      if (start_offset == end_offset) continue;  // empty source: Apply refuses
      for (const TextPosition& drop : positions) {
        for (const bool copy : {false, true}) {
          const std::size_t drop_offset = OffsetOf(kDocument, drop);
          const std::string context =
              "source " + std::to_string(start_offset) + ".." + std::to_string(end_offset) +
              " drop " + std::to_string(drop_offset) + (copy ? " copy" : " move");

          TextViewport viewport = MakeViewport(kDocument);
          const auto moved = drag::Apply(viewport, SelectionRange{start, end}, drop, copy);

          // Oracle: splice the flat string. A drop inside the source (endpoints
          // included) is a no-op, which is what Apply reports as nullopt.
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

}  // namespace

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

}  // namespace microide::tests
