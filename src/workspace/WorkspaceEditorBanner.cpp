// Non-blocking editor banner: the "file changed on disk" surface and the passive
// "reloaded from disk" notice. Replaces the old blocking modal for external file
// changes. State lives on ProjectWorkspaceState::editor_banners; rendering is in
// WorkspaceShellRenderFrame.cpp and hit-testing in WorkspaceEditorMouseCoordinator.

#include "workspace/WorkspaceShell.h"

#include <algorithm>

#include "workspace/EditorTabService.h"

namespace microide::workspace {

const EditorBannerState* ActiveEditorBannerForTab(const ProjectWorkspaceState& state) {
  if (state.editor_banners.empty() || state.active_tab_index >= state.open_tabs.size()) {
    return nullptr;
  }
  const TabEntry& tab = state.open_tabs[state.active_tab_index];
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return nullptr;
  }
  for (const EditorBannerState& banner : state.editor_banners) {
    const bool matches_view = std::any_of(
        tab.editor_state->views.begin(), tab.editor_state->views.end(),
        [&](const auto& view) {
          return view.viewport.path().lexically_normal() == banner.path;
        });
    if (matches_view) {
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
