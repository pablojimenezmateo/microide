#pragma once

#include <filesystem>
#include <string_view>

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class CompareInteractionCoordinator {
 public:
  explicit CompareInteractionCoordinator(WorkspaceShell& shell);

  void OpenPicker();
  bool OpenPickerForPath(const std::filesystem::path& path,
                         std::string_view commit_spec = {});
  void RefreshPicker();
  void MovePickerSelection(int delta);
  void OpenSelectedCommit();
  void OpenWorkingFileFromCompare();
  void OpenMergeResultFile();
  void MoveCompareSelection(int delta);
  void JumpCompareHunk(int delta);
  void ScrollCompareRows(int delta);
  void ScrollCompareColumns(int delta);
  void MoveMergeSelection(int delta);
  void ScrollMergeColumns(int delta);
  void ApplyMergeChoice(compare::MergeChoice choice);

 private:
  WorkspaceShell& shell_;
};

}  // namespace microide::workspace
