#include "workspace/WorkspaceShell.h"

#include <algorithm>

namespace microide::workspace {

namespace {

std::string EditorTabLabel(const editor::TextViewport& viewport) {
  if (!viewport.path().empty()) {
    return viewport.path().filename().string();
  }
  return viewport.is_placeholder() ? "welcome" : "untitled";
}

}  // namespace

void WorkspaceShell::ApplyEditorPreferences(editor::TextViewport& viewport) const {
  viewport.SetTabSize(editor_preferences_.tab_size);
  viewport.SetIndentWidth(editor_preferences_.indent_width);
  viewport.SetSoftTabs(editor_preferences_.soft_tabs);
}

void WorkspaceShell::ApplyEditorPreferencesToAllTabs() {
  ApplyEditorPreferences(text_viewport_);
  for (auto& tab : open_tabs_) {
    if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
      continue;
    }
    for (auto& view : tab.editor_state->views) {
      ApplyEditorPreferences(view.viewport);
    }
  }
}

void WorkspaceShell::ActivateTab(std::size_t index) {
  if (index >= open_tabs_.size()) {
    return;
  }

  if (active_tab_index_ < open_tabs_.size() && active_tab_index_ != index) {
    SyncActiveEditorTab();
  }

  active_tab_index_ = index;
  auto& tab = open_tabs_[index];
  if (tab.kind == TabEntry::Kind::Editor) {
    if (tab.editor_state.has_value() && !tab.editor_state->views.empty()) {
      if (!EnsureEditorTabLoaded(tab)) {
        return;
      }
      NormalizeEditorSplitTree(*tab.editor_state);
      editor::TextViewport* active_view =
          FindEditorView(*tab.editor_state, tab.editor_state->active_leaf_id);
      if (active_view == nullptr && !tab.editor_state->views.empty()) {
        tab.editor_state->active_leaf_id = tab.editor_state->views.front().leaf_id;
        active_view = &tab.editor_state->views.front().viewport;
      }
      if (active_view != nullptr) {
        text_viewport_ = *active_view;
        ApplyEditorPreferences(text_viewport_);
      }
    } else {
      editor::TextViewport loaded_view;
      if (!loaded_view.OpenFile(tab.path)) {
        return;
      }
      ApplyEditorPreferences(loaded_view);
      text_viewport_ = loaded_view;
      tab.editor_state = MakeEditorTabState(loaded_view);
    }
  }
  SyncActiveEditorTabMetadata();
  if (tab.kind == TabEntry::Kind::Compare) {
    RevealActiveCompareSelection();
  } else if (tab.kind == TabEntry::Kind::Merge) {
    RevealActiveMergeSelection();
  } else if (tab.kind == TabEntry::Kind::Editor && !text_viewport_.path().empty()) {
    directory_tree_.SelectPath(text_viewport_.path().lexically_normal());
    RevealSelectedTreeSidebarLine();
  }
  EnsureActiveTabVisible();
  surface_.focus = FocusTarget::Editor;
  ResetCaretBlink();
  RequestActiveTabRedraw(tab.kind == TabEntry::Kind::Editor && !text_viewport_.path().empty());
}

void WorkspaceShell::SyncActiveEditorTab() {
  if (active_tab_index_ >= open_tabs_.size()) {
    return;
  }

  auto& tab = open_tabs_[active_tab_index_];
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return;
  }

  if (tab.editor_state->views.empty()) {
    tab.editor_state = MakeEditorTabState(text_viewport_);
    return;
  }

  NormalizeEditorSplitTree(*tab.editor_state);
  if (auto* active_view = FindEditorViewState(*tab.editor_state, tab.editor_state->active_leaf_id);
      active_view != nullptr) {
    if (active_view->needs_restore) {
      tab.path = EditorViewPath(*active_view);
      tab.title = tab.path.empty() ? "untitled" : tab.path.filename().string();
      return;
    }
    active_view->viewport = text_viewport_;
    active_view->restored_path = text_viewport_.path().lexically_normal();
    active_view->restored_cursor_line = text_viewport_.cursor_line();
    active_view->restored_cursor_column = text_viewport_.cursor_column();
    active_view->restored_scroll_line = text_viewport_.scroll_line();
    active_view->restored_horizontal_scroll = text_viewport_.horizontal_scroll();
    active_view->needs_restore = false;
  }
  if (active_tab_index_ < open_tabs_.size() && &tab == &open_tabs_[active_tab_index_]) {
    SyncActiveEditorTabMetadata();
  }
}

bool WorkspaceShell::ActiveTabIsEditor() const {
  return active_tab_index_ < open_tabs_.size() &&
         open_tabs_[active_tab_index_].kind == TabEntry::Kind::Editor &&
         open_tabs_[active_tab_index_].editor_state.has_value();
}

WorkspaceShell::TabEntry::EditorTabState* WorkspaceShell::ActiveEditorTab() {
  if (!ActiveTabIsEditor()) {
    return nullptr;
  }
  return &open_tabs_[active_tab_index_].editor_state.value();
}

const WorkspaceShell::TabEntry::EditorTabState* WorkspaceShell::ActiveEditorTab() const {
  if (!ActiveTabIsEditor()) {
    return nullptr;
  }
  return &open_tabs_[active_tab_index_].editor_state.value();
}

WorkspaceShell::TabEntry::EditorTabState WorkspaceShell::MakeEditorTabState(
    const editor::TextViewport& view) {
  TabEntry::EditorTabState state;
  state.views.push_back(TabEntry::EditorTabState::EditorViewState{
      .leaf_id = 1,
      .viewport = view,
      .restored_path = view.path().lexically_normal(),
      .restored_cursor_line = view.cursor_line(),
      .restored_cursor_column = view.cursor_column(),
      .restored_scroll_line = view.scroll_line(),
      .restored_horizontal_scroll = view.horizontal_scroll(),
      .needs_restore = false,
  });
  state.active_leaf_id = 1;
  state.next_leaf_id = 2;
  state.split_root = MakeEditorLeafNode(1);
  return state;
}

std::unique_ptr<WorkspaceShell::TabEntry::EditorTabState::EditorSplitNode>
WorkspaceShell::MakeEditorLeafNode(std::size_t leaf_id, float size_fraction) {
  auto leaf = std::make_unique<TabEntry::EditorTabState::EditorSplitNode>();
  leaf->leaf_id = leaf_id;
  leaf->orientation = EditorSplitOrientation::None;
  leaf->size_fraction = size_fraction;
  return leaf;
}

void WorkspaceShell::SyncActiveEditorTabMetadata() {
  if (active_tab_index_ >= open_tabs_.size()) {
    return;
  }

  auto& tab = open_tabs_[active_tab_index_];
  if (tab.kind != TabEntry::Kind::Editor) {
    return;
  }

  const std::filesystem::path active_path = text_viewport_.path().lexically_normal();
  const bool path_changed = tab.path != active_path;
  tab.path = active_path;
  tab.title = EditorTabLabel(text_viewport_);
  if (path_changed && !active_path.empty()) {
    directory_tree_.SelectPath(active_path);
    RevealSelectedTreeSidebarLine();
  }
}

WorkspaceShell::TabEntry::EditorTabState::EditorViewState* WorkspaceShell::FindEditorViewState(
    TabEntry::EditorTabState& editor_tab,
    std::size_t leaf_id) {
  auto it = std::find_if(editor_tab.views.begin(), editor_tab.views.end(), [&](const auto& view) {
    return view.leaf_id == leaf_id;
  });
  return it == editor_tab.views.end() ? nullptr : &*it;
}

const WorkspaceShell::TabEntry::EditorTabState::EditorViewState*
WorkspaceShell::FindEditorViewState(const TabEntry::EditorTabState& editor_tab,
                                    std::size_t leaf_id) const {
  auto it = std::find_if(editor_tab.views.begin(), editor_tab.views.end(), [&](const auto& view) {
    return view.leaf_id == leaf_id;
  });
  return it == editor_tab.views.end() ? nullptr : &*it;
}

std::filesystem::path WorkspaceShell::EditorViewPath(
    const TabEntry::EditorTabState::EditorViewState& view) const {
  return view.needs_restore ? view.restored_path.lexically_normal()
                            : view.viewport.path().lexically_normal();
}

bool WorkspaceShell::RestoreEditorView(TabEntry::EditorTabState::EditorViewState& view) {
  if (!view.needs_restore) {
    return true;
  }
  if (view.restored_path.empty()) {
    return false;
  }

  editor::TextViewport loaded_view;
  if (!loaded_view.OpenFile(view.restored_path)) {
    return false;
  }
  loaded_view.MoveCursorTo(view.restored_cursor_line, view.restored_cursor_column);
  loaded_view.SetScrollLine(view.restored_scroll_line);
  loaded_view.SetHorizontalScroll(view.restored_horizontal_scroll);
  ApplyEditorPreferences(loaded_view);
  view.viewport = std::move(loaded_view);
  view.needs_restore = false;
  return true;
}

bool WorkspaceShell::EnsureEditorTabLoaded(TabEntry& tab) {
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return false;
  }

  auto& editor_state = *tab.editor_state;
  bool loaded_any = false;
  bool active_loaded = false;
  for (auto& view : editor_state.views) {
    if (!view.needs_restore) {
      loaded_any = true;
      if (view.leaf_id == editor_state.active_leaf_id) {
        active_loaded = true;
      }
      continue;
    }
    if (RestoreEditorView(view)) {
      loaded_any = true;
      if (view.leaf_id == editor_state.active_leaf_id) {
        active_loaded = true;
      }
    }
  }

  if (!loaded_any) {
    return false;
  }

  if (!active_loaded) {
    auto loaded_it = std::find_if(editor_state.views.begin(), editor_state.views.end(),
                                  [](const auto& view) { return !view.needs_restore; });
    if (loaded_it != editor_state.views.end()) {
      editor_state.active_leaf_id = loaded_it->leaf_id;
    }
  }

  NormalizeEditorSplitTree(editor_state);
  if (const auto* active_view = FindEditorViewState(editor_state, editor_state.active_leaf_id);
      active_view != nullptr) {
    tab.path = EditorViewPath(*active_view);
    tab.title = tab.path.empty() ? "untitled" : tab.path.filename().string();
  }
  return true;
}

bool WorkspaceShell::ActivateCurrentTabAfterStateLoad() {
  if (open_tabs_.empty()) {
    return true;
  }

  const std::size_t active_index = std::min(active_tab_index_, open_tabs_.size() - 1);
  active_tab_index_ = open_tabs_.size();
  ActivateTab(active_index);
  return active_tab_index_ == active_index;
}

bool WorkspaceShell::ReplaceActiveEditorView(const editor::TextViewport& viewport) {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.empty()) {
    return false;
  }

  editor::TextViewport configured_view = viewport;
  ApplyEditorPreferences(configured_view);

  NormalizeEditorSplitTree(*editor_tab);
  if (auto* active_view = FindEditorView(*editor_tab, editor_tab->active_leaf_id);
      active_view != nullptr) {
    *active_view = configured_view;
    text_viewport_ = configured_view;
    SyncActiveEditorTabMetadata();
    ResetCaretBlink();
    RequestActiveTabRedraw(!text_viewport_.path().empty());
    return true;
  }
  return false;
}

editor::TextViewport* WorkspaceShell::FindEditorView(TabEntry::EditorTabState& editor_tab,
                                                     std::size_t leaf_id) {
  if (auto* view = FindEditorViewState(editor_tab, leaf_id); view != nullptr) {
    return &view->viewport;
  }
  return nullptr;
}

const editor::TextViewport* WorkspaceShell::FindEditorView(
    const TabEntry::EditorTabState& editor_tab,
    std::size_t leaf_id) const {
  if (const auto* view = FindEditorViewState(editor_tab, leaf_id); view != nullptr) {
    return &view->viewport;
  }
  return nullptr;
}

void WorkspaceShell::RequestCloseTab(std::size_t index) {
  RequestCloseTabs({index});
}

void WorkspaceShell::RequestCloseTabs(std::vector<std::size_t> indices) {
  indices.erase(std::remove_if(indices.begin(), indices.end(), [&](std::size_t index) {
                  return index >= open_tabs_.size();
                }),
                indices.end());
  std::sort(indices.begin(), indices.end());
  indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
  if (indices.empty()) {
    return;
  }

  std::vector<std::size_t> dirty_indices;
  dirty_indices.reserve(indices.size());
  for (std::size_t index : indices) {
    if (TabIsDirty(index)) {
      dirty_indices.push_back(index);
    }
  }

  if (!dirty_indices.empty()) {
    if (indices.size() == 1) {
      ShowDirtyPromptForTab(indices.front());
    } else {
      ShowDirtyPromptForTabs(std::move(indices), std::move(dirty_indices));
    }
    return;
  }

  for (std::size_t i = indices.size(); i > 0; --i) {
    CloseTab(indices[i - 1]);
  }
}

void WorkspaceShell::ReloadCleanOpenBuffersFromDisk() {
  SyncActiveEditorTab();
  std::vector<std::filesystem::path> paths;
  for (const auto& tab : open_tabs_) {
    if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
      continue;
    }
    for (const auto& view : tab.editor_state->views) {
      const std::filesystem::path path = EditorViewPath(view);
      if (!path.empty()) {
        paths.push_back(path.lexically_normal());
      }
    }
  }
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  for (const auto& path : paths) {
    ReloadCleanEditorTabsForPath(path);
  }
}

void WorkspaceShell::CloseAllTabs() {
  std::vector<std::size_t> indices;
  indices.reserve(open_tabs_.size());
  for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
    indices.push_back(i);
  }
  RequestCloseTabs(std::move(indices));
}

void WorkspaceShell::CloseTab(std::size_t index) {
  if (index >= open_tabs_.size()) {
    return;
  }
  const bool closing_active = index == active_tab_index_;

  if (active_tab_index_ < open_tabs_.size() && index != active_tab_index_) {
    SyncActiveEditorTab();
  }

  open_tabs_.erase(open_tabs_.begin() + static_cast<std::ptrdiff_t>(index));

  if (open_tabs_.empty()) {
    active_tab_index_ = 0;
    tab_scroll_index_ = 0;
    text_viewport_.SetPlaceholderText(
        "microide\n\n"
        "Project loaded.\n"
        "Use the sidebar to open files.\n");
    surface_.focus = FocusTarget::Editor;
    RequestActiveTabRedraw(false);
    return;
  }

  if (index < active_tab_index_) {
    --active_tab_index_;
  } else if (index == active_tab_index_) {
    active_tab_index_ = std::min(index, open_tabs_.size() - 1);
    auto& tab = open_tabs_[active_tab_index_];
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value() &&
        !tab.editor_state->views.empty()) {
      if (!EnsureEditorTabLoaded(tab)) {
      } else {
        NormalizeEditorSplitTree(*tab.editor_state);
        if (auto* active_view = FindEditorView(*tab.editor_state, tab.editor_state->active_leaf_id);
            active_view != nullptr) {
          text_viewport_ = *active_view;
          ApplyEditorPreferences(text_viewport_);
        }
      }
    } else if (tab.kind == TabEntry::Kind::Editor) {
      editor::TextViewport loaded_view;
      if (!loaded_view.OpenFile(tab.path)) {
      } else {
        ApplyEditorPreferences(loaded_view);
        text_viewport_ = loaded_view;
        tab.editor_state = MakeEditorTabState(loaded_view);
      }
    }
    if (!tab.path.empty()) {
      directory_tree_.SelectPath(tab.path);
      RevealSelectedTreeSidebarLine();
    }
    surface_.focus = FocusTarget::Editor;
  }

  tab_scroll_index_ =
      std::clamp(tab_scroll_index_, 0, std::max(0, static_cast<int>(open_tabs_.size()) - 1));
  EnsureActiveTabVisible();
  if (closing_active) {
    RequestActiveTabRedraw(!text_viewport_.path().empty());
  } else {
    RequestTabStripRedraw();
  }
}

}  // namespace microide::workspace
