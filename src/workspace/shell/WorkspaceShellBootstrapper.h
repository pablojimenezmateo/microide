#pragma once

#include "workspace/WorkspaceEventOrchestrator.h"
#include "workspace/shell/WorkspaceShell.h"

namespace microide::workspace {

class WorkspaceShell::Bootstrapper {
 public:
  explicit Bootstrapper(WorkspaceShell& shell);

  [[nodiscard]] ActionAvailability BuildActionAvailability() const;
  [[nodiscard]] WorkspaceEventDispatcher BuildEventDispatcher() const;
  [[nodiscard]] WorkspaceWakeController BuildWakeController() const;
  [[nodiscard]] WorkspaceRootView BuildRootView() const;

 private:
  WorkspaceShell& shell_;
};

}  // namespace microide::workspace
