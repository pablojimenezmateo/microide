#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class WorkspaceShell::PathMutationCoordinator {
 public:
  explicit PathMutationCoordinator(WorkspaceShell& shell);

  bool HasDirtyEditorTabsForPath(const std::filesystem::path& path,
                                 std::string* blocking_label) const;
  void CloseOpenTabsForPath(const std::filesystem::path& path);
  void ConfirmPromptSurface(DirtyPathResolution resolution);

 private:
  struct DirtyPathTarget {
    enum class Kind {
      EditorView,
      CompareTab,
      MergeTab,
    };

    Kind kind = Kind::EditorView;
    std::size_t tab_index = 0;
    std::size_t leaf_id = 0;
  };

  std::vector<DirtyPathTarget> DirtyPathTargetsForPath(const std::filesystem::path& path) const;
  std::vector<std::size_t> DirtyTabIndicesForPath(const std::filesystem::path& path) const;
  std::vector<std::size_t> AffectedCompareTabIndices(const std::filesystem::path& path) const;
  std::vector<std::size_t> AffectedMergeTabIndices(const std::filesystem::path& path) const;
  bool ResolveDirtyTabsForPath(const std::filesystem::path& path,
                               DirtyPromptState::Kind prompt_kind,
                               DirtyPathResolution resolution);
  void RefreshDiagnosticsAfterMutation();
  void RetargetDiagnosticsForRename(const std::filesystem::path& old_path,
                                    const std::filesystem::path& new_path);
  void ClearDiagnosticsForPath(const std::filesystem::path& path);
  void RefreshProjectViewsAfterMutation(const std::filesystem::path& preferred_tree_path);
  void RetargetOpenTabsForRename(const std::filesystem::path& old_path,
                                 const std::filesystem::path& new_path,
                                 bool preserve_unsaved_state = true);

  WorkspaceShell& shell_;
};

}  // namespace microide::workspace
