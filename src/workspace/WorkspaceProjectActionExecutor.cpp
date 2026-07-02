#include "workspace/WorkspaceActionCoordinator.h"

#include <string>
#include <utility>
#include <vector>

#include "workspace/WorkspaceActionRequests.h"

namespace microide::workspace {

ActionCoordinator::DispatchResult ActionCoordinator::ExecuteProject(
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
        switch (context_.OpenNativeProjectPicker()) {
          case ProjectOpenPickerResult::Launched:
          case ProjectOpenPickerResult::AlreadyOpen:
            return DispatchResult::Handled;
          case ProjectOpenPickerResult::Unavailable:
            if (source == ActionSource::Menu) {
              // No native picker: fall back to the command palette pre-filled with the
              // command so the user only types the path (then Enter runs it).
              context_.OpenCommandPalette("project-open ");
            }
            return DispatchResult::Handled;
        }
        return DispatchResult::Handled;
      }
      if (!context_.OpenProject(request.path, true, true)) {
        return reject("Failed to open project: " + request.path.string());
      }
      return DispatchResult::Handled;
    }
    case ActionId::ProjectClose:
      if (!context_.HasActiveProject()) {
        return reject("No active project");
      }
      context_.RequestCloseProject(context_.ActiveProjectIndex());
      return DispatchResult::Handled;
    case ActionId::DetachProjectToNewWindow:
      if (!context_.HasActiveProject()) {
        return reject("No active project");
      }
      if (!context_.DetachActiveProjectToNewWindow()) {
        return reject("This project cannot be moved to a new window");
      }
      return DispatchResult::Handled;
    case ActionId::ProjectCopyAbsolutePath:
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      context_.WriteClipboardText(context_.ProjectRoot().lexically_normal().string());
      return DispatchResult::Handled;
    case ActionId::ProjectNext:
    case ActionId::ProjectPrev: {
      if (!context_.HasActiveProject()) {
        return reject("No active project");
      }
      if (context_.ProjectCount() == 1) {
        return reject("Only one project tab is open");
      }
      const ProjectCycleRequest request =
          BuildProjectCycleRequest(id == ActionId::ProjectNext ? 1 : -1);
      const int project_count = static_cast<int>(context_.ProjectCount());
      const int next_index =
          (static_cast<int>(context_.ActiveProjectIndex()) + request.delta + project_count) %
          project_count;
      context_.SwitchProject(static_cast<std::size_t>(next_index), true);
      return DispatchResult::Handled;
    }
    default:
      return DispatchResult::Unhandled;
  }
}

}  // namespace microide::workspace
