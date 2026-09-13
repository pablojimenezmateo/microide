#pragma once

#include <string>
#include <vector>

#include "workspace/actions/WorkspaceActionServices.h"

namespace microide::workspace {

class ActionCoordinator {
 public:
  // Binds the shell's single long-lived context (see WorkspaceShell::MakeActionContext).
  // By reference, not by value: the context is ~180 std::functions, so taking it by
  // value cost two moves of that struct — each a manager call per function — on every
  // action dispatch, including the ones on the keyboard input path.
  explicit ActionCoordinator(WorkspaceActionContext& context);

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

  WorkspaceActionContext& context_;
};

}  // namespace microide::workspace
