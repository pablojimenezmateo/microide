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
  // The constructor's service wiring, one service per call, in this order: each
  // Configure()s a shell-owned service with the shell callbacks it needs.
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

}  // namespace microide::workspace
