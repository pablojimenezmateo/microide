#pragma once

#include <string>
#include <vector>

#include "workspace/WorkspaceActionServices.h"

namespace microide::workspace {

class ActionCoordinator {
 public:
  explicit ActionCoordinator(WorkspaceActionContext context);

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

  WorkspaceActionContext context_;
};

}  // namespace microide::workspace
