#include "TestSupport.h"

#include <cstdint>

#include "editor/LineSpan.h"
#include "editor/TextBuffer.h"
#include "editor/TextLayout.h"
#include "editor/TextLayoutCache.h"
#include "editor/TextViewport.h"
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

const char* const kRows[] = {
    "abcdef",
    "\t\tindented",
    "ab\tcd\tef",
    "a\xE4\xB8\xAD\x62",
    "e\xCC\x81xy",
    "\t\xE4\xB8\xAD\t\xCC\x81z",
};

bool IsBoundary(std::string_view line, std::size_t column) {
  return TextLayout::ClampTextColumn(line, column) == column;
}

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
  Expect(TextLayout::AdvanceVisualColumn(5, '\t', 0) == 6, "tab_size 0 guarded to width 1");
}

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

void TestTextVisualRoundTrip() {
  for (const char* row : kRows) {
    const std::string_view line(row);
    for (std::size_t c = 0; c <= line.size(); ++c) {
      if (!IsBoundary(line, c)) {
        continue;
      }
      if (c < line.size() &&
          TextLayout::VisualColumnForTextColumn(line, TextLayout::NextTextColumn(line, c),
                                                kTabSize) ==
              TextLayout::VisualColumnForTextColumn(line, c, kTabSize)) {
        continue;
      }
      const std::size_t visual = TextLayout::VisualColumnForTextColumn(line, c, kTabSize);
      Expect(TextLayout::TextColumnForVisualColumn(line, visual, kTabSize) == c,
             "visual(boundary) inverts to the same boundary");
    }
  }
}

void TestWideAndZeroWidthCodepointsTakeTheirCells() {
  const std::string_view cjk = "a\xE4\xB8\xAD\x62";
  Expect(TextLayout::VisualColumnForTextColumn(cjk, 4, kTabSize) == 3,
         "`a` + 中 = three cells before the `b`");
  Expect(TextLayout::VisualColumnForTextColumn(cjk, 5, kTabSize) == 4, "the line is four cells");
  Expect(TextLayout::TextColumnForVisualColumn(cjk, 1, kTabSize) == 1,
         "visual 1 is the wide glyph's own boundary");
  Expect(TextLayout::TextColumnForVisualColumn(cjk, 2, kTabSize) == 1,
         "visual 2 -- the glyph's midpoint -- rounds to its start, as a tab's midpoint does");
  Expect(TextLayout::TextColumnForVisualColumn(cjk, 3, kTabSize) == 4,
         "visual 3 is the boundary after the wide glyph");

  const std::string_view accent = "e\xCC\x81xy";
  Expect(TextLayout::VisualColumnForTextColumn(accent, 1, kTabSize) == 1, "the base is a cell");
  Expect(TextLayout::VisualColumnForTextColumn(accent, 3, kTabSize) == 1,
         "the combining mark adds no cell");
  Expect(TextLayout::VisualColumnForTextColumn(accent, accent.size(), kTabSize) == 3,
         "e-acute, x, y: three cells");
  Expect(TextLayout::TextColumnForVisualColumn(accent, 1, kTabSize) == 3,
         "visual 1 lands after the mark, never between base and mark");
  Expect(TextLayout::TextColumnForVisualColumn(accent, 0, kTabSize) == 0, "visual 0 is the start");

  const std::string_view emoji = "\xF0\x9F\x98\x80!";
  Expect(TextLayout::VisualColumnForTextColumn(emoji, 4, kTabSize) == 2, "an emoji is wide");
  Expect(TextLayout::MeasureLineFacts(emoji, kTabSize).visual_columns == 3,
         "MeasureLineFacts agrees");
  Expect(!TextLayout::MeasureLineFacts(emoji, kTabSize).plain_ascii, "and is not plain ASCII");

  const LayoutLine wide = TextLayout::BuildVisibleLine(cjk, 0, 16, kTabSize);
  Expect(wide.source_columns.size() == 4, "four cells for a 中 b");
  Expect(wide.source_columns[1] == 1 && wide.source_columns[2] == 1,
         "both cells of the wide glyph map to its byte");
  Expect(wide.text == "a\xE4\xB8\xAD\x62", "the row text holds each glyph once");
  Expect(wide.text_offsets[1] == 1 && wide.text_offsets[2] == 4 && wide.text_offsets[3] == 4,
         "the trailing cell's offset equals the next cell's (no text of its own)");
  const LayoutLine marked = TextLayout::BuildVisibleLine(accent, 0, 16, kTabSize);
  Expect(marked.source_columns.size() == 3, "three cells for e-acute x y");
  Expect(marked.text == "e\xCC\x81xy" && marked.text_offsets[1] == 3,
         "the mark's bytes belong to the base's cell");
  const LayoutLine wide_marked =
      TextLayout::BuildVisibleLine("\xE4\xB8\xAD\xCC\x81z", 0, 16, kTabSize);
  Expect(wide_marked.source_columns.size() == 3 && wide_marked.text == "\xE4\xB8\xAD\xCC\x81z" &&
             wide_marked.text_offsets[1] == 5 && wide_marked.text_offsets[2] == 5,
         "the mark joins the wide glyph's lead cell, the trailing cell stays empty");
  const LayoutLine scrolled = TextLayout::BuildVisibleLine(cjk, 2, 16, kTabSize);
  Expect(scrolled.source_columns.size() == 2 && scrolled.text == " b",
         "a wide glyph cut by the scroll edge leaves a blank trailing cell");
}

void TestWrapRowsPartitionTheLineAndResumeFromAnyBoundary() {
  std::uint64_t state = 0x9E3779B97F4A7C15ull;
  const auto next = [&state](std::uint64_t bound) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return bound == 0 ? 0 : static_cast<std::size_t>(state % bound);
  };
  static constexpr const char* kAtoms[] = {"a", "b", " ", "  ", "\t", "\xE4\xB8\xAD",
                                           "\xC3\xA9", "e\xCC\x81", "\xF0\x9F\x98\x80",
                                           "word", "longerword", "-", "\xCC\x81"};
  struct Row {
    std::size_t start;
    std::size_t end;
    std::size_t indent;
  };
  for (int trial = 0; trial < 400; ++trial) {
    std::string line;
    const std::size_t atoms = next(30);
    for (std::size_t i = 0; i < atoms; ++i) {
      line += kAtoms[next(std::size(kAtoms))];
    }
    const std::size_t tab_size = 2 + next(7);
    const std::size_t wrap_columns = 1 + next(24);
    const std::string context = "trial " + std::to_string(trial) + " wrap=" +
                                std::to_string(wrap_columns) + " tab=" +
                                std::to_string(tab_size) + " line=[" + line + "]";

    std::vector<Row> rows;
    TextLayout::WrapLineSegments(line, tab_size, wrap_columns,
                                 [&](std::size_t start, std::size_t end, std::size_t indent) {
                                   rows.push_back(Row{start, end, indent});
                                 });
    const std::size_t width = TextLayout::VisualColumnForTextColumn(line, line.size(), tab_size);
    Expect(!rows.empty() && rows.front().start == 0 && rows.back().end == width,
           "rows span the whole line: " + context);
    const TextLayout::LineVisualColumnMap map(line, tab_size);
    const auto is_boundary = [&](std::size_t visual) {
      for (std::size_t c = 0; c <= line.size(); ++c) {
        if (TextLayout::ClampTextColumn(line, c) == c && map.VisualColumnFor(c) == visual) {
          return true;
        }
      }
      return false;
    };
    for (std::size_t r = 0; r < rows.size(); ++r) {
      const Row& row = rows[r];
      Expect(row.start <= row.end, "a row does not run backwards: " + context);
      if (r > 0) {
        Expect(rows[r - 1].end == row.start, "rows are contiguous: " + context);
        Expect(row.indent == TextLayout::HangingIndentFor(line, tab_size, wrap_columns),
               "a continuation row carries the hanging indent: " + context);
        Expect(row.end > row.start, "a continuation row is never empty: " + context);
      } else {
        Expect(row.indent == 0, "the first row has no hanging indent: " + context);
      }
      Expect(is_boundary(row.start) && is_boundary(row.end),
             "a break sits on a code-point boundary: " + context);
      const std::size_t budget = wrap_columns - row.indent;
      if (row.end - row.start > budget) {
        const std::size_t start_byte =
            TextLayout::TextColumnForVisualColumn(line, row.start, tab_size);
        std::size_t after = start_byte;
        TextLayout::AdvanceVisualColumnAt(row.start, line, start_byte, tab_size, &after);
        Expect(map.VisualColumnFor(after) >= row.end,
               "a row overflows its width only for one indivisible code point: " + context);
      }
    }

    for (std::size_t r = 1; r < rows.size(); ++r) {
      const std::size_t boundary_byte =
          TextLayout::TextColumnForVisualColumn(line, rows[r].start, tab_size);
      Expect(map.VisualColumnFor(boundary_byte) == rows[r].start,
             "the boundary maps back to a byte offset: " + context);
      std::vector<Row> resumed;
      TextLayout::WrapLineSegmentsFrom(
          std::string_view(line).substr(boundary_byte), tab_size, wrap_columns, rows[r].start,
          TextLayout::HangingIndentFor(line, tab_size, wrap_columns),
          [&](std::size_t start, std::size_t end, std::size_t indent) {
            resumed.push_back(Row{start, end, indent});
          });
      Expect(resumed.size() == rows.size() - r, "resuming yields the remaining rows: " + context);
      for (std::size_t k = 0; k < resumed.size() && r + k < rows.size(); ++k) {
        Expect(resumed[k].start == rows[r + k].start && resumed[k].end == rows[r + k].end &&
                   resumed[k].indent == rows[r + k].indent,
               "a resumed row equals the whole-line row: " + context);
      }
    }
    Expect(TextLayout::WrapLineRowCount(line, tab_size, wrap_columns) == rows.size(),
           "WrapLineRowCount agrees: " + context);
  }
}

void TestWrapCountsWideGlyphsAsTwoCells() {
  const std::string_view line = "ab\xE4\xB8\xAD\xE6\x96\x87";
  std::vector<std::pair<std::size_t, std::size_t>> rows;
  TextLayout::WrapLineSegments(line, kTabSize, 5,
                               [&](std::size_t start, std::size_t end, std::size_t) {
                                 rows.emplace_back(start, end);
                               });
  Expect(rows.size() == 2, "six cells over a five-cell width wrap once");
  Expect(rows[0].first == 0 && rows[0].second == 4 && rows[1].first == 4 && rows[1].second == 6,
         "the break falls before the glyph that would not fit whole");
}

void TestResolveVisualColumnMatchesTextLayout() {
  for (const char* row : kRows) {
    const std::string_view line(row);
    const std::size_t width = TextLayout::VisualColumnForTextColumn(line, line.size(), kTabSize);
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

void TestVisualColumnFastPathMatchesReferenceWalk() {
  const auto reference_walk = [](std::string_view line, std::size_t text_column) {
    const std::size_t clamped = TextLayout::ClampTextColumn(line, text_column);
    std::size_t visual = 0;
    for (std::size_t i = 0; i < clamped;) {
      const std::size_t length = util::Utf8SequenceLength(line, i);
      visual = TextLayout::AdvanceVisualColumn(visual, line[i], kTabSize);
      if (length > 1) {
        visual += util::GridCellWidth(util::DecodeUtf8Codepoint(line.substr(i, length))) - 1;
      }
      i += length;
    }
    return visual;
  };

  const std::string kSpecials[] = {"\t", "\xC3\xA9", "\xE4\xB8\xAD", "\xFF"};
  for (std::size_t length = 0; length <= 20; ++length) {
    const std::string ascii(length, 'a');
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
      std::size_t next_text = i;
      const std::size_t next_visual =
          TextLayout::AdvanceVisualColumnAt(visual_column, line, i, kTabSize, &next_text);
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
        } else if (cell == 0) {
          out.text.append(line, i, next_text - i);
        } else if (absolute_cell == horizontal_scroll) {
          out.text.push_back(' ');
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
  lines.push_back(std::string(600, 'x') + "\t" + std::string(600, 'y'));
  lines.push_back(std::string(600, 'x') + "\xE4\xB8\xAD" + std::string(600, 'y'));
  std::string four_byte;
  std::string three_byte;
  for (int i = 0; i < 300; ++i) {
    four_byte += "\xf0\x9f\x98\x80";
    three_byte += "\xE4\xB8\xAD";
  }
  lines.push_back(four_byte);
  lines.push_back(three_byte);
  lines.push_back(std::string(40, 'x') + four_byte);

  LayoutLine expected;
  LayoutLine windowed;
  std::string scratch;
  for (const std::string& line : lines) {
    microide::editor::TextBuffer buffer;
    buffer.ResetFromText(line + "\n");
    if (line.size() >= 2) {
      const std::size_t mid = line.size() / 2;
      buffer.ReplaceTextRange(0, mid, 0, mid, "Z");
      buffer.ReplaceTextRange(0, mid, 0, mid + 1, "");
    }
    const microide::editor::LineSpan span(buffer);
    const LineLayoutFacts facts = TextLayout::MeasureLineFacts(line, kTabSize);
    if (line.size() >= 2) {
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

}

void TestTextLayoutCachePlainPrefixMemoSurvivesAnEdit() {
  using microide::editor::TextLayoutCache;
  const std::string e_acute = "\xc3\xa9";
  struct Case {
    const char* what;
    std::string before;
    std::string after;
    std::size_t edit_line;
    std::size_t column;
    std::size_t max_rescanned;
  };
  const std::string clean(1000, 'a');
  std::string dirty_early = clean;
  dirty_early.replace(100, 1, e_acute);
  std::string dirty_late = clean;
  dirty_late.replace(600, 1, e_acute);
  const auto insert = [](std::string text, std::size_t at, std::string_view what) {
    return text.insert(at, what);
  };
  const std::vector<Case> cases = {
      {"a clean line re-reads only from the edit", clean, insert(clean, 500, "b"), 0, 500, 501},
      {"an offending byte before the edit is still the answer", dirty_early,
       insert(dirty_early, 500, "b"), 0, 500, 0},
      {"an offending byte after the edit is found again", dirty_late, insert(dirty_late, 500, "b"), 0,
       500, 502},
      {"an inserted offending byte is found", clean, insert(clean, 200, e_acute), 0, 200, 1},
      {"a line above the edit is untouched", clean, clean, 1, 0, 0},
      {"an edit with no column re-reads the line", clean, insert(clean, 500, "b"), 0,
       TextLayoutCache::kNoEditColumn, 1001},
  };
  for (const Case& c : cases) {
    std::vector<std::string> lines = {c.before, "second line"};
    TextLayoutCache warm;
    (void)warm.PlainAsciiPrefixEnd(lines, 0, c.before.size(), 1);
    if (c.edit_line == 0) {
      lines[0] = c.after;
    } else {
      lines[1] = "second line, edited";
    }
    warm.CarryPlainPrefixMemosAcrossEdit(lines, c.edit_line, c.column, 1, 2);
    util::ResetPerformanceCounters();
    const std::size_t got = warm.PlainAsciiPrefixEnd(lines, 0, lines[0].size(), 2);
    const std::uint64_t rescanned =
        util::ReadPerformanceCounter(util::PerfCounterId::EditorVisibleLineLayoutPrefixBytesScanned);
    TextLayoutCache cold;
    const std::size_t want = cold.PlainAsciiPrefixEnd(lines, 0, lines[0].size(), 2);
    Expect(got == want, std::string(c.what) + ": " + std::to_string(got) + " != " + std::to_string(want));
    Expect(rescanned <= c.max_rescanned,
           std::string(c.what) + ": re-read " + std::to_string(rescanned) + " bytes");
  }
}

void TestSoftWrapTypingKeepsThePlainPrefixMemo() {
  microide::editor::TextViewport viewport;
  std::string line(200000, 'a');
  line += "\t";
  viewport.LoadContent(line + "\n");
  viewport.SetViewportSize(40, 80);
  viewport.SetSoftWrap(true);
  viewport.SetTabSize(3);
  viewport.MoveCursorTo(0, 100000);
  (void)viewport.PlainAsciiPrefixEnd(0, 150000);
  for (int i = 0; i < 3; ++i) {
    util::ResetPerformanceCounters();
    viewport.InsertText("x");
    (void)viewport.PlainAsciiPrefixEnd(0, 150000);
    const std::uint64_t read =
        util::ReadPerformanceCounter(util::PerfCounterId::EditorVisibleLineLayoutPrefixBytesScanned);
    Expect(read <= 50001, "a keystroke at column 100000 should re-read only from there, read " +
                              std::to_string(read) + " bytes");
  }
}

void TestTextLayoutCacheVisibleWorkingSetDoesNotEvict() {
  std::vector<std::string> lines;
  lines.reserve(512);
  for (std::size_t i = 0; i < 512; ++i) {
    lines.push_back("line " + std::to_string(i) + " with some content to lay out");
  }
  microide::editor::TextLayoutCache cache;

  constexpr std::size_t kGenerousVisibleRows = 200;
  cache.ResetStats();
  for (std::size_t pass = 0; pass < 3; ++pass) {
    for (std::size_t i = 0; i < kGenerousVisibleRows; ++i) {
      (void)cache.VisibleLineLayoutRefCached(lines, i, 0, 80, kTabSize, 0);
    }
  }
  Expect(cache.stats().visible_line_evictions == 0,
         "a frame-sized visible working set must never evict, or a reference handed to the "
         "renderer could dangle mid-frame");

  cache.InvalidateAll();
  cache.ResetStats();
  for (std::size_t i = 0; i < lines.size(); ++i) {
    (void)cache.VisibleLineLayoutRefCached(lines, i, 0, 80, kTabSize, 0);
  }
  Expect(cache.stats().visible_line_evictions > 0,
         "querying more distinct lines than the cache holds must evict — otherwise the "
         "no-eviction assertion above proves nothing");
}

void TestTextLayoutCacheRecycledEntriesMatchFreshBuilds() {
  std::vector<std::string> lines;
  lines.reserve(600);
  for (std::size_t i = 0; i < 600; ++i) {
    lines.push_back(std::string(i % 37, 'x') + "\tline-" + std::to_string(i) + " \xc3\xa9 end");
  }
  microide::editor::TextLayoutCache cache;

  cache.ResetStats();
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const microide::editor::LayoutLine& cached =
        cache.VisibleLineLayoutRefCached(lines, i, 0, 80, kTabSize, 0);
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

  for (std::size_t i = 0; i < lines.size(); ++i) {
    const microide::editor::LayoutLine& cached =
        cache.VisibleLineLayoutRefCached(lines, i, 20, 16, kTabSize, 0);
    const microide::editor::LayoutLine fresh =
        microide::editor::TextLayout::BuildVisibleLine(lines[i], 20, 16, kTabSize);
    Expect(cached.text == fresh.text, "a recycled entry must not keep a stale text tail");
    Expect(cached.text_offsets == fresh.text_offsets,
           "a recycled entry must not keep stale text offsets");
  }
}

void TestTextLayoutCacheHitProtectsEntryFromEviction() {
  std::vector<std::string> lines;
  lines.reserve(1024);
  for (std::size_t i = 0; i < 1024; ++i) {
    lines.push_back("line " + std::to_string(i) + " with distinguishable content");
  }
  microide::editor::TextLayoutCache cache;

  for (std::size_t i = 0; i < 512; ++i) {
    (void)cache.VisibleLineLayoutRefCached(lines, i, 0, 80, kTabSize, 0);
  }

  const microide::editor::LayoutLine& pinned =
      cache.VisibleLineLayoutRefCached(lines, 0, 0, 80, kTabSize, 0);
  const std::string expected = pinned.text;
  Expect(expected.find("line 0 ") == 0, "the pinned entry must hold line 0's own text");

  cache.ResetStats();
  for (std::size_t i = 512; i < lines.size(); ++i) {
    (void)cache.VisibleLineLayoutRefCached(lines, i, 0, 80, kTabSize, 0);
    const microide::editor::LayoutLine& again =
        cache.VisibleLineLayoutRefCached(lines, 0, 0, 80, kTabSize, 0);
    Expect(&again == &pinned, "an entry hit every round must not be evicted or relocated");
    Expect(pinned.text == expected, "a held reference must never observe another row's glyphs");
  }
  Expect(cache.stats().visible_line_evictions > 0,
         "the loop must actually evict, or this proves nothing");
  Expect(cache.stats().visible_line_hits >= lines.size() - 512,
         "each round's re-read of line 0 must be a hit");
}

void TestTextLayoutCacheStaleContentRevisionRebuildsWidthTable() {
  std::vector<std::string> lines = {"aaaa", "bbbbbbbb", "cc"};
  microide::editor::TextLayoutCache cache;

  Expect(cache.MaxVisualColumns(lines, kTabSize, 1) == 8,
         "the first build must measure the widest line");
  Expect(cache.LineFactsIfCurrent(lines.size(), 1, kTabSize, 1)
                 .visual_columns == 8,
         "the width table must describe the line it was built from");

  lines[1] = "b";

  util::ResetPerformanceCounters();
  Expect(cache.MaxVisualColumns(lines, kTabSize, 2) == 4,
         "a new content revision must not be answered from pre-edit widths");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorLineWidthRebuildStaleRevision) == 1,
         "the rebuild must be attributed to the stale revision, not silently folded into "
         "another reason — this counter is what makes such a path findable in a perf run");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorLineWidthTableBuilds) == 1,
         "exactly one build, and its reason counter must sum to it");
  Expect(cache.LineFactsIfCurrent(lines.size(), 1, kTabSize, 2)
                 .visual_columns == 1,
         "the rebuilt table must describe the post-edit line");

  util::ResetPerformanceCounters();
  Expect(cache.MaxVisualColumns(lines, kTabSize, 2) == 4,
         "an unchanged revision must be served from the memoized maximum");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorLineWidthTableBuilds) == 0,
         "an unchanged revision must not rebuild the width table");
  util::ResetPerformanceCounters();
}

void TestTextLayoutCacheInvalidateAllClearsVisibleLineCache() {
  const std::vector<std::string> lines = {"alpha alpha", "bravo bravo", "charlie charlie"};
  microide::editor::TextLayoutCache cache;

  for (std::size_t i = 0; i < lines.size(); ++i) {
    (void)cache.VisibleLineLayoutRefCached(lines, i, 0, 40, kTabSize, 0);
  }
  cache.ResetStats();
  for (std::size_t i = 0; i < lines.size(); ++i) {
    (void)cache.VisibleLineLayoutRefCached(lines, i, 0, 40, kTabSize, 0);
  }
  Expect(cache.stats().visible_line_hits == lines.size(),
         "the warmed visible-line cache should serve every repeat query from cache");

  cache.InvalidateAll();
  cache.ResetStats();
  for (std::size_t i = 0; i < lines.size(); ++i) {
    (void)cache.VisibleLineLayoutRefCached(lines, i, 0, 40, kTabSize, 0);
  }
  Expect(cache.stats().visible_line_queries == lines.size(),
         "every line should be queried again after InvalidateAll");
  Expect(cache.stats().visible_line_hits == 0,
         "InvalidateAll must clear the visible-line cache — it documents wiping EVERY cache, "
         "and the viewport copy/move constructors depend on that");

  for (std::size_t i = 0; i < lines.size(); ++i) {
    (void)cache.VisibleLineLayoutRefCached(lines, i, 0, 40, kTabSize, 0);
  }
  cache.ClearVisibleLineAndMaxColumns();
  cache.ResetStats();
  (void)cache.VisibleLineLayoutRefCached(lines, 0, 0, 40, kTabSize, 0);
  Expect(cache.stats().visible_line_hits == 0,
         "ClearVisibleLineAndMaxColumns must still clear the visible-line cache");
}

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
      const std::size_t previous = TextLayout::PreviousTextColumn(line, column);
      const std::size_t next = TextLayout::NextTextColumn(line, column);
      Expect(previous <= clamped && next >= clamped && next <= line.size(),
             "Previous/NextTextColumn must bracket the clamped column");
    }
  }
}

void TestClampTextColumnBoundsMalformedSequences() {
  const std::string malformed[] = {
      std::string("\x80\x80\x80\x80\x80\x80", 6),
      std::string("a\xC3", 2),
      std::string("\xF0\x9F\x98", 3),
      std::string("\xFF\xFE" "ab", 4),
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

void TestColumnStepsAgreeWithForwardTilingOnAnyBytes() {
  const auto tiling = [](std::string_view line) {
    std::vector<std::size_t> starts;
    for (std::size_t at = 0; at < line.size(); at += util::Utf8SequenceLength(line, at)) {
      starts.push_back(at);
    }
    starts.push_back(line.size());
    return starts;
  };
  const std::string units[] = {
      "a", "\x7F", std::string("\x80", 1), "\xBF", "\xC3", "\xC3\xA9", "\xE4\xB8", "\xE4\xB8\xAD",
      "\xE0\x80\x80", "\xF0\x9F\x98", "\xF0\x9F\x98\x80", "\xFF"};
  std::vector<std::string> lines = {"\x7F\x80\x81\x82\x83\x84" "b"};
  for (const std::string& first : units) {
    for (const std::string& second : units) {
      for (const std::string& third : units) {
        lines.push_back(first + second + third);
      }
    }
  }
  for (const std::string& line : lines) {
    const std::vector<std::size_t> starts = tiling(line);
    for (std::size_t column = 0; column <= line.size() + 1; ++column) {
      const std::size_t within = std::min(column, line.size());
      std::size_t floor_start = 0;
      for (const std::size_t start : starts) {
        if (start <= within) {
          floor_start = start;
        }
      }
      const std::size_t clamped = TextLayout::ClampTextColumn(line, column);
      Expect(clamped == floor_start,
             "ClampTextColumn must land on the forward tiling's boundary at or before the column");
      if (clamped == 0) {
        continue;
      }
      std::size_t want_previous = 0;
      for (const std::size_t start : starts) {
        if (start < clamped) {
          want_previous = start;
        }
      }
      Expect(TextLayout::PreviousTextColumn(line, column) == want_previous,
             "PreviousTextColumn must step back exactly one unit of the forward tiling");
    }
  }
  const std::string every_byte_tail("\x7F\x80\x81", 3);
  Expect(TextLayout::VisualColumnForTextColumn(every_byte_tail, 1, kTabSize) == 1,
         "the caret between \\x7F and a stray \\x80 is one column in");
}

void TestMeasureLineFactsRecordsTabIndentation() {
  std::uint64_t state = 0x1234567887654321ULL;
  const auto next = [&](std::size_t bound) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return static_cast<std::size_t>(state % bound);
  };
  for (int round = 0; round < 300; ++round) {
    const std::size_t tabs = next(4) == 0 ? 0 : next(300);
    std::string line(tabs, '\t');
    const std::size_t tail = next(10);
    for (std::size_t i = 0; i < tail; ++i) {
      line.push_back(static_cast<char>('a' + next(26)));
    }
    const int shape = next(5);
    if (shape == 1) line += "\t";
    if (shape == 2) line += "\xc3\xa9";
    if (shape == 3) line.insert(0, "x");
    const std::size_t tab_size = 1 + next(8);
    const LineLayoutFacts facts = TextLayout::MeasureLineFacts(line, tab_size);
    const std::size_t width = TextLayout::VisualColumnForTextColumn(line, line.size(), tab_size);
    Expect(facts.visual_columns == width, "the measured width agrees with the walk");
    const std::size_t run = line.find_first_not_of('\t') == std::string::npos
                                ? line.size()
                                : line.find_first_not_of('\t');
    const bool plain_after =
        line.find_first_not_of("abcdefghijklmnopqrstuvwxyz", run) == std::string::npos;
    const bool expect_tab_indented = run > 0 && plain_after;
    Expect(facts.tab_indented == expect_tab_indented &&
               (!facts.tab_indented || facts.leading_tabs == run),
           "tab-indented means leading tabs then plain ASCII only");
    Expect(facts.plain_ascii == (run == 0 && plain_after),
           "plain ASCII means no tab and no multi-byte character anywhere");
    microide::editor::TextLayoutCache cache;
    std::vector<std::string> lines = {line};
    (void)cache.MaxVisualColumns(lines, tab_size, 1);
    const LineLayoutFacts packed = cache.LineFactsIfCurrent(1, 0, tab_size, 1);
    Expect(packed.known && packed.visual_columns == width && packed.plain_ascii == facts.plain_ascii,
           "the packed entry keeps the width and the plain flag");
    Expect(packed.tab_indented == (facts.tab_indented && run <= 255) &&
               packed.leading_tabs == (packed.tab_indented ? run : 0),
           "the packed entry keeps the tab run when it fits");
  }
}

void RegisterTextLayoutTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TextLayout/SoftWrapTypingKeepsThePlainPrefixMemo",
          TestSoftWrapTypingKeepsThePlainPrefixMemo);
  AddTest(tests, "TextLayout/CachePlainPrefixMemoSurvivesAnEdit",
          TestTextLayoutCachePlainPrefixMemoSurvivesAnEdit);
  AddTest(tests, "TextLayout/MeasureLineFactsRecordsTabIndentation",
          TestMeasureLineFactsRecordsTabIndentation);
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
  AddTest(tests, "TextLayout/ColumnStepsAgreeWithForwardTilingOnAnyBytes",
          TestColumnStepsAgreeWithForwardTilingOnAnyBytes);
  AddTest(tests, "TextLayout/WideAndZeroWidthCodepointsTakeTheirCells",
          TestWideAndZeroWidthCodepointsTakeTheirCells);
  AddTest(tests, "TextLayout/WrapRowsPartitionTheLineAndResumeFromAnyBoundary",
          TestWrapRowsPartitionTheLineAndResumeFromAnyBoundary);
  AddTest(tests, "TextLayout/WrapCountsWideGlyphsAsTwoCells",
          TestWrapCountsWideGlyphsAsTwoCells);
  AddTest(tests, "TextLayout/ResolveVisualColumnMatchesTextLayout",
          TestResolveVisualColumnMatchesTextLayout);
}

}
