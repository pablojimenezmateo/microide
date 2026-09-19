// Word- and line-granular drag selection, as PROPERTIES rather than fixtures.
//
// `SelectionGranularityTests` pins eight specific drags. What has to hold is
// smaller and stronger, and it is what the header promises: a granular drag
// "may never shrink below the unit the initiating click selected", and it must
// land on whole units. Those two sentences are checkable for every pointer
// position in a document rather than for eight of them -- so this drags to
// EVERY position, in both directions, over the shapes the unit scans get wrong
// (multi-byte letters, an operator run, tabs, an empty line, a line that is one
// character, trailing whitespace).
//
// Three surfaces run this code (the editor pane, the compare right pane, the
// merge result pane), so a defect here is a defect in all three at once.

#include "TestSupport.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "editor/EditTypes.h"
#include "editor/TextLayout.h"
#include "editor/TextViewport.h"
#include "workspace/coordinators/SelectionGranularity.h"
#include "workspace/state/WorkspaceInteractionState.h"

namespace microide::tests {
namespace {

using microide::editor::SelectionRange;
using microide::editor::TextLayout;
using microide::editor::TextPosition;
using microide::editor::TextViewport;
using microide::workspace::InteractionState;
namespace granularity = microide::workspace::selection_granularity;

bool Less(const TextPosition& a, const TextPosition& b) {
  return a.line < b.line || (a.line == b.line && a.column < b.column);
}
bool LessEq(const TextPosition& a, const TextPosition& b) { return !Less(b, a); }

std::string Show(const TextPosition& p) {
  return "(" + std::to_string(p.line) + "," + std::to_string(p.column) + ")";
}

std::string Show(const SelectionRange& r) { return Show(r.start) + "-" + Show(r.end); }

// Documents built from the shapes the word/line unit scans get wrong.
const std::vector<std::string>& Documents() {
  static const std::vector<std::string> documents = {
      "alpha beta gamma\n",
      "a->b.c(d, e) === f\n",
      "caf\xc3\xa9 na\xc3\xafve \xe6\x97\xa5\xe6\x9c\xac\n",
      "\tindented\ttabs\t\n",
      "one\n\nthree\n",
      "x\n",
      "trailing   \n",
      "first line\nsecond line\nthird line\n",
  };
  return documents;
}

void RunGranularDragSweep(InteractionState::SelectionGranularity granularity_kind, int clicks) {
  const char* kind = clicks == 2 ? "word" : "line";
  std::size_t drags = 0;
  std::size_t grew_past_seed = 0;

  for (const std::string& content : Documents()) {
    TextViewport probe;
    probe.SetViewportSize(20, 200);
    probe.LoadContent(content, "/tmp/granularity-sweep.txt");
    const std::size_t line_count = probe.line_count();

    // Seed from every position, then drag to every position.
    for (std::size_t seed_line = 0; seed_line < line_count; ++seed_line) {
      const std::size_t seed_width = probe.lines().LineLength(seed_line);
      for (std::size_t seed_column = 0; seed_column <= seed_width; ++seed_column) {
        TextViewport viewport;
        viewport.SetViewportSize(20, 200);
        viewport.LoadContent(content, "/tmp/granularity-sweep.txt");
        InteractionState state;
        viewport.MoveCursorTo(seed_line, seed_column);
        granularity::ApplyClick(state, viewport, clicks);
        if (state.selection_granularity != granularity_kind) {
          // The click found no unit here (whitespace with nothing either side,
          // an empty line). Character granularity is the documented answer and
          // there is no drag to judge.
          continue;
        }
        const SelectionRange seed = TextViewport::NormalizeRange(*viewport.selection_range());

        for (std::size_t line = 0; line < line_count; ++line) {
          const std::size_t width = viewport.lines().LineLength(line);
          for (std::size_t column = 0; column <= width; ++column) {
            const TextPosition target{line, column};
            granularity::ExtendToPointer(state, viewport, target);
            ++drags;

            const auto selected = viewport.selection_range();
            const std::string where = std::string(kind) + " drag seeded at " + Show(seed.start) +
                                      "-" + Show(seed.end) + " to " + Show(target);
            Expect(selected.has_value(), where + ": a granular drag always has a selection");
            const SelectionRange range = TextViewport::NormalizeRange(*selected);

            // 1. Never shrinks below the seed. This is the header's promise.
            Expect(LessEq(range.start, seed.start) && LessEq(seed.end, range.end),
                   where + ": the drag shrank below its seed, got " + Show(range));
            if (Less(range.start, seed.start) || Less(seed.end, range.end)) {
              ++grew_past_seed;
            }

            // 2. Covers the pointer, so the drag reaches what is under it.
            Expect(LessEq(range.start, target) && LessEq(target, range.end),
                   where + ": the drag does not cover the pointer, got " + Show(range));

            // 3. In bounds and on codepoint boundaries -- a selection edge
            //    mid-scalar corrupts UTF-8 the moment it is typed over.
            for (const TextPosition& edge : {range.start, range.end}) {
              Expect(edge.line < viewport.line_count(),
                     where + ": edge " + Show(edge) + " names a line that does not exist");
              const std::string_view text = viewport.lines().LineView(edge.line);
              Expect(edge.column <= text.size(),
                     where + ": edge " + Show(edge) + " is past the end of its line");
              Expect(TextLayout::ClampTextColumn(text, edge.column) == edge.column,
                     where + ": edge " + Show(edge) + " splits a character");
            }

            // 4. Idempotent: dragging to the same point again changes nothing.
            granularity::ExtendToPointer(state, viewport, target);
            const SelectionRange again =
                TextViewport::NormalizeRange(*viewport.selection_range());
            Expect(again.start == range.start && again.end == range.end,
                   where + ": a second drag to the same point moved the selection, " +
                       Show(range) + " -> " + Show(again));
          }
        }
      }
    }
  }

  // Vacuity: the sweep must have dragged, and must have dragged BEYOND the seed
  // at least sometimes -- a run where every drag stayed inside its seed would
  // satisfy every rule above without testing the growth they are about.
  Expect(drags > 2000, std::string("the ") + kind + " sweep barely dragged: " +
                           std::to_string(drags));
  Expect(grew_past_seed > 500, std::string("the ") + kind +
                                   " sweep almost never grew past its seed: " +
                                   std::to_string(grew_past_seed));
}

void TestWordDragKeepsItsSeedAndItsUnits() {
  RunGranularDragSweep(InteractionState::SelectionGranularity::Word, /*clicks=*/2);
}

void TestLineDragKeepsItsSeedAndItsUnits() {
  RunGranularDragSweep(InteractionState::SelectionGranularity::Line, /*clicks=*/3);
}

}  // namespace

void RegisterSelectionGranularityPropertyTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SelectionGranularityProperty/WordDragKeepsItsSeedAndItsUnits",
          TestWordDragKeepsItsSeedAndItsUnits);
  AddTest(tests, "SelectionGranularityProperty/LineDragKeepsItsSeedAndItsUnits",
          TestLineDragKeepsItsSeedAndItsUnits);
}

}  // namespace microide::tests
