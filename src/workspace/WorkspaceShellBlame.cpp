#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

void WorkspaceShell::InvalidateEditorBlamePath(const std::filesystem::path& path) {
  if (context_.current_project_state.root.empty() || path.empty()) {
    return;
  }
  git_blame_service_.InvalidatePath(context_.current_project_state.root, path.lexically_normal());
}

void WorkspaceShell::ClearEditorBlame() {
  editor_blame_overlay_service_.ClearVisibleOverlay();
  active_editor_hover_target_.reset();
  git_blame_service_.Clear();
}

}  // namespace microide::workspace
