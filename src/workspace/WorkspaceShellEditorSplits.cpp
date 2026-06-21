#include "workspace/WorkspaceShell.h"

#include <optional>
#include <vector>

#include "workspace/EditorTabService.h"

namespace microide::workspace {

// Editor splits are modelled as editor *groups* above the tab level (Phase B+).
// At this layer a tab renders into a single pane covering the editor surface
// (below any external-change banner). The legacy in-tab split tree was removed.

SDL_FRect WorkspaceShell::EditorSurfaceBelowBanner(const SDL_FRect& editor_surface) const {
  if (ActiveEditorBannerForTab(context_.current_project_state) == nullptr) {
    return editor_surface;
  }
  const SDL_FRect strip = ComputeEditorBannerStripRect(editor_surface);
  SDL_FRect content = editor_surface;
  content.y += strip.h;
  content.h = std::max(0.0f, content.h - strip.h);
  return content;
}

std::vector<WorkspaceShell::EditorPaneLayout> WorkspaceShell::ComputeEditorPaneLayouts(
    const SDL_FRect& editor_surface) const {
  std::vector<EditorPaneLayout> panes;
  const auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr) {
    return panes;
  }
  panes.push_back(EditorPaneLayout{
      .leaf_id = 0,
      .rect = EditorSurfaceBelowBanner(editor_surface),
      .active = true,
  });
  return panes;
}

std::vector<WorkspaceShell::EditorSplitDividerLayout>
WorkspaceShell::ComputeEditorSplitDividerLayouts(const SDL_FRect& /*editor_surface*/) const {
  return {};
}

}  // namespace microide::workspace
