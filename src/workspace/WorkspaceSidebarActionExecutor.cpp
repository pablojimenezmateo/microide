#include "workspace/WorkspaceActionCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "workspace/WorkspaceActionRequests.h"
#include "workspace/WorkspaceSidebarRegistry.h"

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
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      const std::filesystem::path path = context_.ResolveTreeActionPath(source);
      if (path.empty()) {
        return reject("No path selected");
      }

      std::string clipboard_text;
      if (id == ActionId::CopyRelativePath) {
        std::error_code error;
        const std::filesystem::path relative =
            std::filesystem::relative(path, context_.ProjectRoot(), error);
        if (error || relative.empty()) {
          return reject("Unable to resolve a relative path for the selection");
        }
        clipboard_text = relative.generic_string();
      } else {
        clipboard_text = path.lexically_normal().string();
      }

      context_.WriteClipboardText(clipboard_text);
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
