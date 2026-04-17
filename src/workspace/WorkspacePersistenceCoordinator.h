#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class WorkspaceShell::PersistenceCoordinator {
 public:
  explicit PersistenceCoordinator(WorkspaceShell& shell);

  void RefreshAvailableColorschemeNames();
  bool ApplyColorscheme(std::string_view name, bool persist, bool log_feedback);
  bool ApplyUiScale(float scale, bool persist, bool log_feedback);
  bool RestoreUserConfig();
  void SaveUserConfig() const;
  bool RestoreConfigState();
  void SaveConfigState() const;
  bool RestoreSessionState();
  void SaveSessionState();
  bool RestoreWorkspaceSession();
  void SaveWorkspaceSession() const;

 private:
  std::filesystem::path SessionStatePath() const;
  std::filesystem::path WorkspaceSessionStatePath() const;

  std::optional<PersistedEditorTabState> BuildPersistedCompareTabState(
      const TabEntry& tab) const;
  std::optional<PersistedEditorTabState> BuildPersistedMergeTabState(
      const TabEntry& tab) const;
  std::optional<PersistedEditorTabState> BuildPersistedEditorTabState(
      std::size_t tab_index,
      TabEntry& tab);

  WorkspaceShell& shell_;
};

}  // namespace microide::workspace
