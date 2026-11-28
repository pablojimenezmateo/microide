#pragma once

#include "project/FileWriteService.h"

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "util/TextFileIO.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/SavePreparation.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

class TabCoordinator {
 public:
  struct Operations {
    std::function<void(const std::filesystem::path&)> invalidate_editor_blame_path;
    std::function<void(const std::filesystem::path&)> notify_plugin_buffer_save;
    std::function<void(const std::filesystem::path&)> notify_plugin_buffer_open;
    std::function<void(const std::filesystem::path&)> schedule_lsp_buffer_open;
    std::function<void(const std::filesystem::path&)> notify_lsp_buffer_close;
    std::function<void(const editor::TextViewport&, std::size_t, std::size_t)>
        notify_lsp_buffer_reloaded;
    std::function<std::size_t(const std::filesystem::path&)> count_open_buffer_views;
    std::function<std::unordered_map<std::string, std::size_t>()> open_buffer_view_counts;
    std::function<SavePreparation(const std::filesystem::path&,
                                  editor::TextViewport&,
                                  std::string*,
                                  SaveMode)>
        prepare_editor_view_for_save;
    std::function<void(editor::TextViewport&)> apply_editor_preferences;
    std::function<void(editor::TextViewport&)> apply_detected_indent_on_open;
    std::function<TabEntry::EditorTabState(const editor::TextViewport&)> make_editor_tab_state;
    std::function<std::filesystem::path(const TabEntry::EditorTabState&)> editor_view_path;
    std::function<void()> reveal_selected_tree_sidebar_line;
    std::function<void()> reveal_active_compare_selection;
    std::function<void()> reveal_active_merge_selection;
    std::function<void()> ensure_active_tab_visible;
    std::function<void()> ensure_all_active_tabs_visible;
    std::function<void()> reset_caret_blink;
    std::function<void()> request_frame;
    std::function<void()> invalidate_tab_strip_geometry;
    std::function<EditorGroupRectsLayout()> editor_group_rects;
    std::function<void()> request_automatic_git_sidebar_refresh;
    std::function<void(std::uint64_t stable_id)> tab_closed;
    std::function<void(const std::filesystem::path&)> request_external_change_banner;
    std::function<void(const std::filesystem::path&, std::string_view reason)> notify_save_failed;
    std::function<std::uint64_t(const std::filesystem::path&)> begin_async_file_read;
    std::function<void(std::uint64_t)> cancel_async_file_read;
    std::function<void(std::uint64_t tab_id, bool saved)> settle_save;
    std::function<void(const std::filesystem::path&)> recheck_disk_after_save;
  };

  TabCoordinator(ProjectCatalogState& project_catalog,
                 ProjectWorkspaceState& current_project_state,
                 project::FileWriteService& file_writes,
                 Operations operations);

  bool Save(std::size_t index, SaveMode mode);
  void FinishAsyncSave(std::uint64_t tab_id, const editor::TextViewport::PreparedSave& prepared,
                       editor::TextViewport::WrittenSave written);
  [[nodiscard]] std::vector<std::string> TakeLandedSaveFailures() { return std::exchange(landed_save_failures_, {}); }
  static std::string SaveFailureMessage(const std::filesystem::path& path, std::string_view reason);
  bool SaveGroupTab(std::size_t group_index, std::size_t index,
                    SaveMode mode);
  bool SaveGroupTabAs(std::size_t group_index, std::size_t index,
                      const std::filesystem::path& path, std::string* error);
  bool IsDirty(std::size_t index) const;
  bool CloseWouldDiscardEdits(std::size_t index) const;
  std::vector<std::size_t> DirtyIndices() const;
  std::vector<std::size_t> DirtyIndicesForProject(std::size_t project_index) const;
  bool HasDirtyTabForProject(std::size_t project_index) const;
  static bool ProjectHasDirtyTab(const ProjectCatalogState& catalog,
                                 const ProjectWorkspaceState& current_project,
                                 std::size_t project_index);
  std::vector<GroupTabRef> DirtyGroupTabs() const;
  std::vector<GroupTabRef> DirtyGroupTabsForProject(std::size_t project_index) const;
  bool ActiveTabIsEditor() const;
  TabEntry::EditorTabState* ActiveEditorTab();
  const TabEntry::EditorTabState* ActiveEditorTab() const;
  editor::TextViewport* ActiveEditorViewport();
  void Activate(std::size_t index);
  void SyncActiveEditorTab();
  bool ActivateCurrentTabAfterStateLoad();
  void SyncActiveEditorTabMetadata();
  using EchoGuard = EditorReloadEchoGuard;
  bool ReloadCleanEditorTabsForPath(const std::filesystem::path& path,
                                    EchoGuard echo_guard = EchoGuard::Check,
                                    std::vector<std::filesystem::path>* reloaded_paths = nullptr);
  void ReloadEditorTabsForPathFromDisk(const std::filesystem::path& path);
  void NoteDiskChangedUnderEditsForPath(const std::filesystem::path& path);
  bool NoteMissedDiskChangeUnderEdits(const std::filesystem::path& path);
  bool RevertEditorTabsForPath(const std::filesystem::path& path);
  bool ReopenEditor(DeferredTabHandle view);
  bool ReloadEditorTabsForPath(const std::filesystem::path& path, bool clean_only,
                               EchoGuard echo_guard = EchoGuard::Check);
  bool OverwriteEditorTabsForPath(const std::filesystem::path& path);
  bool DiskSignatureMatchesOpenView(const std::filesystem::path& path) const;

  enum class ExternalChangeVerdict {
    OwnEcho,
    Changed,
    NeedsContentConfirm,
  };
  [[nodiscard]] ExternalChangeVerdict ClassifyExternalChange(
      const std::filesystem::path& path) const;
  [[nodiscard]] bool ExternalChangeIsOwnEcho(const std::filesystem::path& path,
                                             std::uint64_t disk_content_hash,
                                             const util::FileSignature& read_signature) const;
  [[nodiscard]] std::vector<std::filesystem::path> OpenSymlinkAliasesOf(
      const std::filesystem::path& normalized_target) const;
  void RefreshRealPathsOf(const std::filesystem::path& normalized_path);
  bool OpenUntitled();
  const editor::TextViewport* FindOpenEditorViewOfPath(
      const std::filesystem::path& normalized_path) const;
  bool OpenEditorViewForPath(const std::filesystem::path& path, editor::TextViewport& view) const;
  static constexpr std::uintmax_t kAsyncOpenThresholdBytes = 4ull * 1024 * 1024;
  [[nodiscard]] bool ShouldOpenOffThread(const std::filesystem::path& normalized_path) const;
  [[nodiscard]] std::uint64_t BeginOffThreadOpen(const std::filesystem::path& normalized_path,
                                                 editor::TextViewport& view) const;
  bool OpenFileInNewTab(const std::filesystem::path& path);
  bool OpenNewBufferInNewTab(const std::filesystem::path& path);
  bool OpenVirtualDocumentInNewTab(const std::filesystem::path& virtual_path,
                                   std::string_view content,
                                   std::string_view title);
  void ReloadVirtualDocumentTabs(const std::filesystem::path& virtual_path,
                                 std::string_view content);
  void Close(std::size_t index);
  struct SaveForCloseResult {
    bool saved = false;
    std::uint64_t deferred_tab_id = 0;
  };
  SaveForCloseResult SaveForClose(std::size_t index);
  bool RefuseIfChangedOnDisk(const editor::TextViewport& viewport,
                             const std::filesystem::path& normalized_path);
  bool RefuseIfReadOnly(const std::filesystem::path& normalized_path, bool banner_shows);
  enum class PaneSave { Written, Deferred, Refused };
  bool BeginAsyncSave(TabEntry& tab, SaveMode mode);
  PaneSave WritePaneViewport(editor::TextViewport& viewport,
                             editor::AsyncBufferWork& pending_format_save, SaveMode mode);
  void AfterFileSaved(const std::filesystem::path& normalized_path);
  void CloseGroupTab(std::size_t group_index, std::size_t index);
  bool SplitEditorGroup(EditorSplitOrientation orientation);
  bool FocusOtherGroup();
  bool FocusEditorGroupAt(std::size_t index);
  void ReconcileActiveCaretsWithDocuments();
  void ObserveNavigationLocation();
  std::optional<EditorNavigationHistory::Location> StepNavigation(int direction);
  bool FocusEditorGroupInDirection(EditorGroupDirection direction);
  bool MoveEditorGroupInDirection(EditorGroupDirection direction);
  bool CloseEditorGroup();
  std::size_t EditorGroupCount() const { return state_.editor_groups.size(); }
  bool MoveActiveTo(std::size_t index);
  bool MoveTabToNewGroup(std::size_t from_group,
                         std::size_t from_index,
                         std::size_t target_group,
                         EditorSplitOrientation orientation,
                         bool insert_before);
  bool MoveTabToGroup(std::size_t from_group,
                      std::size_t from_index,
                      std::size_t to_group,
                      std::size_t to_slot);
  std::optional<std::size_t> FindIndexBySpecifier(std::string_view specifier,
                                                  std::string* error_message) const;
  bool ReopenActive();
  void RetryFailedOpen(const std::filesystem::path& path);

  static bool TabStateIsDirty(const TabEntry& tab);

 private:
  bool RestoreEditorTab(TabEntry::EditorTabState& editor_state);
  bool LoadRestoreView(const DeferredTabHandle& restore, editor::TextViewport& view,
                       std::uint64_t& read_id);
  bool EnsureEditorTabLoaded(TabEntry& tab);
  bool LoadEditorTabForActivation(TabEntry& tab);
  void HydrateGroupActiveTab(EditorGroup& group);
  std::filesystem::path LspCloseCandidatePath(const TabEntry& tab) const;
  void MaybeNotifyLspClose(const TabEntry& tab);
  void RememberClosedEditor(const TabEntry& tab);
  void CancelPendingAsyncRead(const TabEntry& tab);
  void CollapseFocusedGroup();
  void CollapseGroupAt(std::size_t gi);
  std::size_t AdjacentGroupInDirection(EditorGroupDirection direction) const;
  bool RelocateEditorGroup(std::size_t from,
                           std::size_t target,
                           EditorSplitOrientation orientation,
                           bool before);
  void RefreshFocusedGroupActiveTab();
  void EnsureEveryGroupActiveTabVisible();
  static TabEntry CloneEditorTabForSplit(const TabEntry& tab);

  ProjectCatalogState& project_catalog_;
  ProjectWorkspaceState& state_;
  project::FileWriteService& file_writes_;
  Operations operations_;
  std::vector<std::string> landed_save_failures_;
};

}
