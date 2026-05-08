#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

std::size_t WorkspaceShell::CountOpenBufferViews(const std::filesystem::path& path) const {
  const std::filesystem::path normalized_path = path.lexically_normal();
  if (normalized_path.empty()) {
    return 0;
  }

  std::size_t count = 0;
  for (const auto& tab : context_.current_project_state.open_tabs) {
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      for (const auto& view : tab.editor_state->views) {
        if (EditorViewPath(view) == normalized_path) {
          ++count;
        }
      }
      continue;
    }
    if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
      if (tab.compare->right_editable &&
          tab.compare->right_viewport.path().lexically_normal() == normalized_path) {
        ++count;
      }
      continue;
    }
    if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value() &&
        tab.merge->result_viewport.path().lexically_normal() == normalized_path) {
      ++count;
    }
  }
  return count;
}

}  // namespace microide::workspace
