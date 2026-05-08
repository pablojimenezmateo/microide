#include "workspace/WorkspaceShell.h"

#include <algorithm>

#include "editor/RuntimeSyntaxRegistry.h"
#include "workspace/EditorTabService.h"

namespace microide::workspace {

namespace {

editor::LanguageContractView BuildEditorLanguageContractView(
    const WorkspaceLanguageContract& contracts,
    std::string_view language_id,
    bool auto_close_enabled,
    bool surround_enabled,
    bool smart_indent_enabled) {
  editor::LanguageContractView view;
  const auto resolved = contracts.ResolveView(language_id);
  if (const LanguageContract* contract = resolved.contract; contract != nullptr) {
    view.auto_close_pairs.reserve(contract->auto_close_pairs.size());
    for (const auto& pair : contract->auto_close_pairs) {
      view.auto_close_pairs.push_back(editor::LanguagePair{pair.open, pair.close});
    }
    view.surround_pairs.reserve(contract->surround_pairs.size());
    for (const auto& pair : contract->surround_pairs) {
      view.surround_pairs.push_back(editor::LanguagePair{pair.open, pair.close});
    }
    view.indent_after_open_patterns = contract->indent_after_open_patterns;
    view.dedent_on_close_chars = contract->dedent_on_close_chars;
    view.line_comment = contract->line_comment;
    view.block_comment_open = contract->block_comment.open;
    view.block_comment_close = contract->block_comment.close;
    view.inhibit_pairs_in_strings = contract->inhibit_pairs_in_strings;
    view.inhibit_pairs_in_comments = contract->inhibit_pairs_in_comments;
  }
  view.auto_close_enabled = auto_close_enabled;
  view.surround_enabled = surround_enabled;
  view.smart_indent_enabled = smart_indent_enabled;
  return view;
}

}  // namespace

void WorkspaceShell::ApplyEditorPreferences(editor::TextViewport& viewport) const {
  const auto setting_enabled = [this](std::string_view id, bool default_value) {
    const auto value = GetSettingValue(id);
    if (!value.has_value()) {
      return default_value;
    }
    return *value != "false" && *value != "0" && *value != "off";
  };

  viewport.SetTabSize(context_.current_project_state.editor_preferences.tab_size);
  viewport.SetIndentWidth(context_.current_project_state.editor_preferences.indent_width);
  viewport.SetSoftTabs(context_.current_project_state.editor_preferences.soft_tabs);
  viewport.SetSoftWrap(context_.current_project_state.editor_preferences.soft_wrap);
  viewport.SetSaveTrimTrailingWhitespace(
      setting_enabled("editor.save.trim_trailing_whitespace", true));
  viewport.SetSaveEnsureFinalNewline(
      setting_enabled("editor.save.ensure_final_newline", true));

  const std::string language_id =
      editor::runtime_syntax::DetectFiletype(viewport.path(), viewport.lines());
  viewport.SetLanguageContractView(BuildEditorLanguageContractView(
      language_contract_,
      language_id,
      setting_enabled("editor.brackets.auto_close.enabled", true),
      setting_enabled("editor.brackets.surround.enabled", true),
      setting_enabled("editor.indent.smart.enabled", true)));
}

void WorkspaceShell::ApplyEditorPreferencesToAllTabs() {
  ApplyEditorPreferences(context_.current_project_state.welcome_surface.viewport);
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
  MakeEditorTabService().Activate(index);
}

void WorkspaceShell::SyncActiveEditorTab() {
  MakeEditorTabService().SyncActiveEditorTab();
}

bool WorkspaceShell::ActiveTabIsEditor() const {
  return const_cast<WorkspaceShell*>(this)->MakeEditorTabService().ActiveTabIsEditor();
}

WorkspaceShell::TabEntry::EditorTabState* WorkspaceShell::ActiveEditorTab() {
  return MakeEditorTabService().ActiveEditorTab();
}

const WorkspaceShell::TabEntry::EditorTabState* WorkspaceShell::ActiveEditorTab() const {
  return const_cast<WorkspaceShell*>(this)->MakeEditorTabService().ActiveEditorTab();
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
  MakeEditorTabService().SyncActiveEditorTabMetadata();
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

bool WorkspaceShell::ActivateCurrentTabAfterStateLoad() {
  return MakeEditorTabService().ActivateCurrentTabAfterStateLoad();
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
    context_.current_project_state.welcome_surface.viewport = configured_view;
    const std::filesystem::path new_path = configured_view.path().lexically_normal();
    if (!old_path.empty() && old_path != new_path && CountOpenBufferViews(old_path) == 0) {
      NotifyLspBufferClose(old_path);
    }
    if (!new_path.empty()) {
      NotifyPluginBufferOpen(new_path);
    }
    SyncActiveEditorTabMetadata();
    ResetCaretBlink();
    RequestActiveTabRedraw(!context_.current_project_state.welcome_surface.viewport.path().empty());
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
  return MakeEditorTabService().ActiveEditorViewport();
}

const editor::TextViewport* WorkspaceShell::ActiveEditorViewport() const {
  return const_cast<WorkspaceShell*>(this)->MakeEditorTabService().ActiveEditorViewport();
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
  ++reload_clean_open_buffers_from_disk_invocation_count_;
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
  MakeEditorTabService().Close(index);
}

}  // namespace microide::workspace
