// Multi-caret MOTION, checked as invariants over the whole caret set.
//
// The edit side already has a reference model
// (`TextViewportMultiCaretReferenceTests.cpp`); motion did not. Motion is where
// a caret set gets malformed without anything visibly breaking until the NEXT
// keystroke: the multi-caret edit appliers assume the set arrives normalised
// (sorted, deduped, non-overlapping selections), so a motion that leaves two
// cursors overlapping makes the following edit silently do nothing, and a
// motion that leaves a column mid-codepoint corrupts UTF-8 on the following
// insert.
//
// So rather than pin expected coordinates -- which go stale whenever the wrap
// width or the word rule changes -- this asserts the properties every motion
// owes the set, for every motion verb, over documents built from the shapes the
// column arithmetic can get wrong (tabs, multi-byte and wide glyphs, empty
// lines, a line long enough to wrap several times):
//
//   frozen      motion never changes the document text
//   in bounds   every caret and anchor is a real position on a codepoint
//               boundary
//   normalised  cursors are sorted, none is duplicated, and no two selections
//               overlap (they may touch when both are non-collapsed)
//   collapsing  a plain (non-Shift) motion leaves no selection anywhere
//   anchored    a Shift motion moves no anchor: the set of anchors after it is
//               a subset of the set before (cursors may merge away, never
//               re-anchor)
//   monotone    the cursor count never grows

#include "TestSupport.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "editor/EditTypes.h"
#include "editor/TextLayout.h"
#include "editor/TextViewport.h"

namespace microide::tests {
namespace {

using microide::editor::SelectionRange;
using microide::editor::TextLayout;
using microide::editor::TextPosition;
using microide::editor::TextViewport;

bool Less(const TextPosition& a, const TextPosition& b) {
  return a.line < b.line || (a.line == b.line && a.column < b.column);
}

std::string Show(const TextPosition& p) {
  return "(" + std::to_string(p.line) + "," + std::to_string(p.column) + ")";
}

// One cursor: its caret plus the range it covers, normalised so start <= end.
struct Cursor {
  TextPosition caret;
  std::optional<TextPosition> anchor;
  SelectionRange range;
  bool collapsed = true;
  bool primary = false;
};

std::vector<Cursor> Cursors(const TextViewport& viewport) {
  std::vector<Cursor> out;
  const auto push = [&](const TextPosition& caret, const std::optional<TextPosition>& anchor,
                        bool primary) {
    Cursor c;
    c.primary = primary;
    c.caret = caret;
    c.anchor = anchor;
    c.collapsed = !anchor.has_value() || *anchor == caret;
    c.range = c.collapsed ? SelectionRange{caret, caret}
                          : (Less(*anchor, caret) ? SelectionRange{*anchor, caret}
                                                  : SelectionRange{caret, *anchor});
    out.push_back(c);
  };
  const TextPosition primary{viewport.cursor_line(), viewport.cursor_column()};
  std::optional<TextPosition> primary_anchor;
  if (const std::optional<SelectionRange> sel = viewport.selection_range(); sel.has_value()) {
    primary_anchor = sel->start == primary ? sel->end : sel->start;
  }
  push(primary, primary_anchor, true);
  for (const auto& secondary : viewport.secondary_caret_ranges()) {
    push(secondary.position, secondary.selection_anchor, false);
  }
  std::sort(out.begin(), out.end(), [](const Cursor& a, const Cursor& b) {
    if (Less(a.range.start, b.range.start)) return true;
    if (Less(b.range.start, a.range.start)) return false;
    return Less(a.range.end, b.range.end);
  });
  return out;
}

std::string Describe(const std::vector<Cursor>& cursors) {
  std::string out;
  for (const Cursor& c : cursors) {
    out += "[" + Show(c.range.start) + "-" + Show(c.range.end) + (c.collapsed ? "" : (c.caret == c.range.start ? "<" : ">")) + (c.primary ? "*" : "") + "]";
  }
  return out;
}

std::string DocumentText(const TextViewport& viewport) {
  std::string out;
  for (std::size_t line = 0; line < viewport.line_count(); ++line) {
    if (line > 0) out.push_back('\n');
    out.append(viewport.lines().LineView(line));
  }
  return out;
}

// A position is legal when its line exists, its column is within that line, and
// it sits on a codepoint boundary: the clamp is the identity there.
bool PositionIsLegal(const TextViewport& viewport, const TextPosition& p) {
  if (p.line >= viewport.line_count()) return false;
  const std::string_view text = viewport.lines().LineView(p.line);
  if (p.column > text.size()) return false;
  return TextLayout::ClampTextColumn(text, p.column) == p.column;
}

enum class Motion {
  kLeft,
  kRight,
  kUp,
  kDown,
  kWordLeft,
  kWordRight,
  kHome,
  kEnd,
  kPageUp,
  kPageDown,
};

constexpr Motion kAllMotions[] = {
    Motion::kLeft,   Motion::kRight, Motion::kUp,     Motion::kDown,   Motion::kWordLeft,
    Motion::kWordRight, Motion::kHome, Motion::kEnd, Motion::kPageUp, Motion::kPageDown,
};

const char* MotionName(Motion motion) {
  switch (motion) {
    case Motion::kLeft: return "Left";
    case Motion::kRight: return "Right";
    case Motion::kUp: return "Up";
    case Motion::kDown: return "Down";
    case Motion::kWordLeft: return "WordLeft";
    case Motion::kWordRight: return "WordRight";
    case Motion::kHome: return "Home";
    case Motion::kEnd: return "End";
    case Motion::kPageUp: return "PageUp";
    case Motion::kPageDown: return "PageDown";
  }
  return "?";
}

void Apply(TextViewport& viewport, Motion motion, bool extend) {
  switch (motion) {
    case Motion::kLeft: viewport.MoveCursorHorizontal(-1, extend); break;
    case Motion::kRight: viewport.MoveCursorHorizontal(1, extend); break;
    case Motion::kUp: viewport.MoveCursorVertical(-1, extend); break;
    case Motion::kDown: viewport.MoveCursorVertical(1, extend); break;
    case Motion::kWordLeft: viewport.MoveCursorWord(-1, extend); break;
    case Motion::kWordRight: viewport.MoveCursorWord(1, extend); break;
    case Motion::kHome: viewport.MoveCursorLineStart(extend); break;
    case Motion::kEnd: viewport.MoveCursorLineEnd(extend); break;
    case Motion::kPageUp: viewport.Page(-1, extend); break;
    case Motion::kPageDown: viewport.Page(1, extend); break;
  }
}

// Every invariant the caret set owes after one motion. `context` names the step.
// What the sweep actually exercised, so a green run cannot be vacuous.
struct Coverage {
  int multi_cursor_steps = 0;
  int selection_steps = 0;
  int merge_steps = 0;
  int wrapped_line_steps = 0;
};

void CheckWellFormed(const TextViewport& viewport,
                     const std::vector<Cursor>& before,
                     const std::string& text_before,
                     bool extend,
                     bool frozen_text,
                     Coverage& coverage,
                     const std::string& context) {
  if (frozen_text) {
    Expect(DocumentText(viewport) == text_before, context + ": motion changed the document text");
  }

  const std::vector<Cursor> after = Cursors(viewport);
  if (before.size() > 1) ++coverage.multi_cursor_steps;
  if (after.size() < before.size()) ++coverage.merge_steps;
  if (std::any_of(before.begin(), before.end(), [](const Cursor& c) { return !c.collapsed; })) {
    ++coverage.selection_steps;
  }

  // The secondary set is kept sorted by position: the edit appliers walk it
  // highest-first and every group-indexed cache reads that order.
  const auto secondaries = viewport.secondary_caret_ranges();
  for (std::size_t i = 1; i < secondaries.size(); ++i) {
    Expect(Less(secondaries[i - 1].position, secondaries[i].position) ||
               secondaries[i - 1].position == secondaries[i].position,
           context + ": the secondary caret vector came back out of order");
  }
  const std::string set = " set " + Describe(after) + " (was " + Describe(before) + ")";

  Expect(!after.empty(), context + ": the caret set went empty");
  Expect(after.size() <= before.size(), context + ": motion GREW the caret set," + set);

  for (const Cursor& c : after) {
    Expect(PositionIsLegal(viewport, c.caret),
           context + ": caret " + Show(c.caret) + " is out of bounds or mid-codepoint," + set);
    if (c.anchor.has_value()) {
      Expect(PositionIsLegal(viewport, *c.anchor),
             context + ": anchor " + Show(*c.anchor) + " is out of bounds or mid-codepoint," + set);
    }
    if (!extend) {
      // A plain motion collapses every selection, and an edit consumes them.
      Expect(c.collapsed, context + ": a plain motion or an edit left a selection behind," + set);
    }
  }

  for (std::size_t i = 1; i < after.size(); ++i) {
    const Cursor& prev = after[i - 1];
    const Cursor& cur = after[i];
    Expect(!(cur.caret == prev.caret && cur.anchor == prev.anchor),
           context + ": two identical cursors survived," + set);
    // Normalised: ranges may touch only when neither is collapsed. Any deeper
    // overlap is the shape the multi-caret edit appliers refuse to run over.
    const bool touching_is_allowed = !prev.collapsed && !cur.collapsed;
    const bool overlaps = touching_is_allowed ? Less(cur.range.start, prev.range.end)
                                              : !Less(prev.range.end, cur.range.start);
    Expect(!overlaps, context + ": two cursors overlap," + set);
  }

  if (extend && after.size() == before.size()) {
    // A Shift motion re-anchors nothing. Two cursors can only swap order by
    // overlapping, and overlapping cursors merge -- so an unchanged cursor count
    // means the sorted sets correspond one to one, and cursor i's anchor after
    // must be its anchor before, or the caret it just anchored at if it was
    // collapsed. (When a merge DID happen the union's far end is the other
    // cursor's post-motion caret, which is VS Code's rule and not a re-anchor.)
    for (std::size_t i = 0; i < after.size(); ++i) {
      if (!after[i].anchor.has_value()) continue;
      const TextPosition expected =
          before[i].anchor.value_or(before[i].caret);
      Expect(*after[i].anchor == expected,
             context + ": a Shift motion moved an anchor from " + Show(expected) + " to " +
                 Show(*after[i].anchor) + "," + set);
    }
  }
}

// Documents built from the shapes the column arithmetic gets wrong.
const std::vector<std::string>& MotionDocuments() {
  static const std::vector<std::string> documents = {
      "alpha beta gamma\ndelta\n\nepsilon zeta\n",
      "\tindented\n\t\tdeeper\n  spaces  here\n\n",
      "caf\xc3\xa9 na\xc3\xafve\n\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e text\n\xf0\x9f\x98\x80 emoji\n",
      "one_very_long_line_with_no_break_opportunity_at_all_so_it_has_to_be_split_mid_word_by_the_wrapper\nshort\n",
      "\n\n\n",
      "a\n",
      "trailing spaces   \n   leading\n\tmixed \t tabs\t\n",
      "x = f(a, b);  // comment\ny = g(c);\nz = h();\n",
  };
  return documents;
}

void RunMotionSweep(bool soft_wrap) {
  std::mt19937 rng(soft_wrap ? 20260919u : 20260920u);
  Coverage coverage;
  auto pick = [&](std::size_t n) {
    return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng);
  };

  for (const std::string& content : MotionDocuments()) {
    for (int iteration = 0; iteration < 200; ++iteration) {
      TextViewport viewport;
      viewport.SetViewportSize(4, 24);
      viewport.SetSoftWrap(soft_wrap);
      viewport.LoadContent(content, "/tmp/multi-caret-motion.txt");

      auto random_position = [&]() {
        const std::size_t line = pick(viewport.line_count());
        const std::string_view text = viewport.lines().LineView(line);
        return TextPosition{line, TextLayout::ClampTextColumn(text, pick(text.size() + 1))};
      };

      const TextPosition primary = random_position();
      viewport.MoveCursorTo(primary.line, primary.column);
      if (pick(2) == 0) {
        const TextPosition to = random_position();
        viewport.MoveCursorTo(to.line, to.column, /*extend_selection=*/true);
      }
      for (std::size_t s = 0, n = pick(4); s < n; ++s) {
        const TextPosition a = random_position();
        if (pick(2) == 0) {
          viewport.AddSecondaryCaret(a.line, a.column);
        } else {
          viewport.AddSecondaryCaretWithRange(SelectionRange{a, random_position()});
        }
      }

      for (int step = 0; step < 12; ++step) {
        const std::vector<Cursor> before = Cursors(viewport);
        const std::string text_before = DocumentText(viewport);
        // One step in eight is an EDIT, so the sweep also judges the caret set a
        // remap leaves behind -- the edits themselves have their own reference
        // model, but nothing checked that the set they produce is still one a
        // motion (or the next edit) can run over.
        const std::size_t roll = pick(8);
        std::string context = soft_wrap ? "wrapped " : "unwrapped ";
        bool extend = false;
        bool frozen_text = true;
        if (roll == 0) {
          frozen_text = false;
          context += "type";
          viewport.InsertCharacter('q');
        } else if (roll == 1) {
          frozen_text = false;
          context += "backspace";
          viewport.Backspace();
        } else {
          const Motion motion = kAllMotions[pick(std::size(kAllMotions))];
          extend = pick(2) == 0;
          context += std::string(extend ? "Shift+" : "") + MotionName(motion);
          Apply(viewport, motion, extend);
        }
        context += " step " + std::to_string(step);
        CheckWellFormed(viewport, before, text_before, extend, frozen_text, coverage, context);
      }
    }
  }
  // A green sweep is only evidence if it actually built multi-cursor sets, gave
  // them selections, and made some of them merge.
  const std::string tail = std::string(soft_wrap ? " (wrapped)" : " (unwrapped)") +
                           ": multi=" + std::to_string(coverage.multi_cursor_steps) +
                           " sel=" + std::to_string(coverage.selection_steps) +
                           " merge=" + std::to_string(coverage.merge_steps);
  Expect(coverage.multi_cursor_steps > 1000, "the sweep barely built multi-cursor sets" + tail);
  Expect(coverage.selection_steps > 1000, "the sweep barely built selections" + tail);
  Expect(coverage.merge_steps > 200, "the sweep never merged two cursors" + tail);
}

void TestMultiCaretMotionKeepsTheSetWellFormed() { RunMotionSweep(/*soft_wrap=*/false); }

void TestMultiCaretMotionKeepsTheSetWellFormedWhenWrapped() { RunMotionSweep(/*soft_wrap=*/true); }

}  // namespace

void RegisterEditorMultiCaretMotionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorMultiCaretMotion/KeepsTheSetWellFormed",
          TestMultiCaretMotionKeepsTheSetWellFormed);
  AddTest(tests, "EditorMultiCaretMotion/KeepsTheSetWellFormedWhenWrapped",
          TestMultiCaretMotionKeepsTheSetWellFormedWhenWrapped);
}

}  // namespace microide::tests
