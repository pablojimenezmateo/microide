#include "workspace/shell/WorkspaceShell.h"

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
  // Single-viewport entry point: resolve the shared snapshot for this one call.
  // The all-tabs path resolves once and calls the overload below directly.
  ApplyEditorPreferences(viewport, include_contract,
                         ResolveEditorPreferenceSettings(
                             [this](std::string_view id) { return GetSettingValue(id); }));
}

void WorkspaceShell::ApplyEditorPreferences(editor::TextViewport& viewport,
                                            bool include_contract,
                                            const EditorPreferenceSettings& settings) const {
  // A `.editorconfig` in the repository is the project author's statement of how
  // its files are formatted, so it wins over our own settings for the properties
  // it names — matching VSCode, where EditorConfig overrides both the configured
  // indent and detectIndentation. Properties it does not name fall through to the
  // user/project setting untouched. Resolution is memoized per path; a project
  // with no `.editorconfig` costs one hash lookup here.
  static const project::EditorConfigProperties kNoEditorConfig;
  const project::EditorConfigProperties* editor_config = &kNoEditorConfig;
  if (settings.editorconfig_enabled && !viewport.path().empty()) {
    // Point the resolver at the active root before asking. A project-catalog slot
    // that was reset carries its root but a fresh resolver, and an unset root
    // resolves to "no opinion" silently — this is a path compare in the common
    // case, and cheap next to the filetype detection below.
    context_.current_project_state.editor_config.EnsureProjectRoot(
        context_.current_project_state.root);
    editor_config = &context_.current_project_state.editor_config.Resolve(viewport.path());
  }

  const EditorPreferences& preferences = context_.current_project_state.editor_preferences;
  // Cheap per-viewport setters, always applied.
  viewport.SetTabSize(editor_config->tab_size.value_or(preferences.tab_size));
  viewport.SetIndentWidth(editor_config->indent_width.value_or(preferences.indent_width));
  viewport.SetSoftTabs(editor_config->soft_tabs.value_or(preferences.soft_tabs));
  viewport.SetSoftWrap(preferences.soft_wrap);
  viewport.SetSaveTrimTrailingWhitespace(
      editor_config->trim_trailing_whitespace.value_or(settings.trim_trailing_whitespace));
  viewport.SetSaveEnsureFinalNewline(
      editor_config->insert_final_newline.value_or(settings.ensure_final_newline));
  // editor.line_endings: "auto" keeps the file's detected ending; lf/crlf force it.
  // `.editorconfig`'s end_of_line still wins where it names one.
  viewport.SetSaveLineEnding(editor_config->line_ending.has_value()
                                 ? editor_config->line_ending
                                 : settings.save_line_ending);

  // The per-tab contract work, skipped when no contract-affecting toggle changed —
  // see the family split in ApplyLiveSettings (TD-2026-07-17A-103). Both halves are
  // memoized now (TD-2026-08-03-110): the filetype behind the viewport's own
  // memo (nothing about the buffer changed, so a settings walk re-detects
  // nothing), and the view behind the contract table's shared cache, so 40 tabs
  // of one language share one view instead of owning 40 copies of it.
  if (!include_contract) {
    return;
  }
  viewport.SetLanguageContractView(language_contract_.ResolveEditorView(
      viewport.language_id(), settings.auto_close, settings.surround, settings.smart_indent));
}

void WorkspaceShell::ApplyEditorPreferencesToAllTabs(bool refresh_language_contracts) {
  util::AddPerformanceCounter(util::PerfCounterId::FrameApplyEditorPreferencesAllTabsCalls);
  // Font size is a project preference but the text renderer is shared across all
  // projects, so push the active project's value here -- this hook fires on
  // project activation, session restore, and live setting changes, so switching
  // projects automatically re-applies that project's font size. SetFontPointSize
  // no-ops when the size is unchanged, so the common case stays cheap.
  text_renderer_.SetFontPointSize(
      static_cast<float>(context_.current_project_state.editor_preferences.font_size));
  // Font family is a user-scope, renderer-global setting (not an EditorPreferences
  // field); push it alongside the size so project activation / session restore
  // re-applies it. SetFontFamily no-ops when the family is unchanged.
  text_renderer_.SetFontFamily(GetSettingValue("editor.font_family").value_or(""));
  NoteLayoutInputsChanged();
  tab_strip_service_.InvalidateTabStripGeometry();
  RequestWindowRedraw();
  // Apply to EVERY editor group, not just the focused one: preference fields (tab_size,
  // soft_wrap, auto_close, smart_indent, save-trim, …) live on each per-tab TextViewport,
  // so a split's non-focused pane would otherwise keep the stale config and render/behave
  // differently once clicked into. Mirrors the all-groups walks in ReloadCleanOpenBuffers
  // and RetargetOpenTabsForRename.
  // Split by setting family: the cheap runtime setters (tab size, wrap, save
  // normalization) always apply, but the O(tabs) filetype-detect + language-contract
  // rebuild is skipped when no contract-affecting toggle changed. A project with
  // thousands of restored tabs no longer rebuilds every tab's contract for a
  // font-size / save-flag / wrap checkbox change (TD-2026-07-17A-103).
  // Resolve the shared settings ONCE for the whole walk. They are shell-global,
  // so reading them per viewport made this O(tabs x settings) in lookups -- and
  // since each lookup returns std::optional<std::string> by value, O(tabs x
  // settings) in allocations too. Gated by `settings_change_many_tabs`.
  const EditorPreferenceSettings settings = ResolveEditorPreferenceSettings(
      [this](std::string_view id) { return GetSettingValue(id); });
  for (auto& group : context_.current_project_state.editor_groups) {
    ApplyEditorPreferences(group.welcome_surface.viewport, refresh_language_contracts, settings);
    for (auto& tab : group.open_tabs) {
      // Every EDITABLE viewport in the workspace, not just the plain editor tabs.
      // A compare tab's right pane and a merge tab's result pane are the same
      // `editor::TextViewport` reached by the same actions and the same key
      // handling, so a walk that skipped them left them on whatever preferences
      // happened to be current when the tab opened: tab size, indent width, soft
      // tabs, the two save-normalization flags, the save line ending and the
      // language contract all silently stopped following the settings there, and
      // Word Wrap looked like it did nothing at all (TD-2026-08-13-200).
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
  // Memoized: this runs for every editor group on every prepared frame, and the
  // fold model itself is fingerprint-gated to a no-op on a settled frame -- so
  // without the memo the only work the frame did here was re-detecting a
  // filetype that had not changed. Measured at ~0.85 ms/frame on a 50k-line
  // buffer, which was 60% of the whole editor_sticky_scroll_scroll scenario.
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
  // Resolve folding freshness once per prepared frame for every editor group's
  // active tab (both panes of a split fold independently). EnsureFoldingModelFreshForTab
  // is fingerprint-gated to a no-op on settled frames and folds/expands based on the
  // live editor.fold.enabled setting, so this subsumes the render path's former
  // per-pane refresh and fold-disabled clear. The render TUs then only read the
  // resolved model — no state mutation inside RenderClip.
  for (EditorGroup& group : context_.current_project_state.editor_groups) {
    TabEntry::EditorTabState* editor_tab = GroupActiveEditorTab(group);
    editor::TextViewport* viewport = GroupActiveViewport(group);
    EnsureFoldingModelFreshForTab(editor_tab, viewport);
    if (editor_tab != nullptr && viewport != nullptr &&
        RevealCaretsInsideCollapsedFolds(*editor_tab, *viewport)) {
      // The caret's row changed with the rows above it; this runs before the
      // frame's layout, so the scroll and the repaint both land in this frame.
      viewport->RevealCaret();
      RequestEditorSurfaceRedraw();
    }
  }
}

editor::FoldingModel* WorkspaceShell::GroupFoldingModelPtr(EditorGroup& group) {
  TabEntry::EditorTabState* editor_tab = GroupActiveEditorTab(group);
  return editor_tab == nullptr ? nullptr : editor_tab->folding_model.get();
}

void WorkspaceShell::ActivateTab(std::size_t index) {
  // Leaving the current buffer for a different tab is a focus change, so flush dirty
  // buffers before the switch (mirrors the window-blur autosave). MaybeAutosaveDirtyTabs
  // is a no-op unless editor.autosave == on_focus_change, so the common case pays only
  // one setting read.
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
  // On a tab switch, prewarm the newly active filetype's rule-regex compile on
  // the highlight worker so the first visible-line render does not pay it on the
  // UI thread. The service gates on the viewport identity, so detection (cheap:
  // bounded head scan, eager regexes) and the prewarm run once per switch, not
  // every settled frame.
  highlight_prefetch_service_.PrewarmForViewport(viewport, [viewport]() {
    return editor::runtime_syntax::DetectState(viewport->path(), viewport->lines()).definition_id;
  });
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

  // Prompt for the tabs whose close would actually discard edits. A dirty buffer
  // another pane still shows lives on when this view closes — VS Code closes
  // such an editor silently — so it gets no Save / Discard / Cancel here.
  std::vector<std::size_t> dirty_indices;
  dirty_indices.reserve(indices.size());
  for (std::size_t index : indices) {
    if (MakeEditorTabService().CloseWouldDiscardEdits(index)) {
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
  // Collect across every group so a buffer open only in the non-focused split is still
  // reloaded (ReloadCleanEditorTabsForPath itself reloads all groups per path).
  for (const EditorGroup& group : context_.current_project_state.editor_groups) {
    for (const auto& tab : group.open_tabs) {
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
  // Count across every editor group: a buffer shared by several panes of a split
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
