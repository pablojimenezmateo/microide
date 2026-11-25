#include "workspace/actions/WorkspaceActionCoordinator.h"

#include "util/PerformanceTrace.h"
#include "workspace/registries/WorkspaceCommandRegistry.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace microide::workspace {

ActionCoordinator::ActionCoordinator(WorkspaceActionContext& context) : context_(context) {}

bool ActionCoordinator::Execute(ActionId id,
                                const std::vector<std::string>& args,
                                ActionSource source) {
  const bool input = id == ActionId::PressKey || id == ActionId::Pointer;
  if (!input) {
    context_.PrepareForAction(source);
    context_.NoteActionDispatched(id, source);
  }
  static const bool qa_dry_run = util::PerformanceTrace::FlagEnabled("MICROIDE_QA_ACTION_DRY_RUN");
  if (qa_dry_run && source != ActionSource::Command) {
    return true;
  }
  if (!input) {
    context_.NoteActionRan(id);
  }
  if (args.empty() && (source == ActionSource::Menu || source == ActionSource::Shortcut)) {
    if (const ActionSpec* spec = FindWorkspaceActionSpec(id);
        spec != nullptr && !spec->command_name.empty() &&
        CommandUsageRequiresArgument(spec->command_usage)) {
      if (!context_.OpenArgumentPicker(id)) {
        context_.OpenCommandArgumentsPrompt(spec->command_name);
      }
      return true;
    }
    if (context_.OpenOptionalArgumentPicker(id)) {
      return true;
    }
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
  if (const auto handled = dispatch_result(ExecuteRemote(id, args, source, &rejection_feedback)); handled.has_value()) {
    return *handled;
  }
  if (const auto handled = dispatch_result(ExecuteDebug(id, args, source, &rejection_feedback)); handled.has_value()) {
    return *handled;
  }
  if (const auto handled =
          dispatch_result(ExecuteGlobal(id, args, source, &rejection_feedback));
      handled.has_value()) {
    return *handled;
  }

  return true;
}

}
