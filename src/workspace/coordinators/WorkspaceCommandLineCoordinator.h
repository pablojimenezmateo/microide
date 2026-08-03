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
    // Returns a reference to the host-owned, stable published command-name vector so
    // command-line completion does not copy the whole plugin registry on every open/
    // keystroke (TD-2026-07-17A-012). The reference is consumed synchronously.
    std::function<const std::vector<std::string>&()> plugin_command_names;
    std::function<std::vector<std::string>()> sidebar_view_ids;
    std::function<PluginCommandResult(const std::string&, const std::vector<std::string>&)>
        execute_plugin_command;
  };

  CommandLineCoordinator(ProjectWorkspaceState& state,
                         std::vector<std::string>& available_colorscheme_names,
                         Operations operations);

  bool RejectAction(ActionSource source, std::string feedback);
  // Parse and run a full command line (verb + arguments), dispatching to the matching
  // workspace action or plugin command. Feedback lands in CommandFeedbackState::text.
  bool ExecuteCommandLine(const std::string& command_line);
  // Tab-complete the active token of `input` in place (command verbs, paths, colorscheme
  // names, sidebar ids, …) and record a feedback line describing the result.
  void CompleteInput(editor::SingleLineEditor& input);

 private:
  void ClearFeedback();
  void SetFeedback(std::string feedback);

  ProjectWorkspaceState& state_;
  std::vector<std::string>& available_colorscheme_names_;
  Operations operations_;
};

}  // namespace microide::workspace
