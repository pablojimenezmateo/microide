#pragma once

#include <string>
#include <vector>

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class WorkspaceShell::ActionCoordinator {
 public:
  explicit ActionCoordinator(WorkspaceShell& shell);

  bool Execute(ActionId id, const std::vector<std::string>& args, ActionSource source);

 private:
  enum class DispatchResult {
    Unhandled,
    Handled,
    Rejected,
  };

  DispatchResult ExecuteProject(ActionId id,
                                const std::vector<std::string>& args,
                                ActionSource source,
                                std::string* rejection_feedback);
  DispatchResult ExecuteSidebar(ActionId id,
                                const std::vector<std::string>& args,
                                ActionSource source,
                                std::string* rejection_feedback);
  DispatchResult ExecuteSearch(ActionId id,
                               const std::vector<std::string>& args,
                               ActionSource source,
                               std::string* rejection_feedback);
  DispatchResult ExecuteTab(ActionId id,
                            const std::vector<std::string>& args,
                            ActionSource source,
                            std::string* rejection_feedback);
  DispatchResult ExecuteEdit(ActionId id,
                             const std::vector<std::string>& args,
                             ActionSource source,
                             std::string* rejection_feedback);
  DispatchResult ExecuteGlobal(ActionId id,
                               const std::vector<std::string>& args,
                               ActionSource source,
                               std::string* rejection_feedback);

  WorkspaceShell& shell_;
};

}  // namespace microide::workspace
