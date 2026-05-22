#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "compare/MergeModel.h"
#include "workspace/WorkspaceCompareInteractionCoordinator.h"
#include "workspace/WorkspaceDiffTabCoordinator.h"

namespace microide::workspace {

class CompareMergeService {
 public:
  CompareMergeService(DiffTabCoordinator diff_tabs,
                      CompareInteractionCoordinator interactions);

  std::optional<std::size_t> FindOpenCompareTabIndex(const std::filesystem::path& path,
                                                     std::string_view left_ref,
                                                     std::string_view right_ref) const;
  std::optional<std::size_t> FindOpenMergeTabIndex(const std::filesystem::path& path) const;
  void OpenComparison(const project::GitCommitEntry& commit);
  bool OpenMergeEditor(const std::filesystem::path& base_path,
                       const std::filesystem::path& incoming_path,
                       const std::filesystem::path& current_path,
                       const std::filesystem::path& output_path);
  bool OpenWorkingTreeComparison(const std::filesystem::path& path,
                                 const std::string& left_ref,
                                 const std::string& left_label);
  bool OpenBranchHeadComparison(const std::filesystem::path& path,
                                const std::string& left_ref,
                                const std::string& left_label,
                                const std::string& right_ref,
                                const std::string& right_label);
  bool OpenGitConflictMerge(const std::filesystem::path& path);

  void OpenPicker();
  bool OpenPickerForPath(const std::filesystem::path& path, std::string_view commit_spec = {});
  void RefreshPicker();
  void MovePickerSelection(int delta);
  void OpenSelectedCommit();
  void OpenWorkingFileFromCompare();
  void OpenMergeResultFile();
  void MoveCompareSelection(int delta);
  void JumpCompareHunk(int delta);
  void JumpCompareReviewFile(int delta);
  void CopyComparePath();
  void CopyCompareHunkPatch();
  void CopyCompareFilePatch();
  void StageCompareHunk();
  void StageCompareSelectedLines();
  void UnstageCompareHunk();
  void UnstageCompareSelectedLines();
  void OpenDiscardCompareHunkPrompt();
  void OpenDiscardCompareSelectedLinesPrompt();
  void ToggleCompareIgnoreWhitespace();
  void ToggleCompareShowWhitespace();
  void ScrollCompareRows(int delta);
  void ScrollCompareColumns(int delta);
  void MoveMergeSelection(int delta);
  void ScrollMergeColumns(int delta);
  void ApplyMergeChoice(compare::MergeChoice choice);

 private:
  DiffTabCoordinator diff_tabs_;
  CompareInteractionCoordinator interactions_;
};

}  // namespace microide::workspace
