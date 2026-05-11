#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "workspace/WorkspaceSidebarState.h"

namespace microide::workspace {

struct OutlineFlatRow {
  std::string label;
  int depth = 0;
  std::size_t jump_line = 0;
  std::size_t jump_column = 0;
  bool has_children = false;
  bool expanded = false;
  std::string path_key;
};

std::vector<OutlineFlatRow> BuildOutlineFlatRows(const OutlineSidebarState& state);

}  // namespace microide::workspace
