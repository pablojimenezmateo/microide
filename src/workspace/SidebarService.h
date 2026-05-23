#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspaceSidebarCoordinator.h"

namespace microide::workspace {

class SidebarService {
 public:
  explicit SidebarService(SidebarCoordinator coordinator);

  void ShowMode(SidebarMode mode, bool temporary = false);
  void ShowTree(const std::filesystem::path& root = {});
  void ShowSearch(std::string query = {}, bool temporary = false);
  void ShowProblems();
  void ShowGit();
  void ShowTests();
  bool ShowPlugin(std::string_view id, bool temporary = false);
  void Close();
  void Toggle();
  void RestorePrevious();
  void RefreshProjectFiles();
  void RefreshGit();
  bool RefreshProblems();
  bool RefreshTests();
  bool RefreshPlugin();
  void RevealSelectedTreeLine();
  void RevealSelectedGitLine();
  void RevealSelectedProblemsLine();
  void RevealSelectedTestsLine();
  void RevealSelectedPluginLine();
  void MoveGitSelection(int delta);
  void MoveProblemsSelection(int delta);
  void MoveTestsSelection(int delta);
  void MovePluginSelection(int delta);
  bool OpenGitEntry(std::size_t entry_index);
  bool OpenProblemItem();
  bool OpenTestItem();
  bool RunTestItem();
  bool OpenPluginItem();
  bool CanStageAllGitEntries() const;
  bool CanDiscardAllGitEntries() const;
  bool StageAllGitEntries();
  void OpenDiscardAllGitPrompt();
  bool DiscardAllGitEntries();
  bool StageGitEntry(std::size_t entry_index);
  bool UnstageGitEntry(std::size_t entry_index);
  bool DiscardGitEntry(std::size_t entry_index);
  void OpenDiscardGitEntryPrompt(std::size_t entry_index);
  bool DispatchGitSidebarAction(GitSidebarActionId action, std::size_t entry_index);
  void ReconcileOpenTabsAfterPathDiscard(const std::filesystem::path& path);

 private:
  SidebarCoordinator coordinator_;
};

}  // namespace microide::workspace
