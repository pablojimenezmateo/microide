#include "workspace/WorkspaceActionCoordinator.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "workspace/WorkspaceActionRequests.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/WorkspaceCommandPromptCoordinator.h"
#include "workspace/WorkspacePersistenceCoordinator.h"

namespace microide::workspace {

WorkspaceShell::ActionCoordinator::DispatchResult WorkspaceShell::ActionCoordinator::ExecuteGlobal(
    ActionId id,
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
  PersistenceCoordinator persistence(shell_);

  switch (id) {
    case ActionId::Colorscheme: {
      const std::optional<ColorschemeRequest> request = BuildColorschemeRequest(args);
      if (!request.has_value()) {
        return DispatchResult::Handled;
      }
      if (request->list) {
        persistence.RefreshAvailableColorschemeNames();
        return DispatchResult::Handled;
      }
      persistence.RefreshAvailableColorschemeNames();
      persistence.ApplyColorscheme(request->name, true, true);
      return DispatchResult::Handled;
    }
    case ActionId::TabSize: {
      const std::optional<EditorPreferenceSizeRequest> request =
          BuildEditorPreferenceSizeRequest(args);
      if (!request.has_value()) {
        return reject("tab-size requires an integer from 1 to 16");
      }
      shell_.editor_preferences_.tab_size = request->value;
      shell_.ApplyEditorPreferencesToAllTabs();
      persistence.SaveConfigState();
      return DispatchResult::Handled;
    }
    case ActionId::IndentWidth: {
      const std::optional<EditorPreferenceSizeRequest> request =
          BuildEditorPreferenceSizeRequest(args);
      if (!request.has_value()) {
        return reject("indent-width requires an integer from 1 to 16");
      }
      shell_.editor_preferences_.indent_width = request->value;
      shell_.ApplyEditorPreferencesToAllTabs();
      persistence.SaveConfigState();
      return DispatchResult::Handled;
    }
    case ActionId::UiScale: {
      const std::optional<UiScaleRequest> request = BuildUiScaleRequest(args);
      if (!request.has_value()) {
        return reject("ui-scale requires a preset or numeric value");
      }
      switch (request->kind) {
        case UiScaleRequest::Kind::Step:
          persistence.ApplyUiScale(StepUiScale(shell_.ui_scale_, request->delta), true, true);
          break;
        case UiScaleRequest::Kind::Reset:
          persistence.ApplyUiScale(1.0f, true, true);
          break;
        case UiScaleRequest::Kind::Direct:
          persistence.ApplyUiScale(request->scale, true, true);
          break;
      }
      return DispatchResult::Handled;
    }
    case ActionId::SoftTabs: {
      const std::optional<SoftTabsRequest> request = BuildSoftTabsRequest(args);
      if (!request.has_value()) {
        return reject("soft-tabs expects on or off");
      }
      shell_.editor_preferences_.soft_tabs = request->enabled;
      shell_.ApplyEditorPreferencesToAllTabs();
      persistence.SaveConfigState();
      return DispatchResult::Handled;
    }
    case ActionId::Focus: {
      const FocusRequest request = BuildFocusRequest(args);
      switch (request.target) {
        case FocusRequestTarget::Sidebar:
          if (shell_.surface_.sidebar_visible) {
            shell_.surface_.focus = FocusTarget::Sidebar;
            return DispatchResult::Handled;
          }
          break;
        case FocusRequestTarget::Editor:
          shell_.surface_.focus = FocusTarget::Editor;
          return DispatchResult::Handled;
        case FocusRequestTarget::Panel:
          if (shell_.surface_.command_mode || shell_.ActiveTerminalTab() != nullptr) {
            shell_.surface_.focus = FocusTarget::Panel;
            return DispatchResult::Handled;
          }
          break;
        case FocusRequestTarget::Unknown:
          break;
      }
      return reject("Cannot focus target: " +
                    (request.raw_target.empty() ? std::string("<empty>") : request.raw_target));
    }
    case ActionId::OpenCommandPrompt: {
      const bool bottom_panel_was_visible = shell_.BottomPanelVisible();
      shell_.surface_.command_mode = true;
      shell_.surface_.focus = FocusTarget::Panel;
      shell_.command_.input.clear();
      CommandPromptCoordinator(shell_).ResetSessionState();
      shell_.RequestCommandModeTransitionRedraw(bottom_panel_was_visible);
      return DispatchResult::Handled;
    }
    case ActionId::PluginsReload:
      if (!shell_.plugin_host_.enabled()) {
        return reject("Lua plugin runtime unavailable");
      }
      shell_.ReloadPluginsForCurrentProject();
      CommandPromptCoordinator(shell_).SetFeedback(shell_.PluginRuntimeReloadSummary());
      return DispatchResult::Handled;
    case ActionId::Quit:
      shell_.RequestQuit();
      return DispatchResult::Handled;
    default:
      return DispatchResult::Unhandled;
  }
}

}  // namespace microide::workspace
