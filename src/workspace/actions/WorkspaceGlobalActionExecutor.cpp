#include "workspace/actions/WorkspaceActionCoordinator.h"
#include "util/Log.h"
#include "workspace/debug/DebugService.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "util/Parse.h"
#include "util/StringUtil.h"
#include "workspace/actions/WorkspaceActionRequests.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/registries/WorkspaceSidebarRegistry.h"
namespace microide::workspace {

ActionCoordinator::DispatchResult ActionCoordinator::ExecuteGlobal(ActionId id,
                                                                  const std::vector<std::string>& args,
                                                                  ActionSource source,
                                                                  std::string* rejection_feedback) {
  (void)source;
  const auto reject = RejectInto(rejection_feedback);
  switch (id) {
    case ActionId::CodeActions:
    case ActionId::Completion:
    case ActionId::InsertSnippet:
    case ActionId::TestsDiscover:
      return DispatchResult::Unhandled;
    case ActionId::Colorscheme: {
      const std::optional<ColorschemeRequest> request = BuildColorschemeRequest(args);
      if (!request.has_value()) {
        // Bare (the palette's Color Theme…, Ctrl+K Ctrl+T): pick one, as VS Code does.
        context_.OpenColorThemePicker();
        return DispatchResult::Handled;
      }
      if (request->list) {
        // `colorscheme list` refreshed the names and then said nothing.
        context_.RefreshAvailableColorschemeNames();
        context_.ReportColorschemeNames();
        return DispatchResult::Handled;
      }
      context_.RefreshAvailableColorschemeNames();
      // The apply used to report nothing for a name no theme has: the command
      // answered ok and the colors stayed put.
      if (!context_.ApplyColorscheme(request->name)) {
        return reject("Unknown colorscheme: " + request->name + " (colorscheme list)");
      }
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
      // A palette row dispatches its action with NO arguments, so these three
      // rows ("Tab Size", "Indent Width", "UI Scale") could only ever answer
      // their own usage error. They take a VALUE rather than a boolean, so there
      // is nothing to toggle: seed the palette with the verb and let the user type
      // just the value -- the fallback `open` and `project-open` already use when
      // they are invoked from a UI surface with nothing to act on. A *typed* bare
      // verb keeps rejecting: that user is already at a command line with the
      // value in reach.
      if (args.empty() && source != ActionSource::Command) {
        context_.OpenCommandPalette("tab-size ");
        return DispatchResult::Handled;
      }
      const std::optional<EditorPreferenceSizeRequest> request =
          BuildEditorPreferenceSizeRequest(args);
      if (!request.has_value()) {
        return reject("tab-size requires an integer from 1 to 16");
      }
      context_.SetTabSize(request->value);
      return DispatchResult::Handled;
    }
    case ActionId::IndentWidth: {
      if (args.empty() && source != ActionSource::Command) {  // see TabSize above
        context_.OpenCommandPalette("indent-width ");
        return DispatchResult::Handled;
      }
      const std::optional<EditorPreferenceSizeRequest> request =
          BuildEditorPreferenceSizeRequest(args);
      if (!request.has_value()) {
        return reject("indent-width requires an integer from 1 to 16");
      }
      context_.SetIndentWidth(request->value);
      return DispatchResult::Handled;
    }
    case ActionId::UiScale: {
      if (args.empty() && source != ActionSource::Command) {  // see TabSize above
        context_.OpenCommandPalette("ui-scale ");
        return DispatchResult::Handled;
      }
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
      // Bare `soft-tabs` toggles, exactly as its sibling `wrap` does below. Both
      // usages declare the argument optional (`soft-tabs [on|off]`), and both are
      // palette rows -- which dispatch with NO arguments, so rejecting here made
      // "Soft Tabs" a row that could only ever answer "soft-tabs expects on or
      // off". Same dead-entry shape the bare `open` fix repaired.
      if (args.empty()) {
        context_.SetSoftTabs(!context_.SoftTabsEnabled());
        return DispatchResult::Handled;
      }
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
    case ActionId::FocusNextPart:
    case ActionId::FocusPreviousPart:
      context_.CycleSurfaceFocus(id == ActionId::FocusPreviousPart);
      return DispatchResult::Handled;
    case ActionId::OpenCommandPalette:
      context_.OpenCommandPalette();
      return DispatchResult::Handled;
    case ActionId::FocusNotifications:
      if (!context_.FocusNotifications()) {
        return reject("No notifications to focus");
      }
      return DispatchResult::Handled;
    case ActionId::CancelSaveWait: {
      const std::optional<std::size_t> id =
          args.empty() ? std::nullopt : util::ParseSize(args[0]);
      if (!id.has_value() || !context_.CancelSaveWait(*id)) {
        return reject("Nothing is waiting to save");
      }
      return DispatchResult::Handled;
    }
    case ActionId::OpenUrl:
      if (args.empty() || !context_.OpenExternalUrl(args[0])) {
        return reject(args.empty() ? "usage: open-url <url>" : "Cannot open \"" + args[0] + "\"");
      }
      return DispatchResult::Handled;
    case ActionId::BrowseFolder: {
      std::string refusal = context_.OpenFolderPick(args.empty() ? std::filesystem::path{}
                                                                 : std::filesystem::path(args.front()));
      if (!refusal.empty()) {
        return reject(std::move(refusal));
      }
      return DispatchResult::Handled;
    }
    case ActionId::LineEnding: {
      if (args.empty()) {
        if (!context_.OpenLineEndingPick()) {
          return reject("No editable file to change the line endings of");
        }
        return DispatchResult::Handled;
      }
      const std::string ending = util::ToLowerAscii(args.front());
      if (ending != "lf" && ending != "crlf") {
        return reject("line-ending expects lf or crlf");
      }
      if (!context_.SetActiveLineEnding(ending == "lf" ? util::LineEnding::LF : util::LineEnding::CRLF)) {
        return reject("No editable file to change the line endings of");
      }
      return DispatchResult::Handled;
    }
    case ActionId::Indentation:
      if (!context_.OpenIndentationPick()) {
        return reject("No editable file to change the indentation of");
      }
      return DispatchResult::Handled;
    case ActionId::ShowOutput:
      if (args.empty()) {
        return reject("usage: show-output <channel-id>");
      }
      if (!context_.ShowOutputChannel(args[0])) {
        return reject("No output channel \"" + args[0] + "\"");
      }
      return DispatchResult::Handled;
    case ActionId::NotificationAction: {
      if (args.size() < 2) {
        return reject("usage: notification-action <row-key|row-index> <action-label|action-index>");
      }
      std::string error;
      const std::optional<NotificationAction> action =
          context_.TakeNotificationAction(args[0], args[1], &error);
      if (!action.has_value()) {
        return reject(error);
      }
      // Exactly what a click on the button runs. A rejection of the inner action is
      // reported by that call; this one only has to say it did not succeed.
      return Execute(action->id, action->args, source) ? DispatchResult::Handled
                                                       : DispatchResult::Rejected;
    }
    case ActionId::BannerAction: {
      if (args.empty()) {
        return reject("usage: banner-action <Compare|Reload|Overwrite|Keep|Retry|Save As>");
      }
      // A label may be two words ("Save As").
      std::string label = args[0];
      for (std::size_t i = 1; i < args.size(); ++i) {
        label += ' ';
        label += args[i];
      }
      std::string error;
      return context_.ActivateEditorBanner(label, &error) ? DispatchResult::Handled
                                                          : reject(error);
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
      // Nothing to stop: Shift+F5 between sessions closed the debug pane the user
      // had opened and said "Debugging stopped" (QA dbg-exception-breakpoints).
      if (!context_.debug().IsSessionActive()) {
        return reject("No debug session is running");
      }
      context_.StopDebuggingWithFeedback();
      return DispatchResult::Handled;
    case ActionId::PickLaunchConfig:
      if (!context_.DebuggerEnabled()) {
        return reject("Debugging is disabled (enable it in Settings → Debugger)");
      }
      context_.OpenLaunchConfigPicker();
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
      // Every quit says what asked for it: an unrequested clean exit was a soak
      // "crash" with nothing to say why (soak 7ab9a5476905).
      util::Log("microide quit: the Quit action");
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
    default:
      return DispatchResult::Unhandled;
  }
}

}  // namespace microide::workspace
