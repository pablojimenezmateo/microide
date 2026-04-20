#include "workspace/WorkspaceActionCoordinator.h"

#include <string>
#include <utility>
#include <vector>

#include "workspace/WorkspaceActionRequests.h"
#include "workspace/WorkspaceCommandPromptCoordinator.h"

namespace microide::workspace {

WorkspaceShell::ActionCoordinator::DispatchResult WorkspaceShell::ActionCoordinator::ExecuteProject(
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
    case ActionId::ProjectOpen: {
      const ProjectOpenRequest request = BuildProjectOpenRequest(args);
      if (request.use_native_picker) {
        switch (shell_.OpenNativeProjectPicker(nullptr)) {
          case ProjectOpenDialogLaunchResult::Launched:
          case ProjectOpenDialogLaunchResult::AlreadyOpen:
            return DispatchResult::Handled;
          case ProjectOpenDialogLaunchResult::Unavailable:
            if (source == ActionSource::Menu) {
              const bool bottom_panel_was_visible = shell_.BottomPanelVisible();
              shell_.panel_state_.command_mode = true;
              shell_.surface_.focus = FocusTarget::Panel;
              shell_.command_.input = "project-open ";
              CommandPromptCoordinator(shell_).ResetSessionState();
              shell_.RequestCommandModeTransitionRedraw(bottom_panel_was_visible);
            }
            return DispatchResult::Handled;
        }
        return DispatchResult::Handled;
      }
      if (!shell_.OpenProjectTab(request.path, true, true)) {
        return reject("Failed to open project: " + request.path.string());
      }
      return DispatchResult::Handled;
    }
    case ActionId::ProjectClose:
      if (shell_.project_catalog_.entries.empty() || shell_.project_root_.empty()) {
        return reject("No active project");
      }
      shell_.RequestCloseProject(shell_.project_catalog_.active_index);
      return DispatchResult::Handled;
    case ActionId::ProjectNext:
    case ActionId::ProjectPrev: {
      if (shell_.project_catalog_.entries.empty() || shell_.project_root_.empty()) {
        return reject("No active project");
      }
      if (shell_.project_catalog_.entries.size() == 1) {
        return reject("Only one project tab is open");
      }
      const ProjectCycleRequest request =
          BuildProjectCycleRequest(id == ActionId::ProjectNext ? 1 : -1);
      const int project_count = static_cast<int>(shell_.project_catalog_.entries.size());
      const int next_index =
          (static_cast<int>(shell_.project_catalog_.active_index) + request.delta + project_count) %
          project_count;
      shell_.SwitchProject(static_cast<std::size_t>(next_index), true);
      return DispatchResult::Handled;
    }
    default:
      return DispatchResult::Unhandled;
  }
}

}  // namespace microide::workspace
