#include "workspace/coordinators/WorkspaceTabCoordinator.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "workspace/WorkspaceLayout.h"

// The editor-GROUP half of the tab coordinator: everything that creates, feeds,
// empties or collapses an editor group, plus the two ways a tab crosses from one
// group into another (a cross-group drop and a drag-to-split). Split out of
// WorkspaceTabCoordinator.cpp to stay under the coordinator TU cap; the seam is
// one the file already had -- nothing above it knows there is more than one
// group.

namespace microide::workspace {

namespace {

// Take tab `index` out of `group` and re-home the group's active index across the
// hole, exactly as a close would.
TabEntry LiftTabFromGroup(EditorGroup& group, std::size_t index) {
  TabEntry lifted = std::move(group.open_tabs[index]);
  group.open_tabs.erase(group.open_tabs.begin() + static_cast<std::ptrdiff_t>(index));
  if (group.active_tab_index > index) {
    --group.active_tab_index;
  } else if (!group.open_tabs.empty() && group.active_tab_index >= group.open_tabs.size()) {
    group.active_tab_index = group.open_tabs.size() - 1;
  }
  return lifted;
}

}  // namespace

void TabCoordinator::HydrateGroupActiveTab(EditorGroup& group) {
  if (group.active_tab_index >= group.open_tabs.size()) {
    return;
  }
  TabEntry& tab = group.open_tabs[group.active_tab_index];
  if (tab.kind != TabEntry::Kind::Editor) {
    return;
  }
  (void)LoadEditorTabForActivation(tab);
}

bool TabCoordinator::MoveTabToGroup(std::size_t from_group,
                                    std::size_t from_index,
                                    std::size_t to_group,
                                    std::size_t to_slot) {
  if (from_group == to_group || from_group >= state_.editor_groups.size() ||
      to_group >= state_.editor_groups.size()) {
    return false;
  }
  if (from_index >= state_.editor_groups[from_group].open_tabs.size()) {
    return false;
  }
  if (state_.editor_groups[to_group].open_tabs.size() >= kMaxOpenTabsPerGroup) {
    return false;
  }

  // Flush live caret/scroll into the tab before it leaves: the metadata sync is
  // scoped to the focused group's active tab, and after the move this tab is
  // neither of those in the group it came from.
  if (from_group == state_.focused_group_index &&
      from_index == state_.editor_groups[from_group].active_tab_index) {
    SyncActiveEditorTabMetadata();
  }

  TabEntry moved = LiftTabFromGroup(state_.editor_groups[from_group], from_index);

  std::size_t landed_index = 0;
  {
    EditorGroup& to = state_.editor_groups[to_group];
    landed_index = std::min(to_slot, to.open_tabs.size());
    to.open_tabs.insert(to.open_tabs.begin() + static_cast<std::ptrdiff_t>(landed_index),
                        std::move(moved));
    to.active_tab_index = landed_index;
  }

  // Set focus BEFORE the collapse: CollapseGroupAt re-homes `focused_group_index`
  // across the erase, so writing the destination first is what keeps focus on the
  // moved tab when the source group was to its left and disappears.
  state_.focused_group_index = to_group;
  state_.surface.focus = FocusTarget::Editor;
  if (state_.editor_groups[from_group].open_tabs.empty()) {
    // VS Code drops a split whose last editor is dragged out.
    CollapseGroupAt(from_group);
  } else {
    // The tab that just left may have been the source group's ACTIVE one, which
    // promotes a neighbour there. A never-activated (session-restored) neighbour
    // is still deferred, and an unhydrated active tab resolves to the group's
    // welcome surface -- so the half you dragged out of went blank and showed the
    // Welcome screen instead of the tab its strip was highlighting.
    HydrateGroupActiveTab(state_.editor_groups[from_group]);
  }

  // A tab that was never activated in its old group can still be deferred; it is
  // the destination's active tab now, so it has to hydrate like any activation.
  if (EditorGroup& landed = state_.editor_groups[state_.clamped_focused_group_index()];
      landed_index < landed.open_tabs.size()) {
    (void)LoadEditorTabForActivation(landed.open_tabs[landed_index]);
  }
  // Both strips changed length, and the per-group geometry cache keys only on
  // (tab_count, window_width) — without this drop the destination can render the
  // source's cached widths whenever the two happen to agree.
  operations_.invalidate_tab_strip_geometry();
  RefreshFocusedGroupActiveTab(true);
  return true;
}

bool TabCoordinator::MoveTabToNewGroup(std::size_t from_group,
                                       std::size_t from_index,
                                       EditorSplitOrientation orientation,
                                       bool insert_before) {
  if (orientation == EditorSplitOrientation::None ||
      state_.editor_groups.size() >= kMaxEditorGroups ||
      from_group >= state_.editor_groups.size() ||
      from_index >= state_.editor_groups[from_group].open_tabs.size()) {
    return false;
  }
  // A group with one tab has nothing to give: the carved group would BE the old
  // one under a new index, and the emptied source would collapse straight back.
  if (state_.editor_groups[from_group].open_tabs.size() < 2) {
    return false;
  }
  // Flush live caret/scroll before the tab leaves, for the same reason the
  // cross-group move does: the metadata sync only ever writes the focused group's
  // active tab, which this tab stops being.
  if (from_group == state_.focused_group_index &&
      from_index == state_.editor_groups[from_group].active_tab_index) {
    SyncActiveEditorTabMetadata();
  }

  EditorGroup carved;
  carved.open_tabs.push_back(LiftTabFromGroup(state_.editor_groups[from_group], from_index));
  carved.active_tab_index = 0;

  // A drop on the left/top edge puts the carved group AHEAD of the one it came
  // from, so the tab lands under the pointer rather than jumping to the far side.
  const std::size_t carved_index = insert_before ? from_group : from_group + 1;
  state_.editor_groups.insert(
      state_.editor_groups.begin() + static_cast<std::ptrdiff_t>(carved_index), std::move(carved));
  const std::size_t source_index = insert_before ? from_group + 1 : from_group;
  state_.group_split_orientation = orientation;
  state_.group_split_fraction = 0.5f;
  state_.focused_group_index = carved_index;
  state_.surface.focus = FocusTarget::Editor;

  HydrateGroupActiveTab(state_.editor_groups[carved_index]);
  HydrateGroupActiveTab(state_.editor_groups[source_index]);
  operations_.invalidate_tab_strip_geometry();
  RefreshFocusedGroupActiveTab(true);
  return true;
}

TabEntry TabCoordinator::CloneEditorTabForSplit(const TabEntry& tab) {
  TabEntry clone;
  clone.kind = TabEntry::Kind::Editor;
  clone.path = tab.path;
  clone.title = tab.title;
  if (tab.editor_state.has_value()) {
    TabEntry::EditorTabState editor_state;
    // Copying the viewport shares the underlying DocumentState (live shared
    // buffer) while keeping an independent scroll/cursor/selection. The folding
    // model is non-copyable and view-local, so the clone gets a fresh one.
    editor_state.viewport = tab.editor_state->viewport;
    editor_state.restored_path = tab.editor_state->restored_path;
    editor_state.restored_cursor_line = tab.editor_state->restored_cursor_line;
    editor_state.restored_cursor_column = tab.editor_state->restored_cursor_column;
    editor_state.restored_scroll_line = tab.editor_state->restored_scroll_line;
    editor_state.restored_horizontal_scroll = tab.editor_state->restored_horizontal_scroll;
    editor_state.needs_restore = tab.editor_state->needs_restore;
    editor_state.snippet_session = tab.editor_state->snippet_session;
    clone.editor_state = std::move(editor_state);
  } else if (tab.deferred_handle.has_value()) {
    clone.deferred_handle = tab.deferred_handle;
  }
  return clone;
}

bool TabCoordinator::SplitEditorGroup(EditorSplitOrientation orientation) {
  if (orientation == EditorSplitOrientation::None || state_.editor_groups.empty()) {
    return false;
  }

  // With two groups already open, just retarget the divider orientation and move
  // focus to the other group (capped at two groups).
  if (state_.editor_groups.size() >= 2) {
    state_.group_split_orientation = orientation;
    state_.focused_group_index = state_.focused_group_index == 0 ? 1 : 0;
    state_.surface.focus = FocusTarget::Editor;
    RefreshFocusedGroupActiveTab(true);
    return true;
  }

  EditorGroup& source = state_.focused_group();
  if (source.active_tab_index >= source.open_tabs.size()) {
    return false;
  }
  const TabEntry& active = source.open_tabs[source.active_tab_index];
  if (active.kind != TabEntry::Kind::Editor || !active.editor_state.has_value()) {
    return false;
  }
  // Capture fresh scroll/cursor into the source tab before cloning so the new
  // group starts at the same view position.
  SyncActiveEditorTabMetadata();

  EditorGroup new_group;
  new_group.open_tabs.push_back(CloneEditorTabForSplit(active));
  new_group.active_tab_index = 0;
  state_.editor_groups.push_back(std::move(new_group));
  state_.group_split_orientation = orientation;
  state_.group_split_fraction = 0.5f;
  state_.focused_group_index = state_.editor_groups.size() - 1;
  state_.surface.focus = FocusTarget::Editor;
  RefreshFocusedGroupActiveTab(true);
  return true;
}

bool TabCoordinator::FocusOtherGroup() {
  if (state_.editor_groups.size() < 2) {
    return false;
  }
  state_.focused_group_index = state_.focused_group_index == 0 ? 1 : 0;
  state_.surface.focus = FocusTarget::Editor;
  RefreshFocusedGroupActiveTab(true);
  return true;
}

void TabCoordinator::RefreshFocusedGroupActiveTab(bool editor_redraw) {
  operations_.ensure_active_tab_visible();
  operations_.request_active_tab_redraw(editor_redraw);
}

void TabCoordinator::CloseGroupTab(std::size_t group_index, std::size_t index) {
  if (group_index == state_.focused_group_index) {
    Close(index);
    return;
  }
  if (group_index >= state_.editor_groups.size()) {
    return;
  }
  EditorGroup& group = state_.editor_groups[group_index];
  if (index >= group.open_tabs.size()) {
    return;
  }
  // Same last-view LSP-didClose accounting as Close() (count includes the tab being
  // closed, so ==1 means this is the final view).
  MaybeNotifyLspClose(group.open_tabs[index]);
  const bool closing_active = index == group.active_tab_index;
  group.open_tabs.erase(group.open_tabs.begin() + static_cast<std::ptrdiff_t>(index));
  if (group.active_tab_index > index) {
    --group.active_tab_index;
  } else if (!group.open_tabs.empty() && group.active_tab_index >= group.open_tabs.size()) {
    group.active_tab_index = group.open_tabs.size() - 1;
  }
  if (group.open_tabs.empty()) {
    CollapseGroupAt(group_index);
  } else {
    if (closing_active) {
      HydrateGroupActiveTab(group);
    }
    operations_.invalidate_tab_strip_geometry();
  }
}

void TabCoordinator::CollapseGroupAt(std::size_t gi) {
  if (gi >= state_.editor_groups.size()) {
    return;
  }
  state_.editor_groups.erase(state_.editor_groups.begin() + static_cast<std::ptrdiff_t>(gi));
  if (state_.editor_groups.empty()) {
    state_.editor_groups.emplace_back();
  }
  // Re-home the focused index across the erase: unchanged if it preceded gi, decremented
  // if it followed, and clamped into range if it was gi itself or now past the end.
  if (state_.focused_group_index > gi) {
    --state_.focused_group_index;
  }
  if (state_.focused_group_index >= state_.editor_groups.size()) {
    state_.focused_group_index = state_.editor_groups.size() - 1;
  }
  if (state_.editor_groups.size() < 2) {
    state_.group_split_orientation = EditorSplitOrientation::None;
    state_.group_split_fraction = 0.5f;
  }
  operations_.invalidate_tab_strip_geometry();
}

void TabCoordinator::CollapseFocusedGroup() {
  const std::size_t removed =
      state_.focused_group_index < state_.editor_groups.size() ? state_.focused_group_index : 0;
  state_.editor_groups.erase(state_.editor_groups.begin() +
                             static_cast<std::ptrdiff_t>(removed));
  if (state_.editor_groups.empty()) {
    state_.editor_groups.emplace_back();
  }
  state_.focused_group_index = 0;
  state_.group_split_orientation = EditorSplitOrientation::None;
  state_.group_split_fraction = 0.5f;
  state_.surface.focus = FocusTarget::Editor;
  // Erasing a group reindexes the survivors; the per-group tab-strip geometry
  // cache keys only on (tab_count, window_width) and is indexed by group slot, so
  // without this drop the survivor could render the destroyed group's cached tab
  // titles/widths whenever their tab_count and the window width happen to match.
  operations_.invalidate_tab_strip_geometry();
}

bool TabCoordinator::CloseEditorGroup() {
  if (state_.editor_groups.size() < 2) {
    return false;
  }
  // Releasing this group's tabs may drop the last open view of a shared buffer;
  // fire LSP didClose for any path now unreferenced by the surviving group.
  // Count views once up front so this is O(views), not O(tabs * views).
  const std::unordered_map<std::string, std::size_t> view_counts =
      operations_.open_buffer_view_counts ? operations_.open_buffer_view_counts()
                                          : std::unordered_map<std::string, std::size_t>{};
  const EditorGroup& closing = state_.focused_group();
  // Count THIS group's own views per path first. A buffer can be open in more than
  // one tab of the closing group (e.g. an editor tab plus a compare/merge tab whose
  // editable side is the same path), so the old `global count == 1` test never
  // fired for it and leaked the didClose + stored diagnostics. Fire once per path
  // whose ENTIRE view count lives in this group, i.e. closing the group drops the
  // last open view.
  std::unordered_map<std::string, std::size_t> closing_counts;
  for (const TabEntry& tab : closing.open_tabs) {
    const std::filesystem::path path = LspCloseCandidatePath(tab);
    if (!path.empty()) {
      ++closing_counts[path.generic_string()];
    }
  }
  for (const auto& [path_key, closing_count] : closing_counts) {
    const auto it = view_counts.find(path_key);
    const std::size_t total_views = it != view_counts.end() ? it->second : closing_count;
    if (total_views <= closing_count) {
      operations_.notify_lsp_buffer_close(std::filesystem::path(path_key));
    }
  }
  CollapseFocusedGroup();
  const editor::TextViewport* active_vp = ActiveEditorViewport();
  RefreshFocusedGroupActiveTab(active_vp != nullptr && !active_vp->path().empty());
  return true;
}

}  // namespace microide::workspace
