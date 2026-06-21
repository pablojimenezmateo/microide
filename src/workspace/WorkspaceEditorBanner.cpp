// Non-blocking editor banner: the "file changed on disk" surface and the passive
// "reloaded from disk" notice. Replaces the old blocking modal for external file
// changes. State lives on ProjectWorkspaceState::editor_banners; rendering is in
// WorkspaceShellRenderFrame.cpp and hit-testing in WorkspaceEditorMouseCoordinator.

#include "workspace/WorkspaceShell.h"

#include <algorithm>

#include "workspace/EditorTabService.h"

namespace microide::workspace {

const EditorBannerState* ActiveEditorBannerForTab(const ProjectWorkspaceState& state) {
  if (state.editor_banners.empty() || state.focused_group().active_tab_index >= state.focused_group().open_tabs.size()) {
    return nullptr;
  }
  const TabEntry& tab = state.focused_group().open_tabs[state.focused_group().active_tab_index];
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return nullptr;
  }
  const std::filesystem::path active_path =
      tab.editor_state->viewport.path().lexically_normal();
  for (const EditorBannerState& banner : state.editor_banners) {
    if (active_path == banner.path) {
      return &banner;
    }
  }
  return nullptr;
}

void SetEditorBanner(ProjectWorkspaceState& state, EditorBannerState::Kind kind,
                     const std::filesystem::path& path) {
  if (path.empty()) {
    return;
  }
  const std::filesystem::path normalized = path.lexically_normal();
  for (EditorBannerState& banner : state.editor_banners) {
    if (banner.path == normalized) {
      banner.kind = kind;
      return;
    }
  }
  state.editor_banners.push_back(EditorBannerState{kind, normalized});
}

bool DismissEditorBannerForPath(ProjectWorkspaceState& state, const std::filesystem::path& path) {
  const std::filesystem::path normalized = path.lexically_normal();
  auto& banners = state.editor_banners;
  const auto before = banners.size();
  banners.erase(std::remove_if(banners.begin(), banners.end(),
                               [&](const EditorBannerState& banner) {
                                 return banner.path == normalized;
                               }),
                banners.end());
  return banners.size() != before;
}

void WorkspaceShell::ActivateEditorBannerAction(EditorBannerAction action,
                                                const std::filesystem::path& path) {
  switch (action) {
    case EditorBannerAction::Reload:
      MakeEditorTabService().ReloadEditorTabsForPathFromDisk(path);
      RefreshOpenCompareTabsForPath(path);
      break;
    case EditorBannerAction::Overwrite:
      MakeEditorTabService().OverwriteEditorTabsForPath(path);
      break;
    case EditorBannerAction::Keep:
      break;
  }
  DismissEditorBannerForPath(context_.current_project_state, path);
  RequestEditorSurfaceRedraw();
}

}  // namespace microide::workspace
