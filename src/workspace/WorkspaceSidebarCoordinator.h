#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class WorkspaceShell::SidebarCoordinator {
 public:
  explicit SidebarCoordinator(WorkspaceShell& shell);

  void ShowMode(SidebarMode mode, bool temporary = false);
  void ShowTree(const std::filesystem::path& root = {});
  void ShowSearch(std::string query = {}, bool temporary = false);
  void ShowGit();
  void Close();
  void Toggle();
  void RestorePrevious();
  void RefreshProjectFiles();
  void RefreshGit();
  void RevealSelectedGitLine();
  void MoveGitSelection(int delta);
  bool OpenGitEntry(std::size_t entry_index);
  bool CanStageAllGitEntries() const;
  bool CanDiscardAllGitEntries() const;
  bool StageAllGitEntries();
  void OpenDiscardAllGitPrompt();
  bool DiscardAllGitEntries();
  bool StageGitEntry(std::size_t entry_index);
  bool UnstageGitEntry(std::size_t entry_index);
  bool DiscardGitEntry(std::size_t entry_index);
  void ReconcileOpenTabsAfterPathDiscard(const std::filesystem::path& path);

 private:
  WorkspaceShell& shell_;
};

}  // namespace microide::workspace
