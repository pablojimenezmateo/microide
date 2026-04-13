#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class WorkspaceShell::TabCoordinator {
 public:
  explicit TabCoordinator(WorkspaceShell& shell);

  std::string ActiveTitle() const;
  bool Save(std::size_t index);
  bool IsDirty(std::size_t index) const;
  std::vector<std::size_t> DirtyIndices() const;
  std::vector<std::size_t> DirtyIndicesForProject(std::size_t project_index) const;
  void ReloadCleanEditorTabsForPath(const std::filesystem::path& path);
  bool OpenUntitled();
  bool OpenFileInNewTab(const std::filesystem::path& path);
  bool MoveActiveTo(std::size_t index);
  std::optional<std::size_t> FindIndexBySpecifier(std::string_view specifier,
                                                  std::string* error_message) const;
  bool ReopenActive();

 private:
  WorkspaceShell& shell_;
};

}  // namespace microide::workspace
