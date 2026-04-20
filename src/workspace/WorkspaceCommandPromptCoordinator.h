#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspaceActionTypes.h"
#include "workspace/WorkspaceProjectState.h"

namespace microide::workspace {

class CommandPromptCoordinator {
 public:
  struct PluginCommandResult {
    bool handled = false;
    std::string feedback;
    std::string error;
  };

  struct Operations {
    std::function<bool(ActionId, const std::vector<std::string>&, ActionSource)> execute_action;
    std::function<std::vector<std::string>()> plugin_command_names;
    std::function<std::vector<std::string>()> sidebar_view_ids;
    std::function<PluginCommandResult(const std::string&, const std::vector<std::string>&)>
        execute_plugin_command;
    std::function<bool()> bottom_panel_visible;
    std::function<void(bool)> request_command_mode_transition_redraw;
  };

  CommandPromptCoordinator(ProjectWorkspaceState& state,
                           std::vector<std::string>& available_colorscheme_names,
                           Operations operations);

  void ResetSessionState();
  void ClearFeedback();
  void SetFeedback(std::string feedback);
  bool RejectAction(ActionSource source, std::string feedback);
  void AppendInput(std::string_view input);
  bool HandleKeyDown(const SDL_KeyboardEvent& event);
  bool ExecuteCommandLine(const std::string& command_line);

  static std::string PromptStatusText(const CommandState& command);

 private:
  void PushHistory(std::string command_line);
  void StepHistory(int delta);
  void CompleteInput();

  ProjectWorkspaceState& state_;
  std::vector<std::string>& available_colorscheme_names_;
  Operations operations_;
};

}  // namespace microide::workspace
