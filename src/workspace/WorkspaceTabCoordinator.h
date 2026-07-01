#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "workspace/WorkspaceProjectState.h"

namespace microide::workspace {

class TabCoordinator {
 public:
  struct Operations {
    std::function<void(const std::filesystem::path&)> invalidate_editor_blame_path;
    std::function<void(const std::filesystem::path&)> notify_plugin_buffer_save;
    std::function<void(const std::filesystem::path&)> notify_plugin_buffer_open;
    std::function<void(const std::filesystem::path&)> notify_lsp_buffer_close;
    std::function<std::size_t(const std::filesystem::path&)> count_open_buffer_views;
    // Whole-workspace open-view counts keyed by normalized generic path, built
    // once so closing a multi-tab group is O(views) rather than O(tabs*views).
    std::function<std::unordered_map<std::string, std::size_t>()> open_buffer_view_counts;
    std::function<bool(const std::filesystem::path&, editor::TextViewport&, std::string*)>
        prepare_editor_view_for_save;
    std::function<void(editor::TextViewport&)> apply_editor_preferences;
    std::function<void(editor::TextViewport&)> apply_detected_indent_on_open;
    std::function<TabEntry::EditorTabState(const editor::TextViewport&)> make_editor_tab_state;
    std::function<std::filesystem::path(const TabEntry::EditorTabState&)> editor_view_path;
    std::function<void()> reveal_selected_tree_sidebar_line;
    std::function<void()> reveal_active_compare_selection;
    std::function<void()> reveal_active_merge_selection;
    std::function<void()> ensure_active_tab_visible;
    std::function<void()> reset_caret_blink;
    std::function<void(bool)> request_active_tab_redraw;
    std::function<void()> request_tab_strip_redraw;
    std::function<void()> request_editor_surface_redraw;
    std::function<void()> request_automatic_git_sidebar_refresh;
    std::function<void(std::size_t)> activate_tab;
    // Raised when a save is refused because the file changed on disk since the
    // buffer was loaded/last saved. The host surfaces the external-change banner
    // so the user can Reload / Overwrite / Keep instead of silently clobbering.
    std::function<void(const std::filesystem::path&)> request_external_change_banner;
    // Raised when a write actually fails (permission/disk error), as opposed to a
    // refused-overwrite (which raises request_external_change_banner). The host
    // posts an error toast so the failure is never silently swallowed.
    std::function<void(const std::filesystem::path&)> notify_save_failed;
  };

  TabCoordinator(ProjectCatalogState& project_catalog,
                 ProjectWorkspaceState& current_project_state,
                 Operations operations);

  std::string ActiveTitle() const;
  bool Save(std::size_t index);
  bool IsDirty(std::size_t index) const;
  std::vector<std::size_t> DirtyIndices() const;
  std::vector<std::size_t> DirtyIndicesForProject(std::size_t project_index) const;
  bool ActiveTabIsEditor() const;
  TabEntry::EditorTabState* ActiveEditorTab();
  const TabEntry::EditorTabState* ActiveEditorTab() const;
  editor::TextViewport* ActiveEditorViewport();
  const editor::TextViewport* ActiveEditorViewport() const;
  void Activate(std::size_t index);
  void SyncActiveEditorTab();
  bool ActivateCurrentTabAfterStateLoad();
  void SyncActiveEditorTabMetadata();
  void ReloadCleanEditorTabsForPath(const std::filesystem::path& path);
  void ReloadEditorTabsForPathFromDisk(const std::filesystem::path& path);
  // Force-saves every dirty editor view on `path`, bypassing the save-time
  // disk-conflict guard (the user explicitly chose to overwrite). Returns true
  // if at least one view was saved.
  bool OverwriteEditorTabsForPath(const std::filesystem::path& path);
  // True if every open editor view on `path` recorded a disk signature equal to
  // `signature` (i.e. the on-disk change was our own write). Used to suppress the
  // redundant self-write reload. False when no view matches the path.
  bool DiskSignatureMatchesOpenView(const std::filesystem::path& path,
                                    const util::FileSignature& signature) const;
  bool OpenUntitled();
  bool OpenFileInNewTab(const std::filesystem::path& path);
  bool OpenVirtualDocumentInNewTab(const std::filesystem::path& virtual_path,
                                   std::string_view content,
                                   std::string_view title);
  void ReloadVirtualDocumentTabs(const std::filesystem::path& virtual_path,
                                 std::string_view content);
  void Close(std::size_t index);
  // Editor groups (max 2). Splitting clones the focused group's active editor tab
  // into a new group (shared buffer, independent view) and focuses it; if two
  // groups already exist it just sets the orientation and focuses the other.
  // Returns false when there is no active editor tab to clone.
  bool SplitEditorGroup(EditorSplitOrientation orientation);
  bool FocusOtherGroup();
  bool CloseEditorGroup();
  std::size_t EditorGroupCount() const { return state_.editor_groups.size(); }
  bool MoveActiveTo(std::size_t index);
  std::optional<std::size_t> FindIndexBySpecifier(std::string_view specifier,
                                                  std::string* error_message) const;
  bool ReopenActive();

  static bool TabStateIsDirty(const TabEntry& tab);

 private:
  bool RestoreEditorTab(TabEntry::EditorTabState& editor_state);
  bool EnsureEditorTabLoaded(TabEntry& tab);
  // The normalized buffer path a tab contributes to LSP open-view accounting, or
  // empty if the tab holds no editable buffer view. Mirrors OpenBufferViewPath so
  // the live per-close count and the whole-group count map agree on keys/kinds.
  std::filesystem::path LspCloseCandidatePath(const TabEntry& tab) const;
  // Fire LSP didClose for the tab about to close when it is the last open view of
  // its buffer. Uses a live count (correct for sequential closes, where an earlier
  // close in the same batch drops a shared buffer to its last remaining view).
  void MaybeNotifyLspClose(const TabEntry& tab);
  // Remove the focused (expected-empty) group and collapse back to a single
  // full-area group, resetting split orientation/fraction.
  void CollapseFocusedGroup();
  // Shared tail for group split/focus/close: scroll the (new) focused group's
  // active tab into view and request the matching redraw.
  void RefreshFocusedGroupActiveTab(bool editor_redraw);
  // Clone an editor tab for a split: copies the viewport (sharing the underlying
  // DocumentState for a live shared buffer) with a fresh folding model.
  static TabEntry CloneEditorTabForSplit(const TabEntry& tab);

  ProjectCatalogState& project_catalog_;
  ProjectWorkspaceState& state_;
  Operations operations_;
};

}  // namespace microide::workspace
