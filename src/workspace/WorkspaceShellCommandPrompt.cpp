#include "workspace/WorkspaceShell.h"

#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceCommandPromptCoordinator.h"

namespace microide::workspace {

CommandPromptCoordinator WorkspaceShell::MakeCommandPromptCoordinator() {
  return CommandPromptCoordinator(
      context_.current_project_state,
      available_colorscheme_names_,
      CommandPromptCoordinator::Operations{
          .execute_action =
              [this](ActionId id, const std::vector<std::string>& args, ActionSource source) {
                return ActionCoordinator(MakeActionContext()).Execute(id, args, source);
              },
          .plugin_command_names =
              [this]() {
                const auto& names = plugin_runtime_.Host().CommandNames();
                return std::vector<std::string>(names.begin(), names.end());
              },
          .sidebar_view_ids = [this]() { return OrderedSidebarViewIds(); },
          .execute_plugin_command =
              [this](const std::string& command, const std::vector<std::string>& args) {
                CommandPromptCoordinator::PluginCommandResult result;
                const std::size_t message_count_before = plugin_runtime_.Host().Messages().size();
                std::string plugin_error;
                result.handled = plugin_runtime_.Host().ExecuteCommand(command, args, &plugin_error);
                if (result.handled) {
                  if (plugin_runtime_.Host().Messages().size() > message_count_before) {
                    result.feedback = plugin_runtime_.Host().Messages().back();
                  }
                  return result;
                }
                result.error = std::move(plugin_error);
                return result;
              },
          .bottom_panel_visible = [this]() { return BottomPanelVisible(); },
          .request_command_mode_transition_redraw =
              [this](bool bottom_panel_was_visible) {
                RequestCommandModeTransitionRedraw(bottom_panel_was_visible);
              },
      });
}

}  // namespace microide::workspace
