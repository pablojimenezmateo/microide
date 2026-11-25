#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "editor/EditorRowYLayout.h"

namespace microide::editor {

struct GhostTextInset {
  std::span<const std::string> below_lines;
};

struct FoldGutterMark {
  std::size_t line_index = 0;
  std::size_t visual_row_index = 0;
  bool collapsed = false;
};

struct BreakpointGutterMark {
  std::size_t line_index = 0;
  std::size_t visual_row_index = 0;
  bool enabled = true;
  bool verified = false;
  bool has_condition = false;
  bool is_logpoint = false;
};

enum class OccurrenceKind : std::uint8_t { Text, Read, Write };

struct OccurrenceRange {
  std::size_t line_index = 0;
  std::size_t start_column = 0;
  std::size_t end_column = 0;
  bool is_primary_seed = false;
  OccurrenceKind kind = OccurrenceKind::Text;
};

struct WhitespaceGlyphRun {
  std::size_t visual_row_index = 0;
  std::size_t row_visual_start = 0;
  std::size_t row_visual_end = 0;
  std::size_t cell_visual_start = 0;
  std::size_t cell_visual_extent = 1;
  bool is_tab_rule = false;
};

struct EditorViewModel {
  std::vector<FoldGutterMark> fold_gutter_marks;
  std::vector<BreakpointGutterMark> breakpoint_gutter_marks;
  std::span<const OccurrenceRange> occurrence_ranges;
  std::span<const std::size_t> sticky_lines;
  std::optional<std::size_t> execution_line_index;
  std::span<const RowGap> row_gaps;
  std::span<const RowGapContent> row_gap_contents;
  bool code_lens_above = false;
  struct GhostTextTail {
    std::size_t visual_row = 0;
    std::string_view text;
  };
  std::optional<GhostTextTail> ghost_text_tail;
  std::vector<WhitespaceGlyphRun> whitespace_glyph_runs;
  std::vector<std::size_t> whitespace_row_offsets;
};

}
