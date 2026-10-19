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
  context_.PrepareForAction(source);
  // `key` and `pointer` are input, not actions: what the key or click runs leaves
  // its own receipt, and when it runs nothing there is none -- counted, a driver
  // read "key ran" for every key the terminal took.
  if (id != ActionId::PressKey && id != ActionId::Pointer) {
    context_.NoteActionDispatched(id, source);
  }
  // QA dry run (tools/qa's `keys` and `context-menus` sweeps): a key, a menu row
  // or a context-menu row records which action it reached and runs nothing, so
  // every one can be used in every context without closing tabs, starting
  // debuggers, deleting files or quitting. Commands from the control channel
  // still run -- the sweeps drive with them.
  static const bool qa_dry_run = util::PerformanceTrace::FlagEnabled("MICROIDE_QA_ACTION_DRY_RUN");
  if (qa_dry_run && source != ActionSource::Command) {
    return true;
  }
  // A menu row, palette row or shortcut that names a command it gives no
  // arguments to, when the command cannot run without them: ask for them (VS
  // Code's input box) instead of failing with its usage line. A context menu
  // fills its arguments from the row it was opened on, and a command line typed
  // them, so neither is asked.
  if (args.empty() && (source == ActionSource::Menu || source == ActionSource::Shortcut)) {
    if (const ActionSpec* spec = FindWorkspaceActionSpec(id);
        spec != nullptr && !spec->command_name.empty() &&
        CommandUsageRequiresArgument(spec->command_usage)) {
      context_.OpenCommandArgumentsPrompt(spec->command_name);
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
  if (const auto handled =
          dispatch_result(ExecuteGlobal(id, args, source, &rejection_feedback));
      handled.has_value()) {
    return *handled;
  }

  return true;
}

}  // namespace microide::workspace
