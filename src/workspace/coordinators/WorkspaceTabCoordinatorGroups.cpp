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
  // (tab_count, strip_width) — without this drop the destination can render the
  // source's cached widths whenever the two happen to agree.
  operations_.invalidate_tab_strip_geometry();
  EnsureEveryGroupActiveTabVisible();
  RefreshFocusedGroupActiveTab(true);
  return true;
}

bool TabCoordinator::MoveTabToNewGroup(std::size_t from_group,
                                       std::size_t from_index,
                                       std::size_t target_group,
                                       EditorSplitOrientation orientation,
                                       bool insert_before) {
  if (orientation == EditorSplitOrientation::None ||
      from_group >= state_.editor_groups.size() || target_group >= state_.editor_groups.size() ||
      from_index >= state_.editor_groups[from_group].open_tabs.size()) {
    return false;
  }
  // Splitting a pane with the pane's own only tab has nothing to give: the carved
  // group would BE the old one under a new index, and the emptied source would
  // collapse straight back. Dropping the last tab of ANOTHER pane onto this one is
  // fine -- that pane goes away and this one gains a neighbour.
  if (from_group == target_group && state_.editor_groups[from_group].open_tabs.size() < 2) {
    return false;
  }
  // That same case is the one edge drop a FULL grid can still take: the source
  // pane collapses as the carved one appears, so the pane COUNT is unchanged and
  // this is a move of a pane rather than an addition (TD-2026-08-18-265). Every
  // other carve adds a pane and is refused at the cap.
  const bool pane_count_preserving =
      state_.editor_groups[from_group].open_tabs.size() == 1;
  if (state_.editor_split.full() && !pane_count_preserving) {
    return false;
  }
  // Flush live caret/scroll before the tab leaves, for the same reason the
  // cross-group move does: the metadata sync only ever writes the focused group's
  // active tab, which this tab stops being.
  if (from_group == state_.focused_group_index &&
      from_index == state_.editor_groups[from_group].active_tab_index) {
    SyncActiveEditorTabMetadata();
  }

  if (pane_count_preserving) {
    // The pane is RELOCATED, not carved: `MoveLeaf` removes before it inserts, so
    // the grid never has to hold a ninth leaf and a full grid can take this drop.
    return RelocateEditorGroup(from_group, target_group, orientation, insert_before);
  }

  const std::size_t carved_index =
      state_.editor_split.InsertLeaf(target_group, orientation, insert_before);
  if (carved_index == EditorSplitTree::kNoLeaf) {
    return false;
  }

  EditorGroup carved;
  carved.open_tabs.push_back(LiftTabFromGroup(state_.editor_groups[from_group], from_index));
  carved.active_tab_index = 0;
  state_.editor_groups.insert(
      state_.editor_groups.begin() + static_cast<std::ptrdiff_t>(carved_index), std::move(carved));
  // Every group at or past the insertion point shifted right. The source kept at
  // least one tab (the single-tab case took the relocation path above), so it
  // never collapses here.
  const std::size_t source_index = from_group >= carved_index ? from_group + 1 : from_group;
  state_.focused_group_index = carved_index;
  state_.surface.focus = FocusTarget::Editor;
  HydrateGroupActiveTab(state_.editor_groups[source_index]);
  HydrateGroupActiveTab(state_.editor_groups[state_.clamped_focused_group_index()]);
  operations_.invalidate_tab_strip_geometry();
  EnsureEveryGroupActiveTabVisible();
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
  if (orientation == EditorSplitOrientation::None || state_.editor_groups.empty() ||
      state_.editor_split.full()) {
    return false;
  }

  const std::size_t focused = state_.clamped_focused_group_index();
  const EditorGroup& source = state_.editor_groups[focused];
  if (source.active_tab_index >= source.open_tabs.size()) {
    return false;
  }
  if (const TabEntry& active = source.open_tabs[source.active_tab_index];
      active.kind != TabEntry::Kind::Editor || !active.editor_state.has_value()) {
    return false;
  }
  // Capture fresh scroll/cursor into the source tab before cloning so the new
  // group starts at the same view position.
  SyncActiveEditorTabMetadata();

  EditorGroup new_group;
  new_group.open_tabs.push_back(CloneEditorTabForSplit(source.open_tabs[source.active_tab_index]));
  new_group.active_tab_index = 0;

  // VS Code's "Split Right"/"Split Down" put the new pane after the active one.
  const std::size_t carved_index = state_.editor_split.InsertLeaf(focused, orientation, false);
  if (carved_index == EditorSplitTree::kNoLeaf) {
    return false;
  }
  state_.editor_groups.insert(
      state_.editor_groups.begin() + static_cast<std::ptrdiff_t>(carved_index),
      std::move(new_group));
  state_.focused_group_index = carved_index;
  state_.surface.focus = FocusTarget::Editor;
  operations_.invalidate_tab_strip_geometry();
  EnsureEveryGroupActiveTabVisible();
  RefreshFocusedGroupActiveTab(true);
  return true;
}

bool TabCoordinator::FocusOtherGroup() {
  if (state_.editor_groups.size() < 2) {
    return false;
  }
  // VS Code's "Focus Next Editor Group": walk the panes in layout order and wrap.
  state_.focused_group_index =
      (state_.clamped_focused_group_index() + 1) % state_.editor_groups.size();
  state_.surface.focus = FocusTarget::Editor;
  RefreshFocusedGroupActiveTab(true);
  return true;
}

std::size_t TabCoordinator::AdjacentGroupInDirection(EditorGroupDirection direction) const {
  if (state_.editor_groups.size() < 2 || !operations_.editor_group_rects) {
    return kNoEditorGroup;
  }
  const EditorGroupRectsLayout rects = operations_.editor_group_rects();
  // The rects are a view of the same tree the groups are leaves of; a host with
  // no window yet hands back nothing, and there is no geometry to answer from.
  if (rects.groups.size() != state_.editor_groups.size()) {
    return kNoEditorGroup;
  }
  return AdjacentEditorGroup(rects, state_.clamped_focused_group_index(), direction);
}

bool TabCoordinator::FocusEditorGroupInDirection(EditorGroupDirection direction) {
  const std::size_t target = AdjacentGroupInDirection(direction);
  if (target == kNoEditorGroup) {
    return false;
  }
  state_.focused_group_index = target;
  state_.surface.focus = FocusTarget::Editor;
  RefreshFocusedGroupActiveTab(true);
  return true;
}

bool TabCoordinator::MoveEditorGroupInDirection(EditorGroupDirection direction) {
  const std::size_t focused = state_.clamped_focused_group_index();
  const std::size_t target = AdjacentGroupInDirection(direction);
  if (target == kNoEditorGroup) {
    return false;
  }
  const bool horizontal =
      direction == EditorGroupDirection::Left || direction == EditorGroupDirection::Right;
  // Land on the far side of the neighbour, so moving left through a row of three
  // walks the pane to the front rather than swapping it back and forth.
  const bool before =
      direction == EditorGroupDirection::Left || direction == EditorGroupDirection::Up;
  return RelocateEditorGroup(
      focused, target,
      horizontal ? EditorSplitOrientation::Vertical : EditorSplitOrientation::Horizontal, before);
}

// The one place a pane changes position. Both callers -- the directional move
// above and the count-preserving edge drop in `MoveTabToNewGroup` -- have to keep
// `editor_groups` in step with the tree's leaves and drop the same three caches;
// they were byte-identical when written separately, which is how the group-indexed
// caches drift apart.
bool TabCoordinator::RelocateEditorGroup(std::size_t from,
                                         std::size_t target,
                                         EditorSplitOrientation orientation,
                                         bool before) {
  const std::size_t landed = state_.editor_split.MoveLeaf(from, target, orientation, before);
  if (landed == EditorSplitTree::kNoLeaf) {
    return false;
  }
  // The tree's leaves ARE the groups, in order: the vector has to make the same
  // move or every group-indexed cache reads the wrong pane.
  EditorGroup moved = std::move(state_.editor_groups[from]);
  state_.editor_groups.erase(state_.editor_groups.begin() + static_cast<std::ptrdiff_t>(from));
  state_.editor_groups.insert(state_.editor_groups.begin() + static_cast<std::ptrdiff_t>(landed),
                              std::move(moved));
  state_.focused_group_index = landed;
  state_.surface.focus = FocusTarget::Editor;
  HydrateGroupActiveTab(state_.editor_groups[landed]);
  operations_.invalidate_tab_strip_geometry();
  EnsureEveryGroupActiveTabVisible();
  RefreshFocusedGroupActiveTab(true);
  return true;
}

// Called wherever a group is added, removed or reindexed: every pane's strip
// changes width at that moment, so revealing only the focused pane's active tab
// leaves the others scrolled wherever the previous, wider strip had put them.
void TabCoordinator::EnsureEveryGroupActiveTabVisible() {
  if (operations_.ensure_all_active_tabs_visible) {
    operations_.ensure_all_active_tabs_visible();
  } else if (operations_.ensure_active_tab_visible) {
    operations_.ensure_active_tab_visible();
  }
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
    EnsureEveryGroupActiveTabVisible();
  }
}

void TabCoordinator::CollapseGroupAt(std::size_t gi) {
  if (gi >= state_.editor_groups.size()) {
    return;
  }
  state_.editor_groups.erase(state_.editor_groups.begin() + static_cast<std::ptrdiff_t>(gi));
  // The tree's leaves ARE the groups, in order: dropping the pane hands its room
  // back to its siblings and collapses any branch left with one child.
  state_.editor_split.RemoveLeaf(gi);
  if (state_.editor_groups.empty()) {
    state_.editor_groups.emplace_back();
    state_.editor_split.Reset();
  }
  // Re-home the focused index across the erase: unchanged if it preceded gi, decremented
  // if it followed, and clamped into range if it was gi itself or now past the end.
  if (state_.focused_group_index > gi) {
    --state_.focused_group_index;
  }
  if (state_.focused_group_index >= state_.editor_groups.size()) {
    state_.focused_group_index = state_.editor_groups.size() - 1;
  }
  // Erasing a group reindexes the survivors; the per-group tab-strip geometry
  // cache keys only on (tab_count, strip_width) and is indexed by group slot, so
  // without this drop a survivor could render the destroyed group's cached tab
  // titles/widths whenever their tab_count and the window width happen to match.
  operations_.invalidate_tab_strip_geometry();
  EnsureEveryGroupActiveTabVisible();
}

void TabCoordinator::CollapseFocusedGroup() {
  CollapseGroupAt(state_.clamped_focused_group_index());
  state_.surface.focus = FocusTarget::Editor;
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
