#pragma once

#include <string>
#include <utility>
#include <vector>

#include "workspace/actions/WorkspaceActionServices.h"

namespace microide::workspace {

class ActionCoordinator {
 public:
  explicit ActionCoordinator(WorkspaceActionContext& context);

  bool Execute(ActionId id, const std::vector<std::string>& args, ActionSource source);

 private:
  enum class DispatchResult {
    Unhandled,
    Handled,
    Rejected,
  };

  static auto RejectInto(std::string* rejection_feedback) {
    return [rejection_feedback](std::string feedback) {
      if (rejection_feedback != nullptr) {
        *rejection_feedback = std::move(feedback);
      }
      return DispatchResult::Rejected;
    };
  }

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
  DispatchResult ExecuteRemote(ActionId id, const std::vector<std::string>& args, ActionSource source,
                               std::string* rejection_feedback);
  DispatchResult ExecuteDebug(ActionId id, const std::vector<std::string>& args, ActionSource source,
                              std::string* rejection_feedback);
  DispatchResult ExecuteGlobal(ActionId id,
                               const std::vector<std::string>& args,
                               ActionSource source,
                               std::string* rejection_feedback);

  DispatchResult ExecuteFormatJson(ActionId id, const std::vector<std::string>& args, ActionSource source,
                                   std::string* rejection_feedback);
  DispatchResult ExecutePointer(ActionId id, const std::vector<std::string>& args, ActionSource source,
                                std::string* rejection_feedback);
  DispatchResult ExecuteSortLines(ActionId id, const std::vector<std::string>& args, ActionSource source,
                                  std::string* rejection_feedback);
  DispatchResult ExecuteAddCursorAtMatch(ActionId id, const std::vector<std::string>& args, ActionSource source,
                                         std::string* rejection_feedback);

  DispatchResult ExecuteOpen(ActionId id, const std::vector<std::string>& args, ActionSource source,
                             std::string* rejection_feedback);

  WorkspaceActionContext& context_;
};

}
