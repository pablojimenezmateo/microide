#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class DiffTabCoordinator {
 public:
  explicit DiffTabCoordinator(WorkspaceShell& shell);

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

 private:
  void ActivateCompareTab(std::size_t index, bool dismiss_overlay);
  void ActivateMergeTab(std::size_t index);
  void RefreshExistingCompareTab(std::size_t index,
                                 const std::filesystem::path& normalized_path,
                                 bool only_when_clean);
  static void RestoreMergeViewState(WorkspaceShell::MergeTabState& rebuilt_merge,
                                    const WorkspaceShell::MergeTabState& previous_merge);

  WorkspaceShell& shell_;
};

}  // namespace microide::workspace
