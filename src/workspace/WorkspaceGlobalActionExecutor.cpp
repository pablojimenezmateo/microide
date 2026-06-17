#include "workspace/WorkspaceActionCoordinator.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "util/Parse.h"
#include "workspace/WorkspaceActionRequests.h"
#include "workspace/WorkspaceCommandParsing.h"
namespace microide::workspace {

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
    case ActionId::InlineCompletion:
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
    case ActionId::OpenCommandPrompt: {
      context_.OpenCommandPrompt();
      return DispatchResult::Handled;
    }
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
    case ActionId::DebugBreakpointEditCondition:
    case ActionId::DebugBreakpointEditHitCondition:
    case ActionId::DebugBreakpointEditLogMessage:
    case ActionId::DebugBreakpointRemove:
      // Context-menu only: the breakpoint gutter menu supplies the target line.
      if (source != ActionSource::ContextMenu || !context_.DebuggerEnabled()) {
        return DispatchResult::Unhandled;
      }
      if (id == ActionId::DebugBreakpointRemove) {
        context_.RemoveBreakpointFromMenu();
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
    default:
      return DispatchResult::Unhandled;
  }
}

}  // namespace microide::workspace
