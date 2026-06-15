#include "workspace/WorkspaceShell.h"

#include <filesystem>
#include <set>
#include <string_view>
#include <unordered_set>

#include "util/PerformanceTrace.h"
#include "util/StartupTrace.h"
#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceCommandRegistry.h"

namespace microide::workspace {

namespace {

bool IsVirtualDocumentUri(std::string_view text) {
  return text.starts_with("virtual://");
}

template <typename Callback>
void ForEachOpenEditableBuffer(const ProjectWorkspaceState& state, Callback&& callback) {
  for (const auto& tab : state.open_tabs) {
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      for (const auto& view : tab.editor_state->views) {
        const std::filesystem::path path =
            (view.needs_restore ? view.restored_path : view.viewport.path()).lexically_normal();
        if (path.empty()) {
          continue;
        }
        if (view.needs_restore) {
          callback(path, nullptr);
        } else {
          callback(path, &view.viewport);
        }
      }
      continue;
    }
    if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
        tab.compare->right_editable && !tab.compare->right_viewport.path().empty()) {
      callback(tab.compare->right_viewport.path().lexically_normal(), &tab.compare->right_viewport);
      continue;
    }
    if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value() &&
        !tab.merge->result_viewport.path().empty()) {
      callback(tab.merge->result_viewport.path().lexically_normal(), &tab.merge->result_viewport);
    }
  }
}

}  // namespace

WorkspaceShell::WorkspaceShell() {
  virtual_document_registry_.SetOnChange(
      [this](const std::string& uri) { ReloadVirtualDocumentTabs(uri); });
  tab_strip_chrome_.Configure(
      context_, tab_strip_service_, layout_mode_service_, output_channels_,
      WorkspaceTabStripChrome::Operations{
          .project_tab_display_title =
              [this](std::size_t index) { return ProjectTabDisplayTitle(index); },
          .project_tab_tooltip_label =
              [this](std::size_t index) { return ProjectTabTooltipLabel(index); },
          .project_catalog_entry =
              [this](std::size_t index) -> const ProjectWorkspaceState* {
                return ProjectCatalogEntry(index);
              },
          .project_catalog_root =
              [this](std::size_t index) { return ProjectCatalogRoot(index); },
          .editor_tab_display_title =
              [this](std::size_t index) { return TabDisplayTitle(index); },
          .editor_tab_tooltip_label =
              [this](std::size_t index) { return TabTooltipLabel(index); },
          .current_window_rect = [this]() { return CurrentWindowRect(); },
          .measure_width =
              [this](std::string_view text) { return text_renderer_.MeasureWidth(text); },
          .ensure_output_channel_tab_open =
              [this](std::string_view id) { EnsureOutputChannelTabOpen(id); },
          .close_output_channel_tab =
              [this](std::string_view id) { CloseOutputChannelTab(id); },
          .close_terminal_tab = [this](std::size_t index) { CloseTerminalTab(index); },
          .request_bottom_panel_redraw = [this]() { RequestBottomPanelRedraw(); },
      });
  lsp_service_.Configure(
      context_, completion_registry_, code_action_registry_,
      LspService::Operations{
          .active_editable_viewport = [this]() { return ActiveEditableViewport(); },
          .refresh_problems_sidebar = [this]() { RefreshProblemsSidebar(); },
          .request_editor_surface_redraw = [this]() { RequestEditorSurfaceRedraw(); },
          .request_chrome_redraw = [this]() { RequestChromeRedraw(); },
          .request_bottom_panel_redraw = [this]() { RequestBottomPanelRedraw(); },
      });
  assist_service_.Configure(
      context_, plugin_runtime_, output_channels_, language_contract_,
      AssistService::Operations{
          .get_setting_value =
              [this](std::string_view id) { return GetSettingValue(id); },
          .active_editable_viewport = [this]() { return ActiveEditableViewport(); },
          .active_editor_viewport = [this]() { return ActiveEditorViewport(); },
          .active_editor_tab = [this]() { return ActiveEditorTab(); },
          .active_compare_tab = [this]() { return ActiveCompareTab(); },
          .active_merge_tab = [this]() { return ActiveMergeTab(); },
          .lsp_client_for_viewport =
              [this](const editor::TextViewport& viewport, std::string* language_id) {
                return LspClientForViewport(viewport, language_id);
              },
          .current_lsp_manager = [this]() -> LspManager& { return CurrentLspManager(); },
          .ensure_lsp_document_open =
              [this](const editor::TextViewport& viewport,
                     LspClient& client,
                     std::string_view language_id) {
                EnsureLspDocumentOpen(viewport, client, language_id);
              },
          .begin_tracked_lsp_request = [this]() { BeginTrackedLspRequest(); },
          .finish_tracked_lsp_request = [this]() { FinishTrackedLspRequest(); },
          .show_overlay = [this](OverlayMode mode) { ShowOverlay(mode); },
          .dismiss_overlay = [this](bool focus_editor) { DismissOverlay(focus_editor); },
          .request_overlay_redraw = [this]() { RequestOverlayRedraw(); },
          .execute_command_name =
              [this](std::string_view command_name,
                     const std::vector<std::string>& args,
                     std::string* error_message) {
                return ExecuteCommandName(command_name, args, ActionSource::Menu,
                                          error_message);
              },
          .open_file_in_new_tab =
              [this](const std::filesystem::path& path) { return OpenFileInNewTab(path); },
          .reset_caret_blink = [this]() { ResetCaretBlink(); },
          .request_focused_editor_redraw = [this]() { RequestFocusedEditorRedraw(); },
          .request_active_editable_last_change_redraw =
              [this]() { RequestActiveEditableLastChangeRedraw(); },
          .request_active_editable_blame_neighborhood_redraw =
              [this](std::size_t before_line, std::size_t after_line) {
                RequestActiveEditableBlameNeighborhoodRedraw(before_line, after_line);
              },
          .request_tab_strip_redraw = [this]() { RequestChromeRedraw(); },
          .refresh_compare_tab_derived_state =
              [this](CompareTabState& tab) { RefreshCompareTabDerivedState(tab); },
          .sync_compare_selection_from_viewport =
              [this](CompareTabState& tab, bool keep_top_line) {
                SyncCompareSelectionFromViewport(tab, keep_top_line);
              },
          .update_merge_tracking_after_viewport_edit =
              [this](MergeTabState& tab,
                     const std::vector<std::string>& before_lines,
                     const std::optional<editor::SelectionRange>& selection_before,
                     const editor::TextPosition& cursor_before) {
                UpdateMergeTrackingAfterViewportEdit(tab, before_lines, selection_before,
                                                     cursor_before);
              },
      });
  plugin_runtime_.SetCallbacks(plugin::PluginHost::Callbacks{
      .is_command_name_available =
          [](std::string_view name) { return FindWorkspaceActionByCommand(name) == nullptr; },
      .open_file =
          [this](const plugin::PluginHost::OpenFileRequest& request) {
            const std::filesystem::path normalized_path = request.path.lexically_normal();
            const bool opened = IsVirtualDocumentUri(request.path.generic_string())
                                    ? OpenVirtualDocumentInNewTab(request.path.generic_string())
                                    : OpenFileInNewTab(normalized_path);
            if (!opened) {
              return false;
            }
            if (request.line > 0) {
              const std::size_t target_line = request.line - 1;
              const std::size_t target_column = request.column > 0 ? request.column - 1 : 0;
              if (editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
                viewport->MoveCursorTo(target_line, target_column);
              }
            }
            return true;
          },
      .active_buffer =
          [this]() -> std::optional<plugin::PluginHost::ActiveBuffer> {
            const editor::TextViewport* viewport = ActiveEditableViewport();
            if (viewport == nullptr || viewport->path().empty()) {
              return std::nullopt;
            }
            return plugin::PluginHost::ActiveBuffer{
                .path = viewport->path().lexically_normal(),
                .line = viewport->cursor_line() + 1,
                .column = viewport->cursor_column() + 1,
            };
          },
      .show_sidebar =
          [this](std::string_view id) {
            return ActionCoordinator(MakeActionContext()).Execute(ActionId::SidebarShow, {std::string(id)},
                                                    ActionSource::Shortcut);
          },
      .publish_diagnostics =
          [this](std::string_view owner,
                 const std::filesystem::path& path,
                 std::vector<editor::Diagnostic> diagnostics) {
            if (context_.current_project_state.diagnostics_store.ReplaceForOwnerFile(owner, path, std::move(diagnostics))) {
              RefreshProblemsSidebar();
              RequestEditorSurfaceRedraw();
            }
          },
      .clear_file_diagnostics =
          [this](std::string_view owner, const std::filesystem::path& path) {
            if (context_.current_project_state.diagnostics_store.ClearOwnerFile(owner, path)) {
              RefreshProblemsSidebar();
              RequestEditorSurfaceRedraw();
            }
          },
      .clear_owner_diagnostics =
          [this](std::string_view owner) {
            if (context_.current_project_state.diagnostics_store.ClearOwner(owner)) {
              RefreshProblemsSidebar();
              RequestEditorSurfaceRedraw();
            }
          },
      .error_sink =
          [this](const std::string& text) {
            output_channels_.AppendLine("plugins.error", "Plugin Errors", text);
            plugin_runtime_.AppendError(text);
          },
      .log_sink =
          [this](const std::string& text) {
            output_channels_.AppendLine("plugins.log", "Plugin Log", text);
            plugin_runtime_.AppendLog(text);
          },
      .get_setting =
          [this](std::string_view id) {
            return GetSettingValue(id);
          },
      .request_status_redraw =
          [this]() {
            RequestChromeRedraw();
          },
      .show_notification =
          [this](const std::string& level, const std::string& message) {
            notification_service_.Show(NotificationService::ToneFromLevel(level), message,
                                       SDL_GetTicks());
            // A full redraw is fine here: notifications are infrequent, event-driven
            // posts (never per-frame polling), so this never spins the CPU.
            RequestFullRedraw();
          },
  });
}

void WorkspaceShell::RebuildPhase3Registries() {
  formatter_registry_ = FormatterRegistry{};
  save_participant_registry_ = SaveParticipantRegistry{};
  completion_registry_ = CompletionRegistry{};
  code_action_registry_ = CodeActionRegistry{};
  tool_registry_ = ToolRegistry{};
  test_controller_.Clear();
  std::unordered_set<std::string> active_language_servers;

  const auto& host = plugin_runtime_.Host();
  language_contract_.Refresh(host, [this](std::string_view id) {
    return GetSettingValue(id);
  });

  for (const auto& formatter : host.ContributedFormatters()) {
    formatter_registry_.Register(FormatterSpec{
        .id = formatter.id,
        .language_id = formatter.language_id,
        .label = formatter.label,
        .command = formatter.command,
        .plugin_id = formatter.plugin_id,
    });
  }
  for (const auto& participant : host.ContributedSaveParticipants()) {
    save_participant_registry_.Register(
        SaveParticipantSpec{.id = participant.id, .plugin_id = participant.plugin_id});
  }
  for (const auto& completion : host.ContributedCompletions()) {
    completion_registry_.Register(CompletionProviderSpec{
        .id = completion.id,
        .plugin_id = completion.plugin_id,
        .language_id = completion.language_id,
        .trigger_characters = completion.trigger_characters,
    });
  }
  for (const auto& code_action : host.ContributedCodeActions()) {
    code_action_registry_.Register(CodeActionProviderSpec{
        .id = code_action.id,
        .plugin_id = code_action.plugin_id,
        .language_id = code_action.language_id,
    });
  }
  for (const auto& language_server : host.ContributedLanguageServers()) {
    if (language_server.command.empty() || language_server.language_ids.empty()) {
      continue;
    }
    for (const auto& language_id : language_server.language_ids) {
      active_language_servers.insert(language_id);
    }
    CurrentLspManager().RegisterServer(language_server.language_ids, language_server.command,
                                       "file://" + context_.current_project_state.root.generic_string(),
                                       context_.current_project_state.root.generic_string(),
                                       false, language_server.initialization_options,
                                       language_server.settings, language_server.sandbox);
  }
  CurrentLspManager().BeginShutdownServersNotIn(active_language_servers);
  for (const auto& tool : host.ContributedTools()) {
    tool_registry_.Register(ToolSpec{
        .id = tool.id,
        .plugin_id = tool.plugin_id,
        .label = tool.label,
        .platform = tool.platform,
        .download_url = tool.download_url,
        .sha256 = tool.sha256,
        .install_dir = tool.install_dir,
    });
  }

  // Plugin-contributed language metadata can change auto-close/surround/indent
  // behavior. Re-apply editor preferences so every open viewport receives the
  // refreshed contract and current toggle settings.
  ApplyEditorPreferencesToAllTabs();
}

void WorkspaceShell::RebuildPhase4Registries() {
  scm_registry_ = ScmRegistry{};
  annotation_registry_ = AnnotationRegistry{};

  const auto& host = plugin_runtime_.Host();
  for (const auto& provider : host.ContributedScmProviders()) {
    scm_registry_.Register(ScmProviderSpec{
        .id = provider.id,
        .label = provider.label,
        .plugin_id = provider.plugin_id,
    });
  }
  for (const auto& provider : host.ContributedAnnotationProviders()) {
    annotation_registry_.Register(AnnotationProviderSpec{
        .id = provider.id,
        .label = provider.label,
        .type = provider.type,
        .language_id = provider.language_id,
        .plugin_id = provider.plugin_id,
    });
  }
}

bool WorkspaceShell::ReloadPluginsForCurrentProject(PluginReloadRequest request) {
  util::StartupTrace::Scope trace_scope("WorkspaceShell::ReloadPluginsForCurrentProject");
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::ReloadPluginsForCurrentProject");
  ++reload_plugins_invocation_count_;
  if (startup_options_.plugins_disabled()) {
    request.syntax_definitions = false;
  }
  // Apply the user's per-plugin disable set before reloading so disabled plugins skip setup.
  plugin_runtime_.Host().SetDisabledPlugins(context_.disabled_plugin_ids);
  bool clean_reload;
  {
    util::StartupTrace::Scope plugin_scope("PluginRuntime::Reload");
    util::PerformanceTrace::Scope perf_plugin_scope(
        "WorkspaceShell::ReloadPluginsForCurrentProject::PluginRuntimeReload");
    clean_reload = plugin_runtime_.Reload(context_.current_project_state.root,
                                          request.syntax_definitions);
  }
  {
    util::StartupTrace::Scope registry_scope("RebuildRegistries");
    util::PerformanceTrace::Scope perf_registry_scope(
        "WorkspaceShell::ReloadPluginsForCurrentProject::RebuildRegistries");
    RebuildPhase3Registries();
    RebuildPhase4Registries();
  }
  {
    util::StartupTrace::Scope syntax_scope("InvalidateSyntaxCaches");
    util::PerformanceTrace::Scope perf_syntax_scope(
        "WorkspaceShell::ReloadPluginsForCurrentProject::InvalidateSyntaxCaches");
    const std::span<const std::string_view> changed_languages =
        request.syntax_definitions ? plugin_runtime_.ChangedSyntaxLanguages()
                                   : std::span<const std::string_view>{};
    if (!changed_languages.empty()) {
      InvalidateRuntimeSyntaxStateCaches(changed_languages);
    }
  }
  {
    util::StartupTrace::Scope sidebar_selection_scope("NormalizeSidebarViewSelection");
    util::PerformanceTrace::Scope perf_sidebar_selection_scope(
        "WorkspaceShell::ReloadPluginsForCurrentProject::NormalizeSidebarViewSelection");
    NormalizeSidebarViewSelection();
  }
  {
    util::StartupTrace::Scope plugin_sidebar_scope("RefreshPluginSidebar");
    util::PerformanceTrace::Scope perf_plugin_sidebar_scope(
        "WorkspaceShell::ReloadPluginsForCurrentProject::RefreshPluginSidebar");
    RefreshPluginSidebar();
  }
  if (ActiveSidebarMode() == SidebarMode::Git) {
    util::StartupTrace::Scope git_sidebar_scope("RefreshGitSidebar");
    util::PerformanceTrace::Scope perf_git_sidebar_scope(
        "WorkspaceShell::ReloadPluginsForCurrentProject::RefreshGitSidebar");
    RefreshGitSidebar();
  }
  {
    util::StartupTrace::Scope problems_sidebar_scope("RefreshProblemsSidebar");
    util::PerformanceTrace::Scope perf_problems_sidebar_scope(
        "WorkspaceShell::ReloadPluginsForCurrentProject::RefreshProblemsSidebar");
    RefreshProblemsSidebar();
  }
  {
    util::StartupTrace::Scope open_buffers_scope("NotifyPluginsAboutOpenBuffers");
    util::PerformanceTrace::Scope perf_open_buffers_scope(
        "WorkspaceShell::ReloadPluginsForCurrentProject::NotifyPluginsAboutOpenBuffers");
    NotifyPluginsAboutOpenBuffers(request.replay_buffer_opens, request.open_lsp_documents);
  }
  {
    util::StartupTrace::Scope chrome_redraw_scope("RequestChromeRedraw");
    util::PerformanceTrace::Scope perf_chrome_redraw_scope(
        "WorkspaceShell::ReloadPluginsForCurrentProject::RequestChromeRedraw");
    RequestChromeRedraw();
  }
  {
    util::StartupTrace::Scope editor_redraw_scope("RequestEditorSurfaceRedraw");
    util::PerformanceTrace::Scope perf_editor_redraw_scope(
        "WorkspaceShell::ReloadPluginsForCurrentProject::RequestEditorSurfaceRedraw");
    RequestEditorSurfaceRedraw();
  }
  return clean_reload;
}

void WorkspaceShell::RefreshPluginSurfacesForReactivation() {
  util::PerformanceTrace::Scope perf_scope(
      "WorkspaceShell::RefreshPluginSurfacesForReactivation");
  RebuildPhase3Registries();
  RebuildPhase4Registries();
  NormalizeSidebarViewSelection();
  RefreshPluginSidebar();
  if (ActiveSidebarMode() == SidebarMode::Git) {
    RefreshGitSidebar();
  }
  RequestChromeRedraw();
  RequestEditorSurfaceRedraw();
}

bool WorkspaceShell::ReloadPluginsIfPluginAssetsChanged(bool force_check) {
  const bool changed = plugin_runtime_.ConsumeAssetChanges(force_check);
  if (!changed) {
    return false;
  }

  ReloadPluginsForCurrentProject();
  const std::string summary = "Detected plugin asset changes: " + PluginRuntimeReloadSummary();
  output_channels_.AppendLine("plugins.log", "Plugin Log", summary);
  plugin_runtime_.AppendLog(summary);
  return true;
}

void WorkspaceShell::InvalidateRuntimeSyntaxStateCaches(
    std::span<const std::string_view> changed_languages) {
  if (changed_languages.empty()) {
    return;
  }

  std::unordered_set<std::string_view> changed_language_set(changed_languages.begin(),
                                                             changed_languages.end());
  const auto should_invalidate_viewport = [&changed_language_set](const editor::TextViewport& viewport) {
    const std::string language =
        editor::runtime_syntax::DetectFiletype(viewport.path(), viewport.lines());
    return !language.empty() &&
           changed_language_set.contains(std::string_view(language));
  };

  if (editor::TextViewport* viewport = ActiveEditorViewport();
      viewport != nullptr && should_invalidate_viewport(*viewport)) {
    viewport->InvalidateSyntaxHighlighting();
  }

  for (auto& tab : context_.current_project_state.open_tabs) {
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      for (auto& view : tab.editor_state->views) {
        if (!view.needs_restore && should_invalidate_viewport(view.viewport)) {
          view.viewport.InvalidateSyntaxHighlighting();
        }
      }
      continue;
    }

    if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
        !tab.compare->right_viewport.path().empty()) {
      const std::string language = editor::runtime_syntax::DetectFiletype(
          tab.compare->right_viewport.path(), tab.compare->right_viewport.lines());
      if (language.empty() || !changed_language_set.contains(std::string_view(language))) {
        continue;
      }
      RefreshCompareTabDerivedState(*tab.compare);
      continue;
    }

    if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
      auto& merge_tab = *tab.merge;
      const std::string language =
          editor::runtime_syntax::DetectFiletype(merge_tab.result_viewport.path(),
                                                 merge_tab.result_viewport.lines());
      if (language.empty() || !changed_language_set.contains(std::string_view(language))) {
        continue;
      }
      merge_tab.incoming_initial_syntax_state =
          editor::SyntaxHighlighter::InitialState(merge_tab.output_path, merge_tab.model.incoming_lines);
      merge_tab.current_initial_syntax_state =
          editor::SyntaxHighlighter::InitialState(merge_tab.output_path, merge_tab.model.current_lines);
      merge_tab.incoming_current_syntax_state = merge_tab.incoming_initial_syntax_state;
      merge_tab.current_current_syntax_state = merge_tab.current_initial_syntax_state;
      merge_tab.incoming_tokens.assign(merge_tab.model.incoming_lines.size(), {});
      merge_tab.current_tokens.assign(merge_tab.model.current_lines.size(), {});
      merge_tab.incoming_syntax_rows_tokenized = 0;
      merge_tab.current_syntax_rows_tokenized = 0;
      merge_tab.result_viewport.InvalidateSyntaxHighlighting();
    }
  }
}

std::string WorkspaceShell::PluginRuntimeReloadSummary() const {
  return plugin_runtime_.ReloadSummary();
}

void WorkspaceShell::NotifyPluginsAboutOpenBuffers(bool replay_plugin_buffer_opens,
                                                   bool open_lsp_documents) {
  if (!plugin_runtime_.enabled()) {
    return;
  }
  std::set<std::filesystem::path> opened_paths;
  ForEachOpenEditableBuffer(context_.current_project_state,
                            [&](const std::filesystem::path& path,
                                const editor::TextViewport* viewport) {
                              if (!opened_paths.insert(path).second) {
                                return;
                              }
                              if (replay_plugin_buffer_opens) {
                                plugin_runtime_.Host().OnBufferOpen(path);
                              }
                              if (viewport == nullptr || !open_lsp_documents) {
                                return;
                              }
                              std::string language_id;
                              LspClient* client = LspClientForViewport(*viewport, &language_id);
                              if (client != nullptr) {
                                EnsureLspDocumentOpen(*viewport, *client, language_id);
                              }
                            });
}

void WorkspaceShell::NotifyPluginBufferOpen(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  if (!plugin_runtime_.enabled() && normalized_path.empty()) {
    return;
  }
  if (plugin_runtime_.enabled() && !normalized_path.empty()) {
    plugin_runtime_.Host().OnBufferOpen(normalized_path);
  }
}

void WorkspaceShell::NotifyPluginBufferSave(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  if (plugin_runtime_.enabled() && !normalized_path.empty()) {
    plugin_runtime_.Host().OnBufferSave(normalized_path);
  }
  if (normalized_path.empty()) {
    return;
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().lexically_normal() != normalized_path) {
    return;
  }
  const std::string language_id = editor::runtime_syntax::DetectFiletype(viewport->path(), viewport->lines());
  if (language_id.empty()) {
    return;
  }
  LspClient* client = CurrentLspManager().FindStartedServer(language_id);
  if (client == nullptr) {
    return;
  }
  client->SetDiagnosticsCallback([this, project = &context_.current_project_state](
                                     std::string uri,
                                     std::vector<LspClient::Diagnostic> diagnostics) {
    PublishLspDiagnostics(*project, std::move(uri), std::move(diagnostics));
  });
  EnsureLspDocumentOpen(*viewport, *client, language_id);
  client->DidSave("file://" + normalized_path.generic_string());
}

void WorkspaceShell::NotifyLspBufferClose(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  if (normalized_path.empty()) {
    return;
  }
  const std::string language_id = editor::runtime_syntax::DetectFiletype(normalized_path, {});
  if (language_id.empty()) {
    return;
  }
  LspClient* client = CurrentLspManager().FindStartedServer(language_id);
  if (client == nullptr) {
    return;
  }
  const std::string uri = "file://" + normalized_path.generic_string();
  if (client->HasOpenDocument(uri)) {
    client->DidClose(uri);
  }
  context_.current_project_state.diagnostics_store.ClearOwnerFile("lsp", normalized_path);
  RefreshProblemsSidebar();
  RequestEditorSurfaceRedraw();
}

bool WorkspaceShell::ConsumePluginAsyncProcessCallbacks() {
  const bool consumed = plugin_runtime_.ConsumeAsyncProcessCallbacks();
  if (consumed) {
    RequestFullRedraw();
  }
  return consumed;
}

}  // namespace microide::workspace
