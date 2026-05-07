#include "workspace/WorkspaceActionCoordinator.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

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
    case ActionId::AuthLogin: {
      if (args.empty()) {
        return reject("auth-login requires a provider id");
      }
      std::vector<std::string> scopes;
      if (args.size() > 1) {
        scopes.assign(args.begin() + 1, args.end());
      }
      std::string error_message;
      if (!context_.LoginAuthProvider(args.front(), scopes, &error_message)) {
        return reject(error_message.empty() ? "Auth login failed" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::AuthRefresh: {
      if (args.size() < 2) {
        return reject("auth-refresh requires a provider id and session id");
      }
      std::string error_message;
      if (!context_.RefreshAuthSession(args[0], args[1], &error_message)) {
        return reject(error_message.empty() ? "Auth refresh failed" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::AuthLogout: {
      if (args.size() < 2) {
        return reject("auth-logout requires a provider id and session id");
      }
      std::string error_message;
      if (!context_.LogoutAuthSession(args[0], args[1], &error_message)) {
        return reject(error_message.empty() ? "Auth logout failed" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::CodeActions:
    case ActionId::Completion:
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
    case ActionId::OpenAiProviderPicker:
      context_.OpenAiProviderPicker();
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
    case ActionId::DebugStart: {
      if (args.empty()) {
        return reject("debug-start requires a debugger type");
      }
      std::string error_message;
      if (!context_.StartDebugger(args.front(), &error_message)) {
        return reject(error_message.empty() ? "Debugger start failed" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::DebugStop:
      context_.StopDebugger();
      return DispatchResult::Handled;
    case ActionId::McpTool: {
      if (args.empty()) {
        return reject("mcp requires a tool id");
      }
      const std::string input_json = args.size() > 1 ? JoinCommandArguments(args, 1) : "{}";
      std::string error_message;
      if (!context_.InvokeMcpTool(args.front(), input_json, &error_message)) {
        return reject(error_message.empty() ? "MCP tool failed" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::PluginsReload:
      if (!context_.PluginRuntimeEnabled()) {
        return reject("Lua plugin runtime unavailable");
      }
      context_.ReloadPluginsWithFeedback();
      return DispatchResult::Handled;
    case ActionId::ShowChat: {
      if (args.empty()) {
        context_.ShowChatPanel();
        return DispatchResult::Handled;
      }
      std::string error_message;
      if (!context_.StartChatRequest(JoinCommandArguments(args, 0), &error_message)) {
        return reject(error_message.empty() ? "Chat request failed" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::ShowOutput:
      context_.ShowOutputChannel(args.empty() ? std::string_view{} : std::string_view(args.front()));
      return DispatchResult::Handled;
    case ActionId::Tasks: {
      if (args.empty()) {
        if (!context_.ShowTaskPickerOverlay()) {
          return reject("No tasks registered");
        }
        return DispatchResult::Handled;
      }
      std::string error_message;
      if (!context_.RunTaskById(args.front(), &error_message)) {
        return reject(error_message.empty() ? "Task run failed" : error_message);
      }
      return DispatchResult::Handled;
    }
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
