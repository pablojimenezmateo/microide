#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "project/CompileCommandsLocator.h"
#include "util/JsonValue.h"
#include "util/PathMatch.h"
#include "util/PerformanceTrace.h"
#include "util/StartupTrace.h"
#include "workspace/FileUri.h"
#include "workspace/LspPositionEncoding.h"
#include "workspace/LspViewportPositions.h"
#include "workspace/SettingFlags.h"
#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceCommandRegistry.h"
#include "workspace/WorkspacePersistenceCoordinator.h"

namespace microide::workspace {

namespace {

bool IsVirtualDocumentUri(std::string_view text) {
  return text.starts_with("virtual://");
}

// clangd cannot find a compile_commands.json in a non-standard build dir (e.g.
// builds/, out/, cmake-build-*). When the host discovers one, append
// --compile-commands-dir so cross-file features work instead of silently
// degrading. Only clangd understands the flag, so gate on the program name and
// skip when the plugin already set it. Returns the (possibly augmented) command.
std::vector<std::string> AugmentClangdWithCompileCommandsDir(
    std::vector<std::string> command, const std::filesystem::path& project_root) {
  if (command.empty()) {
    return command;
  }
  const std::string program = std::filesystem::path(command.front()).filename().string();
  if (program.rfind("clangd", 0) != 0) {
    return command;  // not clangd; the flag would break other servers
  }
  for (const std::string& arg : command) {
    if (arg.rfind("--compile-commands-dir", 0) == 0) {
      return command;  // plugin/user already specified one
    }
  }
  const std::optional<std::filesystem::path> dir =
      project::DiscoverCompileCommandsDir(project_root);
  if (!dir.has_value()) {
    SDL_Log("cpp-lsp: no compile_commands.json found under %s — clangd cross-file "
            "features will be limited until one is generated",
            project_root.generic_string().c_str());
    return command;
  }
  command.push_back("--compile-commands-dir=" + dir->generic_string());
  SDL_Log("cpp-lsp: pointing clangd at compile_commands.json in %s",
          dir->generic_string().c_str());
  return command;
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
  // Iterate every editor group, not just the focused one: on session restore of a
  // split, each background group's active tab is eager-hydrated but its document
  // would otherwise never receive didOpen / OnBufferOpen (no diagnostics/semantic
  // tokens until the user focuses that pane). The sole caller dedups by path, so a
  // buffer shared across splits is still opened once.
  for (const auto& group : state.editor_groups) {
    for (const auto& tab : group.open_tabs) {
      if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
        const auto& view = *tab.editor_state;
        const std::filesystem::path path =
            (view.needs_restore ? view.restored_path : view.viewport.path()).lexically_normal();
        if (!path.empty()) {
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
        callback(tab.compare->right_viewport.path().lexically_normal(),
                 &tab.compare->right_viewport);
        continue;
      }
      if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value() &&
          !tab.merge->result_viewport.path().empty()) {
        callback(tab.merge->result_viewport.path().lexically_normal(),
                 &tab.merge->result_viewport);
      }
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
              [this](std::size_t group_index, std::size_t index) {
                return TabDisplayTitle(group_index, index);
              },
          .editor_tab_tooltip_label =
              [this](std::size_t group_index, std::size_t index) {
                return TabTooltipLabel(group_index, index);
              },
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
          .apply_workspace_edit_to_open_buffers =
              [this](const std::vector<CodeActionEdit>& edits) {
                bool any_rejected = false;
                const bool applied = ApplyLspWorkspaceEdit(edits, &any_rejected);
                return OpenBufferEditResult{.applied_any = applied,
                                            .any_rejected = any_rejected};
              },
          .get_setting_value = [this](std::string_view id) { return GetSettingValue(id); },
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
          .collect_lsp_context_diagnostics =
              [this](const editor::TextViewport& viewport,
                     const editor::SelectionRange& range,
                     lsp_encoding::PositionEncoding encoding) {
                return CollectLspContextDiagnostics(viewport, range, encoding);
              },
          .apply_lsp_workspace_edit =
              [this](const std::vector<CodeActionEdit>& edits) {
                return ApplyLspWorkspaceEdit(edits);
              },
          .apply_rename_workspace_edit =
              [this](const std::string& new_name, const std::vector<CodeActionEdit>& edits) {
                ApplyRenameWorkspaceEdit(new_name, edits);
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
                     const std::optional<editor::SelectionRange>& selection_before,
                     const editor::TextPosition& cursor_before) {
                UpdateMergeTrackingAfterViewportEdit(tab, selection_before, cursor_before);
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
                .content_revision = viewport->content_revision(),
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
      .settings_revision =
          [this]() {
            return settings_store_.Revision();
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
      std::vector<std::string> command = AugmentClangdWithCompileCommandsDir(
          language_server.command, context_.current_project_state.root);
      CurrentLspManager().RegisterServer(language_server.language_ids, command,
                                         // Percent-encode the rootUri the same way
                                         // document URIs are (FileUriForPath); a raw
                                         // "file://" + path is a malformed URI for a
                                         // root with a space/#/non-ASCII byte, so the
                                         // server would see opened documents as
                                         // outside its workspace (breaking symbols,
                                         // cross-file nav, rename) or reject init.
                                         FileUriForPath(context_.current_project_state.root),
                                         context_.current_project_state.root.generic_string(),
                                         language_server.eager_start,
                                         language_server.initialization_options,
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
  const std::filesystem::path project_root = context_.current_project_state.root;
  plugin_runtime_.ReloadAsync(
      project_root, request.syntax_definitions,
      [this, request, generation, project_root](bool clean_reload) {
        // Drop a stale completion. Two independent guards:
        //  - generation: a newer reload superseded this one (its completion rebuilds).
        //  - project_root: the active project changed between dispatch and completion.
        //    A warm reactivation switches projects WITHOUT bumping the generation
        //    counter, so the counter alone would let a previous project's late reload
        //    publish its registries/sidebars/diagnostics into the now-active project.
        if (generation != reload_plugins_invocation_count_ ||
            project_root != context_.current_project_state.root) {
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

  for (auto& tab : context_.current_project_state.focused_group().open_tabs) {
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      auto& editor_state = *tab.editor_state;
      if (!editor_state.needs_restore && should_invalidate_viewport(editor_state.viewport)) {
        editor_state.viewport.InvalidateSyntaxHighlighting();
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
  NotifyLspBufferOpen(normalized_path);
}

void WorkspaceShell::NotifyLspBufferOpen(const std::filesystem::path& path) {
  // Engage the language server the moment a document becomes the active editor —
  // start a lazily-registered server, send `textDocument/didOpen`, and request
  // semantic tokens/diagnostics. Without this the LSP only woke on the first
  // *edit* or an explicit action (go-to-definition/hover), so a freshly opened OR
  // session-restored file left the status stuck at "LSP: Starting..." (lazy
  // server never started) or painted no diagnostics/semantic colors (server up
  // but the doc was never opened) until the user interacted. Called from both the
  // manual-open path (NotifyPluginBufferOpen) and tab activation/restore, so a
  // file that was already open at startup engages the LSP too. EnsureLspDocumentOpen
  // is idempotent (HasOpenDocument), so re-activations are cheap no-ops.
  const std::filesystem::path normalized_path = path.lexically_normal();
  if (normalized_path.empty()) {
    return;
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().lexically_normal() != normalized_path) {
    return;
  }
  std::string language_id;
  LspClient* client = LspClientForViewport(*viewport, &language_id);
  if (client == nullptr) {
    return;
  }
  EnsureLspDocumentOpen(*viewport, *client, language_id);
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
  client->SetDiagnosticsCallback(
      [this, project_root = context_.current_project_state.root.lexically_normal(), client](
          std::string uri, std::vector<LspClient::Diagnostic> diagnostics) {
        // Resolve the active project at dispatch time and drop the publish if the
        // active project changed since this callback was installed. A raw
        // &current_project_state capture would otherwise publish diagnostics from an
        // old server into whatever project happens to be current now.
        ProjectWorkspaceState& current = context_.current_project_state;
        if (current.root.lexically_normal() != project_root) {
          return;
        }
        PublishLspDiagnostics(current, std::move(uri), LspEncodingForClient(*client),
                              std::move(diagnostics));
      });
  EnsureLspDocumentOpen(*viewport, *client, language_id);
  // Must match the percent-encoded URI the document was opened under
  // (EnsureLspDocumentOpen -> FileUriForPath); a hand-built "file://" + raw path
  // desyncs for any path with a space/non-ASCII/reserved byte, so the server sees
  // didSave for a URI it never opened.
  client->DidSave(FileUriForPath(normalized_path));
  // The buffer is now clean, so the semantic-token overlay becomes render-visible
  // again. Re-request it for the saved content: an edit-then-undo-then-save
  // sequence cleared the overlay on each edit, and semantic tokens are pull-based
  // (the server never pushes them), so without this the identifiers would keep the
  // lexical-only colors until the next unrelated re-request.
  lsp_service_.RequestLspSemanticTokens(*viewport, *client);
  // Inlay hints are equally pull-based; re-request for the saved content so they
  // reappear after the edit path cleared them.
  lsp_service_.RequestLspInlayHints(*viewport, *client);
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
  const std::string language_id = editor::runtime_syntax::DetectFiletype(normalized_path);
  if (language_id.empty()) {
    return;
  }
  LspClient* client = CurrentLspManager().FindStartedServer(language_id);
  if (client == nullptr) {
    return;
  }
  // Percent-encoded to match the open URI (FileUriForPath); a raw "file://" +
  // path never matches HasOpenDocument for special-char paths, so didClose would
  // be skipped and the server would leak the document (and its diagnostics).
  const std::string uri = FileUriForPath(normalized_path);
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
      for (auto& group : context_.current_project_state.editor_groups) {
        for (auto& tab : group.open_tabs) {
          if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
            continue;
          }
          auto& state = *tab.editor_state;
          if (!state.needs_restore &&
              state.viewport.path().lexically_normal() == normalized) {
            viewport = &state.viewport;
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

  // Staleness guard for edits deferred from the plugin worker: the coordinates
  // were computed against `guard_path` at `captured_content_revision`. If the
  // resolved buffer is a different file (the user switched tabs) or has advanced
  // (the user typed) during the async hop, the edit is stale — drop it rather than
  // apply now-invalid coordinates over newer input.
  if (request.has_staleness_guard &&
      (viewport->path().lexically_normal() != request.guard_path ||
       viewport->content_revision() != request.captured_content_revision)) {
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

bool WorkspaceShell::ApplyLspWorkspaceEdit(const std::vector<CodeActionEdit>& edits,
                                           bool* out_any_rejected) {
  const auto mark_rejected = [&]() {
    if (out_any_rejected != nullptr) {
      *out_any_rejected = true;
    }
  };
  if (edits.empty()) {
    return false;
  }

  // Resolve a target path to an already-open editable buffer. An empty path (or
  // one matching the active buffer) resolves to the active editable buffer; other
  // paths must already be open (v1 never edits files on disk, so undo stays
  // coherent). Mirrors ApplyPluginWorkspaceEdit's resolution.
  const auto resolve_uncached = [&](const std::filesystem::path& path) -> editor::TextViewport* {
    if (path.empty()) {
      return ActiveEditableViewport();
    }
    const std::filesystem::path normalized = path.lexically_normal();
    if (editor::TextViewport* active = ActiveEditableViewport();
        active != nullptr && active->path().lexically_normal() == normalized) {
      return active;
    }
    for (auto& group : context_.current_project_state.editor_groups) {
      for (auto& tab : group.open_tabs) {
        if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
          continue;
        }
        auto& state = *tab.editor_state;
        if (!state.needs_restore &&
            state.viewport.path().lexically_normal() == normalized) {
          return &state.viewport;
        }
      }
    }
    return nullptr;
  };

  // A large rename / code action repeatedly targets the same handful of files, so
  // memoize the (normalized) path -> viewport resolution instead of re-scanning
  // every open tab per edit — that inner scan is the second half of the O(edits *
  // touched_files) worst case the parser caps still permit.
  std::unordered_map<std::string, editor::TextViewport*> resolve_cache;
  const auto resolve = [&](const std::filesystem::path& path) -> editor::TextViewport* {
    std::string key = path.lexically_normal().generic_string();
    const auto it = resolve_cache.find(key);
    if (it != resolve_cache.end()) {
      return it->second;
    }
    editor::TextViewport* viewport = resolve_uncached(path);
    resolve_cache.emplace(std::move(key), viewport);
    return viewport;
  };

  // Group edits by the buffer they resolve to; each buffer applies its edits as a
  // single grouped-undo step, highest-position-first so earlier ranges stay valid.
  // `bucket_index` maps a viewport to its slot in `by_viewport` so grouping is O(1)
  // per edit rather than a linear scan (O(edits * touched_files) otherwise).
  std::vector<std::pair<editor::TextViewport*, std::vector<std::pair<editor::SelectionRange, std::string>>>>
      by_viewport;
  std::unordered_map<editor::TextViewport*, std::size_t> bucket_index;
  const auto bucket_for = [&](editor::TextViewport* viewport)
      -> std::vector<std::pair<editor::SelectionRange, std::string>>& {
    const auto it = bucket_index.find(viewport);
    if (it != bucket_index.end()) {
      return by_viewport[it->second].second;
    }
    bucket_index.emplace(viewport, by_viewport.size());
    by_viewport.emplace_back(viewport, std::vector<std::pair<editor::SelectionRange, std::string>>{});
    return by_viewport.back().second;
  };

  for (const CodeActionEdit& edit : edits) {
    editor::TextViewport* viewport = resolve(edit.path);
    if (viewport == nullptr) {
      continue;
    }
    bucket_for(viewport).emplace_back(edit.range, edit.new_text);
  }

  // Snapshot EVERY target buffer before applying so each edited buffer can be
  // re-synced with a FULL document change (the incremental last-change path only
  // carries the last of N edits, which would desync the server). A WorkspaceEdit
  // can touch several open buffers (header+source rename, multi-file fixit); each
  // one's server mirror and stored diagnostics must be reconciled, not just the
  // active tab's.
  editor::TextViewport* active_viewport = ActiveEditableViewport();
  std::vector<std::vector<std::string>> before_snapshots(by_viewport.size());
  for (std::size_t i = 0; i < by_viewport.size(); ++i) {
    if (by_viewport[i].first != nullptr && !by_viewport[i].second.empty()) {
      before_snapshots[i] = by_viewport[i].first->lines().Snapshot();
    }
  }

  bool applied_any = false;
  for (auto& [viewport, buffer_edits] : by_viewport) {
    if (viewport == nullptr || buffer_edits.empty()) {
      continue;
    }
    // Resolve the server's position encoding for this buffer so the edit's LSP
    // `character` offsets (utf-16 code units by spec default) map to the right
    // UTF-8 byte column — mis-mapping on a non-ASCII line splits a multibyte
    // sequence and corrupts the buffer.
    std::string vp_language_id;
    LspClient* vp_client = lsp_service_.LspClientForViewport(*viewport, &vp_language_id);
    const lsp_encoding::PositionEncoding vp_encoding =
        vp_client != nullptr ? LspEncodingForClient(*vp_client)
                             : lsp_encoding::PositionEncoding::Utf8;
    // Convert 0-based LSP coordinates to editor byte columns. A line beyond EOF is a
    // stale/confused/hostile server target: unlike the old forgiving clamp (which
    // silently rewrote it onto the LAST real line and mutated it — visible data loss
    // in dirty state, undo, diagnostics, folds), we now REJECT the whole buffer's edit
    // group, matching the closed-file applier's range policy. The sole allowed
    // beyond-EOF position is the end-of-document sentinel {line == line_count,
    // character == 0} (an append at EOF). LspCharacterToByteColumn still soft-clamps
    // the column within a valid line.
    const std::size_t line_count = viewport->line_count();
    const auto map_position =
        [&](editor::TextPosition pos, bool* ok) -> editor::TextPosition {
      if (line_count == 0) {
        if (pos.line != 0) {
          *ok = false;
        }
        return editor::TextPosition{0, 0};
      }
      if (pos.line == line_count && pos.column == 0) {
        // End-of-document append sentinel: keep it addressed to one-past-last-line.
        return pos;
      }
      if (pos.line >= line_count) {
        *ok = false;
        return pos;
      }
      pos.column = lsp_encoding::LspCharacterToByteColumn(
          std::string_view(viewport->lines()[pos.line]), pos.column, vp_encoding);
      return pos;
    };
    bool group_ok = true;
    for (auto& [range, text] : buffer_edits) {
      range.start = map_position(range.start, &group_ok);
      range.end = map_position(range.end, &group_ok);
    }
    if (!group_ok) {
      // A beyond-EOF target invalidates this buffer's whole group; drop it and record
      // the rejection so a server-initiated applyEdit reports partial failure.
      mark_rejected();
      continue;
    }
    // Apply highest-position-first so earlier ranges stay valid as later ones are
    // applied. For edits at the SAME position (e.g. two inserts at (0,0)), apply
    // the later array entry FIRST: each same-position insert pushes the previous
    // one right, so this leaves the array order intact left-to-right in the result.
    // (A plain stable_sort would reverse them.) We sort an index vector so the
    // original array index is available as the tie-break.
    std::vector<std::size_t> apply_order(buffer_edits.size());
    for (std::size_t i = 0; i < apply_order.size(); ++i) {
      apply_order[i] = i;
    }
    std::sort(apply_order.begin(), apply_order.end(), [&](std::size_t lhs, std::size_t rhs) {
      const editor::SelectionRange a = editor::TextViewport::NormalizeRange(buffer_edits[lhs].first);
      const editor::SelectionRange b = editor::TextViewport::NormalizeRange(buffer_edits[rhs].first);
      if (a.start.line != b.start.line) {
        return a.start.line > b.start.line;
      }
      if (a.start.column != b.start.column) {
        return a.start.column > b.start.column;
      }
      return lhs > rhs;
    });
    // Reject overlapping edits for this buffer (see the closed-file applier): two
    // intersecting ranges double-edit shared bytes order-dependently. Consecutive
    // descending-order entries run higher-start -> lower-start; they overlap when
    // the lower edit's end passes the higher edit's start (touching is allowed).
    bool overlapping = false;
    for (std::size_t i = 1; i < apply_order.size() && !overlapping; ++i) {
      const editor::SelectionRange hi =
          editor::TextViewport::NormalizeRange(buffer_edits[apply_order[i - 1]].first);
      const editor::SelectionRange lo =
          editor::TextViewport::NormalizeRange(buffer_edits[apply_order[i]].first);
      overlapping = lo.end.line > hi.start.line ||
                    (lo.end.line == hi.start.line && lo.end.column > hi.start.column);
    }
    if (overlapping) {
      mark_rejected();
      continue;
    }

    viewport->BeginUndoGroup();
    for (const std::size_t idx : apply_order) {
      viewport->ReplaceRange(buffer_edits[idx].first, buffer_edits[idx].second,
                             /*record_undo=*/true);
    }
    viewport->EndUndoGroup();
    applied_any = true;
  }

  if (applied_any) {
    ResetCaretBlink();
    // Full-document re-sync of EVERY edited buffer. The active buffer additionally
    // drives the visible redraw via RequestActiveEditableChangeRedraw (which itself
    // full-syncs the active viewport); non-active edited buffers are synced
    // directly through the LSP service so their server mirror + diagnostics stay
    // correct even though they are not the visible tab.
    // Every edited buffer's fold model must be recomputed. The fold model's
    // content_revision fingerprint alone does not force a rescan once a file is fully
    // resolved (see the Undo/Redo guard in WorkspaceActionContext), so a same-line-count
    // edit — a Format-Document re-indent, a bracket-renesting code action / rename —
    // would otherwise leave phantom fold markers that hide arbitrary line ranges. Mark
    // each edited tab's fold model dirty, including non-active tabs a multi-file edit
    // touched.
    const auto mark_folds_dirty = [&](editor::TextViewport* edited) {
      if (edited == nullptr) {
        return;
      }
      for (auto& group : context_.current_project_state.editor_groups) {
        for (auto& tab : group.open_tabs) {
          if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value() &&
              &tab.editor_state->viewport == edited && tab.editor_state->folding_model) {
            tab.editor_state->folding_model->MarkDirty();
            return;
          }
        }
      }
    };
    bool active_synced = false;
    for (std::size_t i = 0; i < by_viewport.size(); ++i) {
      editor::TextViewport* viewport = by_viewport[i].first;
      if (viewport == nullptr || by_viewport[i].second.empty()) {
        continue;
      }
      mark_folds_dirty(viewport);
      if (viewport == active_viewport) {
        RequestActiveEditableChangeRedraw(before_snapshots[i], viewport->lines().Snapshot());
        active_synced = true;
      } else {
        lsp_service_.SyncLspForBufferChange(*viewport, before_snapshots[i],
                                            viewport->lines().Snapshot());
      }
    }
    if (!active_synced) {
      // The edit did not touch the active buffer; still refresh its last-change
      // derived state so the caret/redraw bookkeeping stays consistent.
      RequestActiveEditableLastChangeRedraw();
    }
    // Repaint the tab strip so the dirty indicator appears immediately; the edit
    // just flipped the buffer to dirty and RequestChromeRedraw only damages the
    // menu-bar region, leaving the tab dot stale until the next full repaint.
    RequestActiveTabRedraw(/*include_tree_sidebar=*/false);
  }
  return applied_any;
}

namespace {
// True when `path` is open in a hydrated editor tab of any group.
bool IsPathOpenInEditorState(const ProjectWorkspaceState& state,
                            const std::filesystem::path& normalized_path) {
  for (const auto& group : state.editor_groups) {
    for (const auto& tab : group.open_tabs) {
      if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value() &&
          !tab.editor_state->needs_restore &&
          tab.editor_state->viewport.path().lexically_normal() == normalized_path) {
        return true;
      }
    }
  }
  return false;
}
}  // namespace

void WorkspaceShell::ApplyRenameWorkspaceEdit(const std::string& new_name,
                                              const std::vector<CodeActionEdit>& edits) {
  if (edits.empty()) {
    return;
  }
  // Distinct affected files (an empty path targets the active buffer, always open).
  // Dedupe through a hash set (O(edits)) rather than a linear std::find per edit —
  // a large legal rename otherwise pays O(edits * touched_files) here.
  std::vector<std::filesystem::path> affected;
  std::unordered_set<std::string> affected_seen;
  std::size_t closed_count = 0;
  for (const CodeActionEdit& edit : edits) {
    if (edit.path.empty()) {
      continue;
    }
    const std::filesystem::path normalized = edit.path.lexically_normal();
    if (!affected_seen.insert(normalized.generic_string()).second) {
      continue;
    }
    affected.push_back(normalized);
    if (!IsPathOpenInEditorState(context_.current_project_state, normalized)) {
      ++closed_count;
    }
  }

  if (closed_count == 0) {
    // Everything is already open: apply in place, leaving the buffers dirty like any
    // other edit (the user saves as usual).
    ApplyLspWorkspaceEdit(edits);
    return;
  }

  // Some files are not open. Confirm before writing them — editing files the user
  // hasn't opened is an outward, hard-to-undo action. The closed files are applied
  // SILENTLY on disk (VSCode-style); no tabs are opened for them.
  std::string detail = "Renaming to '" + new_name + "' changes " +
                       std::to_string(affected.size()) + (affected.size() == 1 ? " file" : " files") +
                       " (" + std::to_string(closed_count) + " not open). Apply and save?";
  pending_rename_save_ =
      PendingRenameSave{new_name, edits, std::move(affected)};
  OpenPromptSurface(PromptSurfaceState::Action::ConfirmRenameSave,
                    PromptSurfaceState::Kind::Confirm, std::filesystem::path{}, std::string{});
  context_.prompts.surface.detail = std::move(detail);
  RequestChromeRedraw();
}

void WorkspaceShell::DiscardPendingRenameSave() { pending_rename_save_.reset(); }

void WorkspaceShell::CommitPendingRenameSave() {
  if (!pending_rename_save_.has_value()) {
    return;
  }
  const PendingRenameSave pending = std::move(*pending_rename_save_);
  pending_rename_save_.reset();

  // Open files keep their edits applied in place (ApplyLspWorkspaceEdit resolves
  // only already-open buffers; closed files are skipped there), then are saved.
  ApplyLspWorkspaceEdit(pending.edits);

  std::size_t saved = 0;
  bool any_conflict = false;
  for (auto& group : context_.current_project_state.editor_groups) {
    for (auto& tab : group.open_tabs) {
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
          tab.editor_state->needs_restore) {
        continue;
      }
      editor::TextViewport& viewport = tab.editor_state->viewport;
      const std::filesystem::path normalized = viewport.path().lexically_normal();
      if (std::find(pending.affected_paths.begin(), pending.affected_paths.end(), normalized) ==
          pending.affected_paths.end()) {
        continue;
      }
      if (!viewport.dirty()) {
        continue;
      }
      if (viewport.DetectDiskConflict() != editor::TextViewport::DiskConflict::None) {
        any_conflict = true;
        continue;
      }
      if (viewport.Save()) {
        NotifyPluginBufferSave(viewport.path());
        ++saved;
      }
    }
  }

  // Apply the remaining edits to the CLOSED files directly on disk — no tab spam.
  const auto is_open = [this](const std::filesystem::path& normalized) {
    return IsPathOpenInEditorState(context_.current_project_state, normalized);
  };
  const LspService::DiskEditResult disk =
      lsp_service_.ApplyLspEditsToClosedFilesOnDisk(pending.edits, is_open);
  saved += disk.files_written;

  std::string feedback = "Renamed to '" + pending.new_name + "' in " + std::to_string(saved) +
                         (saved == 1 ? " file" : " files");
  if (any_conflict || disk.any_failed) {
    feedback += " (some skipped)";
  }
  context_.current_project_state.panel.feedback.text = std::move(feedback);
  RequestActiveTabRedraw(/*include_tree_sidebar=*/true);
}

std::vector<LspClient::Diagnostic> WorkspaceShell::CollectLspContextDiagnostics(
    const editor::TextViewport& viewport, const editor::SelectionRange& range,
    lsp_encoding::PositionEncoding encoding) const {
  std::vector<LspClient::Diagnostic> result;
  const std::vector<editor::PublishedDiagnostic>* diagnostics =
      context_.current_project_state.diagnostics_store.FindByPathKey(viewport.path_key());
  if (diagnostics == nullptr) {
    return result;
  }

  const auto severity_code = [](editor::DiagnosticSeverity severity) {
    switch (severity) {
      case editor::DiagnosticSeverity::Error:
        return 1;
      case editor::DiagnosticSeverity::Warning:
        return 2;
      case editor::DiagnosticSeverity::Info:
        return 3;
      case editor::DiagnosticSeverity::Hint:
        return 4;
    }
    return 1;
  };

  // Bound the context payload: the merged per-file diagnostic view has no aggregate
  // owner cap, so select overlapping diagnostics through the capped store helper
  // before copying/serializing (TD-2026-07-17A-056). 32 is ample context for a code
  // action; a densely-annotated line no longer materializes a huge request payload.
  constexpr std::size_t kMaxContextDiagnostics = 32;
  const editor::SelectionRange want = editor::TextViewport::NormalizeRange(range);
  const std::vector<editor::PublishedDiagnostic> selected =
      editor::SelectContextDiagnostics(*diagnostics, want, kMaxContextDiagnostics);
  result.reserve(selected.size());
  for (const editor::PublishedDiagnostic& diagnostic : selected) {
    result.push_back(LspClient::Diagnostic{
        .range = LspClient::Range{
            .start = ByteColumnToLspPosition(viewport, diagnostic.range.start.line,
                                             diagnostic.range.start.column, encoding),
            .end = ByteColumnToLspPosition(viewport, diagnostic.range.end.line,
                                           diagnostic.range.end.column, encoding),
        },
        .message = diagnostic.message,
        .severity = severity_code(diagnostic.severity),
    });
  }
  return result;
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
