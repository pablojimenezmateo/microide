#pragma once

#include <cstddef>
#include <filesystem>

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class WorkspaceShell::LifecycleCoordinator {
 public:
  explicit LifecycleCoordinator(WorkspaceShell& shell);

  bool Initialize(const std::filesystem::path& project_root);
  void Shutdown();
  void RequestQuit();
  bool ConsumeQuitRequested();

 private:
  void ResetStartupState();
  void RegisterWakeEvents();
  void DestroyCursors();
  std::size_t DirtyProjectTabCount() const;

  WorkspaceShell& shell_;
};

}  // namespace microide::workspace
