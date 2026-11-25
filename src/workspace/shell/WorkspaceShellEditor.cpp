#include "workspace/shell/WorkspaceShell.h"
#include "workspace/services/PromptSurfaceService.h"
#include "workspace/services/RemoteHostService.h"
#include "workspace/shell/ShellGlueCache.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

#include "editor/RuntimeSyntaxRegistry.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "workspace/services/EditorTabService.h"
#include "workspace/SettingFlags.h"
#include "workspace/WorkspaceFoldingRefresh.h"
#include "workspace/WorkspaceIndentDetectApply.h"

namespace microide::workspace {

void WorkspaceShell::ApplyDetectedIndentOnOpen(editor::TextViewport& viewport) const {
  static const project::EditorConfigProperties kNoEditorConfig;
  const project::EditorConfigProperties* editor_config = &kNoEditorConfig;
  if (SettingFlagEnabled(GetSettingValue("editor.editorconfig.enabled"), true) &&
      !viewport.path().empty()) {
    context_.current_project_state.editor_config.EnsureProjectRoot(
        context_.current_project_state.root);
    editor_config = &context_.current_project_state.editor_config.Resolve(viewport.path());
  }
  ApplyDetectedIndentAfterPreferences(
      viewport, [this](std::string_view id) { return GetSettingValue(id); }, *editor_config);
}

void WorkspaceShell::ApplyEditorPreferences(editor::TextViewport& viewport,
                                            bool include_contract) const {
  ApplyEditorPreferences(viewport, include_contract,
                         ResolveEditorPreferenceSettings(
                             [this](std::string_view id) { return GetSettingValue(id); }));
}

void WorkspaceShell::ApplyEditorPreferences(editor::TextViewport& viewport,
                                            bool include_contract,
                                            const EditorPreferenceSettings& settings) const {
  if (glue_->remote_host_service != nullptr && !viewport.path().empty() &&
      glue_->remote_host_service->IsHostFileCopy(viewport.path())) {
    viewport.SetReadOnly(true);
  }
  static const project::EditorConfigProperties kNoEditorConfig;
  const project::EditorConfigProperties* editor_config = &kNoEditorConfig;
  if (settings.editorconfig_enabled && !viewport.path().empty()) {
    context_.current_project_state.editor_config.EnsureProjectRoot(
        context_.current_project_state.root);
    editor_config = &context_.current_project_state.editor_config.Resolve(viewport.path());
  }

  const EditorPreferences& preferences = context_.current_project_state.editor_preferences;
  ApplyResolvedIndent(viewport, ResolveIndent(viewport.own_indent(), *editor_config,
                                              ResolvedIndent{.tab_size = preferences.tab_size,
                                                             .indent_width = preferences.indent_width,
                                                             .soft_tabs = preferences.soft_tabs}));
  viewport.SetSoftWrap(preferences.soft_wrap);
  viewport.SetSaveTrimTrailingWhitespace(
      editor_config->trim_trailing_whitespace.value_or(settings.trim_trailing_whitespace));
  viewport.SetSaveEnsureFinalNewline(
      editor_config->insert_final_newline.value_or(settings.ensure_final_newline));
  viewport.SetSaveLineEnding(editor_config->line_ending.has_value()
                                 ? editor_config->line_ending
                                 : settings.save_line_ending);

  if (!include_contract) {
    return;
  }
  viewport.SetLanguageContractView(language_contract_.ResolveEditorView(
      viewport.language_id(), settings.auto_close, settings.surround, settings.smart_indent));
}

void WorkspaceShell::ApplyLanguageModeToActiveEditor(const std::string& language) {
  editor::TextViewport* viewport = ActiveEditorViewport();
  if (viewport == nullptr || viewport->language_override() == language) {
    return;
  }
  const std::filesystem::path path = viewport->path();
  if (!path.empty()) {
    NotifyLspBufferClose(path);
  }
  viewport->SetLanguageOverride(language);
  ApplyEditorPreferencesToAllTabs();
  if (!path.empty()) {
    NotifyPluginBufferOpen(path);
    NotifyLspBufferOpen(path);
  }
  RequestPreparedFrame();
}

void WorkspaceShell::ApplyEditorPreferencesToAllTabs(bool refresh_language_contracts) {
  util::AddPerformanceCounter(util::PerfCounterId::FrameApplyEditorPreferencesAllTabsCalls);
  editor_text_renderer_.SetFontPointSize(
      static_cast<float>(context_.current_project_state.editor_preferences.font_size));
  const std::string font_family = GetSettingValue("editor.font_family").value_or("");
  editor_text_renderer_.SetFontFamily(font_family);
  text_renderer_.SetFontFamily(font_family);
  NoteLayoutInputsChanged();
  tab_strip_service_.InvalidateTabStripGeometry();
  RequestPreparedFrame();
  const EditorPreferenceSettings settings = ResolveEditorPreferenceSettings(
      [this](std::string_view id) { return GetSettingValue(id); });
  for (auto& group : context_.current_project_state.editor_groups) {
    ApplyEditorPreferences(group.welcome_surface.viewport, refresh_language_contracts, settings);
    for (auto& tab : group.open_tabs) {
      switch (tab.kind) {
        case TabEntry::Kind::Editor:
          if (tab.editor_state.has_value()) {
            ApplyEditorPreferences(tab.editor_state->viewport, refresh_language_contracts,
                                   settings);
          }
          break;
        case TabEntry::Kind::Compare:
          if (tab.compare.has_value()) {
            ApplyEditorPreferences(tab.compare->right_viewport, refresh_language_contracts,
                                   settings);
          }
          break;
        case TabEntry::Kind::Merge:
          if (tab.merge.has_value()) {
            ApplyEditorPreferences(tab.merge->result_viewport, refresh_language_contracts,
                                   settings);
          }
          break;
      }
    }
  }
}

namespace {

bool SettleFoldReveal(TabEntry::EditorTabState* editor_tab, editor::TextViewport* viewport) {
  if (editor_tab == nullptr || viewport == nullptr || !RevealCaretsInsideCollapsedFolds(*editor_tab, *viewport)) {
    return false;
  }
  viewport->RevealCaret();
  return true;
}

}

editor::FoldingModel* WorkspaceShell::EnsureActiveFoldingModelFresh() {
  TabEntry::EditorTabState* editor_tab = ActiveEditorTab();
  editor::TextViewport* viewport = ActiveEditorViewport();
  editor::FoldingModel* model = EnsureFoldingModelFreshForTab(editor_tab, viewport);
  if (SettleFoldReveal(editor_tab, viewport)) {
    RequestPreparedFrame();
  }
  return model;
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
  const LanguageContract* contract = nullptr;
  {
    util::PerformanceTrace::Scope s("WorkspaceShell::EnsureFoldingModelFreshForTab::Language");
    contract = language_contract_.ResolveView(active_viewport->language_id()).contract;
  }
  {
    util::PerformanceTrace::Scope s("WorkspaceShell::EnsureFoldingModelFreshForTab::EnsureFresh");
    EnsureFoldingModelFresh(*editor_tab, *active_viewport, contract,
                            context_.current_project_state.editor_preferences.tab_size,
                            setting_enabled("editor.fold.enabled", true),
                            active_viewport->visible_lines());
  }
  {
    util::PerformanceTrace::Scope s("WorkspaceShell::EnsureFoldingModelFreshForTab::SetModel");
    editor_tab->viewport.SetFoldingModel(
        editor_tab->folding_model->HasFolds() ? editor_tab->folding_model.get() : nullptr);
  }
  return editor_tab->folding_model.get();
}

void WorkspaceShell::RefreshEditorFoldingModels() {
  util::AddPerformanceCounter(util::PerfCounterId::FrameRefreshEditorFoldingModelsCalls);
  for (EditorGroup& group : context_.current_project_state.editor_groups) {
    TabEntry::EditorTabState* editor_tab = GroupActiveEditorTab(group);
    editor::TextViewport* viewport = GroupActiveViewport(group);
    EnsureFoldingModelFreshForTab(editor_tab, viewport);
    if (SettleFoldReveal(editor_tab, viewport)) {
      RequestPreparedFrame();
    }
  }
}

void WorkspaceShell::ActivateTab(std::size_t index) {
  if (index != context_.current_project_state.focused_group().active_tab_index) {
    MaybeAutosaveDirtyTabs(true);
  }
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
  highlight_prefetch_service_.PrewarmForViewport(viewport, [viewport]() {
    return editor::runtime_syntax::DetectState(viewport->path(), viewport->lines()).definition_id;
  });
  if (auto backfill = viewport->TakeHighlightCheckpointBackfillRequest()) {
    highlight_prefetch_service_.RequestCheckpoints(std::move(*backfill));
  }
  const std::size_t top =
      std::min(viewport->VisualRowLineIndex(viewport->scroll_line()), line_count - 1);
  const std::size_t visible = std::max<std::size_t>(viewport->visible_lines(), 1);
  const std::size_t count =
      std::min({visible * 3, line_count - top, editor::TextViewport::HighlightCacheCapacity()});
  if (!viewport->HasHighlightPrefetchGap(top, count)) {
    return;
  }
  if (!highlight_prefetch_service_.Admit(editor::HighlightPrefetchService::WindowKey{
          .viewport = viewport,
          .content_revision = viewport->content_revision(),
          .syntax_revision = viewport->syntax_revision(),
          .start_line = top,
          .count = count})) {
    return;
  }
  highlight_prefetch_service_.Request(viewport->BuildHighlightPrefetchRequest(top, count));
}

void WorkspaceShell::ConsumeHighlightPrefetchResults() {
  editor::TextViewport* active_viewport = ActiveEditorViewport();
  for (auto& result : highlight_prefetch_service_.DrainResults()) {
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

void WorkspaceShell::SyncActiveEditorTabMetadata() {
  MakeEditorTabService().SyncActiveEditorTabMetadata();
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
  editor_tab->content = EditorTabState::Content::Ready;
  editor_tab->restore.path = configured_view.path().lexically_normal();
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
  RequestPreparedFrame();
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
  return context_.current_project_state.focused_group().active_tab()
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
    if (MakeEditorTabService().CloseWouldDiscardEdits(index)) {
      dirty_indices.push_back(index);
    }
  }

  if (!dirty_indices.empty()) {
    if (indices.size() == 1) {
      MakePromptSurfaceService().ShowDirtyPromptForTab(indices.front());
    } else {
      MakePromptSurfaceService().ShowDirtyPromptForTabs(std::move(indices), std::move(dirty_indices));
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
  for (const EditorGroup& group : context_.current_project_state.editor_groups) {
    for (const auto& tab : group.open_tabs) {
      if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() && tab.compare->right_editable &&
          !tab.compare->load_pending && !tab.compare->right_viewport.path().empty()) {
        paths.push_back(tab.compare->right_viewport.path().lexically_normal());
        continue;
      }
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
        continue;
      }
      const std::filesystem::path path = EditorViewPath(*tab.editor_state);
      if (!path.empty()) {
        paths.push_back(path.lexically_normal());
      }
    }
  }
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  EditorTabService& editor_tabs = MakeEditorTabService();
  bool noted_any = false;
  for (const auto& path : paths) {
    if (editor_tabs.NoteMissedDiskChangeUnderEdits(path)) {
      SetEditorBanner(context_.current_project_state, EditorBannerState::Kind::ExternalChange, path);
      noted_any = true;
    }
    ReloadCleanEditorTabsForPath(path);
  }
  if (noted_any) {
    RequestPreparedFrame();
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
  save_formatter_service_.FlushPendingRuns();
  MakeEditorTabService().Close(index);
}

namespace {

std::optional<std::filesystem::path> OpenBufferViewPath(const TabEntry& tab) {
  if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
    std::filesystem::path view_path =
        (tab.editor_state->content_pending() ? tab.editor_state->restore.path
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

}

std::size_t WorkspaceShell::CountOpenBufferViews(const std::filesystem::path& path) const {
  if (path.empty()) {
    return 0;
  }
  const std::filesystem::path normalized = path.lexically_normal();
  std::size_t count = 0;
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

}
