#include "editor/TextLayout.h"

#include <algorithm>

#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microide::editor {

namespace {

// Offset of the first byte in `text` that is not a plain, single-cell ASCII
// character — that is, the first tab (which expands to a stop) or the first
// byte >= 0x80 (which begins a multi-byte code point). Returns `text.size()`
// when the whole span is plain ASCII.
//
// Every byte before that offset contributes exactly one visual column, so the
// prefix needs no UTF-8 decoding and no per-character tab arithmetic at all.
// Source lines are overwhelmingly that shape, and the width of every line in a
// buffer is computed on open (`TextLayoutCache::MaxVisualColumns`), so this is
// the difference between a byte scan and 50k decoder loops on a large file.
std::size_t FirstNonPlainAsciiByte(std::string_view text) {
  return util::FirstNonAsciiOrByte(text, '\t');
}

}  // namespace

TextLayout::LineVisualColumnMap::LineVisualColumnMap(std::string_view line,
                                                     std::size_t tab_size) {
  // One entry per code point, not per byte. Reserving line.size()+1 is exact for
  // ASCII but over-reserves ~2-3x on long multibyte lines; cap the hint so a
  // pathological (e.g. hundreds of KiB) line built transiently on a hover path
  // cannot reserve far beyond what it needs. Growth past the cap is amortized.
  const std::size_t reserve_hint = std::min<std::size_t>(line.size() + 1, 4096);
  boundaries_.reserve(reserve_hint);
  visuals_.reserve(reserve_hint);
  boundaries_.push_back(0);
  visuals_.push_back(0);
  std::size_t visual = 0;
  for (std::size_t i = 0; i < line.size();) {
    visual = AdvanceVisualColumn(visual, line[i], tab_size);
    i += util::Utf8SequenceLength(line, i);
    boundaries_.push_back(i);
    visuals_.push_back(visual);
  }
  line_visual_width_ = visual;
}

std::size_t TextLayout::LineVisualColumnMap::VisualColumnFor(std::size_t text_column) const {
  if (boundaries_.empty() || text_column <= 0) {
    return 0;
  }
  if (text_column >= boundaries_.back()) {
    return line_visual_width_;
  }
  // First boundary >= text_column; round-down to its predecessor in the same code-point so
  // mid-codepoint queries map to the start of the code point (matching ClampTextColumn).
  auto it = std::lower_bound(boundaries_.begin(), boundaries_.end(), text_column);
  std::size_t index = static_cast<std::size_t>(it - boundaries_.begin());
  if (it == boundaries_.end() || *it > text_column) {
    if (index == 0) {
      return 0;
    }
    --index;
  }
  return visuals_[index];
}

std::size_t TextLayout::VisualColumnForTextColumn(std::string_view line,
                                                  std::size_t text_column,
                                                  std::size_t tab_size) {
  const std::size_t clamped_column = ClampTextColumn(line, text_column);
  // The plain-ASCII prefix is 1 byte = 1 column, so start the decoding loop at
  // the first byte that can break that (see FirstNonPlainAsciiByte).
  const std::size_t plain_prefix = FirstNonPlainAsciiByte(line.substr(0, clamped_column));
  std::size_t visual_column = plain_prefix;
  for (std::size_t i = plain_prefix; i < clamped_column;) {
    visual_column = AdvanceVisualColumn(visual_column, line[i], tab_size);
    i += util::Utf8SequenceLength(line, i);
  }
  return visual_column;
}

std::size_t TextLayout::AdvanceVisualColumnsOver(std::size_t visual_column, std::string_view text,
                                                 std::size_t tab_size) {
  // Same plain-ASCII shortcut as VisualColumnForTextColumn: one byte, one column.
  const std::size_t plain_prefix = FirstNonPlainAsciiByte(text);
  visual_column += plain_prefix;
  for (std::size_t i = plain_prefix; i < text.size();) {
    visual_column = AdvanceVisualColumn(visual_column, text[i], tab_size);
    i += util::Utf8SequenceLength(text, i);
  }
  return visual_column;
}

std::size_t TextLayout::TextColumnForVisualColumn(std::string_view line,
                                                  std::size_t visual_column,
                                                  std::size_t tab_size) {
  std::size_t current_visual = 0;
  for (std::size_t i = 0; i < line.size();) {
    const std::size_t next_text = i + util::Utf8SequenceLength(line, i);
    const std::size_t next_visual =
        AdvanceVisualColumn(current_visual, line[i], tab_size);
    if (visual_column < next_visual) {
      return visual_column - current_visual <= next_visual - visual_column ? i : next_text;
    }
    current_visual = next_visual;
    i = next_text;
  }
  return line.size();
}

std::size_t TextLayout::ClampTextColumn(std::string_view line, std::size_t text_column) {
  const std::size_t clamped_column = std::min(text_column, line.size());
  if (clamped_column >= line.size()) {
    return line.size();
  }

  // Round down to the start of the code point containing `clamped_column` by
  // stepping back over continuation bytes -- at most three, because UTF-8 is
  // self-synchronizing: a lead byte and a continuation byte are distinguishable
  // from the byte alone.
  //
  // This used to re-tile the line from byte 0, one sequence at a time, which made
  // an O(1) question cost O(column). Nothing notices on a 40-character line. On a
  // file with no line breaks in it -- a minified bundle -- the caret sits a
  // megabyte into "the line", and this ran FOUR times per keystroke: twice
  // clamping the edit range's endpoints, once inside VisualColumnForTextColumn for
  // the caret's preferred column, and once inside PreviousTextColumn for a
  // backspace. It was the single largest remaining cost on the edit path after
  // the copies were removed (TD-2026-08-05-131), at ~1.2 ms a call.
  //
  // For well-formed UTF-8 the two agree exactly. They can differ inside a
  // malformed sequence (a truncated lead byte's claimed length is not observable
  // from a later byte); the answer is still a valid clamp -- inside the line, no
  // greater than asked, and never splitting a well-formed code point -- and it
  // matches how the rest of the editor re-syncs (PreviousTextColumn already
  // scanned backwards this way).
  std::size_t column = clamped_column;
  const std::size_t floor = column >= 3 ? column - 3 : 0;
  while (column > floor && (static_cast<unsigned char>(line[column]) & 0xC0u) == 0x80u) {
    --column;
  }
  return column;
}

std::size_t TextLayout::PreviousTextColumn(std::string_view line, std::size_t text_column) {
  const std::size_t clamped_column = ClampTextColumn(line, text_column);
  if (clamped_column == 0) {
    return 0;
  }

  std::size_t pos = clamped_column - 1;
  while (pos > 0 && (static_cast<unsigned char>(line[pos]) & 0xC0u) == 0x80u) {
    --pos;
  }
  return pos;
}

std::size_t TextLayout::NextTextColumn(std::string_view line, std::size_t text_column) {
  const std::size_t clamped_column = ClampTextColumn(line, text_column);
  if (clamped_column >= line.size()) {
    return line.size();
  }
  return std::min(line.size(),
                  clamped_column + util::Utf8SequenceLength(line, clamped_column));
}

LineLayoutFacts TextLayout::MeasureLineFacts(std::string_view line, std::size_t tab_size) {
  const std::size_t plain_prefix = FirstNonPlainAsciiByte(line);
  std::size_t visual_column = plain_prefix;
  for (std::size_t i = plain_prefix; i < line.size();) {
    visual_column = AdvanceVisualColumn(visual_column, line[i], tab_size);
    i += util::Utf8SequenceLength(line, i);
  }
  return LineLayoutFacts{
      .visual_columns = visual_column,
      .plain_ascii = plain_prefix == line.size(),
      .known = true,
  };
}

LayoutLine TextLayout::BuildVisibleLine(std::string_view line,
                                        std::size_t horizontal_scroll,
                                        std::size_t visible_columns,
                                        std::size_t tab_size,
                                        LineLayoutFacts facts) {
  LayoutLine result;
  BuildVisibleLineInto(line, horizontal_scroll, visible_columns, tab_size, result, facts);
  return result;
}

void TextLayout::BuildVisibleLineInto(std::string_view line,
                                      std::size_t horizontal_scroll,
                                      std::size_t visible_columns,
                                      std::size_t tab_size,
                                      LayoutLine& result,
                                      LineLayoutFacts facts) {
  // Whole-line form: derive the width and the walk start here (both need bytes
  // this form has and the window form does not), then share one loop.
  const std::size_t visual_columns =
      facts.known ? facts.visual_columns : VisualColumnForTextColumn(line, line.size(), tab_size);
  std::size_t start_byte = 0;
  if (horizontal_scroll > 0 && !line.empty()) {
    const std::size_t probe = std::min(horizontal_scroll, line.size());
    if (facts.known && facts.plain_ascii) {
      start_byte = probe;
    } else {
      // Reach the first visible cell without decoding what precedes it. Every byte
      // before the line's first tab-or-multibyte byte occupies exactly one visual
      // column, so within that prefix byte offset == visual column and the walk can
      // start at the scroll offset outright. A caller that already knows the whole
      // line is plain ASCII (`facts.plain_ascii`) skips even the scan.
      //
      // This is exact for every line, not a fast path for a special case: the walk
      // simply starts at the first byte where visual column can stop tracking byte
      // offset. On an ordinary line the two forms do the same negligible work; on a
      // line with no newlines in it and the caret a megabyte in, this replaces ~1M
      // decoder iterations per rendered row with a word-at-a-time scan, or with
      // nothing at all when the width cache has the line's facts.
      start_byte = FirstNonPlainAsciiByte(line.substr(0, probe));
      util::AddPerformanceCounter(util::PerfCounterId::EditorVisibleLineLayoutPrefixBytesScanned,
                                  start_byte);
    }
  }
  BuildVisibleLineWindowInto(
      VisibleLineWindow{
          .bytes = line.substr(std::min(start_byte, line.size())),
          .start_byte = start_byte,
          .line_length = line.size(),
      },
      horizontal_scroll, visible_columns, tab_size, result,
      LineLayoutFacts{.visual_columns = visual_columns, .plain_ascii = facts.plain_ascii,
                      .known = true});
}

void TextLayout::BuildVisibleLineWindowInto(const VisibleLineWindow& window,
                                            std::size_t horizontal_scroll,
                                            std::size_t visible_columns,
                                            std::size_t tab_size,
                                            LayoutLine& result,
                                            LineLayoutFacts facts) {
  // `clear()` keeps each buffer's capacity, which is the whole point: a caller
  // recycling an evicted cache entry must not pay three allocations to refill it.
  result.text.clear();
  result.source_columns.clear();
  result.text_offsets.clear();
  result.caret_column = 0;
  result.caret_visible = false;
  result.visual_columns = facts.visual_columns;

  if (visible_columns == 0) {
    return;
  }

  // The append loop emits at most `visible_columns` cells (it breaks once a cell
  // reaches the right edge), so a single up-front reserve eliminates the
  // incremental reallocations on this per-frame soft-wrap path. `text` may still
  // grow for multibyte-heavy lines, but the common ASCII case stays alloc-free.
  result.text_offsets.reserve(visible_columns);
  result.source_columns.reserve(visible_columns);
  result.text.reserve(visible_columns);

  const std::string_view bytes = window.bytes;
  const std::size_t base = window.start_byte;
  // `start_byte` sits inside the line's plain-ASCII prefix by construction, so the
  // visual column there equals the byte column -- which is what makes starting the
  // walk mid-line exact rather than approximate.
  std::size_t visual_column = base;
  for (std::size_t j = 0; j < bytes.size();) {
    const char character = bytes[j];
    const std::size_t next_text = j + util::Utf8SequenceLength(bytes, j);
    const std::size_t next_visual =
        AdvanceVisualColumn(visual_column, character, tab_size);
    const std::size_t width = next_visual - visual_column;

    for (std::size_t cell = 0; cell < width; ++cell) {
      const std::size_t absolute_cell = visual_column + cell;
      if (absolute_cell < horizontal_scroll) {
        continue;
      }
      if (absolute_cell >= horizontal_scroll + visible_columns) {
        break;
      }
      result.text_offsets.push_back(result.text.size());
      if (character == '\t') {
        result.text.push_back(' ');
      } else {
        result.text.append(bytes, j, next_text - j);
      }
      result.source_columns.push_back(base + j);
    }

    visual_column = next_visual;
    j = next_text;
    if (visual_column >= horizontal_scroll + visible_columns) {
      break;
    }
  }
}

std::size_t TextLayout::VisualColumnFromLayoutClipped(const LayoutLine& layout,
                                                       std::size_t row_start_visual,
                                                       std::size_t row_end_visual,
                                                       std::size_t source_column) {
  // Empty layout: any source column is "off-row". A sentinel beyond row_end_visual lets std::min
  // clip the decoration correctly.
  if (layout.source_columns.empty()) {
    return row_end_visual + 1;
  }
  // lower_bound returns the first cell whose source byte is >= source_column. That cell's visual
  // column is `row_start_visual + cell_index`.
  const auto& sc = layout.source_columns;
  const auto it = std::lower_bound(sc.begin(), sc.end(), source_column);
  if (it == sc.end()) {
    // source_column is past the last visible source byte. The "natural" visual column for that
    // position is the cell immediately after the last visible cell — i.e.
    // `row_start_visual + source_columns.size()`. That value:
    //   - equals row_end_visual when the row is filled, giving a correct clip;
    //   - equals "just past the end of a short line" when the line is shorter than the row, giving
    //     a correct end-of-line decoration boundary;
    //   - is always >= the legacy walk result for in-window source columns.
    return row_start_visual + sc.size();
  }
  if (it == sc.begin() && *it > source_column) {
    // source_column precedes the leftmost visible source byte → before the row's left edge.
    return row_start_visual > 0 ? row_start_visual - 1 : 0;
  }
  const std::size_t cell_index = static_cast<std::size_t>(it - sc.begin());
  return row_start_visual + cell_index;
}

std::size_t TextLayout::ResolveVisualColumn(const LayoutLine* layout,
                                            const LineVisualColumnMap* visual_map,
                                            std::size_t row_visual_start,
                                            std::size_t row_visual_end,
                                            std::size_t source_column) {
  if (layout != nullptr) {
    return VisualColumnFromLayoutClipped(*layout, row_visual_start, row_visual_end, source_column);
  }
  if (visual_map != nullptr) {
    return visual_map->VisualColumnFor(source_column);
  }
  return source_column;
}

TextLayout::ByteRange TextLayout::IdentifierRangeAt(std::string_view line,
                                                    std::size_t text_column) {
  const auto is_ident = [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  };
  if (text_column >= line.size() || !is_ident(static_cast<unsigned char>(line[text_column]))) {
    return {};
  }
  std::size_t start = text_column;
  while (start > 0 && is_ident(static_cast<unsigned char>(line[start - 1]))) {
    --start;
  }
  std::size_t end = text_column + 1;
  while (end < line.size() && is_ident(static_cast<unsigned char>(line[end]))) {
    ++end;
  }

  // Extend the range leftward across member-access operators (`.` and `->`) so a
  // hover on the trailing member of `foo.bar` / `ptr->field` / `a.b.c` evaluates the
  // whole chain rather than just the bare word. Only a `.` or `->` immediately
  // preceding the current start, and itself immediately preceded by an identifier
  // byte, is absorbed; anything else (whitespace, a stray `>`, a multibyte UTF-8
  // byte, a second `.`) terminates the walk so the boundary stays deterministic.
  for (;;) {
    std::size_t op_start = start;
    if (start >= 2 && line[start - 1] == '>' && line[start - 2] == '-') {
      op_start = start - 2;  // "->"
    } else if (start >= 1 && line[start - 1] == '.') {
      op_start = start - 1;  // "."
    } else {
      break;
    }
    if (op_start == 0 || !is_ident(static_cast<unsigned char>(line[op_start - 1]))) {
      break;
    }
    std::size_t run_start = op_start;
    while (run_start > 0 && is_ident(static_cast<unsigned char>(line[run_start - 1]))) {
      --run_start;
    }
    start = run_start;
  }
  return {start, end};
}

}  // namespace microide::editor
