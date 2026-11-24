#include "workspace/state/EditorGroupTabs.h"

namespace microide::workspace {

TabEntry::EditorTabState* GroupActiveEditorTab(EditorGroup& group) {
  if (group.active_tab_index >= group.open_tabs.size()) {
    return nullptr;
  }
  TabEntry& tab = group.active_tab();
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return nullptr;
  }
  return &tab.editor_state.value();
}

const TabEntry::EditorTabState* GroupActiveEditorTab(const EditorGroup& group) {
  if (group.active_tab_index >= group.open_tabs.size()) {
    return nullptr;
  }
  const TabEntry& tab = group.active_tab();
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return nullptr;
  }
  return &tab.editor_state.value();
}

editor::TextViewport* GroupActiveViewport(EditorGroup& group) {
  TabEntry::EditorTabState* editor_tab = GroupActiveEditorTab(group);
  if (editor_tab == nullptr) {
    return &group.welcome_surface.viewport;
  }
  return &editor_tab->viewport;
}

const editor::TextViewport* GroupActiveViewport(const EditorGroup& group) {
  const TabEntry::EditorTabState* editor_tab = GroupActiveEditorTab(group);
  if (editor_tab == nullptr) {
    return &group.welcome_surface.viewport;
  }
  return &editor_tab->viewport;
}

editor::FoldingModel* GroupFoldingModelPtr(EditorGroup& group) {
  TabEntry::EditorTabState* editor_tab = GroupActiveEditorTab(group);
  return editor_tab == nullptr ? nullptr : editor_tab->folding_model.get();
}

TabEntry::EditorTabState MakeEditorTabState(const editor::TextViewport& view) {
  TabEntry::EditorTabState state;
  state.viewport = view;
  state.restore.path = view.path().lexically_normal();
  state.restore.cursor_line = view.cursor_line();
  state.restore.cursor_column = view.cursor_column();
  state.restore.scroll_line = view.scroll_line();
  state.restore.horizontal_scroll = view.horizontal_scroll();
  state.content = EditorTabState::Content::Ready;
  return state;
}

std::filesystem::path EditorViewPath(const TabEntry::EditorTabState& editor_state) {
  return editor_state.content_pending() ? editor_state.restore.path.lexically_normal()
                                        : editor_state.viewport.path().lexically_normal();
}

}  // namespace microide::workspace
