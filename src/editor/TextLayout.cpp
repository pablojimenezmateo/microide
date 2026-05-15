#include "editor/TextLayout.h"

#include <algorithm>

#include "util/StringUtil.h"

namespace microide::editor {

std::size_t TextLayout::VisualColumnForTextColumn(const std::string& line,
                                                  std::size_t text_column,
                                                  std::size_t tab_size) {
  const std::size_t clamped_column = ClampTextColumn(line, text_column);
  std::size_t visual_column = 0;
  for (std::size_t i = 0; i < clamped_column;) {
    visual_column = AdvanceVisualColumn(visual_column, line[i], tab_size);
    i += util::Utf8SequenceLength(line, i);
  }
  return visual_column;
}

std::size_t TextLayout::TextColumnForVisualColumn(const std::string& line,
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

std::size_t TextLayout::ClampTextColumn(const std::string& line, std::size_t text_column) {
  const std::size_t clamped_column = std::min(text_column, line.size());
  if (clamped_column >= line.size()) {
    return line.size();
  }

  std::size_t current = 0;
  while (current < clamped_column) {
    const std::size_t next = current + util::Utf8SequenceLength(line, current);
    if (next > clamped_column) {
      break;
    }
    current = next;
  }
  return current;
}

std::size_t TextLayout::PreviousTextColumn(const std::string& line, std::size_t text_column) {
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

std::size_t TextLayout::NextTextColumn(const std::string& line, std::size_t text_column) {
  const std::size_t clamped_column = ClampTextColumn(line, text_column);
  if (clamped_column >= line.size()) {
    return line.size();
  }
  return std::min(line.size(),
                  clamped_column + util::Utf8SequenceLength(line, clamped_column));
}

LayoutLine TextLayout::BuildVisibleLine(const std::string& line,
                                        std::size_t horizontal_scroll,
                                        std::size_t visible_columns,
                                        std::size_t tab_size) {
  LayoutLine result;
  result.visual_columns = VisualColumnForTextColumn(line, line.size(), tab_size);

  if (visible_columns == 0) {
    return result;
  }

  std::size_t visual_column = 0;
  for (std::size_t i = 0; i < line.size();) {
    const char character = line[i];
    const std::size_t next_text = i + util::Utf8SequenceLength(line, i);
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
        result.text.append(line, i, next_text - i);
      }
      result.source_columns.push_back(i);
    }

    visual_column = next_visual;
    i = next_text;
    if (visual_column >= horizontal_scroll + visible_columns) {
      break;
    }
  }

  return result;
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

std::size_t TextLayout::AdvanceVisualColumn(std::size_t visual_column,
                                            char character,
                                            std::size_t tab_size) {
  if (character != '\t') {
    return visual_column + 1;
  }

  const std::size_t safe_tab_size = std::max<std::size_t>(1, tab_size);
  const std::size_t remainder = visual_column % safe_tab_size;
  return visual_column + (remainder == 0 ? safe_tab_size : safe_tab_size - remainder);
}

}  // namespace microide::editor
