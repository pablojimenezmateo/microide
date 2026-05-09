#pragma once

#include <cstddef>
#include <vector>

namespace microide::editor {

struct FoldGutterMark {
  std::size_t line_index = 0;
  std::size_t visual_row_index = 0;
  bool collapsed = false;
};

struct OccurrenceRange {
  std::size_t line_index = 0;
  std::size_t start_column = 0;
  std::size_t end_column = 0;
  bool is_primary_seed = false;
};

struct EditorViewModel {
  std::vector<FoldGutterMark> fold_gutter_marks;
  std::vector<OccurrenceRange> occurrence_ranges;
};

}  // namespace microide::editor
