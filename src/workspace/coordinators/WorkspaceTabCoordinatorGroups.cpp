#include "workspace/coordinators/WorkspaceTabCoordinator.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "workspace/WorkspaceLayout.h"
#include "util/InvokeIfSet.h"

namespace microide::workspace {

namespace {

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

}

void TabCoordinator::HydrateGroupActiveTab(EditorGroup& group) {
  if (group.active_tab_index >= group.open_tabs.size()) {
    return;
  }
  TabEntry& tab = group.active_tab();
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

  state_.focused_group_index = to_group;
  state_.surface.focus = FocusTarget::Editor;
  if (state_.editor_groups[from_group].open_tabs.empty()) {
    CollapseGroupAt(from_group);
  } else {
    HydrateGroupActiveTab(state_.editor_groups[from_group]);
  }

  if (EditorGroup& landed = state_.editor_groups[state_.clamped_focused_group_index()];
      landed_index < landed.open_tabs.size()) {
    (void)LoadEditorTabForActivation(landed.open_tabs[landed_index]);
  }
  operations_.invalidate_tab_strip_geometry();
  EnsureEveryGroupActiveTabVisible();
  RefreshFocusedGroupActiveTab();
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
  if (from_group == target_group && state_.editor_groups[from_group].open_tabs.size() < 2) {
    return false;
  }
  const bool pane_count_preserving =
      state_.editor_groups[from_group].open_tabs.size() == 1;
  if (state_.editor_split.full() && !pane_count_preserving) {
    return false;
  }
  if (from_group == state_.focused_group_index &&
      from_index == state_.editor_groups[from_group].active_tab_index) {
    SyncActiveEditorTabMetadata();
  }

  if (pane_count_preserving) {
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
  const std::size_t source_index = from_group >= carved_index ? from_group + 1 : from_group;
  state_.focused_group_index = carved_index;
  state_.surface.focus = FocusTarget::Editor;
  HydrateGroupActiveTab(state_.editor_groups[source_index]);
  HydrateGroupActiveTab(state_.editor_groups[state_.clamped_focused_group_index()]);
  operations_.invalidate_tab_strip_geometry();
  EnsureEveryGroupActiveTabVisible();
  RefreshFocusedGroupActiveTab();
  return true;
}

TabEntry TabCoordinator::CloneEditorTabForSplit(const TabEntry& tab) {
  TabEntry clone;
  clone.kind = TabEntry::Kind::Editor;
  clone.path = tab.path;
  clone.title = tab.title;
  clone.untitled_number = tab.untitled_number;
  if (tab.editor_state.has_value()) {
    TabEntry::EditorTabState editor_state;
    editor_state.viewport = tab.editor_state->viewport;
    editor_state.restore = *EditorTabViewState(tab);
    editor_state.content = tab.editor_state->content == TabEntry::EditorTabState::Content::Ready
                               ? TabEntry::EditorTabState::Content::Ready
                               : TabEntry::EditorTabState::Content::Deferred;
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
  if (const TabEntry& active = source.active_tab();
      active.kind != TabEntry::Kind::Editor || !active.editor_state.has_value()) {
    return false;
  }
  SyncActiveEditorTabMetadata();

  EditorGroup new_group;
  new_group.open_tabs.push_back(CloneEditorTabForSplit(source.active_tab()));
  new_group.active_tab_index = 0;

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
  RefreshFocusedGroupActiveTab();
  return true;
}

void TabCoordinator::ReconcileActiveCaretsWithDocuments() {
  for (EditorGroup& group : state_.editor_groups) {
    if (group.active_tab_index >= group.open_tabs.size()) {
      continue;
    }
    TabEntry& tab = group.active_tab();
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      tab.editor_state->viewport.ReconcileCaretsWithDocument();
      tab.editor_state->viewport.TraceCaretBounds("at frame preparation");
    }
    if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
      tab.compare->right_viewport.ReconcileCaretsWithDocument();
    }
    if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
      tab.merge->result_viewport.ReconcileCaretsWithDocument();
    }
  }
}

bool TabCoordinator::FocusOtherGroup() {
  if (state_.editor_groups.size() < 2) {
    return false;
  }
  state_.focused_group_index =
      (state_.clamped_focused_group_index() + 1) % state_.editor_groups.size();
  state_.surface.focus = FocusTarget::Editor;
  RefreshFocusedGroupActiveTab();
  return true;
}

bool TabCoordinator::FocusEditorGroupAt(std::size_t index) {
  if (index >= state_.editor_groups.size()) {
    return false;
  }
  state_.focused_group_index = index;
  state_.surface.focus = FocusTarget::Editor;
  RefreshFocusedGroupActiveTab();
  return true;
}

std::size_t TabCoordinator::AdjacentGroupInDirection(EditorGroupDirection direction) const {
  if (state_.editor_groups.size() < 2 || !operations_.editor_group_rects) {
    return kNoEditorGroup;
  }
  const EditorGroupRectsLayout rects = operations_.editor_group_rects();
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
  RefreshFocusedGroupActiveTab();
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
  const bool before =
      direction == EditorGroupDirection::Left || direction == EditorGroupDirection::Up;
  return RelocateEditorGroup(
      focused, target,
      horizontal ? EditorSplitOrientation::Vertical : EditorSplitOrientation::Horizontal, before);
}

bool TabCoordinator::RelocateEditorGroup(std::size_t from,
                                         std::size_t target,
                                         EditorSplitOrientation orientation,
                                         bool before) {
  const std::size_t landed = state_.editor_split.MoveLeaf(from, target, orientation, before);
  if (landed == EditorSplitTree::kNoLeaf) {
    return false;
  }
  EditorGroup moved = std::move(state_.editor_groups[from]);
  state_.editor_groups.erase(state_.editor_groups.begin() + static_cast<std::ptrdiff_t>(from));
  state_.editor_groups.insert(state_.editor_groups.begin() + static_cast<std::ptrdiff_t>(landed),
                              std::move(moved));
  state_.focused_group_index = landed;
  state_.surface.focus = FocusTarget::Editor;
  HydrateGroupActiveTab(state_.editor_groups[landed]);
  operations_.invalidate_tab_strip_geometry();
  EnsureEveryGroupActiveTabVisible();
  RefreshFocusedGroupActiveTab();
  return true;
}

void TabCoordinator::EnsureEveryGroupActiveTabVisible() {
  if (operations_.ensure_all_active_tabs_visible) {
    operations_.ensure_all_active_tabs_visible();
  } else util::InvokeIfSet(operations_.ensure_active_tab_visible);
}

void TabCoordinator::RefreshFocusedGroupActiveTab() {
  operations_.ensure_active_tab_visible();
  operations_.request_frame();
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
  MaybeNotifyLspClose(group.open_tabs[index]);
  CancelPendingAsyncRead(group.open_tabs[index]);
  RememberClosedEditor(group.open_tabs[index]);
  const bool closing_active = index == group.active_tab_index;
  const std::uint64_t closed_id = group.open_tabs[index].stable_id;
  group.open_tabs.erase(group.open_tabs.begin() + static_cast<std::ptrdiff_t>(index));
  util::InvokeIfSet(operations_.tab_closed, closed_id);
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
  state_.editor_split.RemoveLeaf(gi);
  if (state_.editor_groups.empty()) {
    state_.editor_groups.emplace_back();
    state_.editor_split.Reset();
  }
  if (state_.focused_group_index > gi) {
    --state_.focused_group_index;
  }
  if (state_.focused_group_index >= state_.editor_groups.size()) {
    state_.focused_group_index = state_.editor_groups.size() - 1;
  }
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
  const std::unordered_map<std::string, std::size_t> view_counts =
      operations_.open_buffer_view_counts ? operations_.open_buffer_view_counts()
                                          : std::unordered_map<std::string, std::size_t>{};
  const EditorGroup& closing = state_.focused_group();
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
  RefreshFocusedGroupActiveTab();
  return true;
}

}
