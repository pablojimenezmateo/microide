#pragma once

#include <string>
#include <string_view>

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class WorkspaceShell::CommandPromptCoordinator {
 public:
  explicit CommandPromptCoordinator(WorkspaceShell& shell);

  void ResetSessionState();
  void ClearFeedback();
  void SetFeedback(std::string feedback);
  bool RejectAction(ActionSource source, std::string feedback);
  void AppendInput(std::string_view input);
  bool HandleKeyDown(const SDL_KeyboardEvent& event);
  bool ExecuteCommandLine(const std::string& command_line);

  static std::string PromptStatusText(const WorkspaceShell& shell);

 private:
  void PushHistory(std::string command_line);
  void StepHistory(int delta);
  void CompleteInput();

  WorkspaceShell& shell_;
};

}  // namespace microide::workspace
