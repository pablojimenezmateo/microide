#include "TestSupport.h"

#include "editor/TextLayout.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::LayoutLine;
using microide::editor::TextLayout;

constexpr std::size_t kTabSize = 4;

// Representative rows exercising every width class the display-column service
// must agree on: pure ASCII, leading tabs, interior tabs, a CJK codepoint
// (3 UTF-8 bytes, one cell in this model), a combining mark (each codepoint is
// its own cell), and a mix of all of them.
const char* const kRows[] = {
    "abcdef",             // ASCII
    "\t\tindented",       // leading tabs
    "ab\tcd\tef",         // interior tabs
    "a\xE4\xB8\xAD" "b",  // a 中 b  (中 = U+4E2D, 3 bytes)
    "e\xCC\x81xy",        // e + combining acute (U+0301, 2 bytes)
    "\t\xE4\xB8\xAD\t\xCC\x81z",  // tab, CJK, tab, combining, ASCII
};

// True when `column` sits on a codepoint boundary (ClampTextColumn is a no-op).
bool IsBoundary(std::string_view line, std::size_t column) {
  return TextLayout::ClampTextColumn(line, column) == column;
}

void TestAdvanceVisualColumnTabStops() {
  Expect(TextLayout::AdvanceVisualColumn(0, '\t', 4) == 4, "tab at 0 -> next stop 4");
  Expect(TextLayout::AdvanceVisualColumn(1, '\t', 4) == 4, "tab at 1 -> next stop 4");
  Expect(TextLayout::AdvanceVisualColumn(3, '\t', 4) == 4, "tab at 3 -> next stop 4");
  Expect(TextLayout::AdvanceVisualColumn(4, '\t', 4) == 8, "tab on a stop advances a full width");
  Expect(TextLayout::AdvanceVisualColumn(3, 'a', 4) == 4, "non-tab advances one cell");
  Expect(TextLayout::AdvanceVisualColumn(0, 'a', 4) == 1, "ascii from 0 -> 1");
  // tab_size 0 is guarded to width 1 (never a divide-by-zero or a stall).
  Expect(TextLayout::AdvanceVisualColumn(5, '\t', 0) == 6, "tab_size 0 guarded to width 1");
}

// The precomputed LineVisualColumnMap must return byte-identical results to the
// direct VisualColumnForTextColumn walk at every codepoint boundary, on every
// width class. This is the invariant every cached-vs-uncached caller relies on.
void TestVisualColumnMapMatchesDirectWalk() {
  for (const char* row : kRows) {
    const std::string_view line(row);
    const TextLayout::LineVisualColumnMap map(line, kTabSize);
    Expect(map.LineVisualWidth() ==
               TextLayout::VisualColumnForTextColumn(line, line.size(), kTabSize),
           "map width == direct full-line visual width");
    for (std::size_t c = 0; c <= line.size(); ++c) {
      if (!IsBoundary(line, c)) {
        continue;
      }
      const std::size_t direct = TextLayout::VisualColumnForTextColumn(line, c, kTabSize);
      Expect(map.VisualColumnFor(c) == direct, "cached map matches direct walk at a boundary");
    }
  }
}

// Text<->visual column mapping round-trips at every codepoint boundary: the
// visual column of a boundary maps back to exactly that boundary (caret placement
// after a mouse hit / vertical move must not drift across tabs or wide glyphs).
void TestTextVisualRoundTrip() {
  for (const char* row : kRows) {
    const std::string_view line(row);
    for (std::size_t c = 0; c <= line.size(); ++c) {
      if (!IsBoundary(line, c)) {
        continue;
      }
      const std::size_t visual = TextLayout::VisualColumnForTextColumn(line, c, kTabSize);
      Expect(TextLayout::TextColumnForVisualColumn(line, visual, kTabSize) == c,
             "visual(boundary) inverts to the same boundary");
    }
  }
}

// 023: the inlay-hint column resolver and the row-decoration column resolver are
// the same function (TextLayout::ResolveVisualColumn). Both the cell-grid
// `layout` path and the `visual_map` path must agree with each other and with the
// direct text-layout walk, so a hint's phantom cells anchor on exactly the visual
// column its annotated glyph renders at.
void TestResolveVisualColumnMatchesTextLayout() {
  for (const char* row : kRows) {
    const std::string_view line(row);
    const std::size_t width = TextLayout::VisualColumnForTextColumn(line, line.size(), kTabSize);
    // Full-width window: layout describes the whole line.
    const LayoutLine layout = TextLayout::BuildVisibleLine(line, 0, width + 8, kTabSize);
    const TextLayout::LineVisualColumnMap map(line, kTabSize);
    for (std::size_t c = 0; c <= line.size(); ++c) {
      if (!IsBoundary(line, c)) {
        continue;
      }
      const std::size_t direct = TextLayout::VisualColumnForTextColumn(line, c, kTabSize);
      const std::size_t via_layout = TextLayout::ResolveVisualColumn(&layout, nullptr, 0, width, c);
      const std::size_t via_map = TextLayout::ResolveVisualColumn(nullptr, &map, 0, width, c);
      const std::size_t identity = TextLayout::ResolveVisualColumn(nullptr, nullptr, 0, width, c);
      Expect(via_layout == direct, "layout-path inlay column matches text layout");
      Expect(via_map == direct, "visual-map-path inlay column matches text layout");
      Expect(identity == c, "identity path returns the source column unchanged");
    }
  }
}

}  // namespace

void RegisterTextLayoutTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TextLayout/AdvanceVisualColumnTabStops", TestAdvanceVisualColumnTabStops);
  AddTest(tests, "TextLayout/VisualColumnMapMatchesDirectWalk",
          TestVisualColumnMapMatchesDirectWalk);
  AddTest(tests, "TextLayout/TextVisualRoundTrip", TestTextVisualRoundTrip);
  AddTest(tests, "TextLayout/ResolveVisualColumnMatchesTextLayout",
          TestResolveVisualColumnMatchesTextLayout);
}

}  // namespace microide::tests
