#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "workspace/CompareInput.h"
#include "workspace/git/CompareTabLoad.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

class DiffTabCoordinator {
 public:
  struct Operations {
    std::function<void()> sync_active_editor_tab;
    std::function<void(const std::filesystem::path&)> notify_plugin_buffer_open;
    std::function<void()> reveal_active_compare_selection;
    std::function<void()> reveal_active_merge_selection;
    std::function<void()> ensure_active_tab_visible;
    std::function<void(bool)> dismiss_overlay;
    std::function<void()> request_frame;
    std::function<std::optional<TabEntry>(const std::filesystem::path&, const CompareTabState&,
                                          const CompareRevisionSides*)>
        rebuild_compare_tab_entry;
    std::function<std::optional<TabEntry>(const std::filesystem::path&,
                                          const std::string&,
                                          const std::string&,
                                          const std::string&,
                                          const std::string&,
                                          std::size_t,
                                          bool,
                                          bool,
                                          bool)>
        build_compare_tab_from_buffers;
    std::function<std::optional<TabEntry>(CompareInput, CompareInput)> build_plain_compare_tab;
    std::function<std::optional<TabEntry>(const std::filesystem::path&,
                                          const std::filesystem::path&,
                                          const std::filesystem::path&,
                                          const std::filesystem::path&)>
        build_merge_tab_entry;
    std::function<std::optional<TabEntry>(const std::filesystem::path&,
                                          const std::string&,
                                          const std::string&,
                                          const std::string&,
                                          const std::string&,
                                          const std::string&,
                                          const std::string&,
                                          std::size_t,
                                          bool)>
        build_merge_tab_from_buffers;
    std::function<void(MergeTabState&, const std::filesystem::path&)> finalize_git_merge_tab;
    std::function<void(CompareTabState&)> refresh_compare_tab_derived_state;
    std::function<std::uint64_t(CompareTabLoadRequest)> begin_compare_tab_load;
    std::function<void(const std::filesystem::path& path)> begin_git_conflict_prefetch;
    std::function<void(editor::TextViewport&)> apply_editor_preferences;
    std::function<void(std::string)> report_compare_load_failure;
    std::function<std::uint64_t(CompareTabRefreshRequest)> begin_compare_tab_refresh;
    std::function<void(std::uint64_t)> cancel_file_read;
  };

  static constexpr std::uintmax_t kAsyncCompareThresholdBytes = 4ull * 1024 * 1024;

  DiffTabCoordinator(ProjectWorkspaceState& state, Operations operations);

  std::optional<std::size_t> FindOpenCompareTabIndex(const std::filesystem::path& path,
                                                     std::string_view left_ref,
                                                     std::string_view right_ref) const;
  std::optional<std::size_t> FindOpenMergeTabIndex(const std::filesystem::path& path) const;
  std::optional<std::size_t> FindOpenPlainCompareTabIndex(
      const std::filesystem::path& left_path, const std::filesystem::path& right_path) const;
  void OpenComparison(const project::GitCommitEntry& commit);
  bool OpenPlainComparison(CompareInput left, CompareInput right);
  bool OpenMergeEditor(const std::filesystem::path& base_path,
                       const std::filesystem::path& incoming_path,
                       const std::filesystem::path& current_path,
                       const std::filesystem::path& output_path);
  bool OpenWorkingTreeComparison(const std::filesystem::path& path,
                                 const std::string& left_ref,
                                 const std::string& left_label,
                                 const project::GitRevisionBlobCache* prefetched = nullptr);
  bool OpenBranchHeadComparison(const std::filesystem::path& path,
                                const std::string& left_ref,
                                const std::string& left_label,
                                const std::string& right_ref,
                                const std::string& right_label,
                                const project::GitRevisionBlobCache* prefetched = nullptr,
                                const std::vector<std::filesystem::path>* review_files = nullptr);
  bool OpenGitConflictMerge(const std::filesystem::path& path,
                            const project::GitRevisionBlobCache* prefetched = nullptr);

  bool StartPendingCompareLoad(CompareTabState& compare_tab);
  void RefreshCompareTabsForPath(const std::filesystem::path& normalized_path);
  bool StartCompareTabRefresh(CompareTabState& compare_tab);
  bool ApplyCompareTabRefresh(std::uint64_t id, bool cancelled, const CompareRevisionSides& sides);
  void ApplyCompareTabLoad(std::uint64_t id, bool cancelled, bool read_ok,
                           CompareTabLoadResult& result);

 private:
  bool LoadsOffThread(const std::filesystem::path& normalized_path,
                      const project::GitRevisionBlobCache* prefetched) const;
  bool AddCompareTab(TabEntry tab, bool load_off_thread);
  void ActivateCompareTab(std::size_t index, bool dismiss_overlay);
  void ActivateMergeTab(std::size_t index);
  void RefreshExistingCompareTab(std::size_t index,
                                 const std::filesystem::path& normalized_path,
                                 bool only_when_clean);
  static void RestoreMergeViewState(MergeTabState& rebuilt_merge,
                                    const MergeTabState& previous_merge);

  ProjectWorkspaceState& state_;
  Operations operations_;
};

}
