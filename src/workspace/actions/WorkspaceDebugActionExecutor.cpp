#include "workspace/actions/WorkspaceActionCoordinator.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "util/Parse.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/debug/DebugService.h"

namespace microide::workspace {

namespace {

// Parse a `<file> <line>` breakpoint-command target. `line` is 1-based on the
// wire and returned 0-based to match the BreakpointStore. Relative paths
// resolve against the project root. Returns nullopt with *error on bad input.
std::optional<std::pair<std::filesystem::path, std::size_t>> ParseBreakpointTarget(
    const std::filesystem::path& project_root, const std::vector<std::string>& args, std::string* error) {
  if (args.size() < 2) {
    *error = "expected <file> <line>";
    return std::nullopt;
  }
  const std::optional<std::size_t> line = util::ParseSize(args[1]);
  if (!line.has_value() || *line < 1) {
    *error = "<line> must be an integer >= 1";
    return std::nullopt;
  }
  std::filesystem::path path(args[0]);
  if (!path.is_absolute()) {
    path = project_root / path;
  }
  return std::make_pair(path.lexically_normal(), *line - 1);
}

}  // namespace

// The debugger's actions: launching and stepping sessions, the debug pane's views and
// watches, and breakpoints of every kind.
ActionCoordinator::DispatchResult ActionCoordinator::ExecuteDebug(ActionId id, const std::vector<std::string>& args,
                                                                  ActionSource source,
                                                                  std::string* rejection_feedback) {
  const auto reject = RejectInto(rejection_feedback);
  switch (id) {
    case ActionId::DebugToggleEnabled:
      // The master toggle is intentionally ungated: it flips `debug.enabled`
      // itself and reports the new state via a toast.
      context_.ToggleDebuggerEnabled();
      return DispatchResult::Handled;
    case ActionId::DebugContinue:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      context_.debug().Continue();
      return DispatchResult::Handled;
    case ActionId::DebugStartOrContinue:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      if (context_.debug().IsSessionActive()) {
        context_.debug().Continue();
      } else {
        context_.StartDebuggingWithFeedback();
      }
      return DispatchResult::Handled;
    case ActionId::DebugStepOver:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      context_.debug().StepOver();
      return DispatchResult::Handled;
    case ActionId::DebugStepIn:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      context_.debug().StepIn();
      return DispatchResult::Handled;
    case ActionId::DebugStepOut:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      context_.debug().StepOut();
      return DispatchResult::Handled;
    case ActionId::DebugPause:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      context_.debug().Pause();
      return DispatchResult::Handled;
    case ActionId::DebugReverseContinue:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      // The session drops the request for an adapter that cannot run backwards;
      // say so instead of answering "ok" for a step that never happened (the
      // toolbar and menus already hide it for such an adapter).
      if (!context_.debug().SupportsStepBack()) {
        return reject(
            "Reverse Continue needs a debug session whose adapter can run backwards "
            "(supportsStepBack); this one cannot");
      }
      context_.debug().ReverseContinue();
      return DispatchResult::Handled;
    case ActionId::DebugStepBack:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      // The session drops the request for an adapter that cannot run backwards;
      // say so instead of answering "ok" for a step that never happened (the
      // toolbar and menus already hide it for such an adapter).
      if (!context_.debug().SupportsStepBack()) {
        return reject(
            "Step Back needs a debug session whose adapter can run backwards "
            "(supportsStepBack); this one cannot");
      }
      context_.debug().StepBack();
      return DispatchResult::Handled;
    case ActionId::DebugRestart:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      context_.debug().Restart();
      return DispatchResult::Handled;
    case ActionId::DebugSwitchSession: {
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      if (context_.debug().SessionCount() < 2) {
        return reject("Need at least two debug sessions to switch");
      }
      // No arg → cycle to the next session; an arg is a 1-based session index.
      int index = -1;
      if (!args.empty()) {
        if (const std::optional<int> parsed = util::ParseInt(args[0]); parsed.has_value()) {
          index = *parsed;
        } else {
          return reject("debug-switch-session expects a session number");
        }
      }
      context_.debug().SwitchSession(index);
      return DispatchResult::Handled;
    }
    case ActionId::DebugStopAllSessions:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      if (!context_.debug().IsSessionActive()) {
        return reject("No debug session is running");
      }
      context_.StopAllDebugSessions();
      return DispatchResult::Handled;
    case ActionId::DebugConsoleRepl:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      if (!context_.debug().IsSessionActive()) {
        return reject("Start a debug session to evaluate expressions");
      }
      context_.OpenDebugReplPrompt();
      return DispatchResult::Handled;
    case ActionId::DebugPaneToggle:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      context_.ToggleDebugPane();
      return DispatchResult::Handled;
    case ActionId::DebugPaneShowCallStack:
    case ActionId::DebugPaneShowVariables:
    case ActionId::DebugPaneShowWatch:
    case ActionId::DebugPaneShowBreakpoints: {
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      const DebugPaneMode mode = id == ActionId::DebugPaneShowVariables     ? DebugPaneMode::Variables
                                 : id == ActionId::DebugPaneShowWatch       ? DebugPaneMode::Watch
                                 : id == ActionId::DebugPaneShowBreakpoints ? DebugPaneMode::Breakpoints
                                                                            : DebugPaneMode::CallStack;
      context_.ShowDebugPaneSurface(mode);
      return DispatchResult::Handled;
    }
    case ActionId::DebugShowOutput:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      context_.ShowDebugOutput();
      return DispatchResult::Handled;
    case ActionId::DebugCopyValue:
    case ActionId::DebugAddToWatch: {
      // Context-menu only, like the breakpoint items: both act on the row the menu
      // was opened over, which the pane selects before opening it.
      if (source != ActionSource::ContextMenu || !context_.DebuggerEnabled()) {
        return DispatchResult::Unhandled;
      }
      const WorkspaceActionContext::DebugValueRowSelection row = context_.SelectedDebugValueRow();
      if (!row.valid) {
        return reject("No debug value selected");
      }
      if (id == ActionId::DebugCopyValue) {
        if (row.value.empty()) {
          return reject("That row has no value to copy");
        }
        context_.WriteClipboardText(row.value);
        context_.WritePrimarySelectionText(row.value);
        return DispatchResult::Handled;
      }
      if (row.name.empty()) {
        return reject("That row has no expression to watch");
      }
      context_.AddDebugWatchExpression(row.name);
      return DispatchResult::Handled;
    }
    case ActionId::DebugBreakpointEditCondition:
    case ActionId::DebugBreakpointEditHitCondition:
    case ActionId::DebugBreakpointEditLogMessage:
    case ActionId::DebugBreakpointClearCondition:
    case ActionId::DebugBreakpointToggleEnabled:
    case ActionId::DebugBreakpointRemove:
      // Context-menu only: the breakpoint gutter menu supplies the target line.
      if (source != ActionSource::ContextMenu || !context_.DebuggerEnabled()) {
        return DispatchResult::Unhandled;
      }
      if (id == ActionId::DebugBreakpointRemove) {
        context_.RemoveBreakpointFromMenu();
      } else if (id == ActionId::DebugBreakpointClearCondition || id == ActionId::DebugBreakpointToggleEnabled) {
        context_.BreakpointQuickActionFromMenu(id);
      } else {
        context_.EditBreakpointModifierFromMenu(id);
      }
      return DispatchResult::Handled;
    case ActionId::BreakpointToggle: {
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      editor::TextViewport* viewport = context_.ActiveEditableViewport();
      if (viewport == nullptr || viewport->path().empty()) {
        return reject("No active file");
      }
      // Same store call as the gutter click, on the caret's line (0-based store
      // coordinates), then the adapter learns about the change the same way.
      const std::filesystem::path path = viewport->path();
      context_.MutableBreakpointStore().Toggle(path, viewport->cursor_line());
      context_.ResendBreakpoints(path);
      context_.NotifyEditorCaretMoved();
      return DispatchResult::Handled;
    }
    case ActionId::BreakpointSet: {
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      std::string error;
      const auto target = ParseBreakpointTarget(context_.ProjectRoot(), args, &error);
      if (!target.has_value()) {
        return reject("breakpoint-set: " + error);
      }
      context_.MutableBreakpointStore().Set(target->first, target->second);
      const std::string condition = JoinCommandArguments(args, 2);
      if (!condition.empty()) {
        context_.MutableBreakpointStore().SetCondition(target->first, target->second, condition);
      }
      context_.ResendBreakpoints(target->first);
      return DispatchResult::Handled;
    }
    case ActionId::BreakpointRemove: {
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      std::string error;
      const auto target = ParseBreakpointTarget(context_.ProjectRoot(), args, &error);
      if (!target.has_value()) {
        return reject("breakpoint-remove: " + error);
      }
      context_.MutableBreakpointStore().Remove(target->first, target->second);
      context_.ResendBreakpoints(target->first);
      return DispatchResult::Handled;
    }
    case ActionId::BreakpointEnable:
    case ActionId::BreakpointDisable: {
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      std::string error;
      const auto target = ParseBreakpointTarget(context_.ProjectRoot(), args, &error);
      if (!target.has_value()) {
        return reject("breakpoint-enable/disable: " + error);
      }
      context_.MutableBreakpointStore().Set(target->first, target->second, id == ActionId::BreakpointEnable);
      context_.ResendBreakpoints(target->first);
      return DispatchResult::Handled;
    }
    case ActionId::BreakpointCondition:
    case ActionId::BreakpointHitCondition:
    case ActionId::BreakpointLogMessage: {
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      std::string error;
      const auto target = ParseBreakpointTarget(context_.ProjectRoot(), args, &error);
      if (!target.has_value()) {
        return reject("breakpoint modifier: " + error);
      }
      const std::string expr = JoinCommandArguments(args, 2);
      std::optional<std::string> value = expr.empty() ? std::nullopt : std::optional<std::string>(expr);
      editor::BreakpointStore& store = context_.MutableBreakpointStore();
      if (id == ActionId::BreakpointCondition) {
        store.SetCondition(target->first, target->second, std::move(value));
      } else if (id == ActionId::BreakpointHitCondition) {
        store.SetHitCondition(target->first, target->second, std::move(value));
      } else {
        store.SetLogMessage(target->first, target->second, std::move(value));
      }
      context_.ResendBreakpoints(target->first);
      return DispatchResult::Handled;
    }
    case ActionId::BreakpointClear: {
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      editor::BreakpointStore& store = context_.MutableBreakpointStore();
      if (args.empty()) {
        // Clear everything, then push an empty set to each previously-affected
        // file so a live session drops them too.
        const std::vector<editor::BreakpointStore::FileBreakpoints> files = store.SnapshotAll();
        store.Clear();
        for (const editor::BreakpointStore::FileBreakpoints& file : files) {
          context_.ResendBreakpoints(file.path);
        }
        return DispatchResult::Handled;
      }
      std::filesystem::path path(args[0]);
      if (!path.is_absolute()) {
        path = context_.ProjectRoot() / path;
      }
      path = path.lexically_normal();
      store.ClearFile(path);
      context_.ResendBreakpoints(path);
      return DispatchResult::Handled;
    }
    case ActionId::BreakpointFunctionAdd: {
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      const std::string name = JoinCommandArguments(args, 0);
      if (name.empty()) {
        return reject("breakpoint-function-add: a function name is required");
      }
      context_.debug().AddFunctionBreakpoint(name);
      return DispatchResult::Handled;
    }
    case ActionId::BreakpointFunctionRemove:
    case ActionId::BreakpointFunctionToggle: {
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      const std::string name = JoinCommandArguments(args, 0);
      if (name.empty()) {
        return reject("breakpoint-function: a function name is required");
      }
      if (id == ActionId::BreakpointFunctionRemove) {
        context_.debug().RemoveFunctionBreakpointByName(name);
      } else {
        context_.debug().ToggleFunctionBreakpointByName(name);
      }
      return DispatchResult::Handled;
    }
    case ActionId::BreakpointFunctionCondition: {
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      if (args.empty() || args[0].empty()) {
        return reject("breakpoint-function-condition: a function name is required");
      }
      const std::string expr = JoinCommandArguments(args, 1);
      context_.debug().SetFunctionBreakpointConditionByName(
          args[0], expr.empty() ? std::nullopt : std::optional<std::string>(expr));
      return DispatchResult::Handled;
    }
    case ActionId::BreakpointExceptionCondition: {
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      if (args.empty() || args[0].empty()) {
        return reject("breakpoint-exception-condition: a filter id is required");
      }
      const std::string expr = JoinCommandArguments(args, 1);
      // An unknown filter, or a condition on one that takes none, answered "ok"
      // for a condition that was never set.
      if (const std::string error = context_.debug().SetExceptionFilterCondition(
              args[0], expr.empty() ? std::nullopt : std::optional<std::string>(expr));
          !error.empty()) {
        return reject(error);
      }
      return DispatchResult::Handled;
    }
    case ActionId::DebugLaunch: {
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      const std::string name = JoinCommandArguments(args, 0);
      const std::string error = context_.StartNamedDebugConfig(name);
      if (!error.empty()) {
        return reject(error);
      }
      return DispatchResult::Handled;
    }
    case ActionId::DebugRun: {
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      // debug-run [--type <adapter>] <program> [args...]
      std::size_t index = 0;
      std::string type;
      if (index < args.size() && args[index] == "--type") {
        if (index + 1 >= args.size()) {
          return reject("debug-run: --type requires an adapter name");
        }
        type = args[index + 1];
        index += 2;
      }
      if (index >= args.size() || args[index].empty()) {
        return reject("debug-run: a program path is required");
      }
      const std::string& program = args[index];
      const std::vector<std::string> program_args(args.begin() + index + 1, args.end());
      const std::string error = context_.StartAdHocDebug(program, program_args, type);
      if (!error.empty()) {
        return reject(error);
      }
      return DispatchResult::Handled;
    }
    default:
      return DispatchResult::Unhandled;
  }
}

}  // namespace microide::workspace
