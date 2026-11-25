#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/actions/WorkspaceActionTypes.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

class CommandLineCoordinator {
 public:
  struct PluginCommandResult {
    bool handled = false;
    std::string feedback;
    std::string error;
  };

  struct Operations {
    std::function<bool(ActionId, const std::vector<std::string>&, ActionSource)> execute_action;
    std::function<const std::vector<std::string>&()> plugin_command_names;
    std::function<std::vector<std::string>()> sidebar_view_ids;
    std::function<std::vector<std::string>()> remote_hosts;
    std::function<PluginCommandResult(const std::string&, const std::vector<std::string>&)>
        execute_plugin_command;
  };

  CommandLineCoordinator(ProjectWorkspaceState& state,
                         std::vector<std::string>& available_colorscheme_names,
                         Operations operations);

  bool RejectAction(ActionSource source, std::string feedback);
  bool ExecuteCommandLine(const std::string& command_line);
  void CompleteInput(editor::SingleLineEditor& input);

 private:
  void ClearFeedback();
  void SetFeedback(std::string feedback);

  ProjectWorkspaceState& state_;
  std::vector<std::string>& available_colorscheme_names_;
  Operations operations_;
};

}
