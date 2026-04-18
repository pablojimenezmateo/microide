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

WorkspaceShell::ActionCoordinator::DispatchResult WorkspaceShell::ActionCoordinator::ExecuteSidebar(
    ActionId id,
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
      const SidebarToolRequest request = ParseBuiltinSidebarToolRequest(args);
      const std::string plugin_id = args.empty() ? std::string{} : args.front();
      if (request.tool != nullptr && request.tool->mode == SidebarMode::Git) {
        if (shell_.surface_.sidebar_visible && shell_.surface_.sidebar_mode == request.tool->mode) {
          shell_.CloseSidebar();
        } else {
          shell_.ShowGitSidebar();
        }
        return DispatchResult::Handled;
      }
      if (request.tool != nullptr && request.tool->mode == SidebarMode::Tree) {
        if (shell_.surface_.sidebar_visible && shell_.surface_.sidebar_mode == request.tool->mode) {
          shell_.CloseSidebar();
        } else {
          shell_.ShowTreeSidebar(request.root);
        }
        return DispatchResult::Handled;
      }
      if (request.tool != nullptr && request.tool->mode == SidebarMode::Search) {
        if (shell_.surface_.sidebar_visible && shell_.surface_.sidebar_mode == request.tool->mode &&
            !shell_.surface_.sidebar_temporary) {
          shell_.CloseSidebar();
        } else {
          shell_.ShowSearchSidebar(request.query, false);
        }
        return DispatchResult::Handled;
      }
      if (request.tool != nullptr && request.tool->mode == SidebarMode::Problems) {
        if (shell_.surface_.sidebar_visible && shell_.surface_.sidebar_mode == request.tool->mode) {
          shell_.CloseSidebar();
        } else {
          shell_.ShowProblemsSidebar();
        }
        return DispatchResult::Handled;
      }
      if (!plugin_id.empty() && shell_.plugin_host_.FindSidebarProvider(plugin_id) != nullptr) {
        if (shell_.surface_.sidebar_visible && shell_.surface_.sidebar_mode == SidebarMode::Plugin &&
            shell_.surface_.sidebar_plugin_id == plugin_id) {
          shell_.CloseSidebar();
        } else if (!shell_.ShowPluginSidebar(plugin_id, false)) {
          return reject("Failed to show plugin sidebar: " + plugin_id);
        }
        return DispatchResult::Handled;
      }
      shell_.ToggleSidebar();
      return DispatchResult::Handled;
    }
    case ActionId::SidebarShow: {
      const SidebarToolRequest request = ParseBuiltinSidebarToolRequest(args);
      const std::string plugin_id = args.empty() ? std::string{} : args.front();
      if (request.tool != nullptr && request.tool->mode == SidebarMode::Git) {
        shell_.ShowGitSidebar();
        return DispatchResult::Handled;
      }
      if (request.tool != nullptr && request.tool->mode == SidebarMode::Tree) {
        shell_.ShowTreeSidebar(request.root);
        return DispatchResult::Handled;
      }
      if (request.tool != nullptr && request.tool->mode == SidebarMode::Search) {
        shell_.ShowSearchSidebar(request.query, false);
        return DispatchResult::Handled;
      }
      if (request.tool != nullptr && request.tool->mode == SidebarMode::Problems) {
        shell_.ShowProblemsSidebar();
        return DispatchResult::Handled;
      }
      if (!plugin_id.empty() && shell_.plugin_host_.FindSidebarProvider(plugin_id) != nullptr) {
        if (!shell_.ShowPluginSidebar(plugin_id, false)) {
          return reject("Failed to show plugin sidebar: " + plugin_id);
        }
        return DispatchResult::Handled;
      }
      shell_.surface_.sidebar_visible = true;
      shell_.surface_.focus = FocusTarget::Sidebar;
      return DispatchResult::Handled;
    }
    case ActionId::SidebarHide:
    case ActionId::SidebarClose:
      shell_.CloseSidebar();
      return DispatchResult::Handled;
    case ActionId::SidebarWidth: {
      const std::optional<SidebarWidthRequest> request = BuildSidebarWidthRequest(args);
      if (!request.has_value()) {
        return reject("sidebar-width requires a numeric width");
      }
      const float current_width =
          shell_.CurrentWindowRect().has_value() ? shell_.CurrentWindowRect()->w : 1.0f;
      shell_.surface_.sidebar_width =
          ClampSidebarWidth(request->width, std::max(1.0f, current_width));
      return DispatchResult::Handled;
    }
    case ActionId::TreeRefresh:
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      shell_.RefreshProjectFiles();
      shell_.ReloadCleanOpenBuffersFromDisk();
      return DispatchResult::Handled;
    case ActionId::GitRefresh:
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      shell_.RefreshProjectFiles();
      shell_.ReloadCleanOpenBuffersFromDisk();
      return DispatchResult::Handled;
    case ActionId::CreateFile:
    case ActionId::CreateDirectory: {
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      const std::filesystem::path base_path = shell_.TreeMutationBasePath(source);
      if (base_path.empty()) {
        return reject("No target directory selected");
      }
      shell_.OpenPromptSurface(id == ActionId::CreateFile ? PromptSurfaceState::Action::CreateFile
                                                          : PromptSurfaceState::Action::CreateDirectory,
                               PromptSurfaceState::Kind::TextInput, base_path);
      return DispatchResult::Handled;
    }
    case ActionId::RenamePath: {
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      const std::filesystem::path path = shell_.ResolveTreeActionPath(source);
      if (path.empty()) {
        return reject("No path selected");
      }
      shell_.OpenPromptSurface(PromptSurfaceState::Action::RenamePath,
                               PromptSurfaceState::Kind::TextInput, path,
                               path.filename().string());
      return DispatchResult::Handled;
    }
    case ActionId::DeletePath: {
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      const std::filesystem::path path = shell_.ResolveTreeActionPath(source);
      if (path.empty()) {
        return reject("No path selected");
      }
      shell_.OpenPromptSurface(PromptSurfaceState::Action::DeletePath,
                               PromptSurfaceState::Kind::Confirm, path);
      return DispatchResult::Handled;
    }
    case ActionId::CopyRelativePath:
    case ActionId::CopyAbsolutePath: {
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      const std::filesystem::path path = shell_.ResolveTreeActionPath(source);
      if (path.empty()) {
        return reject("No path selected");
      }

      std::string clipboard_text;
      if (id == ActionId::CopyRelativePath) {
        std::error_code error;
        const std::filesystem::path relative =
            std::filesystem::relative(path, shell_.project_root_, error);
        if (error || relative.empty()) {
          return reject("Unable to resolve a relative path for the selection");
        }
        clipboard_text = relative.generic_string();
      } else {
        clipboard_text = path.lexically_normal().string();
      }

      shell_.WriteClipboardText(clipboard_text);
      return DispatchResult::Handled;
    }
    case ActionId::Tree: {
      const TreeRootRequest request = BuildTreeRootRequest(args);
      if (request.root.empty() && shell_.project_root_.empty()) {
        return reject("No active project");
      }
      shell_.ShowTreeSidebar(request.root);
      return DispatchResult::Handled;
    }
    default:
      return DispatchResult::Unhandled;
  }
}

}  // namespace microide::workspace
