#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class DirtyPromptCoordinator {
 public:
  explicit DirtyPromptCoordinator(WorkspaceShell& shell);

  void Confirm();

 private:
  std::optional<std::size_t> FindProjectIndexByRoot(const std::filesystem::path& root) const;
  bool SaveDirtyTabs(std::span<const std::size_t> tab_indices);
  bool SwitchProjectByRoot(const std::filesystem::path& root);
  void ConfirmCloseTab(const WorkspaceShell::DirtyPromptState& prompt);
  void ConfirmCloseTabs(const WorkspaceShell::DirtyPromptState& prompt);
  void ConfirmCloseProject(const WorkspaceShell::DirtyPromptState& prompt);
  void ConfirmQuit(const WorkspaceShell::DirtyPromptState& prompt);

  WorkspaceShell& shell_;
};

}  // namespace microide::workspace
