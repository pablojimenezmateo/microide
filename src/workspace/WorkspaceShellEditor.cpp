#include "workspace/WorkspaceShell.h"

#include <algorithm>

namespace microide::workspace {

namespace {

std::string EditorTabLabel(const editor::TextViewport& viewport) {
  if (!viewport.path().empty()) {
    return viewport.path().filename().string();
  }
  return viewport.is_placeholder() ? "Welcome" : "Untitled";
}

}  // namespace

void WorkspaceShell::ApplyEditorPreferences(editor::TextViewport& viewport) const {
  viewport.SetTabSize(context_.current_project_state.editor_preferences.tab_size);
  viewport.SetIndentWidth(context_.current_project_state.editor_preferences.indent_width);
  viewport.SetSoftTabs(context_.current_project_state.editor_preferences.soft_tabs);
}

void WorkspaceShell::ApplyEditorPreferencesToAllTabs() {
  ApplyEditorPreferences(context_.current_project_state.text_viewport);
  for (auto& tab : context_.current_project_state.open_tabs) {
    if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
      continue;
    }
    for (auto& view : tab.editor_state->views) {
      ApplyEditorPreferences(view.viewport);
    }
  }
}

void WorkspaceShell::ActivateTab(std::size_t index) {
  if (index >= context_.current_project_state.open_tabs.size()) {
    return;
  }

  if (context_.current_project_state.active_tab_index < context_.current_project_state.open_tabs.size() && context_.current_project_state.active_tab_index != index) {
    SyncActiveEditorTab();
  }

  context_.current_project_state.active_tab_index = index;
  auto& tab = context_.current_project_state.open_tabs[index];
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
        context_.current_project_state.text_viewport = *active_view;
        ApplyEditorPreferences(context_.current_project_state.text_viewport);
      }
    } else {
      editor::TextViewport loaded_view;
      if (!loaded_view.OpenFile(tab.path)) {
        return;
      }
      ApplyEditorPreferences(loaded_view);
      context_.current_project_state.text_viewport = loaded_view;
      tab.editor_state = MakeEditorTabState(loaded_view);
    }
  }
  SyncActiveEditorTabMetadata();
  if (tab.kind == TabEntry::Kind::Compare) {
    RevealActiveCompareSelection();
  } else if (tab.kind == TabEntry::Kind::Merge) {
    RevealActiveMergeSelection();
  } else if (tab.kind == TabEntry::Kind::Editor && !context_.current_project_state.text_viewport.path().empty()) {
    context_.current_project_state.directory_tree.SelectPath(context_.current_project_state.text_viewport.path().lexically_normal());
    RevealSelectedTreeSidebarLine();
  }
  EnsureActiveTabVisible();
  context_.current_project_state.surface.focus = FocusTarget::Editor;
  ResetCaretBlink();
  RequestActiveTabRedraw(tab.kind == TabEntry::Kind::Editor && !context_.current_project_state.text_viewport.path().empty());
}

void WorkspaceShell::SyncActiveEditorTab() {
  if (context_.current_project_state.active_tab_index >= context_.current_project_state.open_tabs.size()) {
    return;
  }

  auto& tab = context_.current_project_state.open_tabs[context_.current_project_state.active_tab_index];
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return;
  }

  if (tab.editor_state->views.empty()) {
    tab.editor_state = MakeEditorTabState(context_.current_project_state.text_viewport);
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
    active_view->restored_path = active_view->viewport.path().lexically_normal();
    active_view->restored_cursor_line = active_view->viewport.cursor_line();
    active_view->restored_cursor_column = active_view->viewport.cursor_column();
    active_view->restored_scroll_line = active_view->viewport.scroll_line();
    active_view->restored_horizontal_scroll = active_view->viewport.horizontal_scroll();
    active_view->needs_restore = false;
  }
  if (context_.current_project_state.active_tab_index < context_.current_project_state.open_tabs.size() && &tab == &context_.current_project_state.open_tabs[context_.current_project_state.active_tab_index]) {
    SyncActiveEditorTabMetadata();
  }
}

bool WorkspaceShell::ActiveTabIsEditor() const {
  return context_.current_project_state.active_tab_index < context_.current_project_state.open_tabs.size() &&
         context_.current_project_state.open_tabs[context_.current_project_state.active_tab_index].kind == TabEntry::Kind::Editor &&
         context_.current_project_state.open_tabs[context_.current_project_state.active_tab_index].editor_state.has_value();
}

WorkspaceShell::TabEntry::EditorTabState* WorkspaceShell::ActiveEditorTab() {
  if (!ActiveTabIsEditor()) {
    return nullptr;
  }
  return &context_.current_project_state.open_tabs[context_.current_project_state.active_tab_index].editor_state.value();
}

const WorkspaceShell::TabEntry::EditorTabState* WorkspaceShell::ActiveEditorTab() const {
  if (!ActiveTabIsEditor()) {
    return nullptr;
  }
  return &context_.current_project_state.open_tabs[context_.current_project_state.active_tab_index].editor_state.value();
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
  if (context_.current_project_state.active_tab_index >= context_.current_project_state.open_tabs.size()) {
    return;
  }

  auto& tab = context_.current_project_state.open_tabs[context_.current_project_state.active_tab_index];
  if (tab.kind != TabEntry::Kind::Editor) {
    return;
  }

  const editor::TextViewport* viewport = ActiveEditorViewport();
  const std::filesystem::path active_path =
      viewport != nullptr ? viewport->path().lexically_normal() : std::filesystem::path{};
  const bool path_changed = tab.path != active_path;
  tab.path = active_path;
  tab.title = viewport != nullptr ? EditorTabLabel(*viewport) : "untitled";
  if (path_changed && !active_path.empty()) {
    context_.current_project_state.directory_tree.SelectPath(active_path);
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
  if (context_.current_project_state.open_tabs.empty()) {
    return true;
  }

  const std::size_t active_index = std::min(context_.current_project_state.active_tab_index, context_.current_project_state.open_tabs.size() - 1);
  context_.current_project_state.active_tab_index = context_.current_project_state.open_tabs.size();
  ActivateTab(active_index);
  return context_.current_project_state.active_tab_index == active_index;
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
    const std::filesystem::path old_path = active_view->path().lexically_normal();
    *active_view = configured_view;
    context_.current_project_state.text_viewport = configured_view;
    const std::filesystem::path new_path = configured_view.path().lexically_normal();
    if (!old_path.empty() && old_path != new_path && CountOpenBufferViews(old_path) == 0) {
      NotifyLspBufferClose(old_path);
    }
    if (!new_path.empty()) {
      NotifyPluginBufferOpen(new_path);
    }
    SyncActiveEditorTabMetadata();
    ResetCaretBlink();
    RequestActiveTabRedraw(!context_.current_project_state.text_viewport.path().empty());
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

editor::TextViewport* WorkspaceShell::ActiveEditorViewport() {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.empty()) {
    return &context_.current_project_state.text_viewport;
  }
  if (auto* viewport = FindEditorView(*editor_tab, editor_tab->active_leaf_id); viewport != nullptr) {
    return viewport;
  }
  return &editor_tab->views.front().viewport;
}

const editor::TextViewport* WorkspaceShell::ActiveEditorViewport() const {
  const auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.empty()) {
    return &context_.current_project_state.text_viewport;
  }
  if (const auto* viewport = FindEditorView(*editor_tab, editor_tab->active_leaf_id);
      viewport != nullptr) {
    return viewport;
  }
  return &editor_tab->views.front().viewport;
}

editor::TextViewport* WorkspaceShell::ActiveNavigableViewport() {
  if (ActiveTabIsCompare()) {
    auto* compare_tab = ActiveCompareTab();
    return compare_tab != nullptr && compare_tab->right_view_active ? &compare_tab->right_viewport
                                                                    : nullptr;
  }
  if (ActiveTabIsMerge()) {
    auto* merge_tab = ActiveMergeTab();
    return merge_tab != nullptr ? &merge_tab->result_viewport : nullptr;
  }
  return ActiveEditorViewport();
}

const editor::TextViewport* WorkspaceShell::ActiveNavigableViewport() const {
  if (ActiveTabIsCompare()) {
    const auto* compare_tab = ActiveCompareTab();
    return compare_tab != nullptr && compare_tab->right_view_active ? &compare_tab->right_viewport
                                                                    : nullptr;
  }
  if (ActiveTabIsMerge()) {
    const auto* merge_tab = ActiveMergeTab();
    return merge_tab != nullptr ? &merge_tab->result_viewport : nullptr;
  }
  return ActiveEditorViewport();
}

std::filesystem::path WorkspaceShell::ActiveTabPath() const {
  if (context_.current_project_state.active_tab_index >= context_.current_project_state.open_tabs.size()) {
    return {};
  }
  return context_.current_project_state.open_tabs[context_.current_project_state.active_tab_index]
      .path.lexically_normal();
}

void WorkspaceShell::RequestCloseTab(std::size_t index) {
  RequestCloseTabs({index});
}

void WorkspaceShell::RequestCloseTabs(std::vector<std::size_t> indices) {
  indices.erase(std::remove_if(indices.begin(), indices.end(), [&](std::size_t index) {
                  return index >= context_.current_project_state.open_tabs.size();
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
  for (const auto& tab : context_.current_project_state.open_tabs) {
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
  indices.reserve(context_.current_project_state.open_tabs.size());
  for (std::size_t i = 0; i < context_.current_project_state.open_tabs.size(); ++i) {
    indices.push_back(i);
  }
  RequestCloseTabs(std::move(indices));
}

void WorkspaceShell::CloseTab(std::size_t index) {
  if (index >= context_.current_project_state.open_tabs.size()) {
    return;
  }
  const bool closing_active = index == context_.current_project_state.active_tab_index;
  const TabEntry& closing_tab = context_.current_project_state.open_tabs[index];

  if (context_.current_project_state.active_tab_index < context_.current_project_state.open_tabs.size() && index != context_.current_project_state.active_tab_index) {
    SyncActiveEditorTab();
  }

  if (closing_tab.kind == TabEntry::Kind::Editor && closing_tab.editor_state.has_value()) {
    for (const auto& view : closing_tab.editor_state->views) {
      const std::filesystem::path path = EditorViewPath(view);
      if (!path.empty() && CountOpenBufferViews(path) == 1) {
        NotifyLspBufferClose(path);
      }
    }
  } else if (closing_tab.kind == TabEntry::Kind::Compare && closing_tab.compare.has_value()) {
    const auto& compare_tab = *closing_tab.compare;
    if (compare_tab.right_editable && !compare_tab.right_viewport.path().empty() &&
        CountOpenBufferViews(compare_tab.right_viewport.path()) == 1) {
      NotifyLspBufferClose(compare_tab.right_viewport.path());
    }
  } else if (closing_tab.kind == TabEntry::Kind::Merge && closing_tab.merge.has_value()) {
    const auto& merge_tab = *closing_tab.merge;
    if (!merge_tab.result_viewport.path().empty() &&
        CountOpenBufferViews(merge_tab.result_viewport.path()) == 1) {
      NotifyLspBufferClose(merge_tab.result_viewport.path());
    }
  }

  context_.current_project_state.open_tabs.erase(context_.current_project_state.open_tabs.begin() + static_cast<std::ptrdiff_t>(index));

  if (context_.current_project_state.open_tabs.empty()) {
    context_.current_project_state.active_tab_index = 0;
    context_.current_project_state.tab_scroll_index = 0;
    context_.current_project_state.text_viewport.SetPlaceholderText(
        "microide\n\n"
        "Project loaded.\n"
        "Use the sidebar to open files.\n");
    context_.current_project_state.surface.focus = FocusTarget::Editor;
    RequestActiveTabRedraw(false);
    return;
  }

  if (index < context_.current_project_state.active_tab_index) {
    --context_.current_project_state.active_tab_index;
  } else if (index == context_.current_project_state.active_tab_index) {
    context_.current_project_state.active_tab_index = std::min(index, context_.current_project_state.open_tabs.size() - 1);
    auto& tab = context_.current_project_state.open_tabs[context_.current_project_state.active_tab_index];
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value() &&
        !tab.editor_state->views.empty()) {
      if (!EnsureEditorTabLoaded(tab)) {
      } else {
        NormalizeEditorSplitTree(*tab.editor_state);
        if (auto* active_view = FindEditorView(*tab.editor_state, tab.editor_state->active_leaf_id);
            active_view != nullptr) {
          context_.current_project_state.text_viewport = *active_view;
          ApplyEditorPreferences(context_.current_project_state.text_viewport);
        }
      }
    } else if (tab.kind == TabEntry::Kind::Editor) {
      editor::TextViewport loaded_view;
      if (!loaded_view.OpenFile(tab.path)) {
      } else {
        ApplyEditorPreferences(loaded_view);
        context_.current_project_state.text_viewport = loaded_view;
        tab.editor_state = MakeEditorTabState(loaded_view);
      }
    }
    if (!tab.path.empty()) {
      context_.current_project_state.directory_tree.SelectPath(tab.path);
      RevealSelectedTreeSidebarLine();
    }
    context_.current_project_state.surface.focus = FocusTarget::Editor;
  }

  context_.current_project_state.tab_scroll_index =
      std::clamp(context_.current_project_state.tab_scroll_index, 0, std::max(0, static_cast<int>(context_.current_project_state.open_tabs.size()) - 1));
  EnsureActiveTabVisible();
  if (closing_active) {
    RequestActiveTabRedraw(!context_.current_project_state.text_viewport.path().empty());
  } else {
    RequestTabStripRedraw();
  }
}

}  // namespace microide::workspace
