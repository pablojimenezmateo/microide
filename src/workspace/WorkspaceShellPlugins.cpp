#include "workspace/WorkspaceShell.h"

#include <filesystem>
#include <set>
#include <string_view>

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
          editor::TextViewport restored_view;
          if (!restored_view.OpenFile(path)) {
            continue;
          }
          callback(path, restored_view);
        } else {
          callback(path, view.viewport);
        }
      }
      continue;
    }
    if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
        tab.compare->right_editable && !tab.compare->right_viewport.path().empty()) {
      callback(tab.compare->right_viewport.path().lexically_normal(), tab.compare->right_viewport);
      continue;
    }
    if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value() &&
        !tab.merge->result_viewport.path().empty()) {
      callback(tab.merge->result_viewport.path().lexically_normal(), tab.merge->result_viewport);
    }
  }
}

}  // namespace

WorkspaceShell::WorkspaceShell() {
  virtual_document_registry_.SetOnChange(
      [this](const std::string& uri) { ReloadVirtualDocumentTabs(uri); });
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
  });
}

void WorkspaceShell::RebuildPhase3Registries() {
  formatter_registry_ = FormatterRegistry{};
  save_participant_registry_ = SaveParticipantRegistry{};
  completion_registry_ = CompletionRegistry{};
  code_action_registry_ = CodeActionRegistry{};
  task_registry_ = TaskRegistry{};
  tool_registry_ = ToolRegistry{};
  test_controller_.Clear();
  dap_manager_.ShutdownAll();
  lsp_manager_.ShutdownAll();
  context_.current_project_state.diagnostics_store.ClearOwner("lsp");

  const auto& host = plugin_runtime_.Host();
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
    if (language_server.command.empty()) {
      continue;
    }
    lsp_manager_.RegisterServer(language_server.language_id, language_server.command,
                                "file://" + context_.current_project_state.root.generic_string());
  }
  for (const auto& task : host.ContributedTasks()) {
    task_registry_.Register(TaskSpec{
        .id = task.id,
        .plugin_id = task.plugin_id,
        .label = task.label,
        .group = task.group,
        .command = task.command,
        .cwd = task.cwd,
        .run_in_shell = task.run_in_shell,
    });
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
  for (const auto& debugger : host.ContributedDebuggers()) {
    dap_manager_.RegisterDebugger(debugger.type, debugger.command);
  }
}

void WorkspaceShell::RebuildPhase4Registries() {
  scm_registry_ = ScmRegistry{};
  annotation_registry_ = AnnotationRegistry{};
  auth_provider_registry_.Clear();

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
  for (const auto& provider : host.ContributedAuthProviders()) {
    auth_provider_registry_.RegisterProvider(AuthProviderSpec{
        .id = provider.id,
        .label = provider.label,
        .plugin_id = provider.plugin_id,
    });
  }
}

void WorkspaceShell::RebuildPhase5Registries() {
  ai_provider_registry_ = AiProviderRegistry{};
  inline_completion_registry_.Clear();
  conversation_registry_.Clear();
  external_agent_registry_.Clear();
  mcp_tool_registry_.Clear();
  ai_context_manager_.Clear();

  const auto& host = plugin_runtime_.Host();
  for (const auto& provider : host.ContributedAiProviders()) {
    ai_provider_registry_.Register(AiProviderSpec{
        .id = provider.id,
        .label = provider.label,
        .type = provider.type,
        .api_key_name = provider.id + ".api_key",
        .models = provider.models,
        .plugin_id = provider.plugin_id,
    });
  }
  for (const auto& agent : host.ContributedExternalAgents()) {
    external_agent_registry_.RegisterAgent(ExternalAgentSpec{
        .id = agent.id,
        .label = agent.label,
        .protocol = agent.protocol,
        .endpoint = agent.endpoint,
        .capabilities = agent.capabilities,
        .plugin_id = agent.plugin_id,
    });
  }
  for (const auto& tool : host.ContributedMcpTools()) {
    mcp_tool_registry_.RegisterTool(McpToolSpec{
        .id = tool.id,
        .name = tool.name,
        .description = tool.description,
        .input_schema = tool.input_schema,
        .plugin_id = tool.plugin_id,
    });
    mcp_tool_registry_.SetPermission(ToolPermission{
        .tool_id = tool.id,
        .agent_id = "*",
        .level = ToolPermissionLevel::PromptRequired,
    });
  }
  if (context_.current_project_state.panel.chat.conversation_id.empty()) {
    context_.current_project_state.panel.chat.conversation_id =
        conversation_registry_.CreateConversation("Chat", {});
  }
}

bool WorkspaceShell::ReloadPluginsForCurrentProject() {
  const bool clean_reload = plugin_runtime_.Reload(context_.current_project_state.root);
  RebuildPhase3Registries();
  RebuildPhase4Registries();
  RebuildPhase5Registries();
  InvalidateRuntimeSyntaxStateCaches();
  NormalizeSidebarViewSelection();
  RefreshPluginSidebar();
  RefreshGitSidebar();
  RefreshProblemsSidebar();
  NotifyPluginsAboutOpenBuffers();
  RequestChromeRedraw();
  RequestEditorSurfaceRedraw();
  return clean_reload;
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

void WorkspaceShell::InvalidateRuntimeSyntaxStateCaches() {
  if (editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
    viewport->InvalidateSyntaxHighlighting();
  }

  for (auto& tab : context_.current_project_state.open_tabs) {
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      for (auto& view : tab.editor_state->views) {
        if (!view.needs_restore) {
          view.viewport.InvalidateSyntaxHighlighting();
        }
      }
      continue;
    }

    if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
      RefreshCompareTabDerivedState(*tab.compare);
      continue;
    }

    if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
      auto& merge_tab = *tab.merge;
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

void WorkspaceShell::NotifyPluginsAboutOpenBuffers() {
  if (!plugin_runtime_.enabled()) {
    return;
  }
  std::set<std::filesystem::path> opened_paths;
  ForEachOpenEditableBuffer(context_.current_project_state,
                            [&](const std::filesystem::path& path,
                                const editor::TextViewport& viewport) {
                              if (!opened_paths.insert(path).second) {
                                return;
                              }
                              plugin_runtime_.Host().OnBufferOpen(path);
                              std::string language_id;
                              LspClient* client = LspClientForViewport(viewport, &language_id);
                              if (client != nullptr) {
                                EnsureLspDocumentOpen(viewport, *client, language_id);
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
  std::string language_id;
  LspClient* client = LspClientForViewport(*viewport, &language_id);
  if (client == nullptr) {
    return;
  }
  EnsureLspDocumentOpen(*viewport, *client, language_id);
  client->DidSave("file://" + normalized_path.generic_string());
}

void WorkspaceShell::NotifyLspBufferClose(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  if (normalized_path.empty()) {
    return;
  }
  editor::TextViewport viewport;
  if (!viewport.OpenFile(normalized_path)) {
    return;
  }
  std::string language_id;
  LspClient* client = LspClientForViewport(viewport, &language_id);
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

}  // namespace microide::workspace
