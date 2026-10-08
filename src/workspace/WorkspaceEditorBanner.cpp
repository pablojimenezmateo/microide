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

std::span<const EditorBannerButton> EditorBannerButtonsFor(EditorBannerState::Kind kind) {
  static constexpr EditorBannerButton kExternalChange[] = {
      {EditorBannerAction::Compare, "Compare", 72.0f},
      {EditorBannerAction::Reload, "Reload", 64.0f},
      {EditorBannerAction::Overwrite, "Overwrite", 78.0f, /*destructive=*/true},
      {EditorBannerAction::Keep, "Keep", 54.0f},
  };
  static constexpr EditorBannerButton kOpenFailed[] = {
      {EditorBannerAction::Retry, "Retry", 58.0f},
  };
  switch (kind) {
    case EditorBannerState::Kind::ExternalChange:
      return kExternalChange;
    case EditorBannerState::Kind::OpenFailed:
      return kOpenFailed;
    case EditorBannerState::Kind::ReloadedNotice:
      break;
  }
  return {};
}

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
  const bool failed = tab.editor_state->content == TabEntry::EditorTabState::Content::Failed;
  for (const EditorBannerState& banner : state.editor_banners) {
    // A failed open's banner describes the TAB's state, not the file's: once the
    // tab has content (a retry, a reload, a reopen) it no longer applies.
    if (active_path == banner.path &&
        (banner.kind != EditorBannerState::Kind::OpenFailed || failed)) {
      return &banner;
    }
  }
  return nullptr;
}

void SetEditorBanner(ProjectWorkspaceState& state, EditorBannerState::Kind kind,
                     const std::filesystem::path& path, std::string reason) {
  if (path.empty()) {
    return;
  }
  const std::filesystem::path normalized = path.lexically_normal();
  for (EditorBannerState& banner : state.editor_banners) {
    if (banner.path == normalized) {
      banner.kind = kind;
      banner.reason = std::move(reason);
      return;
    }
  }
  state.editor_banners.push_back(EditorBannerState{kind, normalized, std::move(reason)});
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
    case EditorBannerAction::Retry: {
      // Every tab still holding the failed read goes back to "not loaded yet" and
      // is restored again — off the shell thread when the file is large, as the
      // first try was. A failure lands back here through the completion.
      MakeTabCoordinator().RetryFailedOpen(path);
      SyncActiveEditorTabMetadata();
      RequestEditorSurfaceRedraw();
      RequestTabStripRedraw();
      return;
    }
  }
  DismissEditorBannerForPath(context_.current_project_state, path);
  RequestEditorSurfaceRedraw();
}

}  // namespace microide::workspace
