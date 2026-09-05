#include "workspace/actions/WorkspaceActionCoordinator.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "util/Parse.h"
#include "util/StringUtil.h"
#include "workspace/actions/WorkspaceActionRequests.h"
#include "workspace/WorkspaceCommandParsing.h"
namespace microide::workspace {

namespace {

// Parse a `<file> <line>` breakpoint-command target. `line` is 1-based on the
// wire and returned 0-based to match the BreakpointStore. Relative paths
// resolve against the project root. Returns nullopt with *error on bad input.
std::optional<std::pair<std::filesystem::path, std::size_t>> ParseBreakpointTarget(
    const std::filesystem::path& project_root,
    const std::vector<std::string>& args,
    std::string* error) {
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

ActionCoordinator::DispatchResult ActionCoordinator::ExecuteGlobal(ActionId id,
                                                                  const std::vector<std::string>& args,
                                                                  ActionSource source,
                                                                  std::string* rejection_feedback) {
  (void)source;
  const auto reject = [&](std::string feedback) {
    if (rejection_feedback != nullptr) {
      *rejection_feedback = std::move(feedback);
    }
    return DispatchResult::Rejected;
  };
  switch (id) {
    case ActionId::CodeActions:
    case ActionId::Completion:
    case ActionId::InsertSnippet:
    case ActionId::TestsDiscover:
      return DispatchResult::Unhandled;
    case ActionId::Colorscheme: {
      const std::optional<ColorschemeRequest> request = BuildColorschemeRequest(args);
      if (!request.has_value()) {
        return DispatchResult::Handled;
      }
      if (request->list) {
        context_.RefreshAvailableColorschemeNames();
        return DispatchResult::Handled;
      }
      context_.RefreshAvailableColorschemeNames();
      context_.ApplyColorscheme(request->name);
      return DispatchResult::Handled;
    }
    case ActionId::ToggleFullscreen:
      // The shell only records the request; Application owns the SDL window and
      // applies it on the next ConsumeWindowAction poll.
      context_.ToggleWindowFullscreen();
      return DispatchResult::Handled;
    case ActionId::ToggleColorTheme: {
      // Flip between the built-in light and dark themes; any other active scheme
      // resolves to dark first so the toggle has a predictable two-state feel.
      const bool currently_light = util::ToLowerAscii(std::string(
                                       context_.CurrentColorschemeName())) == "light";
      context_.RefreshAvailableColorschemeNames();
      context_.ApplyColorscheme(currently_light ? "default" : "light");
      return DispatchResult::Handled;
    }
    case ActionId::TabSize: {
      const std::optional<EditorPreferenceSizeRequest> request =
          BuildEditorPreferenceSizeRequest(args);
      if (!request.has_value()) {
        return reject("tab-size requires an integer from 1 to 16");
      }
      context_.SetTabSize(request->value);
      return DispatchResult::Handled;
    }
    case ActionId::IndentWidth: {
      const std::optional<EditorPreferenceSizeRequest> request =
          BuildEditorPreferenceSizeRequest(args);
      if (!request.has_value()) {
        return reject("indent-width requires an integer from 1 to 16");
      }
      context_.SetIndentWidth(request->value);
      return DispatchResult::Handled;
    }
    case ActionId::UiScale: {
      const std::optional<UiScaleRequest> request = BuildUiScaleRequest(args);
      if (!request.has_value()) {
        return reject("ui-scale requires a preset or numeric value");
      }
      switch (request->kind) {
        case UiScaleRequest::Kind::Step:
          context_.ApplyUiScale(StepUiScale(context_.UiScale(), request->delta));
          break;
        case UiScaleRequest::Kind::Reset:
          context_.ApplyUiScale(1.0f);
          break;
        case UiScaleRequest::Kind::Direct:
          context_.ApplyUiScale(request->scale);
          break;
      }
      return DispatchResult::Handled;
    }
    case ActionId::SoftTabs: {
      const std::optional<SoftTabsRequest> request = BuildSoftTabsRequest(args);
      if (!request.has_value()) {
        return reject("soft-tabs expects on or off");
      }
      context_.SetSoftTabs(request->enabled);
      return DispatchResult::Handled;
    }
    case ActionId::Wrap: {
      if (args.empty()) {
        context_.SetSoftWrap(!context_.SoftWrapEnabled());
        return DispatchResult::Handled;
      }
      const std::optional<WrapRequest> request = BuildWrapRequest(args);
      if (!request.has_value()) {
        return reject("wrap expects on or off");
      }
      context_.SetSoftWrap(request->enabled);
      return DispatchResult::Handled;
    }
    case ActionId::Focus: {
      const FocusRequest request = BuildFocusRequest(args);
      if (context_.Focus(request.target)) {
        return DispatchResult::Handled;
      }
      return reject("Cannot focus target: " +
                    (request.raw_target.empty() ? std::string("<empty>") : request.raw_target));
    }
    case ActionId::OpenCommandPalette:
      context_.OpenCommandPalette();
      return DispatchResult::Handled;
    case ActionId::OpenSettings:
      context_.OpenSettingsOverlay();
      return DispatchResult::Handled;
    case ActionId::OpenHelpAbout:
    case ActionId::OpenKeyboardShortcuts:
      context_.OpenHelpAboutOverlay();
      return DispatchResult::Handled;
    case ActionId::ToggleStatusBar:
      context_.ToggleStatusBar();
      return DispatchResult::Handled;
    case ActionId::ToggleLayoutMode:
      context_.ToggleLayoutMode();
      return DispatchResult::Handled;
    case ActionId::PluginsReload:
      if (!context_.PluginRuntimeEnabled()) {
        return reject("Lua plugin runtime unavailable");
      }
      context_.ReloadPluginsWithFeedback();
      return DispatchResult::Handled;
    case ActionId::DebugToggleEnabled:
      // The master toggle is intentionally ungated: it flips `debug.enabled`
      // itself and reports the new state via a toast.
      context_.ToggleDebuggerEnabled();
      return DispatchResult::Handled;
    case ActionId::StartDebugging:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      context_.StartDebuggingWithFeedback();
      return DispatchResult::Handled;
    case ActionId::StopDebugging:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      context_.StopDebuggingWithFeedback();
      return DispatchResult::Handled;
    case ActionId::DebugContinue:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      context_.DebugContinue();
      return DispatchResult::Handled;
    case ActionId::DebugStepOver:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      context_.DebugStepOver();
      return DispatchResult::Handled;
    case ActionId::DebugStepIn:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      context_.DebugStepIn();
      return DispatchResult::Handled;
    case ActionId::DebugStepOut:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      context_.DebugStepOut();
      return DispatchResult::Handled;
    case ActionId::DebugPause:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      context_.DebugPause();
      return DispatchResult::Handled;
    case ActionId::DebugReverseContinue:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      context_.DebugReverseContinue();
      return DispatchResult::Handled;
    case ActionId::DebugStepBack:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      context_.DebugStepBack();
      return DispatchResult::Handled;
    case ActionId::DebugRestart:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      context_.DebugRestart();
      return DispatchResult::Handled;
    case ActionId::DebugSwitchSession: {
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      if (context_.DebugSessionCount() < 2) {
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
      context_.DebugSwitchSession(index);
      return DispatchResult::Handled;
    }
    case ActionId::DebugStopAllSessions:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      if (!context_.DebugSessionActive()) {
        return reject("No debug session is running");
      }
      context_.StopAllDebugSessions();
      return DispatchResult::Handled;
    case ActionId::DebugConsoleRepl:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      if (!context_.DebugSessionActive()) {
        return reject("Start a debug session to evaluate expressions");
      }
      context_.OpenDebugReplPrompt();
      return DispatchResult::Handled;
    case ActionId::PickLaunchConfig:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      context_.OpenLaunchConfigPicker();
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
      const DebugPaneMode mode = id == ActionId::DebugPaneShowVariables ? DebugPaneMode::Variables
                                 : id == ActionId::DebugPaneShowWatch    ? DebugPaneMode::Watch
                                 : id == ActionId::DebugPaneShowBreakpoints
                                     ? DebugPaneMode::Breakpoints
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
      const WorkspaceActionContext::DebugValueRowSelection row =
          context_.SelectedDebugValueRow();
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
      } else if (id == ActionId::DebugBreakpointClearCondition ||
                 id == ActionId::DebugBreakpointToggleEnabled) {
        context_.BreakpointQuickActionFromMenu(id);
      } else {
        context_.EditBreakpointModifierFromMenu(id);
      }
      return DispatchResult::Handled;
    case ActionId::TestsRun: {
      std::string error_message;
      const bool ok = args.empty() ? context_.RunAllDiscoveredTests(&error_message)
                                   : context_.RunTests(args, &error_message);
      if (!ok) {
        return reject(error_message.empty() ? "Test run failed" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::Quit:
      context_.RequestQuit();
      return DispatchResult::Handled;
    case ActionId::SetSetting: {
      if (args.empty()) {
        return reject("set-setting requires <id> <value>");
      }
      // value is the remainder so string settings can carry spaces.
      const std::string value = JoinCommandArguments(args, 1);
      if (!context_.SetSettingValue(args[0], value)) {
        return reject("set-setting: unknown setting or invalid value for \"" + args[0] + "\"");
      }
      return DispatchResult::Handled;
    }
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
      context_.MutableBreakpointStore().Set(target->first, target->second,
                                            id == ActionId::BreakpointEnable);
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
      std::optional<std::string> value =
          expr.empty() ? std::nullopt : std::optional<std::string>(expr);
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
      context_.AddFunctionBreakpoint(name);
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
        context_.RemoveFunctionBreakpoint(name);
      } else {
        context_.ToggleFunctionBreakpoint(name);
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
      context_.SetFunctionBreakpointCondition(
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
      context_.SetExceptionFilterCondition(
          args[0], expr.empty() ? std::nullopt : std::optional<std::string>(expr));
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
      const std::string program = args[index];
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
