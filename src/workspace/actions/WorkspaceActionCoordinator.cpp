#include "workspace/actions/WorkspaceActionCoordinator.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace microide::workspace {

ActionCoordinator::ActionCoordinator(WorkspaceActionContext context)
    : context_(std::move(context)) {}

bool ActionCoordinator::Execute(ActionId id,
                                const std::vector<std::string>& args,
                                ActionSource source) {
  context_.PrepareForAction(source);

  std::string rejection_feedback;
  const auto dispatch_result = [&](DispatchResult result) -> std::optional<bool> {
    switch (result) {
      case DispatchResult::Unhandled:
        return std::nullopt;
      case DispatchResult::Handled:
        return true;
      case DispatchResult::Rejected:
        return context_.RejectAction(source, std::move(rejection_feedback));
    }
    return std::nullopt;
  };

  if (const auto handled =
          dispatch_result(ExecuteProject(id, args, source, &rejection_feedback));
      handled.has_value()) {
    return *handled;
  }
  if (const auto handled =
          dispatch_result(ExecuteSidebar(id, args, source, &rejection_feedback));
      handled.has_value()) {
    return *handled;
  }
  if (const auto handled =
          dispatch_result(ExecuteSearch(id, args, source, &rejection_feedback));
      handled.has_value()) {
    return *handled;
  }
  if (const auto handled = dispatch_result(ExecuteTab(id, args, source, &rejection_feedback));
      handled.has_value()) {
    return *handled;
  }
  if (const auto handled =
          dispatch_result(ExecuteEdit(id, args, source, &rejection_feedback));
      handled.has_value()) {
    return *handled;
  }
  if (const auto handled =
          dispatch_result(ExecuteGlobal(id, args, source, &rejection_feedback));
      handled.has_value()) {
    return *handled;
  }

  return true;
}

}  // namespace microide::workspace
