#include "TestSupport.h"

#include "editor/TextLayout.h"
#include "editor/TextLayoutCache.h"

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
    // 'b' is spelled \x62 rather than written plainly: a hex escape consumes as many
    // hex digits as it can, so "a\xE4\xB8\xADb" would swallow the b into \xAD. The
    // previous fix for that was to split the literal ("a\xE4\xB8\xAD" "b"), which is
    // correct but reads exactly like a missing comma in an array of strings — clang
    // warns on it for that reason. Escaping the byte keeps the row unambiguous.
    "a\xE4\xB8\xAD\x62",  // a 中 b  (中 = U+4E2D, 3 bytes)
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

// TextLayoutCache::InvalidateAll documents that it "wipes every cache + every
// cache key", and TextViewport's copy and move constructors call it precisely to
// hand the new viewport clean derived state. It had drifted from that contract:
// it left the visible-line layout LRU populated, because the visible-line half
// of the wipe was duplicated in ClearVisibleLineAndMaxColumns rather than shared.
//
// The visible-line cache is keyed on {line_index, horizontal_scroll,
// visible_columns, tab_size} and NOT on the content revision, so anything that
// claims to wipe it has to actually do so — a retained entry serves the layout of
// a line that no longer exists.
// TD-2026-07-25-104: VisibleLineLayoutRefCached hands out a reference INTO the
// LRU, so an eviction is the only thing that can dangle a reference a caller
// still holds. The safety argument is "a frame's working set is far below
// kVisibleLineCacheLimit, so a frame never evicts what it is still reading" —
// which was documented but unmeasured, so nothing failed if someone lowered the
// limit or a caller started querying many more lines at once.
//
// Pin both halves: a realistically-large visible working set evicts nothing, and
// a set that genuinely exceeds the limit does evict (so this is not passing just
// because the counter is never incremented).
void TestTextLayoutCacheVisibleWorkingSetDoesNotEvict() {
  std::vector<std::string> lines;
  lines.reserve(512);
  for (std::size_t i = 0; i < 512; ++i) {
    lines.push_back("line " + std::to_string(i) + " with some content to lay out");
  }
  microide::editor::TextLayoutCache cache;

  // A generous visible row count — far above any real editor pane, and still
  // comfortably under the cache limit.
  constexpr std::size_t kGenerousVisibleRows = 200;
  cache.ResetStats();
  for (std::size_t pass = 0; pass < 3; ++pass) {
    for (std::size_t i = 0; i < kGenerousVisibleRows; ++i) {
      (void)cache.VisibleLineLayoutRefCached(lines, i, 0, 80, kTabSize);
    }
  }
  Expect(cache.stats().visible_line_evictions == 0,
         "a frame-sized visible working set must never evict, or a reference handed to the "
         "renderer could dangle mid-frame");

  // Control: exceed the limit and confirm the counter does move, so the check
  // above cannot pass vacuously.
  cache.InvalidateAll();
  cache.ResetStats();
  for (std::size_t i = 0; i < lines.size(); ++i) {
    (void)cache.VisibleLineLayoutRefCached(lines, i, 0, 80, kTabSize);
  }
  Expect(cache.stats().visible_line_evictions > 0,
         "querying more distinct lines than the cache holds must evict — otherwise the "
         "no-eviction assertion above proves nothing");
}

void TestTextLayoutCacheInvalidateAllClearsVisibleLineCache() {
  const std::vector<std::string> lines = {"alpha alpha", "bravo bravo", "charlie charlie"};
  microide::editor::TextLayoutCache cache;

  // Warm the visible-line LRU.
  for (std::size_t i = 0; i < lines.size(); ++i) {
    (void)cache.VisibleLineLayoutRefCached(lines, i, 0, 40, kTabSize);
  }
  cache.ResetStats();
  for (std::size_t i = 0; i < lines.size(); ++i) {
    (void)cache.VisibleLineLayoutRefCached(lines, i, 0, 40, kTabSize);
  }
  Expect(cache.stats().visible_line_hits == lines.size(),
         "the warmed visible-line cache should serve every repeat query from cache");

  // After InvalidateAll every one of those keys must miss again.
  cache.InvalidateAll();
  cache.ResetStats();
  for (std::size_t i = 0; i < lines.size(); ++i) {
    (void)cache.VisibleLineLayoutRefCached(lines, i, 0, 40, kTabSize);
  }
  Expect(cache.stats().visible_line_queries == lines.size(),
         "every line should be queried again after InvalidateAll");
  Expect(cache.stats().visible_line_hits == 0,
         "InvalidateAll must clear the visible-line cache — it documents wiping EVERY cache, "
         "and the viewport copy/move constructors depend on that");

  // ClearVisibleLineAndMaxColumns keeps doing the same thing on its own.
  for (std::size_t i = 0; i < lines.size(); ++i) {
    (void)cache.VisibleLineLayoutRefCached(lines, i, 0, 40, kTabSize);
  }
  cache.ClearVisibleLineAndMaxColumns();
  cache.ResetStats();
  (void)cache.VisibleLineLayoutRefCached(lines, 0, 0, 40, kTabSize);
  Expect(cache.stats().visible_line_hits == 0,
         "ClearVisibleLineAndMaxColumns must still clear the visible-line cache");
}

void RegisterTextLayoutTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TextLayoutCache/InvalidateAllClearsVisibleLineCache",
          TestTextLayoutCacheInvalidateAllClearsVisibleLineCache);
  AddTest(tests, "TextLayout/VisibleWorkingSetDoesNotEvict",
          TestTextLayoutCacheVisibleWorkingSetDoesNotEvict);
  AddTest(tests, "TextLayout/AdvanceVisualColumnTabStops", TestAdvanceVisualColumnTabStops);
  AddTest(tests, "TextLayout/VisualColumnMapMatchesDirectWalk",
          TestVisualColumnMapMatchesDirectWalk);
  AddTest(tests, "TextLayout/TextVisualRoundTrip", TestTextVisualRoundTrip);
  AddTest(tests, "TextLayout/ResolveVisualColumnMatchesTextLayout",
          TestResolveVisualColumnMatchesTextLayout);
}

}  // namespace microide::tests
