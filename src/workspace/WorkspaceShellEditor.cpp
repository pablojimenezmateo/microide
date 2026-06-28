#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

#include "editor/RuntimeSyntaxRegistry.h"
#include "util/PerformanceCounters.h"
#include "workspace/EditorTabService.h"
#include "workspace/SettingFlags.h"
#include "workspace/WorkspaceFoldingRefresh.h"
#include "workspace/WorkspaceIndentDetectApply.h"

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

void WorkspaceShell::ApplyDetectedIndentOnOpen(editor::TextViewport& viewport) const {
  ApplyDetectedIndentAfterPreferences(
      viewport, [this](std::string_view id) { return GetSettingValue(id); });
}

void WorkspaceShell::ApplyEditorPreferences(editor::TextViewport& viewport) const {
  const auto setting_enabled = [this](std::string_view id, bool default_value) {
    return SettingFlagEnabled(GetSettingValue(id), default_value);
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
  util::AddPerformanceCounter(util::PerfCounterId::FrameApplyEditorPreferencesAllTabsCalls);
  ApplyEditorPreferences(context_.current_project_state.focused_group().welcome_surface.viewport);
  for (auto& tab : context_.current_project_state.focused_group().open_tabs) {
    if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
      continue;
    }
    ApplyEditorPreferences(tab.editor_state->viewport);
  }
}

WorkspaceShell::TabEntry::EditorTabState* WorkspaceShell::GroupActiveEditorTab(EditorGroup& group) {
  if (group.active_tab_index >= group.open_tabs.size()) {
    return nullptr;
  }
  TabEntry& tab = group.open_tabs[group.active_tab_index];
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return nullptr;
  }
  return &tab.editor_state.value();
}

const WorkspaceShell::TabEntry::EditorTabState* WorkspaceShell::GroupActiveEditorTab(
    const EditorGroup& group) const {
  if (group.active_tab_index >= group.open_tabs.size()) {
    return nullptr;
  }
  const TabEntry& tab = group.open_tabs[group.active_tab_index];
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return nullptr;
  }
  return &tab.editor_state.value();
}

editor::TextViewport* WorkspaceShell::GroupActiveViewport(EditorGroup& group) {
  TabEntry::EditorTabState* editor_tab = GroupActiveEditorTab(group);
  if (editor_tab == nullptr) {
    return &group.welcome_surface.viewport;
  }
  return &editor_tab->viewport;
}

const editor::TextViewport* WorkspaceShell::GroupActiveViewport(const EditorGroup& group) const {
  const TabEntry::EditorTabState* editor_tab = GroupActiveEditorTab(group);
  if (editor_tab == nullptr) {
    return &group.welcome_surface.viewport;
  }
  return &editor_tab->viewport;
}

editor::FoldingModel* WorkspaceShell::EnsureActiveFoldingModelFresh() {
  return EnsureFoldingModelFreshForTab(ActiveEditorTab(), ActiveEditorViewport());
}

editor::FoldingModel* WorkspaceShell::EnsureGroupFoldingModelFresh(EditorGroup& group) {
  return EnsureFoldingModelFreshForTab(GroupActiveEditorTab(group), GroupActiveViewport(group));
}

editor::FoldingModel* WorkspaceShell::EnsureFoldingModelFreshForTab(
    TabEntry::EditorTabState* editor_tab, editor::TextViewport* active_viewport) {
  if (editor_tab == nullptr) {
    return nullptr;
  }
  if (active_viewport == nullptr) {
    return editor_tab->folding_model.get();
  }
  const auto setting_enabled = [this](std::string_view id, bool default_value) {
    return SettingFlagEnabled(GetSettingValue(id), default_value);
  };
  const std::string language_id =
      editor::runtime_syntax::DetectFiletype(active_viewport->path(), active_viewport->lines());
  const auto resolved = language_contract_.ResolveView(language_id);
  EnsureFoldingModelFresh(*editor_tab, *active_viewport, resolved.contract,
                          context_.current_project_state.editor_preferences.tab_size,
                          setting_enabled("editor.fold.enabled", true),
                          active_viewport->visible_lines());
  editor_tab->viewport.SetFoldingModel(editor_tab->folding_model->ranges().empty() &&
                                               editor_tab->folding_model->collapsed_flags().empty()
                                           ? nullptr
                                           : editor_tab->folding_model.get());
  return editor_tab->folding_model.get();
}

void WorkspaceShell::ActivateTab(std::size_t index) {
  MakeEditorTabService().Activate(index);
}

void WorkspaceShell::RequestActiveHighlightPrefetch() {
  if (highlight_prefetch_event_type_ == 0) {
    return;
  }
  editor::TextViewport* viewport = ActiveEditorViewport();
  if (viewport == nullptr || !viewport->syntax_highlighting_enabled()) {
    return;
  }
  const std::size_t line_count = viewport->line_count();
  if (line_count == 0) {
    return;
  }
  // Off-thread checkpoint backfill for a deep cold paint: when the synchronous
  // highlight-state replay was capped short (e.g. session restore scrolled deep
  // into a large file), this hands the worker a bounded chain segment so the
  // chain catches up without freezing the frame. Checked before the visible-band
  // gap early-out so a deep jump that already tokenized its visible window
  // (approximately) still converges to exact highlighting.
  if (auto backfill = viewport->TakeHighlightCheckpointBackfillRequest()) {
    highlight_prefetch_service_.RequestCheckpoints(std::move(*backfill));
  }
  // Prefetch the visible band plus a look-ahead window below it so scrolling
  // down lands on already-tokenized lines instead of stalling the render path.
  const std::size_t top =
      std::min(viewport->VisualRowLineIndex(viewport->scroll_line()), line_count - 1);
  const std::size_t visible = std::max<std::size_t>(viewport->visible_lines(), 1);
  const std::size_t count = std::min(visible * 3, line_count - top);
  if (!viewport->HasHighlightPrefetchGap(top, count)) {
    return;
  }
  highlight_prefetch_service_.Request(viewport->BuildHighlightPrefetchRequest(top, count));
}

void WorkspaceShell::ConsumeHighlightPrefetchResults() {
  editor::TextViewport* active_viewport = ActiveEditorViewport();
  for (auto& result : highlight_prefetch_service_.DrainResults()) {
    // result.viewport is only an identity token: install only when it still
    // matches the live active viewport (the install path additionally drops
    // results whose document revision has moved on). The result is drained and
    // discarded here, so move it in to avoid copying every prefetched token
    // vector on the active-scroll path.
    if (active_viewport != nullptr && result.viewport == active_viewport) {
      active_viewport->InstallPrefetchedHighlights(std::move(result));
    }
  }
  for (const auto& result : highlight_prefetch_service_.DrainCheckpointResults()) {
    if (active_viewport != nullptr && result.viewport == active_viewport) {
      active_viewport->InstallHighlightCheckpoints(result);
    }
  }
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
  state.viewport = view;
  state.restored_path = view.path().lexically_normal();
  state.restored_cursor_line = view.cursor_line();
  state.restored_cursor_column = view.cursor_column();
  state.restored_scroll_line = view.scroll_line();
  state.restored_horizontal_scroll = view.horizontal_scroll();
  state.needs_restore = false;
  return state;
}

void WorkspaceShell::SyncActiveEditorTabMetadata() {
  MakeEditorTabService().SyncActiveEditorTabMetadata();
}

std::filesystem::path WorkspaceShell::EditorViewPath(
    const TabEntry::EditorTabState& editor_state) const {
  return editor_state.needs_restore ? editor_state.restored_path.lexically_normal()
                                     : editor_state.viewport.path().lexically_normal();
}

bool WorkspaceShell::ActivateCurrentTabAfterStateLoad() {
  return MakeEditorTabService().ActivateCurrentTabAfterStateLoad();
}

bool WorkspaceShell::ReplaceActiveEditorView(const editor::TextViewport& viewport) {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr) {
    return false;
  }

  editor::TextViewport configured_view = viewport;
  ApplyEditorPreferences(configured_view);
  ApplyDetectedIndentOnOpen(configured_view);

  const std::filesystem::path old_path = editor_tab->viewport.path().lexically_normal();
  editor_tab->viewport = configured_view;
  editor_tab->needs_restore = false;
  editor_tab->restored_path = configured_view.path().lexically_normal();
  editor_tab->folding_model->Clear();
  const std::filesystem::path new_path = configured_view.path().lexically_normal();
  if (!old_path.empty() && old_path != new_path && CountOpenBufferViews(old_path) == 0) {
    NotifyLspBufferClose(old_path);
  }
  if (!new_path.empty()) {
    NotifyPluginBufferOpen(new_path);
  }
  SyncActiveEditorTabMetadata();
  ResetCaretBlink();
  RequestActiveTabRedraw(!editor_tab->viewport.path().empty());
  return true;
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
  if (context_.current_project_state.focused_group().active_tab_index >= context_.current_project_state.focused_group().open_tabs.size()) {
    return {};
  }
  return context_.current_project_state.focused_group().open_tabs[context_.current_project_state.focused_group().active_tab_index]
      .path.lexically_normal();
}

void WorkspaceShell::RequestCloseTab(std::size_t index) {
  RequestCloseTabs({index});
}

void WorkspaceShell::RequestCloseTabs(std::vector<std::size_t> indices) {
  indices.erase(std::remove_if(indices.begin(), indices.end(), [&](std::size_t index) {
                  return index >= context_.current_project_state.focused_group().open_tabs.size();
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
  for (const auto& tab : context_.current_project_state.focused_group().open_tabs) {
    if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
      continue;
    }
    const std::filesystem::path path = EditorViewPath(*tab.editor_state);
    if (!path.empty()) {
      paths.push_back(path.lexically_normal());
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
  indices.reserve(context_.current_project_state.focused_group().open_tabs.size());
  for (std::size_t i = 0; i < context_.current_project_state.focused_group().open_tabs.size(); ++i) {
    indices.push_back(i);
  }
  RequestCloseTabs(std::move(indices));
}

void WorkspaceShell::CloseTab(std::size_t index) {
  MakeEditorTabService().Close(index);
}

namespace {

// The single normalized buffer path a tab contributes to LSP open-view
// accounting, or nullopt if the tab holds no editable buffer view. Shared by the
// single-path count and the whole-workspace count map so both stay in lockstep.
std::optional<std::filesystem::path> OpenBufferViewPath(const TabEntry& tab) {
  if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
    std::filesystem::path view_path =
        (tab.editor_state->needs_restore ? tab.editor_state->restored_path
                                         : tab.editor_state->viewport.path())
            .lexically_normal();
    if (view_path.empty()) {
      return std::nullopt;
    }
    return view_path;
  }
  if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
      tab.compare->right_editable && !tab.compare->right_viewport.path().empty()) {
    return tab.compare->right_viewport.path().lexically_normal();
  }
  if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value() &&
      !tab.merge->result_viewport.path().empty()) {
    return tab.merge->result_viewport.path().lexically_normal();
  }
  return std::nullopt;
}

}  // namespace

std::size_t WorkspaceShell::CountOpenBufferViews(const std::filesystem::path& path) const {
  if (path.empty()) {
    return 0;
  }
  const std::filesystem::path normalized = path.lexically_normal();
  std::size_t count = 0;
  // Count across every editor group: a buffer shared by both groups in a split
  // must not be reported as closed until the last view in either group is gone.
  for (const EditorGroup& group : context_.current_project_state.editor_groups) {
    for (const TabEntry& tab : group.open_tabs) {
      const std::optional<std::filesystem::path> view = OpenBufferViewPath(tab);
      if (view.has_value() && *view == normalized) {
        ++count;
      }
    }
  }
  return count;
}

std::unordered_map<std::string, std::size_t> WorkspaceShell::OpenBufferViewCounts() const {
  // One O(views) pass producing the same per-path counts CountOpenBufferViews
  // would return individually, so bulk-close paths avoid an O(tabs * views)
  // rescan (and repeated path normalization) when deciding LSP didClose.
  std::unordered_map<std::string, std::size_t> counts;
  for (const EditorGroup& group : context_.current_project_state.editor_groups) {
    for (const TabEntry& tab : group.open_tabs) {
      const std::optional<std::filesystem::path> view = OpenBufferViewPath(tab);
      if (view.has_value()) {
        ++counts[view->generic_string()];
      }
    }
  }
  return counts;
}

}  // namespace microide::workspace
