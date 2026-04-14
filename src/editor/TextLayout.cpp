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

  std::size_t previous = 0;
  for (std::size_t current = 0; current < clamped_column;) {
    previous = current;
    current += util::Utf8SequenceLength(line, current);
  }
  return previous;
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
                                        std::size_t caret_text_column,
                                        std::size_t tab_size) {
  LayoutLine result;
  result.visual_columns = VisualColumnForTextColumn(line, line.size(), tab_size);

  const std::size_t caret_visual =
      VisualColumnForTextColumn(line, caret_text_column, tab_size);
  if (caret_visual >= horizontal_scroll &&
      caret_visual <= horizontal_scroll + visible_columns) {
    result.caret_visible = true;
    result.caret_column = caret_visual - horizontal_scroll;
  }

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
