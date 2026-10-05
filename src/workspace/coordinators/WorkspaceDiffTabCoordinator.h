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
    std::function<void(bool)> request_active_tab_redraw;
    std::function<std::optional<TabEntry>(const std::filesystem::path&,
                                          const project::GitCommitEntry&,
                                          std::size_t)>
        build_compare_tab_entry;
    std::function<std::optional<TabEntry>(const std::filesystem::path&, const CompareTabState&)>
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
    // Post a working-tree compare load off the shell thread (TD-2026-09-29-312).
    // Returns the id the completion will carry, or 0 when no reader is wired — in
    // which case the open stays synchronous, exactly as before. The completion
    // must reach ApplyCompareTabLoad, whatever its outcome.
    std::function<std::uint64_t(CompareTabLoadRequest)> begin_compare_tab_load;
    std::function<void(editor::TextViewport&)> apply_editor_preferences;
    std::function<void(std::string)> report_compare_load_failure;
  };

  // A working-tree compare at least this large opens as a stand-in and loads off
  // the shell thread. The editor's async-open threshold, for the same reason: below
  // it the whole load is a fraction of a frame and a handoff costs more than it
  // saves (TabCoordinator::kAsyncOpenThresholdBytes).
  static constexpr std::uintmax_t kAsyncCompareThresholdBytes = 4ull * 1024 * 1024;

  DiffTabCoordinator(ProjectWorkspaceState& state, Operations operations);

  std::optional<std::size_t> FindOpenCompareTabIndex(const std::filesystem::path& path,
                                                     std::string_view left_ref,
                                                     std::string_view right_ref) const;
  std::optional<std::size_t> FindOpenMergeTabIndex(const std::filesystem::path& path) const;
  std::optional<std::size_t> FindOpenPlainCompareTabIndex(
      const std::filesystem::path& left_path, const std::filesystem::path& right_path) const;
  void OpenComparison(const project::GitCommitEntry& commit);
  // Opens a non-git comparison of two arbitrary sides (file/buffer/clipboard).
  // `left` is the reference side, `right` the primary (possibly editable) side.
  bool OpenPlainComparison(CompareInput left, CompareInput right);
  bool OpenMergeEditor(const std::filesystem::path& base_path,
                       const std::filesystem::path& incoming_path,
                       const std::filesystem::path& current_path,
                       const std::filesystem::path& output_path);
  // `prefetched` is an optional bulk-read hint (project::GitRevisionBlobCache);
  // omitting it, or passing one that does not hold a side, just reads that side
  // with its own git spawn. A multi-file review passes one so the whole set costs
  // a couple of spawns instead of two or three per file.
  bool OpenWorkingTreeComparison(const std::filesystem::path& path,
                                 const std::string& left_ref,
                                 const std::string& left_label,
                                 const project::GitRevisionBlobCache* prefetched = nullptr);
  // `review_files` is the project-relative file list this comparison is one entry
  // of, used by the next/previous-review-file jump. Pass it whenever the caller
  // already knows the set: deriving it here costs a whole `git diff` spawn per
  // opened tab, which is per-file work for a per-review answer, and for a commit
  // review the derived list is the wrong set anyway (`<commit>~1...HEAD`, i.e.
  // everything since that commit, rather than the commit's own files).
  bool OpenBranchHeadComparison(const std::filesystem::path& path,
                                const std::string& left_ref,
                                const std::string& left_label,
                                const std::string& right_ref,
                                const std::string& right_label,
                                const project::GitRevisionBlobCache* prefetched = nullptr,
                                const std::vector<std::filesystem::path>* review_files = nullptr);
  bool OpenGitConflictMerge(const std::filesystem::path& path,
                            const project::GitRevisionBlobCache* prefetched = nullptr);

  // Post the load a stand-in compare tab owes, if it owes one and none is in
  // flight. Called for each pane's FRONT compare tab when a frame is prepared, so
  // a tab loads when it is first shown and again after a cancelled load. Returns
  // whether a load was posted.
  bool StartPendingCompareLoad(CompareTabState& compare_tab);
  // Shell thread: the completion of a load posted by StartPendingCompareLoad.
  // `cancelled` leaves the tab owing its load; `read_ok` is the working file's read.
  void ApplyCompareTabLoad(std::uint64_t id, bool cancelled, bool read_ok,
                           CompareTabLoadResult& result);

 private:
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

}  // namespace microide::workspace
