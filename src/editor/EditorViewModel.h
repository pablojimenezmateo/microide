#pragma once

#include <cstddef>
#include <vector>

namespace microide::editor {

struct FoldGutterMark {
  std::size_t line_index = 0;
  std::size_t visual_row_index = 0;
  bool collapsed = false;
};

struct EditorViewModel {
  std::vector<FoldGutterMark> fold_gutter_marks;
};

}  // namespace microide::editor
