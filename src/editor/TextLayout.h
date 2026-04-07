#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace microide::editor {

struct LayoutLine {
  std::string text;
  std::vector<std::size_t> source_columns;
  std::vector<std::size_t> text_offsets;
  std::size_t visual_columns = 0;
  std::size_t caret_column = 0;
  bool caret_visible = false;
};

class TextLayout {
 public:
  static std::size_t VisualColumnForTextColumn(const std::string& line,
                                               std::size_t text_column,
                                               std::size_t tab_size);
  static std::size_t TextColumnForVisualColumn(const std::string& line,
                                               std::size_t visual_column,
                                               std::size_t tab_size);
  static std::size_t ClampTextColumn(const std::string& line, std::size_t text_column);
  static std::size_t PreviousTextColumn(const std::string& line, std::size_t text_column);
  static std::size_t NextTextColumn(const std::string& line, std::size_t text_column);
  static LayoutLine BuildVisibleLine(const std::string& line,
                                     std::size_t horizontal_scroll,
                                     std::size_t visible_columns,
                                     std::size_t caret_text_column,
                                     std::size_t tab_size);

 private:
  static std::size_t AdvanceVisualColumn(std::size_t visual_column,
                                         char character,
                                         std::size_t tab_size);
  static std::size_t Utf8SequenceLength(std::string_view line, std::size_t offset);
};

}  // namespace microide::editor
