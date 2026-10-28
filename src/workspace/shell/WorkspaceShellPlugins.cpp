#include "workspace/shell/WorkspaceShell.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/services/RemoteHostService.h"
#include "workspace/shell/ShellGlueCache.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <system_error>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "editor/EditBatchOrder.h"
#include "project/CompileCommandsLocator.h"
#include "util/JsonValue.h"
#include "util/PathMatch.h"
#include "util/PerformanceTrace.h"
#include "util/StartupTrace.h"
#include "workspace/services/EditorTabService.h"
#include "workspace/FileUri.h"
#include "workspace/lsp/LspPositionEncoding.h"
#include "workspace/services/PromptSurfaceService.h"
#include "workspace/coordinators/WorkspacePathMutationCoordinator.h"
#include "workspace/lsp/LspViewportPositions.h"
#include "workspace/PluginRasterSize.h"
#include "workspace/SettingFlags.h"
#include "workspace/actions/WorkspaceActionCoordinator.h"
// WorkspaceShell's constructor lives here, so this TU must see the complete type
// of everything the shell holds by unique_ptr.
#include "workspace/coordinators/WorkspaceKeyInputCoordinator.h"
#include "workspace/coordinators/WorkspaceChromeMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceEditorMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceMenuCoordinator.h"
#include "workspace/coordinators/WorkspacePanelMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceSidebarMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceTabMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceTextInputCoordinator.h"
#include "workspace/services/SidebarService.h"
#include "workspace/services/CompareMergeService.h"
#include "workspace/services/TerminalPanelService.h"
#include "workspace/services/ProjectCatalogService.h"
#include "workspace/debug/DebugPaneService.h"
#include "workspace/coordinators/WorkspaceMergeMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceCompareMouseCoordinator.h"
#include "workspace/debug/DebugPaneMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceCommandLineCoordinator.h"
#include "workspace/coordinators/WorkspaceLifecycleCoordinator.h"
#include "workspace/registries/WorkspaceCommandRegistry.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"
#include "workspace/shell/WorkspaceShellBootstrapper.h"

namespace microide::workspace {

namespace {

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
            (view.content_pending() ? view.restore.path : view.viewport.path()).lexically_normal();
        if (!path.empty()) {
          if (view.content_pending()) {
            callback(path, nullptr);
          } else {
            callback(path, &view.viewport);
          }
        }
        continue;
      }
      if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
          tab.compare->right_editable && !tab.compare->right_viewport.path().empty()) {
        callback(util::NormalizedPath(tab.compare->right_viewport.path()),
                 &tab.compare->right_viewport);
        continue;
      }
      if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value() &&
          !tab.merge->result_viewport.path().empty()) {
        callback(util::NormalizedPath(tab.merge->result_viewport.path()),
                 &tab.merge->result_viewport);
      }
    }
  }
}

}  // namespace

// WorkspaceShell's CONSTRUCTOR, in the plugin TU. Not an accident and not a good
// home: a constructor that builds `unique_ptr` members needs their complete
// types, and this is the only shell TU that already includes every coordinator
// header. `WorkspaceShell.cpp`, where the destructor is and where a reader
// looks, is within a handful of code lines of its cap and the companion-TU count
// is at its own, so there is nowhere better to put it today. The destructor
// carries the matching pointer (TD-2026-09-13-293b).
WorkspaceShell::WorkspaceShell() {
  // Bind the settings store to the layered backing vectors. The user layer is
  // stable for the shell's lifetime; the project layer is re-bound whenever the
  // active project state is moved (RebindProjectState / reset).
  settings_store_.BindUserLayer(&context_.user_settings);
  settings_store_.BindActiveProject(&context_.current_project_state.settings);
  // Load the persisted recent-projects/files MRU so the welcome surface and the
  // empty file finder can surface them immediately on first paint.
  recents_service_.Configure(persistence_service_);
  // Bound here rather than on the SDL window-init path: git write actions are
  // reachable from the control channel and headless dispatch, where that path never
  // runs, and unbound callbacks would silently swallow every operation's result.
  InitializeGitOperationService();
  virtual_document_registry_.SetOnChange(
      [this](const std::string& uri) { ReloadVirtualDocumentTabs(uri); });
  const Bootstrapper wiring(*this);
  wiring.WireQuickPick();
  wiring.WireTabStrip();
  wiring.WireLsp();
  wiring.WireDebug();
  wiring.WireControlChannel();
  wiring.WireWorkspaceEdits();
  wiring.WireAssist();
  wiring.WirePluginRuntime();
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
  // The eighth sibling, and the one that used to be missing: without this the
  // read side (open-in-tab, reload-on-change, read-only enforcement, the
  // `open_file` virtual:// branch) was complete and unreachable, because the
  // registry's only writer was the test backdoor (TD-2026-07-27-001).
  virtual_document_registry_.Clear();
  for (const auto& document : host.ContributedVirtualDocuments()) {
    virtual_document_registry_.Register(VirtualDocumentSpec{
        .uri = document.uri,
        .language_id = document.language_id,
        .content = document.content,
        .editable = document.editable,
        .plugin_id = document.plugin_id,
    });
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
      const platform::ProcessLauncher& launcher = context_.current_project_state.launcher();
      project::ClangdCommandPlan plan = project::PlanClangdCommand(
          platform::ExpandWorkspaceFolder(language_server.command,
                                          context_.current_project_state.root, launcher),
          context_.current_project_state.root, launcher);
      if (!plan.note.empty()) {
        SDL_Log("%s", plan.note.c_str());
      }
      std::vector<std::string> command = std::move(plan.command);
      CurrentLspManager().RegisterServer(language_server.language_ids,
                                         context_.current_project_state.launcher(), command,
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
      CurrentDapManager().RegisterAdapter(
          adapter.type, context_.current_project_state.launcher(),
          platform::ExpandWorkspaceFolder(adapter.command, context_.current_project_state.root,
                                          context_.current_project_state.launcher()),
          adapter.sandbox);
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
  auto& coordinator = MakePersistenceCoordinator();
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
  const auto plugin_error_count = [this]() {
    const std::vector<std::string>* entries = OutputChannelEntries("plugins.error");
    return entries != nullptr ? entries->size() : std::size_t{0};
  };
  const std::size_t errors_before = plugin_error_count();
  plugin_runtime_.ReloadAsync(
      project_root,
      context_.current_project_state.locality,
      request.syntax_definitions,
      [this, request, generation, project_root, errors_before, plugin_error_count](bool clean_reload) {
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
        // A plugin that failed to load said so only in the Plugin Errors channel
        // unless the user had asked for the reload: at startup, a project switch or
        // a plugin file changing on disk it failed in silence (QA plugin-errors).
        // VS Code raises an error notification when an extension fails to activate.
        if (const std::size_t errors_after = plugin_error_count();
            !request.announce && errors_after > errors_before) {
          const std::vector<std::string>* entries = OutputChannelEntries("plugins.error");
          std::string message = (*entries)[errors_before];
          if (errors_after - errors_before > 1) {
            message += " (and " + std::to_string(errors_after - errors_before - 1) + " more)";
          }
          Notify(NotificationService::Request{
              .tone = NotificationService::Tone::Warning,
              .key = "plugins.load-errors",
              .message = std::move(message),
              .actions = {NotificationAction{.label = "Show Errors",
                                             .id = ActionId::ShowOutput,
                                             .args = {"plugins.error"}}},
          });
        }
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
  // A plugin keybinding on a chord another binding holds is not registered (Help
  // and Settings never advertise a key that cannot fire), and the plugin's author
  // is told which binding has it (TD-2026-10-20-369).
  if (const auto& contributed = plugin_runtime_.Host().ContributedKeybindings(); !contributed.empty()) {
    for (const ShadowedKeybinding& shadowed : ShadowedContributedKeybindings(
             contributed, context_.disabled_keybinding_ids,
             ParseSkipShellList(GetSettingValue("terminal.commands_to_skip_shell")))) {
      plugin_runtime_.AppendLog("keybinding " + shadowed.id + " (" + shadowed.chord + ") not registered: " +
                                shadowed.held_by + " already uses " + shadowed.chord);
    }
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
    // The reload ran off the UI thread, so a file can have become the active
    // editor before the servers it contributes were registered: its open found no
    // server, and the status read "Starting…" with nothing ever started (QA
    // lsp-csharp: `project-open` then `open`). A server becoming available for the
    // active editor is the moment activating it would have engaged one.
    if (const editor::TextViewport* active = ActiveEditableViewport();
        active != nullptr && !active->path().empty()) {
      NotifyLspBufferOpen(active->path());
    }
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
  if (request.announce) {
    // The user asked for this reload: its own summary, now that it has landed.
    std::string summary = PluginRuntimeReloadSummary();
    context_.current_project_state.panel.feedback.text = summary;
    if (!summary.empty()) {
      // Heuristic: surface failures/warnings as Warning, otherwise Info.
      const bool looks_problematic =
          summary.find("error") != std::string::npos || summary.find("fail") != std::string::npos ||
          summary.find("warn") != std::string::npos;
      Notify(looks_problematic ? NotificationService::Tone::Warning : NotificationService::Tone::Info,
             std::move(summary));
    }
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
    const std::string_view language = viewport.language_id();
    return !language.empty() && changed_language_set.contains(language);
  };

  if (editor::TextViewport* viewport = ActiveEditorViewport();
      viewport != nullptr && should_invalidate_viewport(*viewport)) {
    viewport->InvalidateSyntaxHighlighting();
  }

  for (auto& tab : context_.current_project_state.focused_group().open_tabs) {
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      auto& editor_state = *tab.editor_state;
      if (!editor_state.content_pending() && should_invalidate_viewport(editor_state.viewport)) {
        editor_state.viewport.InvalidateSyntaxHighlighting();
      }
      continue;
    }

    if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
        !tab.compare->right_viewport.path().empty()) {
      const std::string_view language = tab.compare->right_viewport.language_id();
      if (language.empty() || !changed_language_set.contains(language)) {
        continue;
      }
      RefreshCompareTabDerivedState(*tab.compare);
      continue;
    }

    if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
      auto& merge_tab = *tab.merge;
      const std::string_view language = merge_tab.result_viewport.language_id();
      if (language.empty() || !changed_language_set.contains(language)) {
        continue;
      }
      merge_tab.incoming_token_window.Reset(
          merge_tab.model.incoming_lines.size(),
          editor::SyntaxHighlighter::InitialState(merge_tab.output_path,
                                                  merge_tab.model.incoming_lines));
      merge_tab.current_token_window.Reset(
          merge_tab.model.current_lines.size(),
          editor::SyntaxHighlighter::InitialState(merge_tab.output_path,
                                                  merge_tab.model.current_lines));
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
  const std::filesystem::path normalized_path = util::NormalizedPath(path);
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
  const std::filesystem::path normalized_path = util::NormalizedPath(path);
  if (normalized_path.empty()) {
    return;
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || !util::SameAsNormalizedPath(viewport->path(), normalized_path)) {
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
  const std::filesystem::path normalized_path = util::NormalizedPath(path);
  if (plugin_runtime_.enabled() && !normalized_path.empty()) {
    plugin_runtime_.Host().OnBufferSave(normalized_path);
  }
  if (normalized_path.empty()) {
    return;
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || !util::SameAsNormalizedPath(viewport->path(), normalized_path)) {
    return;
  }
  // Copied, not bound by reference: this is held across EnsureLspDocumentOpen and
  // the callback install below, and language_id() hands back a reference into a
  // memo that any intervening re-resolve on this viewport would overwrite.
  const std::string language_id = viewport->language_id();
  if (language_id.empty()) {
    return;
  }
  LspClient* client = CurrentLspManager().FindStartedServer(language_id);
  if (client == nullptr) {
    return;
  }
  client->SetDiagnosticsCallback(
      [this, project_root = util::NormalizedPath(context_.current_project_state.root), client](
          std::string uri, std::vector<LspClient::Diagnostic> diagnostics) {
        // Resolve the active project at dispatch time and drop the publish if the
        // active project changed since this callback was installed. A raw
        // &current_project_state capture would otherwise publish diagnostics from an
        // old server into whatever project happens to be current now.
        ProjectWorkspaceState& current = context_.current_project_state;
        if (!util::SameAsNormalizedPath(current.root, project_root)) {
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
  // Code lenses are pull-based too; re-request for the saved content.
  lsp_service_.RequestLspCodeLenses(*viewport, *client);
}

void WorkspaceShell::NotifyLspBufferClose(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = util::NormalizedPath(path);
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
  // Percent-encoded to match the open URI (FileUriForPath); a raw "file://" +
  // path never matches HasOpenDocument for special-char paths, so didClose would
  // be skipped and the server would leak the document (and its diagnostics).
  const std::string uri = FileUriForPath(normalized_path);
  lsp_service_.ForgetDocument(uri);
  if (CurrentLspManager().CloseDocumentEverywhere(uri) == 0) {
    return;
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
  const std::filesystem::path path = util::NormalizedPath(viewport->path());
  if (!request.path.empty() && !util::SameAsNormalizedPath(request.path, path)) {
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
      util::NormalizedPath(viewport->path()), viewport->content_revision(),
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
