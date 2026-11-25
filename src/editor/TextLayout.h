#pragma once

#include <utility>
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "util/StringUtil.h"

namespace microide::editor {

struct LayoutLine {
  std::string text;
  std::vector<std::size_t> source_columns;
  std::vector<std::size_t> text_offsets;
  std::size_t visual_columns = 0;
  std::size_t caret_column = 0;
  bool caret_visible = false;
};

struct LineLayoutFacts {
  std::size_t visual_columns = 0;
  bool plain_ascii = false;
  bool known = false;
  bool tab_indented = false;
  std::size_t leading_tabs = 0;
};

class TextLayout {
 public:
  static bool VisualColumnsAreIdentity(std::string_view line);

  static std::size_t VisualColumnForTextColumn(std::string_view line,
                                               std::size_t text_column,
                                               std::size_t tab_size);
  static std::size_t AdvanceVisualColumnsOver(std::size_t visual_column,
                                              std::string_view text,
                                              std::size_t tab_size);
  static std::size_t TextColumnForVisualColumn(std::string_view line,
                                               std::size_t visual_column,
                                               std::size_t tab_size);
  static std::size_t ClampTextColumn(std::string_view line, std::size_t text_column);
  static std::size_t PreviousTextColumn(std::string_view line, std::size_t text_column);
  static std::size_t NextTextColumn(std::string_view line, std::size_t text_column);
  static void BuildVisibleLineInto(std::string_view line,
                                   std::size_t horizontal_scroll,
                                   std::size_t visible_columns,
                                   std::size_t tab_size,
                                   LayoutLine& out,
                                   LineLayoutFacts facts = {});

  struct VisibleLineWindow {
    std::string_view bytes;
    std::size_t start_byte = 0;
    std::size_t line_length = 0;
  };

  static std::size_t ComputeVisibleLineWindowStart(std::size_t horizontal_scroll,
                                                   std::size_t line_length,
                                                   std::size_t plain_prefix_end) {
    if (horizontal_scroll == 0 || line_length == 0) {
      return 0;
    }
    return std::min({horizontal_scroll, line_length, plain_prefix_end});
  }

  static std::size_t VisibleLineWindowBytes(std::size_t start_byte,
                                            std::size_t horizontal_scroll,
                                            std::size_t visible_columns) {
    const std::size_t reach = horizontal_scroll > start_byte ? horizontal_scroll - start_byte : 0;
    return (reach + visible_columns) * 4 + 4;
  }

  static void BuildVisibleLineWindowInto(const VisibleLineWindow& window,
                                         std::size_t horizontal_scroll,
                                         std::size_t visible_columns,
                                         std::size_t tab_size,
                                         LayoutLine& out,
                                         LineLayoutFacts facts);
  static LayoutLine BuildVisibleLine(std::string_view line,
                                     std::size_t horizontal_scroll,
                                     std::size_t visible_columns,
                                     std::size_t tab_size,
                                     LineLayoutFacts facts = {});

  static LineLayoutFacts MeasureLineFacts(std::string_view line, std::size_t tab_size);

  class LineVisualColumnMap {
   public:
    LineVisualColumnMap(std::string_view line, std::size_t tab_size);
    std::size_t VisualColumnFor(std::size_t text_column) const;
    std::size_t LineVisualWidth() const { return line_visual_width_; }

   private:
    std::vector<std::size_t> boundaries_;
    std::vector<std::size_t> visuals_;
    std::size_t line_visual_width_ = 0;
  };

  static std::size_t VisualColumnFromLayoutClipped(const LayoutLine& layout,
                                                   std::size_t row_start_visual,
                                                   std::size_t row_end_visual,
                                                   std::size_t source_column);

  static std::size_t ResolveVisualColumn(const LayoutLine* layout,
                                         const LineVisualColumnMap* visual_map,
                                         std::size_t row_visual_start,
                                         std::size_t row_visual_end,
                                         std::size_t source_column);

  static std::size_t AdvanceVisualColumn(std::size_t visual_column,
                                         char character,
                                         std::size_t tab_size) {
    if (character != '\t') {
      return visual_column + 1;
    }
    const std::size_t safe_tab_size = std::max<std::size_t>(1, tab_size);
    const std::size_t remainder = visual_column % safe_tab_size;
    return visual_column + (remainder == 0 ? safe_tab_size : safe_tab_size - remainder);
  }

  static std::size_t AdvanceVisualColumnAt(std::size_t visual_column,
                                           std::string_view text,
                                           std::size_t offset,
                                           std::size_t tab_size,
                                           std::size_t* next_offset) {
    const unsigned char lead = static_cast<unsigned char>(text[offset]);
    if (lead < 0x80) {
      *next_offset = offset + 1;
      return AdvanceVisualColumn(visual_column, static_cast<char>(lead), tab_size);
    }
    const std::size_t length = util::Utf8SequenceLength(text, offset);
    *next_offset = offset + length;
    return visual_column +
           util::GridCellWidth(util::DecodeUtf8Codepoint(text.substr(offset, length)));
  }

  template <typename EmitFn>
  static void WrapLineSegments(std::string_view line_text,
                               std::size_t tab_size,
                               std::size_t wrap_columns,
                               EmitFn&& emit) {
    if (line_text.empty()) {
      emit(std::size_t{0}, std::size_t{0}, std::size_t{0});
      return;
    }
    WrapLineSegmentsFrom(line_text, tab_size, wrap_columns, 0,
                         HangingIndentFor(line_text, tab_size, wrap_columns),
                         std::forward<EmitFn>(emit));
  }

  static std::size_t HangingIndentFor(std::string_view line_text,
                                      std::size_t tab_size,
                                      std::size_t wrap_columns) {
    std::size_t indent_visual = 0;
    for (std::size_t k = 0; k < line_text.size(); ++k) {
      const char ch = line_text[k];
      if (ch != ' ' && ch != '\t') {
        break;
      }
      indent_visual = AdvanceVisualColumn(indent_visual, ch, tab_size);
    }
    return std::min(indent_visual, wrap_columns / 2);
  }

  template <typename EmitFn>
  static void WrapLineSegmentsFrom(std::string_view tail,
                                   std::size_t tab_size,
                                   std::size_t wrap_columns,
                                   std::size_t visual_base,
                                   std::size_t hanging_indent,
                                   EmitFn&& emit) {
    const std::string_view line_text = tail;
    if (line_text.empty()) {
      emit(visual_base, visual_base, visual_base == 0 ? std::size_t{0} : hanging_indent);
      return;
    }

    std::size_t row_start_visual = visual_base;
    std::size_t row_start_text = 0;
    std::size_t last_break_visual = visual_base;
    std::size_t last_break_text = 0;
    std::size_t visual = visual_base;
    std::size_t i = 0;
    const std::size_t line_size = line_text.size();
    while (i < line_size) {
      const char ch = line_text[i];
      std::size_t next_i = i;
      const std::size_t next_visual =
          AdvanceVisualColumnAt(visual, line_text, i, tab_size, &next_i);
      const std::size_t seq_len = next_i - i;
      const std::size_t effective_wrap =
          wrap_columns - (row_start_visual == 0 ? 0 : hanging_indent);

      if (next_visual > visual && next_visual - row_start_visual > effective_wrap &&
          visual > row_start_visual) {
        std::size_t break_visual;
        std::size_t break_text;
        if (last_break_text > row_start_text && last_break_visual > row_start_visual) {
          break_visual = last_break_visual;
          break_text = last_break_text;
        } else {
          break_visual = visual;
          break_text = i;
        }
        emit(row_start_visual, break_visual, row_start_visual == 0 ? std::size_t{0} : hanging_indent);
        row_start_visual = break_visual;
        row_start_text = break_text;
        last_break_visual = row_start_visual;
        last_break_text = row_start_text;
        visual = break_visual;
        i = break_text;
        continue;
      }

      const bool zero_width = next_visual == visual;
      visual = next_visual;
      i += seq_len;
      if (ch == ' ' || ch == '\t') {
        last_break_visual = visual;
        last_break_text = i;
      } else if (zero_width && last_break_text == i - seq_len) {
        last_break_text = i;
      }
    }
    emit(row_start_visual, visual, row_start_visual == 0 ? std::size_t{0} : hanging_indent);
  }

  static std::size_t WrapLineRowCount(std::string_view line_text,
                                      std::size_t tab_size,
                                      std::size_t wrap_columns) {
    std::size_t rows = 0;
    WrapLineSegments(line_text, tab_size, wrap_columns,
                     [&rows](std::size_t, std::size_t, std::size_t) { ++rows; });
    return rows;
  }

  struct ByteRange {
    std::size_t start = 0;
    std::size_t end = 0;
    bool empty() const { return start >= end; }
  };

  static ByteRange IdentifierRangeAt(std::string_view line, std::size_t text_column);
};

}
