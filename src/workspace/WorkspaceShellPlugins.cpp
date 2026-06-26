#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <string_view>
#include <unordered_set>

#include "util/JsonValue.h"
#include "util/PathMatch.h"
#include "util/PerformanceTrace.h"
#include "util/StartupTrace.h"
#include "workspace/SettingFlags.h"
#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceCommandRegistry.h"
#include "workspace/WorkspacePersistenceCoordinator.h"

namespace microide::workspace {

namespace {

bool IsVirtualDocumentUri(std::string_view text) {
  return text.starts_with("virtual://");
}

// True when the live editor no longer matches the suggestion's anchor. Cheap
// integer/identity checks first; the path normalize is the last resort so the
// common "user typed / moved" case never allocates.
bool GhostTextStale(
    const editor::TextViewport* viewport,
    const ProjectWorkspaceState::PluginEditorPresentation::GhostText& ghost) {
  if (viewport == nullptr) {
    return true;
  }
  if (viewport->cursor_line() != ghost.anchor_line ||
      viewport->cursor_column() != ghost.anchor_column ||
      viewport->content_revision() != ghost.content_revision) {
    return true;
  }
  return !util::SamePathNormalized(viewport->path(), ghost.path);
}

// Drop the live suggestion and release the bundle if it drained empty.
void ResetGhostText(ProjectWorkspaceState& state) {
  state.plugin_presentation->ghost_text.reset();
  state.MaybeReleasePluginPresentation();
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
  // Bind the settings store to the layered backing vectors. The user layer is
  // stable for the shell's lifetime; the project layer is re-bound whenever the
  // active project state is moved (RebindProjectState / reset).
  settings_store_.BindUserLayer(&context_.user_settings);
  settings_store_.BindActiveProject(&context_.current_project_state.settings);
  // Load the persisted recent-projects/files MRU so the welcome surface and the
  // empty file finder can surface them immediately on first paint.
  recents_service_.Configure(persistence_service_);
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
  // Live theme pointer for baking semantic-token recolor decorations (theme_'s
  // address is stable; a theme switch mutates it in place).
  lsp_service_.SetTheme(&theme_);
  debug_service_.Configure(
      context_,
      DebugService::Operations{
          .append_console_output =
              [this](int session_id, const std::string& label,
                     const dap_protocol::DapOutputEvent& output) {
                AppendDebugConsoleOutput(session_id, label, output);
                control_channel_service_.OnDebugOutput(output.category, output.output);
              },
          .show_debug_console =
              [this](int session_id, const std::string& label) {
                ShowDebugConsole(session_id, label);
              },
          .remove_debug_console = [this](int session_id) { RemoveDebugConsole(session_id); },
          .notify_session_state_changed =
              [this](DebugSession::State /*state*/) { RequestChromeRedraw(); },
          // A terminal end (clean exit OR a crash/kill/launch-rejection) mirrors to
          // the control channel here, so an observer is never stranded waiting on a
          // `terminated` that a non-clean death never sent.
          .notify_session_terminated =
              [this](int session_id, bool failed, const std::string& reason) {
                control_channel_service_.OnDebugTerminated(session_id, reason);
                // A non-clean end (crash / kill / launch rejection) is easy to miss:
                // the session row + transient views disappear. Surface an error toast
                // so the user knows the debugger died and why, not just that it
                // silently vanished. A clean exit needs no toast (it was expected).
                if (failed) {
                  Notify(NotificationService::Tone::Error,
                         reason.empty() ? std::string("Debug adapter exited unexpectedly")
                                        : reason);
                }
              },
          .notify_stop_began =
              [this](const std::string& reason, int thread_id) {
                control_channel_service_.OnDebugStopBegan(reason, thread_id);
              },
          .notify_stop_resolved =
              [this]() { control_channel_service_.OnDebugStopped(); },
          .request_chrome_redraw = [this]() { RequestChromeRedraw(); },
          .request_debug_pane_redraw = [this]() { RequestDebugPaneRedraw(); },
          .request_editor_redraw = [this]() { RequestEditorSurfaceRedraw(); },
          .queue_editor_hover_refresh = [this]() { QueueEditorHoverRefresh(); },
          .focus_source_location =
              [this](const std::filesystem::path& path, std::size_t line) {
                OpenFile(path);
                if (editor::TextViewport* viewport = ActiveEditorViewport();
                    viewport != nullptr) {
                  viewport->MoveCursorTo(line, 0);
                }
                context_.current_project_state.surface.focus = FocusTarget::Editor;
                RequestEditorSurfaceRedraw();
              },
          .show_call_stack_panel =
              [this]() {
                // Auto-open the right-side debug pane on the first stop (Call Stack),
                // but don't yank the user off another surface on every subsequent step.
                OpenDebugPaneOnStop();
              },
      });
  control_channel_service_.Configure(
      context_,
      ControlChannelService::Operations{
          .execute_command_line =
              [this](const std::string& line) { return ExecuteControlCommand(line); },
          .emit_jsonl = [](const std::string& line) { std::cout << line << '\n' << std::flush; },
          .adapters =
              [this]() {
                std::vector<ControlAdapterInfo> adapters;
                for (const auto& detail : CurrentDapManager().AdapterDetails()) {
                  adapters.push_back(ControlAdapterInfo{detail.type, detail.command});
                }
                return adapters;
              },
          .ensure_debugger_enabled = [this]() { EnsureDebuggerEnabledTransiently(); },
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
          .show_signature_help =
              [this](std::string signature, std::string documentation) {
                ShowSignatureHelpPopup(std::move(signature), std::move(documentation));
              },
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
      .publish_decorations =
          [this](std::string_view owner,
                 const std::filesystem::path& path,
                 editor::PluginDecorationData data) {
            if (context_.current_project_state.EnsurePluginPresentation()
                    .decorations.ReplaceForOwnerFile(owner, path, std::move(data))) {
              RequestEditorSurfaceRedraw();
            }
          },
      .clear_file_decorations =
          [this](std::string_view owner, const std::filesystem::path& path) {
            auto& state = context_.current_project_state;
            auto* pres = state.plugin_presentation.get();
            if (pres != nullptr && pres->decorations.ClearOwnerFile(owner, path)) {
              RequestEditorSurfaceRedraw();
              state.MaybeReleasePluginPresentation();
            }
          },
      .clear_owner_decorations =
          [this](std::string_view owner) {
            auto& state = context_.current_project_state;
            auto* pres = state.plugin_presentation.get();
            if (pres != nullptr && pres->decorations.ClearOwner(owner)) {
              RequestEditorSurfaceRedraw();
              state.MaybeReleasePluginPresentation();
            }
          },
      .publish_surface =
          [this](std::string_view owner, std::string_view surface_id,
                 editor::SurfaceContent content) {
            const editor::SurfacePreviewSlot slot = content.preview;
            if (context_.current_project_state.EnsurePluginPresentation()
                    .surfaces.ReplaceForOwnerSurface(owner, surface_id, std::move(content))) {
              if (slot != editor::SurfacePreviewSlot::None) {
                ActivatePluginSurfacePreview(owner, surface_id, slot);
              }
              RequestEditorSurfaceRedraw();
            }
          },
      .clear_surface =
          [this](std::string_view owner, std::string_view surface_id) {
            auto& state = context_.current_project_state;
            auto* pres = state.plugin_presentation.get();
            if (pres != nullptr && pres->surfaces.ClearOwnerSurface(owner, surface_id)) {
              SyncPluginSurfacePreviewClosed();
              RequestEditorSurfaceRedraw();
              state.MaybeReleasePluginPresentation();
            }
          },
      .clear_owner_surfaces =
          [this](std::string_view owner) {
            auto& state = context_.current_project_state;
            auto* pres = state.plugin_presentation.get();
            if (pres != nullptr && pres->surfaces.ClearOwner(owner)) {
              SyncPluginSurfacePreviewClosed();
              RequestEditorSurfaceRedraw();
              state.MaybeReleasePluginPresentation();
            }
          },
      .decode_raster =
          [this](std::uint64_t hash, int format, std::vector<std::byte> bytes, int width,
                 int height) {
            surface_texture_cache_.Request(
                hash,
                format == 0 ? render::SurfaceTextureCache::RasterFormat::Png
                            : render::SurfaceTextureCache::RasterFormat::Rgba8,
                std::move(bytes), width, height);
          },
      .apply_workspace_edit =
          [this](std::string_view /*owner*/,
                 const plugin::PluginHost::WorkspaceEditRequest& request) {
            return ApplyPluginWorkspaceEdit(request);
          },
      .publish_ghost_text =
          [this](std::string_view owner,
                 const plugin::PluginHost::GhostTextRequest& request) {
            PublishPluginGhostText(owner, request);
          },
      .clear_ghost_text =
          [this](std::string_view owner) { ClearPluginGhostText(owner); },
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
            Notify(NotificationService::ToneFromLevel(level), message);
          },
  });
}

void WorkspaceShell::RebuildPhase3Registries(bool reconcile_language_servers) {
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
  // Skipped on project reactivation: the host has no contributed servers there
  // (it was torn down on switch-away and intentionally not reloaded), so an empty
  // contributed set would make BeginShutdownServersNotIn tear down the project's
  // persisted, warm language servers. See RefreshPluginSurfacesForReactivation.
  if (reconcile_language_servers) {
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

    // Debug adapters are reconciled on the same gate: their definitions are
    // cheap (no process spawns until a session starts), but RetainAdaptersIn
    // must not run with an empty contributed set on reactivation, or it would
    // drop a reactivated project's adapters.
    std::unordered_set<std::string> active_debug_adapter_types;
    for (const auto& adapter : host.ContributedDebugAdapters()) {
      if (adapter.command.empty() || adapter.type.empty()) {
        continue;
      }
      active_debug_adapter_types.insert(adapter.type);
      CurrentDapManager().RegisterAdapter(adapter.type, adapter.command, adapter.sandbox);
    }
    CurrentDapManager().RetainAdaptersIn(active_debug_adapter_types);

    // Reconcile plugin-contributed launch configs into the project. Persistence
    // restores configs before plugins reload (a fallback); once plugins are up,
    // the live contributed set is authoritative. The user's selected index is
    // preserved (clamped) so a re-reconcile does not reset the chosen config.
    auto& project_state = context_.current_project_state;
    const std::size_t previous_selected = project_state.selected_launch_config_index;
    project_state.launch_configs.clear();
    for (const auto& config : host.ContributedLaunchConfigs()) {
      if (config.type.empty()) {
        continue;
      }
      LaunchConfig launch_config;
      launch_config.name = config.name;
      launch_config.type = config.type;
      launch_config.request = config.request.empty() ? std::string("launch") : config.request;
      if (!config.arguments_json.empty()) {
        if (auto parsed = util::ParseJson(config.arguments_json); parsed.has_value()) {
          launch_config.arguments = std::move(*parsed);
        }
      }
      project_state.launch_configs.push_back(std::move(launch_config));
    }
    project_state.selected_launch_config_index =
        project_state.launch_configs.empty()
            ? 0
            : std::min(previous_selected, project_state.launch_configs.size() - 1);
  }
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

void WorkspaceShell::RebuildPresentationRegistries() {
  const auto& host = plugin_runtime_.Host();
  theme_registry_.Rebuild(host);
  file_icon_registry_.Rebuild(host);
  // Keep the colorscheme picker in sync with newly (un)contributed plugin themes.
  auto coordinator = MakePersistenceCoordinator();
  coordinator.RefreshAvailableColorschemeNames();
  // If the active colorscheme is a plugin theme, re-apply it so its (possibly
  // changed) colours take effect after the reload. Built-in/filesystem schemes
  // are unaffected by plugin reloads, so only re-apply when ours is contributed.
  const std::string& active = context_.current_project_state.active_colorscheme_name;
  if (theme_registry_.Contains(active)) {
    coordinator.ApplyColorscheme(active, /*persist=*/false, /*log_feedback=*/false);
    RequestEditorSurfaceRedraw();
    RequestChromeRedraw();
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
  // The reload runs the plugin Lua off the UI thread. The post-reload consumption
  // (registry rebuilds, sidebar/syntax refresh, redraws) must run only after the
  // rebuilt contribution snapshot is published, so it lives in the completion. With no
  // worker wired the completion fires inline and the whole flow stays synchronous.
  const std::uint64_t generation = reload_plugins_invocation_count_;
  plugin_runtime_.ReloadAsync(
      context_.current_project_state.root, request.syntax_definitions,
      [this, request, generation](bool clean_reload) {
        // Drop a stale completion: a newer reload has superseded this one and its own
        // completion will rebuild against the live snapshot.
        if (generation != reload_plugins_invocation_count_) {
          return;
        }
        ConsumeReloadResult(request, clean_reload);
      });
  // Reload dispatched. Callers ignore the return; the real result is delivered to the
  // completion above.
  return true;
}

void WorkspaceShell::ConsumeReloadResult(PluginReloadRequest request, bool clean_reload) {
  (void)clean_reload;
  {
    util::StartupTrace::Scope registry_scope("RebuildRegistries");
    util::PerformanceTrace::Scope perf_registry_scope(
        "WorkspaceShell::ReloadPluginsForCurrentProject::RebuildRegistries");
    RebuildPhase3Registries();
    RebuildPhase4Registries();
    RebuildPresentationRegistries();
    RefreshPluginEditorEventInterest();
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
}

void WorkspaceShell::RefreshPluginSurfacesForReactivation() {
  util::PerformanceTrace::Scope perf_scope(
      "WorkspaceShell::RefreshPluginSurfacesForReactivation");
  // Reactivating an already-initialised project deliberately does NOT reload the
  // plugin host (kept fast/warm by 8136af6e). The shared host was torn down on
  // switch-away, so it has no contributed language servers right now. The
  // project's own LspManager, however, was moved into the catalog with its warm
  // servers intact and moved back here. Reconciling the LSP registry against the
  // empty host would make BeginShutdownServersNotIn({}) shut down and erase those
  // warm servers -> "No LSP server". Skip LSP reconciliation so the persisted
  // servers survive untouched. The shell-global registries (formatters,
  // completions, etc.) still rebuild to clear the previous project's leftovers.
  RebuildPhase3Registries(/*reconcile_language_servers=*/false);
  RebuildPhase4Registries();
  RebuildPresentationRegistries();
  RefreshPluginEditorEventInterest();
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
  // Reactive on_buffer_close fires here (the single buffer-close chokepoint),
  // before the LSP/decoration teardown below.
  if (plugin_runtime_.enabled()) {
    plugin_runtime_.Host().OnBufferClose(normalized_path);
  }
  // Semantic-token decorations are published into the plugin-presentation store
  // under "lsp:semantic". Drop them up front, independent of whether an LSP
  // client is still running, so the store does not accumulate stale entries
  // across open/close churn and can release back to its zero-cost (null) state.
  auto& state = context_.current_project_state;
  if (auto* pres = state.plugin_presentation.get(); pres != nullptr) {
    if (pres->decorations.ClearOwnerFile("lsp:semantic", normalized_path)) {
      state.MaybeReleasePluginPresentation();
      RequestEditorSurfaceRedraw();
    }
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
  state.diagnostics_store.ClearOwnerFile("lsp", normalized_path);
  RefreshProblemsSidebar();
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::ActivatePluginSurfacePreview(std::string_view owner,
                                                  std::string_view surface_id,
                                                  editor::SurfacePreviewSlot slot) {
  // v1 routes both Bottom and Side preview requests to the bottom panel (a
  // dedicated side preview pane is a later increment); the surface renders the
  // same way regardless of slot.
  if (slot == editor::SurfacePreviewSlot::None) {
    return;
  }
  auto& panel = context_.current_project_state.panel;
  panel.content = PanelContentKind::PluginSurface;
  panel.surface_owner = std::string(owner);
  panel.surface_id = std::string(surface_id);
  panel.surface_scroll_y = 0;
  context_.current_project_state.surface.focus = FocusTarget::Panel;
  RequestFullRedraw();
}

bool WorkspaceShell::ApplyPluginWorkspaceEdit(
    const plugin::PluginHost::WorkspaceEditRequest& request) {
  // Resolve the target viewport: an empty path edits the active editable buffer;
  // a named path must be an already-open editor buffer (v1 never edits files on
  // disk, so undo stays coherent).
  editor::TextViewport* viewport = nullptr;
  if (request.path.empty()) {
    viewport = ActiveEditableViewport();
  } else {
    const std::filesystem::path normalized = request.path.lexically_normal();
    if (editor::TextViewport* active = ActiveEditableViewport();
        active != nullptr && active->path().lexically_normal() == normalized) {
      viewport = active;
    } else {
      for (auto& tab : context_.current_project_state.open_tabs) {
        if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
          continue;
        }
        for (auto& view : tab.editor_state->views) {
          if (!view.needs_restore &&
              view.viewport.path().lexically_normal() == normalized) {
            viewport = &view.viewport;
            break;
          }
        }
        if (viewport != nullptr) {
          break;
        }
      }
    }
  }
  if (viewport == nullptr) {
    return false;
  }

  // 1-based plugin coordinates → clamped 0-based document positions, evaluated
  // live so caret clamping sees post-edit line lengths.
  const auto clamp_position = [&](std::size_t one_based_line,
                                  std::size_t one_based_column) -> editor::TextPosition {
    const std::size_t line_count = viewport->line_count();
    std::size_t line = one_based_line >= 1 ? one_based_line - 1 : 0;
    if (line_count == 0) {
      return editor::TextPosition{0, 0};
    }
    if (line >= line_count) {
      line = line_count - 1;
    }
    const std::size_t line_length = viewport->lines()[line].size();
    std::size_t column = one_based_column >= 1 ? one_based_column - 1 : 0;
    if (column > line_length) {
      column = line_length;
    }
    return editor::TextPosition{line, column};
  };

  // Build clamped ranges up front, then apply highest-position-first so earlier
  // edits' coordinates stay valid as the document shifts beneath them.
  std::vector<std::pair<editor::SelectionRange, std::string>> applied_edits;
  applied_edits.reserve(request.edits.size());
  for (const auto& edit : request.edits) {
    const editor::TextPosition start = clamp_position(edit.start_line, edit.start_column);
    const editor::TextPosition end =
        edit.end_line >= 1 ? clamp_position(edit.end_line, edit.end_column) : start;
    applied_edits.emplace_back(editor::SelectionRange{start, end}, edit.text);
  }
  std::sort(applied_edits.begin(), applied_edits.end(),
            [](const auto& lhs, const auto& rhs) {
              const editor::SelectionRange a = editor::TextViewport::NormalizeRange(lhs.first);
              const editor::SelectionRange b = editor::TextViewport::NormalizeRange(rhs.first);
              if (a.start.line != b.start.line) {
                return a.start.line > b.start.line;
              }
              return a.start.column > b.start.column;
            });

  if (!applied_edits.empty()) {
    viewport->BeginUndoGroup();
    for (const auto& [range, text] : applied_edits) {
      viewport->ReplaceRange(range, text, /*record_undo=*/true);
    }
    viewport->EndUndoGroup();
  }

  if (request.has_selection) {
    const editor::TextPosition start =
        clamp_position(request.selection_start_line, request.selection_start_column);
    const editor::TextPosition end =
        clamp_position(request.selection_end_line, request.selection_end_column);
    viewport->MoveCursorTo(start.line, start.column, /*extend_selection=*/false);
    viewport->MoveCursorTo(end.line, end.column, /*extend_selection=*/true);
  } else if (request.has_cursor) {
    const editor::TextPosition cursor =
        clamp_position(request.cursor_line, request.cursor_column);
    viewport->MoveCursorTo(cursor.line, cursor.column, /*extend_selection=*/false);
  }

  ResetCaretBlink();
  RequestActiveEditableLastChangeRedraw();
  RequestChromeRedraw();
  return true;
}

void WorkspaceShell::PublishPluginGhostText(
    std::string_view owner, const plugin::PluginHost::GhostTextRequest& request) {
  // Gate at publish so disabling the feature costs nothing (no state allocated).
  if (!SettingFlagEnabled(GetSettingValue("plugins.ghost_text"), false)) {
    return;
  }
  // Ghost text only renders on the focused editable buffer. An empty path targets
  // it; a named path must match it (we never decorate a background buffer).
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr) {
    return;
  }
  const std::filesystem::path path = viewport->path().lexically_normal();
  if (!request.path.empty() && request.path.lexically_normal() != path) {
    return;
  }
  // Resolve the anchor (1-based; 0 => the live caret) and reject anything that no
  // longer sits at the caret — the user moved between the debounced request and
  // the plugin's reply, so the suggestion is stale.
  const std::size_t anchor_line =
      request.anchor_line >= 1 ? request.anchor_line - 1 : viewport->cursor_line();
  const std::size_t anchor_column =
      request.anchor_column >= 1 ? request.anchor_column - 1 : viewport->cursor_column();
  if (anchor_line != viewport->cursor_line() || anchor_column != viewport->cursor_column()) {
    return;
  }

  auto& ghost = context_.current_project_state.EnsurePluginPresentation().ghost_text.emplace();
  ghost.owner = std::string(owner);
  ghost.path = path;
  ghost.anchor_line = anchor_line;
  ghost.anchor_column = anchor_column;
  ghost.content_revision = viewport->content_revision();
  // Split once on '\n' into [tail, below...]. A trailing '\n' yields an empty last
  // row, which renders (correctly) as a blank dimmed line.
  std::size_t start = 0;
  while (true) {
    const std::size_t nl = request.text.find('\n', start);
    if (nl == std::string::npos) {
      ghost.lines.emplace_back(request.text.substr(start));
      break;
    }
    ghost.lines.emplace_back(request.text.substr(start, nl - start));
    start = nl + 1;
  }
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::ClearPluginGhostText(std::string_view owner) {
  auto& state = context_.current_project_state;
  const auto* ghost = state.ghost_text_if_present();
  if (ghost == nullptr || ghost->owner != owner) {
    return;
  }
  ResetGhostText(state);
  RequestEditorSurfaceRedraw();
}

bool WorkspaceShell::AcceptGhostText() {
  auto& state = context_.current_project_state;
  const auto* ghost = state.ghost_text_if_present();
  if (ghost == nullptr) {
    return false;  // No suggestion: let Tab fall through to snippet/indent.
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (GhostTextStale(viewport, *ghost)) {
    ResetGhostText(state);
    return false;
  }
  std::string text;
  for (std::size_t i = 0; i < ghost->lines.size(); ++i) {
    if (i != 0) {
      text.push_back('\n');
    }
    text += ghost->lines[i];
  }
  ResetGhostText(state);
  viewport->BeginUndoGroup();
  viewport->InsertText(text, /*record_undo=*/true);
  viewport->EndUndoGroup();
  ResetCaretBlink();
  RequestEditorSurfaceRedraw();
  RequestChromeRedraw();
  return true;
}

void WorkspaceShell::DismissGhostText() {
  auto& state = context_.current_project_state;
  if (state.ghost_text_if_present() == nullptr) {
    return;
  }
  ResetGhostText(state);
  RequestEditorSurfaceRedraw();
}

void WorkspaceShell::InvalidateGhostTextIfStale() {
  auto& state = context_.current_project_state;
  const auto* ghost = state.ghost_text_if_present();
  if (ghost == nullptr) {
    return;  // Zero-cost when no suggestion is live.
  }
  if (GhostTextStale(ActiveEditableViewport(), *ghost)) {
    ResetGhostText(state);
    RequestEditorSurfaceRedraw();
  }
}

namespace {
// Trailing debounce for reactive editor events: a plugin sees one coalesced
// event ~150 ms after typing / caret motion settles, never on the hot path.
constexpr std::uint32_t kPluginEditorEventDebounceMs = 150;
}  // namespace

void WorkspaceShell::RefreshPluginEditorEventInterest() {
  if (!plugin_runtime_.enabled()) {
    plugin_editor_event_tracker_.SetInterest({});
    plugin_editor_event_tracker_.Reset();
    return;
  }
  plugin_editor_event_tracker_.SetInterest(plugin_runtime_.Host().EditorEventInterests());
  plugin_editor_event_tracker_.Reset();
}

void WorkspaceShell::SamplePluginEditorEvents() {
  // Zero-cost gate: when no loaded plugin subscribes, this returns before
  // touching the viewport, so typing pays only one predicate.
  if (!plugin_editor_event_tracker_.interest().any()) {
    return;
  }
  const editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return;
  }
  bool selection_present = false;
  std::size_t sel_start_line = 0;
  std::size_t sel_start_column = 0;
  std::size_t sel_end_line = 0;
  std::size_t sel_end_column = 0;
  if (const auto range = viewport->selection_range(); range.has_value()) {
    const editor::SelectionRange normalized = editor::TextViewport::NormalizeRange(*range);
    selection_present = true;
    sel_start_line = normalized.start.line + 1;
    sel_start_column = normalized.start.column + 1;
    sel_end_line = normalized.end.line + 1;
    sel_end_column = normalized.end.column + 1;
  }
  plugin_editor_event_tracker_.Sample(
      viewport->path().lexically_normal(), viewport->content_revision(),
      viewport->cursor_line() + 1, viewport->cursor_column() + 1, selection_present,
      sel_start_line, sel_start_column, sel_end_line, sel_end_column, SDL_GetTicks(),
      kPluginEditorEventDebounceMs);
}

bool WorkspaceShell::DispatchDuePluginEditorEvents() {
  if (!plugin_editor_event_tracker_.interest().any()) {
    return false;
  }
  const PluginEditorEventTracker::DueEvents due =
      plugin_editor_event_tracker_.TakeDue(SDL_GetTicks());
  if (!due.any() || due.path.empty()) {
    return false;
  }
  auto& host = plugin_runtime_.Host();
  if (due.change) {
    host.OnBufferChange(due.path, due.change_start_line, due.change_end_line);
  }
  if (due.cursor) {
    host.OnCursorMove(due.path, due.cursor_line, due.cursor_column);
  }
  if (due.selection) {
    host.OnSelectionChange(due.path, due.selection_present, due.selection_start_line,
                           due.selection_start_column, due.selection_end_line,
                           due.selection_end_column);
  }
  return true;
}

void WorkspaceShell::SyncPluginSurfacePreviewClosed() {
  auto& panel = context_.current_project_state.panel;
  if (panel.content != PanelContentKind::PluginSurface) {
    return;
  }
  const auto* pres = context_.current_project_state.plugin_presentation_if_present();
  const editor::SurfaceContent* content =
      pres != nullptr ? pres->surfaces.Find(panel.surface_owner, panel.surface_id) : nullptr;
  if (content == nullptr || content->preview == editor::SurfacePreviewSlot::None) {
    panel.content = PanelContentKind::None;
    panel.surface_owner.clear();
    panel.surface_id.clear();
  }
}

}  // namespace microide::workspace
