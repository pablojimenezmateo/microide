#include "workspace/actions/WorkspaceActionCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "workspace/git/GitSidebarCommandCenter.h"
#include "workspace/actions/WorkspaceActionRequests.h"
#include "workspace/WorkspacePathUtils.h"
#include "workspace/registries/WorkspaceSidebarRegistry.h"

namespace microide::workspace {

ActionCoordinator::DispatchResult ActionCoordinator::ExecuteSidebar(ActionId id,
                                                                    const std::vector<std::string>& args,
                                                                    ActionSource source,
                                                                    std::string* rejection_feedback) {
  const auto reject = [&](std::string feedback) {
    if (rejection_feedback != nullptr) {
      *rejection_feedback = std::move(feedback);
    }
    return DispatchResult::Rejected;
  };

  switch (id) {
    case ActionId::SidebarToggle: {
      const SidebarViewRequest request = context_.ParseSidebarViewRequest(args);
      if (request.view.has_value()) {
        if (!context_.ToggleSidebarView(*request.view, request.root, request.query)) {
          if (request.view->mode == SidebarMode::Plugin) {
            return reject("Failed to show plugin sidebar: " + std::string(request.view->id));
          }
          return DispatchResult::Handled;
        }
        return DispatchResult::Handled;
      }
      context_.ToggleSidebar();
      return DispatchResult::Handled;
    }
    case ActionId::SidebarShow: {
      const SidebarViewRequest request = context_.ParseSidebarViewRequest(args);
      if (request.view.has_value()) {
        if (!context_.ShowSidebarView(*request.view, request.root, request.query)) {
          if (request.view->mode == SidebarMode::Plugin) {
            return reject("Failed to show plugin sidebar: " + std::string(request.view->id));
          }
          return DispatchResult::Handled;
        }
        return DispatchResult::Handled;
      }
      context_.ShowSidebarSurface();
      return DispatchResult::Handled;
    }
    case ActionId::SidebarHide:
    case ActionId::SidebarClose:
      context_.CloseSidebar();
      return DispatchResult::Handled;
    case ActionId::SidebarWidth: {
      const std::optional<SidebarWidthRequest> request = BuildSidebarWidthRequest(args);
      if (!request.has_value()) {
        return reject("sidebar-width requires a numeric width");
      }
      context_.SetSidebarWidth(request->width);
      return DispatchResult::Handled;
    }
    case ActionId::TreeRefresh:
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      context_.RefreshProjectFiles();
      context_.ReloadCleanOpenBuffersFromDisk();
      return DispatchResult::Handled;
    case ActionId::GitRefresh:
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      context_.RefreshProjectFiles();
      context_.ReloadCleanOpenBuffersFromDisk();
      return DispatchResult::Handled;
    case ActionId::GitSwitchBranch:
    case ActionId::GitCreateBranch:
    case ActionId::GitFetch:
    case ActionId::GitPull:
    case ActionId::GitPush:
    case ActionId::GitPublishBranch:
    case ActionId::GitSync:
    case ActionId::GitStash:
    case ActionId::GitStashPop: {
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      // The service runs git on the background executor and reports the outcome as
      // a toast; only a refusal to start (no repo, one already in flight, missing
      // argument) comes back here as a rejection sentence.
      std::string rejection = context_.DispatchGitOperation(id, args);
      if (!rejection.empty()) {
        return reject(std::move(rejection));
      }
      return DispatchResult::Handled;
    }
    case ActionId::GitOpenChanges:
    case ActionId::GitStageToggleEntry:
    case ActionId::GitDiscardEntry: {
      // Git sidebar entry context-menu actions; they act on the selected entry.
      if (source != ActionSource::ContextMenu) {
        return DispatchResult::Unhandled;
      }
      const bool handled =
          id == ActionId::GitStageToggleEntry
              ? context_.ToggleStageSelectedGitEntry()
              : context_.DispatchSelectedGitSidebarAction(
                    id == ActionId::GitOpenChanges ? GitSidebarActionId::DefaultView
                                                   : GitSidebarActionId::Discard);
      if (!handled) {
        return reject("Git action is unavailable for this entry");
      }
      return DispatchResult::Handled;
    }
    case ActionId::CreateFile:
    case ActionId::CreateDirectory: {
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      const std::filesystem::path base_path = context_.TreeMutationBasePath(source);
      if (base_path.empty()) {
        return reject("No target directory selected");
      }
      context_.OpenCreatePathPrompt(id == ActionId::CreateDirectory, base_path);
      return DispatchResult::Handled;
    }
    case ActionId::RenamePath: {
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      const std::filesystem::path path = context_.ResolveTreeActionPath(source);
      if (path.empty()) {
        return reject("No path selected");
      }
      context_.OpenRenamePathPrompt(path);
      return DispatchResult::Handled;
    }
    case ActionId::DeletePath: {
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      const std::filesystem::path path = context_.ResolveTreeActionPath(source);
      if (path.empty()) {
        return reject("No path selected");
      }
      context_.OpenDeletePathPrompt(path);
      return DispatchResult::Handled;
    }
    case ActionId::CopyRelativePath:
    case ActionId::CopyAbsolutePath: {
      if (source != ActionSource::ContextMenu) {
        return DispatchResult::Unhandled;
      }
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      const std::filesystem::path path = context_.ResolveTreeActionPath(source);
      if (path.empty()) {
        return DispatchResult::Unhandled;
      }

      std::string clipboard_text;
      if (id == ActionId::CopyRelativePath) {
        clipboard_text = RelativePathLabel(context_.ProjectRoot(), path);
        if (clipboard_text.empty() ||
            clipboard_text == path.lexically_normal().generic_string()) {
          return reject("Unable to resolve a relative path for the selection");
        }
      } else {
        clipboard_text = path.lexically_normal().string();
      }

      context_.WriteClipboardText(clipboard_text);
      return DispatchResult::Handled;
    }
    case ActionId::ShowInFileExplorer: {
      if (source != ActionSource::ContextMenu) {
        return DispatchResult::Unhandled;
      }
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      const std::filesystem::path path = context_.ResolveTreeActionPath(source);
      if (path.empty()) {
        return DispatchResult::Unhandled;
      }
      std::filesystem::path containing = path.parent_path();
      if (containing.empty()) {
        containing = path;
      }
      if (!context_.RevealPathInFileExplorer(containing)) {
        return reject("Unable to open file explorer");
      }
      return DispatchResult::Handled;
    }
    case ActionId::Tree: {
      const TreeRootRequest request = BuildTreeRootRequest(args);
      if (request.root.empty() && !context_.HasProjectRoot()) {
        return reject("No active project");
      }
      context_.ShowSidebarView(SidebarViewInfo{
                                   .id = "tree",
                                   .label = "Tree",
                                   .mode = SidebarMode::Tree,
                               },
                               request.root, {});
      return DispatchResult::Handled;
    }
    default:
      return DispatchResult::Unhandled;
  }
}

}  // namespace microide::workspace
