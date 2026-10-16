#include "workspace/actions/WorkspaceActionCoordinator.h"

#include "util/PerformanceTrace.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace microide::workspace {

ActionCoordinator::ActionCoordinator(WorkspaceActionContext& context) : context_(context) {}

bool ActionCoordinator::Execute(ActionId id,
                                const std::vector<std::string>& args,
                                ActionSource source) {
  context_.PrepareForAction(source);
  context_.NoteActionDispatched(id, source);
  // QA dry run (tools/qa's `keys` sweep): a key records which action it reached
  // and runs nothing, so every binding can be pressed in every context without
  // closing tabs, starting debuggers or quitting. Commands from the control
  // channel still run -- the sweep drives with them.
  static const bool qa_dry_run = util::PerformanceTrace::FlagEnabled("MICROIDE_QA_ACTION_DRY_RUN");
  if (qa_dry_run && source == ActionSource::Shortcut) {
    return true;
  }

  std::string rejection_feedback;
  const auto dispatch_result = [&](DispatchResult result) -> std::optional<bool> {
    switch (result) {
      case DispatchResult::Unhandled:
        return std::nullopt;
      case DispatchResult::Handled:
        return true;
      case DispatchResult::Rejected:
        context_.NoteActionRejected();
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
