#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "workspace/WorkspaceLayout.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

class TabCoordinator {
 public:
  struct Operations {
    std::function<void(const std::filesystem::path&)> invalidate_editor_blame_path;
    std::function<void(const std::filesystem::path&)> notify_plugin_buffer_save;
    std::function<void(const std::filesystem::path&)> notify_plugin_buffer_open;
    // Schedule (do NOT synchronously run) LSP hydration for the newly-active editor
    // document. Activation must stay non-blocking: the host records the path and runs
    // the didOpen + semantic-token/inlay-hint requests after the tab-switch frame is
    // presented (WorkspaceShell::ConsumeDeferredLspBufferOpen), so a large file's
    // hydration never delays the tab becoming visible. Enforced by the architecture
    // lint (CheckLspDidOpenIsNonBlocking), which forbids the synchronous forms in
    // Activate.
    std::function<void(const std::filesystem::path&)> schedule_lsp_buffer_open;
    std::function<void(const std::filesystem::path&)> notify_lsp_buffer_close;
    // Raised after an on-disk reload replaces an already-open buffer's content, so
    // the LSP server's document mirror is re-synced with a full didChange. The
    // document is never closed across a reload, so didOpen short-circuits
    // (HasOpenDocument) and the server would otherwise keep the PRE-reload text —
    // desyncing diagnostics/hover/completion and corrupting the next incremental
    // edit (its range is computed against the reloaded buffer but applied to the
    // stale mirror). The reloaded viewport already holds the after-content, so the
    // full didChange streams from it; only the pre-reload line count and the first
    // differing line are passed (computed while both buffers exist), avoiding two
    // whole-document snapshots on large external reloads (TD-2026-07-17A-015).
    // Args: (reloaded viewport, before-line-count, first-changed-line).
    std::function<void(const editor::TextViewport&, std::size_t, std::size_t)>
        notify_lsp_buffer_reloaded;
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
    // Every pane's strip, not just the focused one. Adding, removing or
    // reindexing a group resizes EVERY pane's strip (two half-width strips where
    // there was one full-width one), so a pane nobody touched can end up with its
    // active tab scrolled out of view.
    std::function<void()> ensure_all_active_tabs_visible;
    std::function<void()> reset_caret_blink;
    std::function<void(bool)> request_active_tab_redraw;
    std::function<void()> request_tab_strip_redraw;
    // Drop TabStripService's per-group label/width geometry cache. Required when a
    // group is added/removed/reindexed, since that cache keys only on
    // (tab_count, strip_width) and is indexed by group slot — a collapse that
    // shifts a group into another slot with the same tab_count/width would
    // otherwise render the destroyed group's cached titles for the survivor.
    std::function<void()> invalidate_tab_strip_geometry;
    std::function<void()> request_editor_surface_redraw;
    // Every pane's rect, in group order. Directional pane focus/move answer
    // "which pane is to my left" from the geometry that is actually on screen
    // rather than from the tree, which is what makes the answer match what the
    // user is looking at. Empty when the host has no window yet.
    std::function<EditorGroupRectsLayout()> editor_group_rects;
    std::function<void()> request_automatic_git_sidebar_refresh;
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
  // Group-aware save primitive: saves editor_groups[group_index].open_tabs[index]
  // (Editor/Compare/Merge) with the same disk-conflict guard and plugin-save notify
  // as Save(). Bounds-checks both indices. Save() delegates here with the clamped
  // focused group; the all-groups flush paths (autosave, save-on-quit) call it
  // directly so a buffer dirtied in the non-focused split group is not skipped.
  bool SaveGroupTab(std::size_t group_index, std::size_t index);
  // Save As / naming an untitled buffer: rebinds the editor tab at `index` to
  // `path` (refused when another file already sits there) and saves it. On
  // failure `error` says why.
  bool SaveGroupTabAs(std::size_t group_index, std::size_t index,
                      const std::filesystem::path& path, std::string* error);
  bool IsDirty(std::size_t index) const;
  std::vector<std::size_t> DirtyIndices() const;
  std::vector<std::size_t> DirtyIndicesForProject(std::size_t project_index) const;
  // Allocation-free "is anything dirty in this project", for callers that only
  // need the answer and not the indices — the tab strip asks it once per project
  // tab per painted frame.
  bool HasDirtyTabForProject(std::size_t project_index) const;
  // The same predicate, without a coordinator. Building one costs ~40
  // std::function constructions (MakeTabCoordinator), and the tab-strip cache
  // key needs this bit for every project on every frame — a call it must be
  // able to make without paying for a shell coordinator per project per frame.
  static bool ProjectHasDirtyTab(const ProjectCatalogState& catalog,
                                 const ProjectWorkspaceState& current_project,
                                 std::size_t project_index);
  // Dirty tabs across ALL editor groups of the active project.
  std::vector<GroupTabRef> DirtyGroupTabs() const;
  // Dirty tabs across ALL editor groups of any catalog project (active project
  // delegates to DirtyGroupTabs(); inactive walks the catalog entry's groups
  // directly, mirroring DirtyIndicesForProject).
  std::vector<GroupTabRef> DirtyGroupTabsForProject(std::size_t project_index) const;
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
  // Shared reload core for both of the above. Reloads every editor view on `path`
  // across ALL editor groups (not just the focused one) so a split view of the same
  // file in the non-focused group is refreshed too. `clean_only` skips dirty views
  // (the clean-reload path); false reloads unconditionally (the from-disk overwrite).
  void ReloadEditorTabsForPath(const std::filesystem::path& path, bool clean_only);
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
  // The live editor view of `normalized_path` in ANY group, or nullptr. A file is
  // one buffer however many panes show it (VS Code's model per resource, and the
  // contract the plain split's clone already keeps): a second tab on an open file
  // copies this view, sharing its DocumentState, instead of reading disk again —
  // otherwise an edit in one pane is invisible in the other and the two panes
  // save over each other.
  const editor::TextViewport* FindOpenEditorViewOfPath(
      const std::filesystem::path& normalized_path) const;
  // Fills `view` with a view of `path`: a copy of the live view when the file is
  // open anywhere (shared buffer, preferences already applied), else a fresh read
  // from disk with the editor preferences and indent detection applied. False
  // when the read fails.
  bool OpenEditorViewForPath(const std::filesystem::path& path, editor::TextViewport& view) const;
  bool OpenFileInNewTab(const std::filesystem::path& path);
  // A path that does not exist yet opens as an empty buffer bound to it, as
  // `code new.txt` does; the file (and its directories) appear on save. An
  // existing path opens normally.
  bool OpenNewBufferInNewTab(const std::filesystem::path& path);
  bool OpenVirtualDocumentInNewTab(const std::filesystem::path& virtual_path,
                                   std::string_view content,
                                   std::string_view title);
  void ReloadVirtualDocumentTabs(const std::filesystem::path& virtual_path,
                                 std::string_view content);
  void Close(std::size_t index);
  // Group-aware close: closes editor_groups[group_index].open_tabs[index] with the same
  // LSP-didClose accounting as Close(); delegates to Close() for the focused group, and
  // for a background group erases the tab, clamps its active index, and collapses the
  // group if it empties. Used by rename/delete propagation so a split view of the
  // affected file in a non-focused group is not stranded on a defunct path.
  void CloseGroupTab(std::size_t group_index, std::size_t index);
  // Splitting clones the focused group's active editor tab into a NEW pane
  // (shared buffer, independent view) beside it and focuses that pane -- every
  // time, up to `kMaxEditorGroups`. Returns false when there is no active editor
  // tab to clone, or the editor area is already full.
  bool SplitEditorGroup(EditorSplitOrientation orientation);
  bool FocusOtherGroup();
  // VS Code's focusLeft/Right/Above/BelowGroup and moveActiveEditorGroupLeft/...
  // Both resolve the neighbour with `AdjacentEditorGroup`; the move then relocates
  // the pane beside it (`EditorSplitTree::MoveLeaf`), keeping the group vector in
  // step. Return false when no pane lies that way.
  bool FocusEditorGroupInDirection(EditorGroupDirection direction);
  bool MoveEditorGroupInDirection(EditorGroupDirection direction);
  bool CloseEditorGroup();
  std::size_t EditorGroupCount() const { return state_.editor_groups.size(); }
  bool MoveActiveTo(std::size_t index);
  // Move one tab out of `from_group` and into `to_group` at insertion slot
  // `to_slot`, focusing the destination and activating the moved tab there. This
  // is what a tab dragged across a split commits (TD-2026-08-14-213), and what
  // "move editor into other group" would bind to.
  //
  // Not a reorder with a group argument: the tab keeps its identity and its
  // buffer (so no LSP didClose — the view count is unchanged), the source group
  // collapses if this emptied it, and `to_slot` is a raw insertion point in the
  // destination list rather than a post-removal target index. Returns false when
  // either index is out of range, the groups are the same, or the destination is
  // at its per-group tab cap.
  // Carve a NEW editor group holding just tab `from_index` of `from_group`, and
  // split `target_group`'s pane to make room for it -- the model behind VS Code's
  // drag-a-tab-onto-a-pane-edge split. `insert_before` puts the new pane on the
  // leading side of the target (a drop on its left/top edge), otherwise the
  // trailing side; the target may be any pane, including one that is already half
  // of a split. Refuses when the group cap is reached, or when the source and
  // target are the same pane and it holds a single tab -- both no-ops the caller
  // should have rendered as "no drop here".
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

  static bool TabStateIsDirty(const TabEntry& tab);

 private:
  bool RestoreEditorTab(TabEntry::EditorTabState& editor_state);
  bool EnsureEditorTabLoaded(TabEntry& tab);
  // Load an editor tab for activation/promotion: hydrates an already-loaded tab,
  // or opens a deferred/fresh one applying preferences, detected indent, and (for a
  // deferred tab) the restored cursor/scroll/selection. Shared by Activate() and
  // Close()'s promote path so both honor deferred view state identically.
  bool LoadEditorTabForActivation(TabEntry& tab);
  // Hydrate whatever tab is now active in `group`. Only a group's active tab is
  // eagerly loaded on session restore, so any operation that PROMOTES a new tab
  // to active in a group -- closing the active one, dragging it out into another
  // group -- has to run the promoted neighbour through the loader or that group's
  // pane renders the welcome surface instead of the buffer its own strip is
  // highlighting. Shared by every such site rather than re-spelled per call,
  // because the two that spelled it inline disagreed: MoveTabToGroup was missing
  // it entirely.
  void HydrateGroupActiveTab(EditorGroup& group);
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
  // Erase group `gi` (a non-focused split group that just emptied), re-home the focused
  // index across the shift, and drop the split orientation when one group remains.
  void CollapseGroupAt(std::size_t gi);
  // Group index of the pane next to the focused one in `direction`, or
  // `kNoEditorGroup`.
  std::size_t AdjacentGroupInDirection(EditorGroupDirection direction) const;
  // Move pane `from` next to pane `target`, keeping `editor_groups` in step with
  // the tree's leaves. Never changes the pane count, so it is legal at the cap.
  bool RelocateEditorGroup(std::size_t from,
                           std::size_t target,
                           EditorSplitOrientation orientation,
                           bool before);
  // Shared tail for group split/focus/close: scroll the (new) focused group's
  // active tab into view and request the matching redraw.
  void RefreshFocusedGroupActiveTab(bool editor_redraw);
  void EnsureEveryGroupActiveTabVisible();
  // Clone an editor tab for a split: copies the viewport (sharing the underlying
  // DocumentState for a live shared buffer) with a fresh folding model.
  static TabEntry CloneEditorTabForSplit(const TabEntry& tab);

  ProjectCatalogState& project_catalog_;
  ProjectWorkspaceState& state_;
  Operations operations_;
};

}  // namespace microide::workspace
