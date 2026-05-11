#include "workspace/WorkspaceOutlineFlat.h"

namespace microide::workspace {

namespace {

void WalkNode(const OutlineSymbolNode& node,
              int depth,
              const std::string& path_key,
              const OutlineSidebarState& state,
              std::vector<OutlineFlatRow>& out) {
  const bool has_children = !node.children.empty();
  const bool expanded =
      has_children && !state.collapsed_paths.contains(path_key);
  out.push_back(OutlineFlatRow{
      .label = node.name,
      .depth = depth,
      .jump_line = node.selection_line,
      .jump_column = node.selection_column,
      .has_children = has_children,
      .expanded = expanded,
      .path_key = path_key,
  });
  if (!expanded) {
    return;
  }
  for (std::size_t i = 0; i < node.children.size(); ++i) {
    WalkNode(node.children[i], depth + 1, path_key + "/" + std::to_string(i), state, out);
  }
}

}  // namespace

std::vector<OutlineFlatRow> BuildOutlineFlatRows(const OutlineSidebarState& state) {
  std::vector<OutlineFlatRow> out;
  for (std::size_t i = 0; i < state.roots.size(); ++i) {
    WalkNode(state.roots[i], 0, std::to_string(i), state, out);
  }
  return out;
}

}  // namespace microide::workspace
