// Non-blocking editor banner: the "file changed on disk" surface and the passive
// "reloaded from disk" notice. Replaces the old blocking modal for external file
// changes. State lives on ProjectWorkspaceState::editor_banners; rendering is in
// WorkspaceShellRenderFrame.cpp and hit-testing in WorkspaceEditorMouseCoordinator.

#include "workspace/shell/WorkspaceShell.h"
#include "workspace/CompareInput.h"
#include "workspace/services/CompareMergeService.h"

#include <algorithm>

#include "workspace/services/EditorTabService.h"

namespace microide::workspace {

const EditorBannerState* ActiveEditorBannerForTab(const ProjectWorkspaceState& state) {
  const EditorGroup& group = state.focused_group();
  if (state.editor_banners.empty() || !group.has_active_tab()) {
    return nullptr;
  }
  const TabEntry& tab = group.active_tab();
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
    case EditorBannerAction::Compare: {
      // The file as it is on disk — in a remote project, the host's bytes the
      // mirror just pulled — beside this buffer's unsaved version. The banner stays
      // up: the choice is still Reload or Overwrite.
      const editor::TextViewport* viewport = nullptr;
      for (const EditorGroup& group : context_.current_project_state.editor_groups) {
        for (const TabEntry& tab : group.open_tabs) {
          if (viewport == nullptr && tab.editor_state.has_value() &&
              tab.editor_state->viewport.path().lexically_normal() == path.lexically_normal()) {
            viewport = &tab.editor_state->viewport;
          }
        }
      }
      std::optional<CompareInput> on_disk = ReadFileCompareInput(path, /*editable=*/false);
      if (viewport != nullptr && on_disk.has_value()) {
        on_disk->label = path.filename().string() + " (on disk)";
        CompareInput mine{
            .content = viewport->SerializeDocumentText(),
            .label = path.filename().string() + " (your changes)",
            .path = {},
            .editable = false,
        };
        (void)MakeCompareMergeService().OpenPlainComparison(std::move(*on_disk), std::move(mine));
      }
      RequestEditorSurfaceRedraw();
      return;
    }
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
