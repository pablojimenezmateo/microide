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
      {EditorBannerAction::Overwrite, "Overwrite", 78.0f, true},
      {EditorBannerAction::Keep, "Keep", 54.0f},
  };
  static constexpr EditorBannerButton kOpenFailed[] = {
      {EditorBannerAction::Retry, "Retry", 58.0f},
  };
  static constexpr EditorBannerButton kReadOnly[] = {
      {EditorBannerAction::OverwriteReadOnly, "Overwrite", 78.0f, true},
      {EditorBannerAction::SaveAs, "Save As…", 78.0f},
      {EditorBannerAction::Keep, "Keep", 54.0f},
  };
  switch (kind) {
    case EditorBannerState::Kind::ExternalChange:
      return kExternalChange;
    case EditorBannerState::Kind::OpenFailed:
      return kOpenFailed;
    case EditorBannerState::Kind::ReadOnly:
      return kReadOnly;
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
    if (active_path == banner.path &&
        (banner.kind != EditorBannerState::Kind::OpenFailed || failed)) {
      return &banner;
    }
  }
  return nullptr;
}

SDL_FRect ActiveEditorBannerStripRect(const ProjectWorkspaceState& state,
                                      const EditorGroupRectsLayout& group_rects) {
  const std::size_t focused = state.clamped_focused_group_index();
  if (focused >= group_rects.groups.size() || ActiveEditorBannerForTab(state) == nullptr) {
    return SDL_FRect{};
  }
  return ComputeEditorBannerStripRect(group_rects.groups[focused].editor_surface);
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
      const editor::TextViewport* viewport = nullptr;
      for (const EditorGroup& group : context_.current_project_state.editor_groups) {
        for (const TabEntry& tab : group.open_tabs) {
          if (viewport == nullptr && tab.editor_state.has_value() &&
              tab.editor_state->viewport.path().lexically_normal() == path.lexically_normal()) {
            viewport = &tab.editor_state->viewport;
          }
        }
      }
      std::optional<CompareInput> on_disk = ReadFileCompareInput(path, false);
      if (viewport != nullptr && on_disk.has_value()) {
        on_disk->label = path.filename().string() + " (on disk)";
        CompareInput unsaved{
            .content = viewport->SerializeDocumentText(),
            .label = path.filename().string() + " (unsaved)",
            .path = {},
            .editable = false,
        };
        (void)MakeCompareMergeService().diff_tabs().OpenPlainComparison(std::move(*on_disk), std::move(unsaved));
      }
      RequestPreparedFrame();
      return;
    }
    case EditorBannerAction::Reload:
      MakeEditorTabService().ReloadEditorTabsForPathFromDisk(path);
      RefreshOpenCompareTabsForPath(path);
      break;
    case EditorBannerAction::Overwrite:
      if (!MakeEditorTabService().OverwriteEditorTabsForPath(path)) {
        RequestPreparedFrame();
        return;
      }
      break;
    case EditorBannerAction::Keep:
      break;
    case EditorBannerAction::Retry: {
      MakeEditorTabService().RetryFailedOpen(path);
      SyncActiveEditorTabMetadata();
      RequestPreparedFrame();
      RequestPreparedFrame();
      return;
    }
    case EditorBannerAction::OverwriteReadOnly:
      if (!context_.current_project_state.write_gate().MakeWritable(path)) {
        Notify(NotificationService::Tone::Error,
               "Could not make " + path.filename().string() + " writable");
        return;
      }
      DismissEditorBannerForPath(context_.current_project_state, path);
      (void)ExecuteCommandName("save", {}, ActionSource::Menu, nullptr);
      RequestPreparedFrame();
      return;
    case EditorBannerAction::SaveAs:
      DismissEditorBannerForPath(context_.current_project_state, path);
      (void)ExecuteCommandName("save-as", {}, ActionSource::Menu, nullptr);
      RequestPreparedFrame();
      return;
  }
  DismissEditorBannerForPath(context_.current_project_state, path);
  RequestPreparedFrame();
}

}
