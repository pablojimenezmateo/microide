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
  void WireQuickPick() const;
  void WireTabStrip() const;
  void WireLsp() const;
  void WireDebug() const;
  void WireControlChannel() const;
  void WireWorkspaceEdits() const;
  void WireAssist() const;
  void WirePluginRuntime() const;

 private:
  WorkspaceShell& shell_;
};

}
