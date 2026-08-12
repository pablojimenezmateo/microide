#include "TestSupport.h"

#include "editor/LineSpan.h"
#include "editor/TextBuffer.h"
#include "editor/TextLayout.h"
#include "editor/TextLayoutCache.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::LayoutLine;
using microide::editor::LineLayoutFacts;
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

// TD-2026-08-06-159: `VisualColumnsAreIdentity` is what lets a row render skip
// building a whole-line `LineVisualColumnMap` — two heap vectors per row — and use
// the identity mapping instead. A false positive silently misplaces every
// selection fill and caret on the row, so this asserts the claim directly at every
// column of every width class, and asserts the answer is not vacuously "no".
void TestVisualColumnsAreIdentityMatchesTheWalk() {
  bool saw_identity = false;
  bool saw_non_identity = false;
  for (const char* row : kRows) {
    const std::string_view line(row);
    const bool identity = TextLayout::VisualColumnsAreIdentity(line);
    identity ? saw_identity = true : saw_non_identity = true;
    if (!identity) {
      continue;
    }
    for (std::size_t column = 0; column <= line.size(); ++column) {
      const std::size_t walked = TextLayout::VisualColumnForTextColumn(line, column, kTabSize);
      Expect(walked == column,
             std::string("identity claimed for '") + row + "' but column " +
                 std::to_string(column) + " maps to visual " + std::to_string(walked));
    }
  }
  Expect(saw_identity && saw_non_identity,
         "the fixture must contain both identity and non-identity rows");
  // The boundary cases the byte scan has to get right.
  Expect(TextLayout::VisualColumnsAreIdentity(""), "an empty line is trivially identity");
  Expect(!TextLayout::VisualColumnsAreIdentity("\t"), "a lone tab is not identity");
  Expect(!TextLayout::VisualColumnsAreIdentity("abc\t"), "a trailing tab is not identity");
  Expect(!TextLayout::VisualColumnsAreIdentity("\xE4\xB8\xAD"),
         "a multi-byte code point is not identity");
  Expect(TextLayout::VisualColumnsAreIdentity(std::string(4096, 'x')),
         "a long plain-ASCII line is identity");
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

// VisualColumnForTextColumn skips decoding over the plain-ASCII prefix of a line
// and only walks code points from the first tab / first byte >= 0x80. That prefix
// scan reads eight bytes at a time, so the interesting cases are exactly the ones
// a hand-written row array does not cover: the special byte landing at every
// offset inside a chunk, on the chunk boundary, and in the sub-chunk tail.
//
// Differentially check the fast path against a straightforward per-code-point
// reference walk for a special byte at EVERY offset of lines spanning several
// chunk lengths, at every query column.
void TestVisualColumnFastPathMatchesReferenceWalk() {
  const auto reference_walk = [](std::string_view line, std::size_t text_column) {
    const std::size_t clamped = TextLayout::ClampTextColumn(line, text_column);
    std::size_t visual = 0;
    for (std::size_t i = 0; i < clamped;) {
      visual = TextLayout::AdvanceVisualColumn(visual, line[i], kTabSize);
      i += util::Utf8SequenceLength(line, i);
    }
    return visual;
  };

  // Tab, a 2-byte code point, a 3-byte code point, and a lone invalid byte (which
  // the decoder charges as one cell) — every class the prefix scan must refuse to
  // swallow.
  const std::string kSpecials[] = {"\t", "\xC3\xA9", "\xE4\xB8\xAD", "\xFF"};
  for (std::size_t length = 0; length <= 20; ++length) {
    const std::string ascii(length, 'a');
    // The all-plain line itself: the fast path returns without decoding at all.
    for (std::size_t c = 0; c <= ascii.size(); ++c) {
      Expect(TextLayout::VisualColumnForTextColumn(ascii, c, kTabSize) == reference_walk(ascii, c),
             "plain-ASCII fast path matches the reference walk");
    }
    for (const std::string& special : kSpecials) {
      for (std::size_t at = 0; at <= length; ++at) {
        std::string line = ascii;
        line.insert(at, special);
        for (std::size_t c = 0; c <= line.size(); ++c) {
          Expect(TextLayout::VisualColumnForTextColumn(line, c, kTabSize) ==
                     reference_walk(line, c),
                 "prefix-skipping fast path matches the reference walk at every chunk offset");
        }
      }
    }
  }
}

// BuildVisibleLineInto no longer steps code points from column 0 to reach the
// horizontal scroll offset: it starts at the line's first tab-or-multibyte byte,
// where visual column can first stop tracking byte offset, and a caller holding
// `LineLayoutFacts` for an all-plain line skips even that scan (TD-2026-08-05-132).
//
// Both are claims about EVERY line, not fast paths for a special case, so pin them
// differentially against a reference that walks from byte 0 the old way — with the
// special byte at every offset of lines spanning several eight-byte scan chunks,
// and at every scroll offset including past end-of-line.
void TestBuildVisibleLineMatchesWalkFromColumnZero() {
  const auto reference_build = [](std::string_view line, std::size_t horizontal_scroll,
                                  std::size_t visible_columns) {
    LayoutLine out;
    out.visual_columns = TextLayout::VisualColumnForTextColumn(line, line.size(), kTabSize);
    if (visible_columns == 0) {
      return out;
    }
    std::size_t visual_column = 0;
    for (std::size_t i = 0; i < line.size();) {
      const char character = line[i];
      const std::size_t next_text = i + util::Utf8SequenceLength(line, i);
      const std::size_t next_visual =
          TextLayout::AdvanceVisualColumn(visual_column, character, kTabSize);
      for (std::size_t cell = 0; cell < next_visual - visual_column; ++cell) {
        const std::size_t absolute_cell = visual_column + cell;
        if (absolute_cell < horizontal_scroll) {
          continue;
        }
        if (absolute_cell >= horizontal_scroll + visible_columns) {
          break;
        }
        out.text_offsets.push_back(out.text.size());
        if (character == '\t') {
          out.text.push_back(' ');
        } else {
          out.text.append(line, i, next_text - i);
        }
        out.source_columns.push_back(i);
      }
      visual_column = next_visual;
      i = next_text;
      if (visual_column >= horizontal_scroll + visible_columns) {
        break;
      }
    }
    return out;
  };

  const auto same = [](const LayoutLine& a, const LayoutLine& b) {
    return a.text == b.text && a.source_columns == b.source_columns &&
           a.text_offsets == b.text_offsets && a.visual_columns == b.visual_columns;
  };

  const std::string kSpecials[] = {"\t", "\xC3\xA9", "\xE4\xB8\xAD", "\xFF"};
  std::vector<std::string> lines;
  for (std::size_t length = 0; length <= 18; ++length) {
    const std::string ascii(length, 'a');
    lines.push_back(ascii);
    for (const std::string& special : kSpecials) {
      for (std::size_t at = 0; at <= length; ++at) {
        std::string line = ascii;
        line.insert(at, special);
        lines.push_back(std::move(line));
      }
    }
  }

  LayoutLine built;
  for (const std::string& line : lines) {
    const LineLayoutFacts facts = TextLayout::MeasureLineFacts(line, kTabSize);
    Expect(facts.known, "MeasureLineFacts reports its result as known");
    Expect(facts.visual_columns ==
               TextLayout::VisualColumnForTextColumn(line, line.size(), kTabSize),
           "MeasureLineFacts agrees with the direct width walk");
    Expect(facts.plain_ascii == (line.find('\t') == std::string::npos &&
                                 line.find_first_of("\xC3\xE4\xA9\xB8\xAD\xFF") ==
                                     std::string::npos),
           "plain_ascii means no tab and no byte >= 0x80");

    for (std::size_t scroll = 0; scroll <= line.size() + 3; ++scroll) {
      for (const std::size_t columns : {std::size_t{0}, std::size_t{1}, std::size_t{4},
                                        std::size_t{32}}) {
        const LayoutLine expected = reference_build(line, scroll, columns);
        TextLayout::BuildVisibleLineInto(line, scroll, columns, kTabSize, built);
        Expect(same(built, expected),
               "the prefix-skipping build matches a walk from column zero");
        TextLayout::BuildVisibleLineInto(line, scroll, columns, kTabSize, built, facts);
        Expect(same(built, expected),
               "the facts-hinted build matches a walk from column zero");
      }
    }
  }
}

// TD-2026-08-05-133: the visible-row build reads a bounded byte window of the
// line instead of all of it, so a piece-tree line that spans pieces -- every line
// an in-line edit has touched -- is no longer copied whole to render one row.
//
// Two things have to hold and only the first is obvious: the window build must
// agree with the whole-line build (same glyphs, same source columns, same offsets)
// AND the window must be big enough that the walk never runs off the end of it. A
// window one byte short does not crash; it silently drops the row's last cell. So
// the oracle is the whole-line build, over the same generated line set that pins
// the prefix-skipping walk, at every scroll offset and several widths.
void TestBuildVisibleLineWindowMatchesWholeLineBuild() {
  const auto same = [](const LayoutLine& a, const LayoutLine& b) {
    return a.text == b.text && a.source_columns == b.source_columns &&
           a.text_offsets == b.text_offsets && a.visual_columns == b.visual_columns;
  };

  const std::string kSpecials[] = {"\t", "\xC3\xA9", "\xE4\xB8\xAD", "\xFF"};
  std::vector<std::string> lines;
  for (std::size_t length = 0; length <= 18; ++length) {
    const std::string ascii(length, 'a');
    lines.push_back(ascii);
    for (const std::string& special : kSpecials) {
      for (std::size_t at = 0; at <= length; ++at) {
        std::string line = ascii;
        line.insert(at, special);
        lines.push_back(std::move(line));
      }
    }
  }
  // Long lines too, since the window sizing is the whole point and a 20-byte line
  // is shorter than any window it can ask for. The all-multibyte ones are what
  // makes the four-bytes-per-cell factor load-bearing: on a line of four-byte code
  // points the walk consumes exactly four bytes per visual column, so a window
  // sized on columns alone -- or one byte short -- drops the row's last cells.
  lines.push_back(std::string(600, 'x') + "\t" + std::string(600, 'y'));
  lines.push_back(std::string(600, 'x') + "\xE4\xB8\xAD" + std::string(600, 'y'));
  std::string four_byte;
  std::string three_byte;
  for (int i = 0; i < 300; ++i) {
    four_byte += "\xf0\x9f\x98\x80";  // U+1F600, four bytes, one cell
    three_byte += "\xE4\xB8\xAD";     // U+4E2D, three bytes, one cell
  }
  lines.push_back(four_byte);
  lines.push_back(three_byte);
  lines.push_back(std::string(40, 'x') + four_byte);

  LayoutLine expected;
  LayoutLine windowed;
  std::string scratch;
  for (const std::string& line : lines) {
    // Fragment the line so every window read that crosses a piece boundary takes
    // LineWindow's copying path rather than its zero-copy one.
    microide::editor::TextBuffer buffer;
    buffer.ResetFromText(line + "\n");
    // Fragment without changing the content: insert a byte mid-line and delete it
    // again. The tree never re-merges pieces, so every window read that crosses
    // the seam takes LineWindow's copying path. A zero-length splice is a no-op
    // and would leave the line contiguous.
    if (line.size() >= 2) {
      const std::size_t mid = line.size() / 2;
      buffer.ReplaceTextRange(0, mid, 0, mid, "Z");
      buffer.ReplaceTextRange(0, mid, 0, mid + 1, "");
    }
    const microide::editor::LineSpan span(buffer);
    const LineLayoutFacts facts = TextLayout::MeasureLineFacts(line, kTabSize);
    if (line.size() >= 2) {
      // The fixture claims the line spans pieces; prove it, or the copying path
      // this exercises is never taken and the loop below measures nothing.
      util::ResetPerformanceCounters();
      (void)buffer.LineView(0);
      Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorLineMaterializations) == 1,
             "the fixture line must actually span pieces");
      util::ResetPerformanceCounters();
    }

    const std::size_t scroll_limit = std::min<std::size_t>(line.size() + 3, 64);
    for (std::size_t scroll = 0; scroll <= scroll_limit; ++scroll) {
      for (const std::size_t columns :
           {std::size_t{0}, std::size_t{1}, std::size_t{4}, std::size_t{32}}) {
        TextLayout::BuildVisibleLineInto(line, scroll, columns, kTabSize, expected, facts);

        // Exactly what TextLayoutCache does to size and read the window.
        const std::size_t probe = std::min(scroll, line.size());
        const std::size_t plain_prefix_end =
            facts.plain_ascii
                ? line.size()
                : util::FirstNonAsciiOrByte(std::string_view(line).substr(0, probe), '\t');
        const std::size_t start_byte =
            TextLayout::ComputeVisibleLineWindowStart(scroll, line.size(), plain_prefix_end);
        const TextLayout::VisibleLineWindow window{
            .bytes = span.LineWindow(
                0, start_byte, TextLayout::VisibleLineWindowBytes(start_byte, scroll, columns),
                scratch),
            .start_byte = start_byte,
            .line_length = line.size(),
        };
        TextLayout::BuildVisibleLineWindowInto(window, scroll, columns, kTabSize, windowed, facts);
        Expect(same(windowed, expected),
               "the windowed build must match the whole-line build (line of " +
                   std::to_string(line.size()) + " bytes, scroll " + std::to_string(scroll) +
                   ", " + std::to_string(columns) + " columns)");
      }
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
      (void)cache.VisibleLineLayoutRefCached(lines, i, 0, 80, kTabSize, /*content_revision=*/0);
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
    (void)cache.VisibleLineLayoutRefCached(lines, i, 0, 80, kTabSize, /*content_revision=*/0);
  }
  Expect(cache.stats().visible_line_evictions > 0,
         "querying more distinct lines than the cache holds must evict — otherwise the "
         "no-eviction assertion above proves nothing");
}

// Regression: an eviction now RECYCLES the evicted map node — its LayoutLine keeps
// its text/source_columns/text_offsets buffers and is rebuilt in place — so that
// scrolling through fresh content stops paying four allocations per row. A reuse
// that forgot to reset any part of the recycled LayoutLine would serve the
// previous line's content under the new key, which the caller cannot detect.
// Every recycled entry must therefore be byte-identical to a freshly built one.
void TestTextLayoutCacheRecycledEntriesMatchFreshBuilds() {
  std::vector<std::string> lines;
  lines.reserve(600);
  for (std::size_t i = 0; i < 600; ++i) {
    // Deliberately varied: different lengths, a tab, and a multibyte glyph, so a
    // stale buffer tail or a stale visual_columns would show up.
    lines.push_back(std::string(i % 37, 'x') + "\tline-" + std::to_string(i) + " \xc3\xa9 end");
  }
  microide::editor::TextLayoutCache cache;

  // Walk far past the cache limit so most of these are served by the recycle path.
  cache.ResetStats();
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const microide::editor::LayoutLine& cached =
        cache.VisibleLineLayoutRefCached(lines, i, 0, 80, kTabSize, /*content_revision=*/0);
    const microide::editor::LayoutLine fresh =
        microide::editor::TextLayout::BuildVisibleLine(lines[i], 0, 80, kTabSize);
    Expect(cached.text == fresh.text, "a recycled entry must hold the new line's visible text");
    Expect(cached.source_columns == fresh.source_columns,
           "a recycled entry must hold the new line's source columns");
    Expect(cached.text_offsets == fresh.text_offsets,
           "a recycled entry must hold the new line's text offsets");
    Expect(cached.visual_columns == fresh.visual_columns,
           "a recycled entry must hold the new line's visual width");
    Expect(!cached.caret_visible && cached.caret_column == 0,
           "a recycled entry must not carry the evicted line's caret state");
  }
  Expect(cache.stats().visible_line_evictions > 0,
         "the walk must actually evict, or the recycle path was never exercised");

  // Same content at a horizontal scroll: the recycled buffers are longer than the
  // window here, so a missing clear would leave a stale tail behind.
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const microide::editor::LayoutLine& cached =
        cache.VisibleLineLayoutRefCached(lines, i, 20, 16, kTabSize, /*content_revision=*/0);
    const microide::editor::LayoutLine fresh =
        microide::editor::TextLayout::BuildVisibleLine(lines[i], 20, 16, kTabSize);
    Expect(cached.text == fresh.text, "a recycled entry must not keep a stale text tail");
    Expect(cached.text_offsets == fresh.text_offsets,
           "a recycled entry must not keep stale text offsets");
  }
}

// TD-2026-08-12-189: the recency order used to be a FIFO over insertions, so a
// HIT did not protect an entry from the next miss's eviction. An entry inserted
// many frames ago sat at the front of the queue no matter how recently it had
// been read, so a caller holding the reference VisibleLineLayoutRefCached hands
// out could have that node recycled underneath it — the node stays alive
// (extract/insert rewrites it in place), so the symptom is a row painted with
// another row's glyphs, not a crash.
//
// Pin the property that distinguishes an LRU from a FIFO: an entry read on every
// round survives an unbounded number of misses, and the reference stays pointed
// at its own content.
void TestTextLayoutCacheHitProtectsEntryFromEviction() {
  std::vector<std::string> lines;
  lines.reserve(1024);
  for (std::size_t i = 0; i < 1024; ++i) {
    lines.push_back("line " + std::to_string(i) + " with distinguishable content");
  }
  microide::editor::TextLayoutCache cache;

  // Fill past the limit so every query below takes the eviction path.
  for (std::size_t i = 0; i < 512; ++i) {
    (void)cache.VisibleLineLayoutRefCached(lines, i, 0, 80, kTabSize, /*content_revision=*/0);
  }

  const microide::editor::LayoutLine& pinned =
      cache.VisibleLineLayoutRefCached(lines, 0, 0, 80, kTabSize, /*content_revision=*/0);
  const std::string expected = pinned.text;
  Expect(expected.find("line 0 ") == 0, "the pinned entry must hold line 0's own text");

  cache.ResetStats();
  for (std::size_t i = 512; i < lines.size(); ++i) {
    (void)cache.VisibleLineLayoutRefCached(lines, i, 0, 80, kTabSize, /*content_revision=*/0);
    // Re-reading line 0 every round is what an LRU is for. Under the old FIFO
    // this was a miss after the very first eviction, and the node `pinned` refers
    // to had already been recycled for line 512.
    const microide::editor::LayoutLine& again =
        cache.VisibleLineLayoutRefCached(lines, 0, 0, 80, kTabSize, /*content_revision=*/0);
    Expect(&again == &pinned, "an entry hit every round must not be evicted or relocated");
    Expect(pinned.text == expected, "a held reference must never observe another row's glyphs");
  }
  Expect(cache.stats().visible_line_evictions > 0,
         "the loop must actually evict, or this proves nothing");
  Expect(cache.stats().visible_line_hits >= lines.size() - 512,
         "each round's re-read of line 0 must be a hit");
}

// TD-2026-08-06-143: MaxVisualColumns rebuilt on a tab-size or line-count change
// but NOT on a content-revision change — and then stamped the new revision onto
// the table it had just declined to verify. `LineWidthsAreCurrent`, the predicate
// every reader of the per-line width table goes through, checks all three, so a
// content edit that kept the line count and did not splice the table left it
// stale AND marked current: the max came from pre-edit widths, and every later
// LineFactsIfCurrent caller believed a width for text that was no longer there.
//
// No edit path in the tree does that today, which is exactly why this has to be
// tested at the cache boundary — the defect is that nothing STOPS one, not that
// one exists. Mutating the lines behind the cache's back is the point.
void TestTextLayoutCacheStaleContentRevisionRebuildsWidthTable() {
  std::vector<std::string> lines = {"aaaa", "bbbbbbbb", "cc"};
  microide::editor::TextLayoutCache cache;

  Expect(cache.MaxVisualColumns(lines, kTabSize, /*content_revision=*/1) == 8,
         "the first build must measure the widest line");
  Expect(cache.LineFactsIfCurrent(lines.size(), 1, kTabSize, /*content_revision=*/1)
                 .visual_columns == 8,
         "the width table must describe the line it was built from");

  // A content change that keeps the line count, with no splice and no
  // invalidation — i.e. the shape of the path this entry exists to make safe.
  lines[1] = "b";

  util::ResetPerformanceCounters();
  Expect(cache.MaxVisualColumns(lines, kTabSize, /*content_revision=*/2) == 4,
         "a new content revision must not be answered from pre-edit widths");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorLineWidthRebuildStaleRevision) == 1,
         "the rebuild must be attributed to the stale revision, not silently folded into "
         "another reason — this counter is what makes such a path findable in a perf run");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorLineWidthTableBuilds) == 1,
         "exactly one build, and its reason counter must sum to it");
  Expect(cache.LineFactsIfCurrent(lines.size(), 1, kTabSize, /*content_revision=*/2)
                 .visual_columns == 1,
         "the rebuilt table must describe the post-edit line");

  // Control: the same query at an unchanged revision must NOT rebuild, or the
  // assertion above would pass for a cache that simply rebuilds every time.
  util::ResetPerformanceCounters();
  Expect(cache.MaxVisualColumns(lines, kTabSize, /*content_revision=*/2) == 4,
         "an unchanged revision must be served from the memoized maximum");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorLineWidthTableBuilds) == 0,
         "an unchanged revision must not rebuild the width table");
  util::ResetPerformanceCounters();
}

void TestTextLayoutCacheInvalidateAllClearsVisibleLineCache() {
  const std::vector<std::string> lines = {"alpha alpha", "bravo bravo", "charlie charlie"};
  microide::editor::TextLayoutCache cache;

  // Warm the visible-line LRU.
  for (std::size_t i = 0; i < lines.size(); ++i) {
    (void)cache.VisibleLineLayoutRefCached(lines, i, 0, 40, kTabSize, /*content_revision=*/0);
  }
  cache.ResetStats();
  for (std::size_t i = 0; i < lines.size(); ++i) {
    (void)cache.VisibleLineLayoutRefCached(lines, i, 0, 40, kTabSize, /*content_revision=*/0);
  }
  Expect(cache.stats().visible_line_hits == lines.size(),
         "the warmed visible-line cache should serve every repeat query from cache");

  // After InvalidateAll every one of those keys must miss again.
  cache.InvalidateAll();
  cache.ResetStats();
  for (std::size_t i = 0; i < lines.size(); ++i) {
    (void)cache.VisibleLineLayoutRefCached(lines, i, 0, 40, kTabSize, /*content_revision=*/0);
  }
  Expect(cache.stats().visible_line_queries == lines.size(),
         "every line should be queried again after InvalidateAll");
  Expect(cache.stats().visible_line_hits == 0,
         "InvalidateAll must clear the visible-line cache — it documents wiping EVERY cache, "
         "and the viewport copy/move constructors depend on that");

  // ClearVisibleLineAndMaxColumns keeps doing the same thing on its own.
  for (std::size_t i = 0; i < lines.size(); ++i) {
    (void)cache.VisibleLineLayoutRefCached(lines, i, 0, 40, kTabSize, /*content_revision=*/0);
  }
  cache.ClearVisibleLineAndMaxColumns();
  cache.ResetStats();
  (void)cache.VisibleLineLayoutRefCached(lines, 0, 0, 40, kTabSize, /*content_revision=*/0);
  Expect(cache.stats().visible_line_hits == 0,
         "ClearVisibleLineAndMaxColumns must still clear the visible-line cache");
}

// ClampTextColumn rounds a byte offset down to the start of the code point that
// contains it. It used to answer that by re-tiling the line from byte 0, one
// UTF-8 sequence at a time -- an O(1) question at O(column) cost, run four times
// per keystroke on the edit path (TD-2026-08-05-131). It now steps back over
// continuation bytes instead, which is exact because UTF-8 is self-synchronizing.
//
// Differential: for WELL-FORMED UTF-8 the backward step must agree with the
// forward tiling at every offset of every line. That is the whole claim, so test
// it exhaustively rather than on a hand-picked row list.
void TestClampTextColumnMatchesForwardTiling() {
  const auto forward_tiling = [](std::string_view line, std::size_t text_column) {
    const std::size_t clamped = std::min(text_column, line.size());
    if (clamped >= line.size()) {
      return line.size();
    }
    std::size_t current = 0;
    while (current < clamped) {
      const std::size_t next = current + util::Utf8SequenceLength(line, current);
      if (next > clamped) {
        break;
      }
      current = next;
    }
    return current;
  };

  // One representative of each UTF-8 sequence length, plus a tab and plain ASCII,
  // so every continuation-byte run length (0..3) appears at every alignment.
  const std::string units[] = {"a", "\t", "\xC3\xA9", "\xE4\xB8\xAD", "\xF0\x9F\x98\x80"};
  std::vector<std::string> lines = {"", "a", "\xF0\x9F\x98\x80"};
  for (const std::string& first : units) {
    for (const std::string& second : units) {
      for (const std::string& third : units) {
        lines.push_back(first + second + third);
        lines.push_back(second + first + third + second);
      }
    }
  }

  for (const std::string& line : lines) {
    for (std::size_t column = 0; column <= line.size() + 2; ++column) {
      const std::size_t clamped = TextLayout::ClampTextColumn(line, column);
      Expect(clamped == forward_tiling(line, column),
             "ClampTextColumn must agree with the forward tiling on well-formed UTF-8");
      Expect(clamped <= std::min(column, line.size()),
             "ClampTextColumn must never move the column forward");
      Expect(clamped == line.size() ||
                 (static_cast<unsigned char>(line[clamped]) & 0xC0u) != 0x80u,
             "ClampTextColumn must land on a lead byte, never inside a code point");
      // The neighbours the caret actually steps to must stay inside the line and
      // on boundaries too.
      const std::size_t previous = TextLayout::PreviousTextColumn(line, column);
      const std::size_t next = TextLayout::NextTextColumn(line, column);
      Expect(previous <= clamped && next >= clamped && next <= line.size(),
             "Previous/NextTextColumn must bracket the clamped column");
    }
  }
}

// Malformed bytes must still produce a usable clamp: inside the line, never past
// what was asked, and bounded -- a run of stray continuation bytes must not walk
// backwards forever.
void TestClampTextColumnBoundsMalformedSequences() {
  const std::string malformed[] = {
      std::string("\x80\x80\x80\x80\x80\x80", 6),   // continuation bytes with no lead
      std::string("a\xC3", 2),                       // truncated 2-byte sequence
      std::string("\xF0\x9F\x98", 3),                // truncated 4-byte sequence
      std::string("\xFF\xFE" "ab", 4),               // invalid lead bytes
  };
  for (const std::string& line : malformed) {
    for (std::size_t column = 0; column <= line.size() + 2; ++column) {
      const std::size_t clamped = TextLayout::ClampTextColumn(line, column);
      Expect(clamped <= line.size(), "a malformed line must still clamp inside the line");
      Expect(clamped <= std::min(column, line.size()),
             "a malformed line must still clamp at or before the requested column");
      Expect(std::min(column, line.size()) - clamped <= 3,
             "the backward re-sync must be bounded by the longest UTF-8 sequence");
    }
  }
}

void RegisterTextLayoutTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TextLayout/VisualColumnsAreIdentityMatchesTheWalk",
          TestVisualColumnsAreIdentityMatchesTheWalk);
  AddTest(tests, "TextLayout/ClampTextColumnMatchesForwardTiling",
          TestClampTextColumnMatchesForwardTiling);
  AddTest(tests, "TextLayout/ClampTextColumnBoundsMalformedSequences",
          TestClampTextColumnBoundsMalformedSequences);
  AddTest(tests, "TextLayoutCache/InvalidateAllClearsVisibleLineCache",
          TestTextLayoutCacheInvalidateAllClearsVisibleLineCache);
  AddTest(tests, "TextLayoutCache/StaleContentRevisionRebuildsWidthTable",
          TestTextLayoutCacheStaleContentRevisionRebuildsWidthTable);
  AddTest(tests, "TextLayout/RecycledEntriesMatchFreshBuilds",
          TestTextLayoutCacheRecycledEntriesMatchFreshBuilds);
  AddTest(tests, "TextLayout/VisibleWorkingSetDoesNotEvict",
          TestTextLayoutCacheVisibleWorkingSetDoesNotEvict);
  AddTest(tests, "TextLayout/HitProtectsEntryFromEviction",
          TestTextLayoutCacheHitProtectsEntryFromEviction);
  AddTest(tests, "TextLayout/AdvanceVisualColumnTabStops", TestAdvanceVisualColumnTabStops);
  AddTest(tests, "TextLayout/VisualColumnMapMatchesDirectWalk",
          TestVisualColumnMapMatchesDirectWalk);
  AddTest(tests, "TextLayout/VisualColumnFastPathMatchesReferenceWalk",
          TestVisualColumnFastPathMatchesReferenceWalk);
  AddTest(tests, "TextLayout/BuildVisibleLineWindowMatchesWholeLineBuild",
          TestBuildVisibleLineWindowMatchesWholeLineBuild);
  AddTest(tests, "TextLayout/BuildVisibleLineMatchesWalkFromColumnZero",
          TestBuildVisibleLineMatchesWalkFromColumnZero);
  AddTest(tests, "TextLayout/TextVisualRoundTrip", TestTextVisualRoundTrip);
  AddTest(tests, "TextLayout/ResolveVisualColumnMatchesTextLayout",
          TestResolveVisualColumnMatchesTextLayout);
}

}  // namespace microide::tests
