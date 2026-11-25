#include "editor/TextLayout.h"

#include <algorithm>

#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microide::editor {

namespace {

std::size_t FirstNonPlainAsciiByte(std::string_view text) {
  return util::FirstNonAsciiOrByte(text, '\t');
}

}

TextLayout::LineVisualColumnMap::LineVisualColumnMap(std::string_view line,
                                                     std::size_t tab_size) {
  const std::size_t reserve_hint = std::min<std::size_t>(line.size() + 1, 4096);
  boundaries_.reserve(reserve_hint);
  visuals_.reserve(reserve_hint);
  boundaries_.push_back(0);
  visuals_.push_back(0);
  std::size_t visual = 0;
  for (std::size_t i = 0; i < line.size();) {
    visual = AdvanceVisualColumnAt(visual, line, i, tab_size, &i);
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

bool TextLayout::VisualColumnsAreIdentity(std::string_view line) {
  return FirstNonPlainAsciiByte(line) == line.size();
}

std::size_t TextLayout::VisualColumnForTextColumn(std::string_view line,
                                                  std::size_t text_column,
                                                  std::size_t tab_size) {
  const std::size_t clamped_column = ClampTextColumn(line, text_column);
  const std::size_t plain_prefix = FirstNonPlainAsciiByte(line.substr(0, clamped_column));
  std::size_t visual_column = plain_prefix;
  for (std::size_t i = plain_prefix; i < clamped_column;) {
    visual_column = AdvanceVisualColumnAt(visual_column, line, i, tab_size, &i);
  }
  return visual_column;
}

std::size_t TextLayout::AdvanceVisualColumnsOver(std::size_t visual_column, std::string_view text,
                                                 std::size_t tab_size) {
  const std::size_t plain_prefix = FirstNonPlainAsciiByte(text);
  visual_column += plain_prefix;
  for (std::size_t i = plain_prefix; i < text.size();) {
    visual_column = AdvanceVisualColumnAt(visual_column, text, i, tab_size, &i);
  }
  return visual_column;
}

std::size_t TextLayout::TextColumnForVisualColumn(std::string_view line,
                                                  std::size_t visual_column,
                                                  std::size_t tab_size) {
  std::size_t current_visual = 0;
  for (std::size_t i = 0; i < line.size();) {
    std::size_t next_text = i;
    const std::size_t next_visual =
        AdvanceVisualColumnAt(current_visual, line, i, tab_size, &next_text);
    if (next_visual == current_visual) {
      i = next_text;
      continue;
    }
    std::size_t cluster_end = next_text;
    while (cluster_end < line.size()) {
      std::size_t after_mark = cluster_end;
      if (AdvanceVisualColumnAt(next_visual, line, cluster_end, tab_size, &after_mark) !=
          next_visual) {
        break;
      }
      cluster_end = after_mark;
    }
    if (visual_column < next_visual) {
      return visual_column - current_visual <= next_visual - visual_column ? i : cluster_end;
    }
    current_visual = next_visual;
    i = cluster_end;
  }
  return line.size();
}

std::size_t TextLayout::ClampTextColumn(std::string_view line, std::size_t text_column) {
  const std::size_t clamped_column = std::min(text_column, line.size());
  if (clamped_column >= line.size()) {
    return line.size();
  }

  std::size_t column = clamped_column;
  const std::size_t floor = column >= 3 ? column - 3 : 0;
  while (column > floor && util::IsUtf8ContinuationByte(static_cast<unsigned char>(line[column]))) {
    --column;
  }
  return util::Utf8SequenceLength(line, column) > clamped_column - column ? column : clamped_column;
}

std::size_t TextLayout::PreviousTextColumn(std::string_view line, std::size_t text_column) {
  const std::size_t clamped_column = ClampTextColumn(line, text_column);
  if (clamped_column == 0) {
    return 0;
  }

  std::size_t pos = clamped_column - 1;
  const std::size_t floor = clamped_column >= 4 ? clamped_column - 4 : 0;
  while (pos > floor && util::IsUtf8ContinuationByte(static_cast<unsigned char>(line[pos]))) {
    --pos;
  }
  return util::Utf8SequenceLength(line, pos) == clamped_column - pos ? pos : clamped_column - 1;
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
  const std::size_t leading_tabs = util::LeadingByteRun(line, '\t');
  const std::size_t plain_prefix =
      leading_tabs + FirstNonPlainAsciiByte(line.substr(leading_tabs));
  std::size_t visual_column = leading_tabs * tab_size + (plain_prefix - leading_tabs);
  for (std::size_t i = plain_prefix; i < line.size();) {
    visual_column = AdvanceVisualColumnAt(visual_column, line, i, tab_size, &i);
  }
  return LineLayoutFacts{
      .visual_columns = visual_column,
      .plain_ascii = leading_tabs == 0 && plain_prefix == line.size(),
      .known = true,
      .tab_indented = leading_tabs > 0 && plain_prefix == line.size(),
      .leading_tabs = leading_tabs,
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
  if (!facts.known) {
    util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthMeasureBytes, line.size());
  }
  const std::size_t visual_columns =
      facts.known ? facts.visual_columns : VisualColumnForTextColumn(line, line.size(), tab_size);
  std::size_t start_byte = 0;
  if (horizontal_scroll > 0 && !line.empty()) {
    const std::size_t probe = std::min(horizontal_scroll, line.size());
    if (facts.known && facts.plain_ascii) {
      start_byte = probe;
    } else {
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
  result.text.clear();
  result.source_columns.clear();
  result.text_offsets.clear();
  result.caret_column = 0;
  result.caret_visible = false;
  result.visual_columns = facts.visual_columns;

  if (visible_columns == 0) {
    return;
  }

  result.text_offsets.reserve(visible_columns);
  result.source_columns.reserve(visible_columns);
  result.text.reserve(visible_columns);

  const std::string_view bytes = window.bytes;
  const std::size_t base = window.start_byte;
  std::size_t visual_column = base;
  std::size_t last_glyph_next_byte = std::string_view::npos;
  std::size_t last_glyph_text_end = 0;
  std::size_t last_glyph_cell = 0;
  for (std::size_t j = 0; j < bytes.size();) {
    const char character = bytes[j];
    std::size_t next_text = j;
    const std::size_t next_visual =
        AdvanceVisualColumnAt(visual_column, bytes, j, tab_size, &next_text);
    const std::size_t width = next_visual - visual_column;

    if (width == 0) {
      if (last_glyph_next_byte == j) {
        const std::size_t mark_bytes = next_text - j;
        result.text.insert(last_glyph_text_end, bytes, j, mark_bytes);
        for (std::size_t cell = last_glyph_cell + 1; cell < result.text_offsets.size(); ++cell) {
          result.text_offsets[cell] += mark_bytes;
        }
        last_glyph_text_end += mark_bytes;
        last_glyph_next_byte = next_text;
      }
      j = next_text;
      continue;
    }

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
      } else if (cell == 0) {
        result.text.append(bytes, j, next_text - j);
        last_glyph_next_byte = next_text;
        last_glyph_text_end = result.text.size();
        last_glyph_cell = result.text_offsets.size() - 1;
      } else if (absolute_cell == horizontal_scroll) {
        result.text.push_back(' ');
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
  if (layout.source_columns.empty()) {
    return row_end_visual + 1;
  }
  const auto& sc = layout.source_columns;
  const auto it = std::lower_bound(sc.begin(), sc.end(), source_column);
  if (it == sc.end()) {
    return row_start_visual + sc.size();
  }
  if (it == sc.begin() && *it > source_column) {
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
  if (text_column >= line.size()) {
    return {};
  }
  const std::size_t hit = util::Utf8CodepointStartAt(line, text_column);
  std::size_t length = 0;
  if (!util::Utf8IdentifierCodepointAt(line, hit, &length)) {
    return {};
  }
  std::size_t start = util::Utf8IdentifierRunStart(line, hit);
  const std::size_t end = util::Utf8IdentifierRunEnd(line, hit);

  for (;;) {
    std::size_t op_start = start;
    if (start >= 2 && line[start - 1] == '>' && line[start - 2] == '-') {
      op_start = start - 2;
    } else if (start >= 1 && line[start - 1] == '.') {
      op_start = start - 1;
    } else {
      break;
    }
    const std::size_t run_start = util::Utf8IdentifierRunStart(line, op_start);
    if (run_start == op_start) {
      break;
    }
    start = run_start;
  }
  return {start, end};
}

}
