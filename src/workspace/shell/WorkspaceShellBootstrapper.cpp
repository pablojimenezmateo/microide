#include <memory>

#include "workspace/services/RemoteHostService.h"
#include "workspace/shell/WorkspaceShellBootstrapper.h"

#include "workspace/coordinators/SelectionAutoscroll.h"

#include <optional>
#include <utility>

#include "workspace/services/TerminalPanelService.h"
#include "workspace/actions/WorkspaceActionCoordinator.h"
#include "workspace/coordinators/WorkspaceCommandLineCoordinator.h"
#include "workspace/coordinators/WorkspaceKeyInputCoordinator.h"
#include "workspace/coordinators/WorkspaceSidebarMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceTabMouseCoordinator.h"
#include "workspace/shell/WorkspaceShell.h"
#include "workspace/PluginRasterSize.h"
#include "workspace/WorkspaceFileDrop.h"
#include "workspace/coordinators/WorkspaceTextInputCoordinator.h"
#include "workspace/control/ControlUiQuery.h"
#include "util/PerformanceTrace.h"
#include "workspace/WorkspaceCommandParsing.h"
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
#include "util/StartupTrace.h"
#include "workspace/services/EditorTabService.h"
#include "workspace/FileUri.h"
#include "workspace/lsp/LspPositionEncoding.h"
#include "workspace/services/PromptSurfaceService.h"
#include "workspace/coordinators/WorkspacePathMutationCoordinator.h"
#include "workspace/lsp/LspViewportPositions.h"
#include "workspace/SettingFlags.h"
#include "workspace/coordinators/WorkspaceChromeMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceEditorMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceMenuCoordinator.h"
#include "workspace/coordinators/WorkspacePanelMouseCoordinator.h"
#include "workspace/services/SidebarService.h"
#include "workspace/services/CompareMergeService.h"
#include "workspace/services/ProjectCatalogService.h"
#include "workspace/debug/DebugPaneService.h"
#include "workspace/coordinators/WorkspaceMergeMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceCompareMouseCoordinator.h"
#include "workspace/debug/DebugPaneMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceLifecycleCoordinator.h"
#include "workspace/registries/WorkspaceCommandRegistry.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"

namespace microide::workspace {

namespace {

bool IsVirtualDocumentUri(std::string_view text) {
  return text.starts_with("virtual://");
}

}  // namespace

namespace {

// MICROIDE_TRACE_INPUT=1: one log line per key, text and pointer event, naming
// the focus it met, the focus it left, and whether a handler took it. A keystroke
// the QA suite saw go missing on a slow runner (TD-2026-10-20-368) left a buffer
// without it and nothing that said where it went. The QA driver also orders its
// control commands after the input it sent through X by finding it here
// (`_await_typed_input`): a query sent right after a click could otherwise be
// answered before the click was handled.
bool InputTraceEnabled() {
  static const bool enabled = util::PerformanceTrace::FlagEnabled("MICROIDE_TRACE_INPUT");
  return enabled;
}

void TraceTypedEvent(const SDL_Event& event, FocusTarget before, FocusTarget after, bool handled) {
  if (!InputTraceEnabled()) {
    return;
  }
  const std::string_view from = ControlUiFocusName(before);
  const std::string_view to = ControlUiFocusName(after);
  switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      SDL_Log("microide input: button=%d down=%d clicks=%d x=%.0f y=%.0f focus=%.*s->%.*s "
              "handled=%d",
              event.button.button, event.button.down ? 1 : 0, event.button.clicks,
              event.button.x, event.button.y, static_cast<int>(from.size()), from.data(),
              static_cast<int>(to.size()), to.data(), handled ? 1 : 0);
      return;
    case SDL_EVENT_MOUSE_WHEEL:
      SDL_Log("microide input: wheel=%.0f x=%.0f y=%.0f focus=%.*s->%.*s handled=%d",
              event.wheel.y, event.wheel.mouse_x, event.wheel.mouse_y,
              static_cast<int>(from.size()), from.data(), static_cast<int>(to.size()), to.data(),
              handled ? 1 : 0);
      return;
    case SDL_EVENT_MOUSE_MOTION:
      SDL_Log("microide input: motion x=%.0f y=%.0f", event.motion.x, event.motion.y);
      return;
    default:
      break;
  }
  if (event.type == SDL_EVENT_TEXT_INPUT) {
    SDL_Log("microide input: text=\"%s\" focus=%.*s->%.*s handled=%d", event.text.text,
            static_cast<int>(from.size()), from.data(), static_cast<int>(to.size()), to.data(),
            handled ? 1 : 0);
    return;
  }
  SDL_Log("microide input: key=%s mod=0x%x repeat=%d focus=%.*s->%.*s handled=%d",
          SDL_GetKeyName(event.key.key), static_cast<unsigned>(event.key.mod),
          event.key.repeat ? 1 : 0, static_cast<int>(from.size()), from.data(),
          static_cast<int>(to.size()), to.data(), handled ? 1 : 0);
}

}  // namespace

WorkspaceShell::EventResult WorkspaceShell::HandleEvent(const SDL_Event& event) {
  // Escape puts away a card Show Hover pinned to the caret, before anything else
  // reads the key -- as in VS Code, where it closes the hover and nothing more.
  if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE &&
      context_.interaction_state.caret_hover.active) {
    context_.interaction_state.caret_hover.active = false;
    if (active_editor_hover_target_.has_value()) {
      active_editor_hover_target_.reset();
      ++editor_hover_target_generation_;
    }
    RequestWindowRedraw();
    // Traced like every other key: the QA driver orders its control commands
    // after the keys it sent by finding them in this log.
    const FocusTarget focus = context_.current_project_state.surface.focus;
    TraceTypedEvent(event, focus, focus, true);
    return EventResult{.handled = true, .redraw = ConsumePendingRenderInvalidation()};
  }
  const FocusTarget focus_before = context_.current_project_state.surface.focus;
  const DragTarget drag_before = context_.interaction_state.drag_target;
  // A keystroke a read-only view refused is said out loud (VS Code's "Cannot edit
  // in read-only editor"), not swallowed.
  const bool typing = event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_TEXT_INPUT;
  const editor::TextViewport* typed_into = typing ? ActiveEditorViewport() : nullptr;
  const std::uint64_t refused_before = typed_into != nullptr ? typed_into->refused_edits() : 0;
  EventResult result = Bootstrapper(*this).BuildEventDispatcher().Handle(event);
  if (typed_into != nullptr && ActiveEditorViewport() == typed_into &&
      typed_into->refused_edits() > refused_before) {
    Notify(NotificationService::Request{.tone = NotificationService::Tone::Info,
                                        .key = "editor.read-only",
                                        .message = "Cannot edit in read-only editor"});
  }
  const FocusTarget focus_after = context_.current_project_state.surface.focus;
  if (typing || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
      event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_WHEEL ||
      event.type == SDL_EVENT_MOUSE_MOTION) {
    TraceTypedEvent(event, focus_before, focus_after, result.handled);
  }
  // Dividers and scrollbar thumbs paint highlighted while dragged, so a press or
  // release that changes the drag target repaints both regions, whichever
  // handler changed it: pressing the sidebar divider damaged nothing, and its
  // highlight showed only once the first drag motion repainted (QA monkey).
  if (const DragTarget drag_after = context_.interaction_state.drag_target;
      drag_after != drag_before && !result.redraw.full) {
    if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
      for (const DragTarget target : {drag_before, drag_after}) {
        if (const auto rect = DragTargetHighlightRect(*layout, target); rect.has_value()) {
          result.redraw.Add(*rect);
        }
      }
    }
  }
  // Clear any live ghost text the moment the caret/content/focus no longer matches
  // its anchor, before sampling reactive events. No-op (one null check) unless a
  // suggestion is showing, so it stays off the hot path.
  InvalidateGhostTextIfStale();
  // Single chokepoint for reactive editor-event sampling: diff the active buffer's
  // content/caret/selection once per input batch (no-op unless a plugin subscribes).
  SamplePluginEditorEvents();
  // Re-arm the "after delay" autosave debounce on real buffer edits so a settled dirty
  // buffer saves itself. No-op (one setting read) unless autosave=after_delay.
  MaybeArmAutosaveTimer();
  // Always-on crash-safety: re-arm the session-snapshot flush so a settled dirty buffer's
  // unsaved content is durably staged for restore even if the process later crashes.
  MaybeArmSessionFlushTimer();
  // The Host view's subscription follows what is on screen, whichever handler
  // showed or hid it (a key, a click, a project switch). A compare when nothing changed.
  if (glue_->remote_host_service != nullptr) {
    glue_->remote_host_service->SetHostViewVisible(HostViewShown(context_.current_project_state.sidebar),
                                                   context_.current_project_state.root);
  }
  return result;
}

CommandLineCoordinator& WorkspaceShell::MakeCommandLineCoordinator() {
  if (glue_->command_line_coordinator != nullptr) {
    return *glue_->command_line_coordinator;
  }
  glue_->command_line_coordinator = std::make_unique<CommandLineCoordinator>(
      context_.current_project_state,
      available_colorscheme_names_,
      CommandLineCoordinator::Operations{
          .execute_action =
              [this](ActionId id, const std::vector<std::string>& args, ActionSource source) {
                return ActionCoordinator(MakeActionContext()).Execute(id, args, source);
              },
          .plugin_command_names =
              [this]() -> const std::vector<std::string>& {
                // Hand back the stable host-owned vector directly (no copy); the
                // command-line coordinator only iterates it to append prefix
                // candidates (TD-2026-07-17A-012).
                return plugin_runtime_.Host().CommandNames();
              },
          .sidebar_view_ids = [this]() { return OrderedSidebarViewIds(); },
          .remote_hosts = [this]() { return MakeRemoteHostService().KnownHosts(); },
          .execute_plugin_command =
              [this](const std::string& command, const std::vector<std::string>& args) {
                CommandLineCoordinator::PluginCommandResult result;
                // Recognition is synchronous; execution runs on the plugin worker
                // and its real outcome surfaces as a toast on the drain. Confirm
                // dispatch in the prompt so the user never sees silence.
                result.handled = plugin_runtime_.Host().HasCommand(command);
                if (!result.handled) {
                  return result;
                }
                result.feedback = "ran " + command;
                plugin_runtime_.Host().ExecuteCommandAsync(
                    command, args,
                    [this, command](bool ran, std::string error, std::string feedback) {
                      NotifyPluginCommandOutcome(command, ran, error, feedback);
                    });
                return result;
              },
      });
  return *glue_->command_line_coordinator;
}

WorkspaceShell::Bootstrapper::Bootstrapper(WorkspaceShell& shell) : shell_(shell) {}

ActionAvailability WorkspaceShell::Bootstrapper::BuildActionAvailability() const {
  auto* shell = &shell_;
  return ActionAvailability(
      shell->context_,
      shell->debug_service_,
      ActionAvailability::Operations{
          .selected_tree_target_kind = [shell]() { return shell->SelectedTreeTargetKind(); },
          .resolve_tree_action_path =
              [shell](WorkspaceShell::ActionSource source) {
                return shell->ResolveTreeActionPath(source);
              },
          .active_tab_path = [shell]() { return shell->ActiveTabPath(); },
          .active_navigable_viewport = [shell]() { return shell->ActiveNavigableViewport(); },
          .active_editable_viewport = [shell]() { return shell->ActiveEditableViewport(); },
          .current_text_input_surface = [shell]() { return shell->CurrentTextInputSurface(); },
          .active_single_line_text_has_selection =
              [shell]() {
                return shell->MakeTextInputCoordinator().HasSelectionAtActiveSingleLineSurface();
              },
          .has_last_terminal_command = [shell]() { return shell->HasLastTerminalCommand(); },
          .terminal_has_selection = [shell]() { return shell->TerminalHasSelection(); },
          .active_tab_is_editor = [shell]() { return shell->ActiveTabIsEditor(); },
          .editor_group_count = [shell]() { return shell->EditorGroupCount(); },
          .active_tab_is_compare = [shell]() { return shell->ActiveTabIsCompare(); },
          .active_tab_is_merge = [shell]() { return shell->ActiveTabIsMerge(); },
          .active_compare_tab = [shell]() { return shell->ActiveCompareTab(); },
          .active_merge_tab = [shell]() { return shell->ActiveMergeTab(); },
          .active_completion_available = [shell]() {
            return shell->HasActiveCompletionProvider();
          },
          .active_code_actions_available = [shell]() {
            return shell->HasActiveCodeActionProvider();
          },
          .active_definition_available = [shell]() {
            return shell->HasActiveDefinitionProvider();
          },
          .active_references_available = [shell]() {
            return shell->HasActiveReferencesProvider();
          },
          .get_setting_value =
              [shell](std::string_view id) { return shell->GetSettingValue(id); },
          .remote_hosts = [shell]() -> const RemoteHostService* { return shell->glue_->remote_host_service.get(); },
          .has_remote_recent =
              [shell, cached = std::make_shared<std::optional<std::pair<std::uint64_t, bool>>>()]() {
                // Per painted row while the menu is open: the disk once per change.
                const RecentsService& recents = shell->recents_service_;
                if (!cached->has_value() || (*cached)->first != recents.revision()) {
                  const auto& ids = recents.RecentProjects();
                  *cached = std::pair{recents.revision(),
                                      std::any_of(ids.begin(), ids.end(), [](const project::ProjectId& id) {
                                        return RemoteHostService::HasMirrorOnDisk(id);
                                      })};
                }
                return (*cached)->second;
              },
      });
}

WorkspaceEventDispatcher WorkspaceShell::Bootstrapper::BuildEventDispatcher() const {
  auto* shell = &shell_;
  return WorkspaceEventDispatcher(
      WorkspaceEventDispatcher::Runtime{
          .project_open_dialog_event_type = shell->project_open_dialog_event_type_,
          .git_blame_event_type = shell->git_blame_event_type_,
          .git_sidebar_event_type = shell->git_sidebar_event_type_,
          .terminal_event_type = shell->terminal_event_type_,
          .project_file_event_type = shell->project_file_event_type_,
          .lsp_event_type = shell->lsp_event_type_,
          .dap_event_type = shell->dap_event_type_,
          .control_event_type = shell->control_event_type_,
          .plugin_thread_event_type = shell->plugin_thread_event_type_,
          .highlight_prefetch_event_type = shell->highlight_prefetch_event_type_,
          .raster_decode_event_type = shell->surface_texture_cache_.wake_channel(),
      },
      WorkspaceEventDispatcher::State{
          .window_has_input_focus = shell->context_.interaction_state.window_has_input_focus,
          .key_modifiers = shell->context_.interaction_state.key_modifiers,
      },
      WorkspaceEventDispatcher::Operations{
          .sync_terminal_focus = [shell]() { shell->SyncTerminalFocusState(); },
          .consume_pending_render_invalidation =
              [shell]() { return shell->ConsumePendingRenderInvalidation(); },
          .consume_pending_dialog_results =
              [shell]() {
                shell->ConsumePendingProjectOpenDialogResult();
                shell->ConsumePendingOpenFileDialogResult();
                shell->ConsumePendingFontFileDialogResult();
              },
          .reload_plugins_if_assets_changed =
              [shell](bool force_check) {
                return shell->ReloadPluginsIfPluginAssetsChanged(force_check);
              },
          .plugin_runtime_consume_wake_event =
              [shell](Uint32 type) { return shell->plugin_runtime_.ConsumeWakeEvent(type); },
          .reload_project_if_files_changed =
              [shell](bool force_check) { return shell->ReloadProjectIfFilesChanged(force_check); },
          .project_search_handles_event =
              [shell](Uint32 type) { return shell->project_search_runtime_.HandlesEvent(type); },
          .consume_project_search_updates =
              [shell]() { shell->ConsumeProjectSearchUpdates(); },
          .request_focused_editor_redraw =
              [shell]() { shell->RequestFocusedEditorRedraw(); },
          .consume_git_sidebar_refresh =
              [shell]() { shell->ConsumeGitSidebarRefresh(); },
          .apply_git_blame_results =
              [shell]() { return shell->git_blame_service_.ApplyReadyResults(); },
          .consume_highlight_prefetch_results =
              [shell]() { shell->ConsumeHighlightPrefetchResults(); },
          .apply_decoded_raster_sizes =
              [shell]() {
                // A placeholder may be waiting for this image wherever it shows: an
                // inline inset in the editor, or the bottom panel's preview.
                if (ApplyDecodedRasterSizes(shell->surface_texture_cache_, shell->context_)) {
                  shell->RequestEditorSurfaceRedraw();
                  shell->RequestBottomPanelContentRedraw();
                }
              },
          .consume_lsp_callbacks =
              [shell]() { shell->ConsumeLspCallbacks(); },
          .consume_dap_callbacks =
              [shell]() { shell->ConsumeDapCallbacks(); },
          .consume_control_callbacks =
              [shell]() { shell->ConsumeControlCallbacks(); },
          .consume_plugin_thread_actions =
              [shell]() { shell->plugin_runtime_.DrainPluginThreadActions(); },
          .consume_terminal_session_updates =
              [shell]() { shell->ConsumeTerminalSessionUpdates(); },
          .sync_text_input_surface =
              [shell](SDL_Window* window) {
                shell->MakeTextInputCoordinator().SyncTextInputSurface(window);
              },
          .handle_mouse_button_down =
              [shell](const SDL_Event& event) { return shell->HandleMouseButtonDown(event); },
          .handle_mouse_button_up =
              [shell](const SDL_Event& event) { return shell->HandleMouseButtonUp(event); },
          .handle_mouse_motion =
              [shell](const SDL_Event& event) { return shell->HandleMouseMotion(event); },
          .handle_mouse_wheel =
              [shell](const SDL_Event& event) { return shell->HandleMouseWheel(event); },
          .handle_text_editing =
              [shell](const SDL_TextEditingEvent& event) {
                return shell->MakeTextInputCoordinator().HandleTextEditing(event);
              },
          .handle_text_input =
              [shell](const SDL_TextInputEvent& event) {
                return shell->MakeTextInputCoordinator().HandleTextInput(event);
              },
          .handle_window_mouse_leave = [shell]() { shell->ClearMouseHoverState(); },
          .force_cursor_reassert = [shell]() { shell->ForceCursorReassert(); },
          .autosave_on_focus_lost = [shell]() { shell->MaybeAutosaveDirtyTabs(true); },
          .end_selection_gesture =
              [shell]() {
                shell->context_.interaction_state.mouse_selecting = false;
                shell->context_.interaction_state.text_drag =
                    InteractionState::TextDragState::None;
                shell->context_.interaction_state.text_drag_has_drop = false;
                shell->context_.interaction_state.editor_box_selecting = false;
                selection_autoscroll::Disarm(shell->context_.interaction_state);
                // A tab drag is a drag too, and it used to survive the focus loss:
                // the strip stayed with a lifted tab and a floating ghost pinned to
                // a pointer that was no longer sending us anything, and its
                // auto-scroll kept the animation tick awake.
                shell->MakeTabMouseCoordinator().CancelDrag();
              },
          .request_window_redraw = [shell]() { shell->RequestWindowRedraw(); },
          .handle_key_down =
              [shell](const SDL_KeyboardEvent& event) {
                // Escape abandons a live tab drag, ahead of every surface that also
                // answers to Escape — a drag owns the pointer, so while one is in
                // flight it owns the cancel key too.
                if (event.key == SDLK_ESCAPE &&
                    shell->context_.interaction_state.tab_drag.kind != TabDragKind::None &&
                    shell->MakeTabMouseCoordinator().CancelDrag()) {
                  shell->RequestWindowRedraw();
                  return true;
                }
                if (event.key == SDLK_ESCAPE && shell->context_.interaction_state.path_drag.armed &&
                    shell->MakeSidebarMouseCoordinator().CancelPathDrag()) {
                  shell->RequestWindowRedraw();
                  return true;
                }
                return shell->MakeKeyInputCoordinator().HandleKeyDown(event);
              },
          .handle_key_up =
              [shell](const SDL_KeyboardEvent& event) {
                const ProjectWorkspaceState& state = shell->context_.current_project_state;
                if ((event.key != SDLK_LCTRL && event.key != SDLK_RCTRL) || !state.overlay.visible ||
                    state.overlay.mode != OverlayMode::QuickPick ||
                    state.overlay.workflow.quick_pick.kind != QuickPickKind::RecentEditor) {
                  return false;
                }
                return shell->ActivateOverlaySelection();
              },
          .handle_file_drop =
              [shell](const char* dropped_path) {
                if (dropped_path == nullptr) {
                  return false;
                }
                // What a drop means is decided in WorkspaceFileDrop.h and executed
                // through the two-call port below, so the shell gains no member and
                // the behaviour is testable without an SDL event or a window.
                const FileDropRequest request = ResolveFileDrop(
                    std::filesystem::path(dropped_path),
                    !shell->context_.current_project_state.root.empty());
                return ApplyFileDrop(
                    request,
                    FileDropOperations{
                        .open_project =
                            [shell](const std::filesystem::path& root) {
                              return shell->OpenProjectTab(root, true, true);
                            },
                        .open_file =
                            [shell](const std::filesystem::path& path) {
                              shell->OpenFile(path);
                            },
                    });
              },
      });
}

WorkspaceWakeController WorkspaceShell::Bootstrapper::BuildWakeController() const {
  auto* shell = &shell_;
  return WorkspaceWakeController(
      WorkspaceWakeController::Operations{
          .reload_plugins_if_assets_changed =
              [shell](bool force_check) {
                return shell->ReloadPluginsIfPluginAssetsChanged(force_check);
              },
          .caret_blink_animating = [shell]() { return shell->CaretBlinkAnimating(); },
          .current_caret_dirty_rect = [shell]() { return shell->CurrentCaretDirtyRect(); },
      });
}

void WorkspaceShell::Bootstrapper::WireQuickPick() const {
  WorkspaceShell* const shell = &shell_;
  shell->quick_pick_service_.Configure(
      shell->context_.current_project_state.overlay.workflow.quick_pick,
      QuickPickService::Operations{
          .show_overlay = [shell]() { shell->ShowOverlay(OverlayMode::QuickPick); },
          .showing =
              [shell]() {
                return shell->context_.current_project_state.overlay.visible &&
                       shell->context_.current_project_state.overlay.mode == OverlayMode::QuickPick;
              },
          .list_changed =
              [shell]() {
                shell->ResetOverlayScroll();
                shell->RequestOverlayRedraw();
              },
          .start_launch_config =
              [shell](std::size_t index) {
                ProjectWorkspaceState& state = shell->context_.current_project_state;
                if (index >= state.launch_configs.size()) {
                  return;
                }
                // Remembered, so Start Debugging and a restart reuse it.
                state.selected_launch_config_index = index;
                shell->StartDebugging(state.launch_configs[index], state.root.generic_string());
              },
          .apply_colorscheme =
              [shell](std::string_view name, bool persist) {
                (void)shell->MakePersistenceCoordinator().ApplyColorscheme(name, persist, false);
                shell->RequestWindowRedraw();
              },
          .reveal =
              [shell](const std::filesystem::path& path, std::size_t line, std::size_t column) {
                editor::TextViewport* viewport = shell->ActiveEditorViewport();
                if (viewport == nullptr || viewport->path() != path || line >= viewport->lines().size()) {
                  return;
                }
                viewport->JumpCursorTo(line, std::min(column, viewport->lines().LineLength(line)));
                viewport->CenterLine(line);
                shell->RequestFocusedEditorRedraw();
              },
          .restore_view =
              [shell](const QuickPickState::View& view) {
                editor::TextViewport* viewport = shell->ActiveEditorViewport();
                if (viewport == nullptr || viewport->path() != view.path) {
                  return;
                }
                viewport->ApplyRestoredViewState(view.line, view.column, view.scroll, viewport->horizontal_scroll());
                shell->RequestFocusedEditorRedraw();
              },
          .requery =
              [shell](QuickPickKind kind, const std::string& query) {
                if (kind != QuickPickKind::WorkspaceSymbol) {
                  return;
                }
                const std::filesystem::path root = shell->context_.current_project_state.root;
                std::string error;
                const bool asked = shell->assist_service_.navigation().QueryWorkspaceSymbols(
                    query,
                    [shell, root](std::vector<NavigationAssist::WorkspaceSymbolHit> hits) {
                      std::vector<QuickPickItem> items;
                      items.reserve(hits.size());
                      for (NavigationAssist::WorkspaceSymbolHit& hit : hits) {
                        const std::filesystem::path relative = hit.path.lexically_relative(root);
                        std::string where = relative.empty() || util::RelativePathClimbsOut(relative)
                                                ? hit.path.generic_string()
                                                : relative.generic_string();
                        items.push_back(QuickPickItem{
                            .index = items.size(),
                            .value = hit.path.string(),
                            .line = hit.line,
                            .column = static_cast<std::size_t>(std::max(hit.character, 0)),
                            .primary_label = std::move(hit.name),
                            .secondary_label = hit.container.empty() ? where : hit.container + " · " + where,
                            .search_text = {},
                        });
                      }
                      (void)shell->quick_pick_service_.DeliverItems(QuickPickKind::WorkspaceSymbol, std::move(items));
                    },
                    &error);
                if (!asked) {
                  shell->quick_pick_service_.Fail(QuickPickKind::WorkspaceSymbol,
                                           error.empty() ? "No language server to ask" : error);
                }
              },
          .open_workspace_symbol =
              [shell](const std::filesystem::path& path, std::size_t line, std::size_t character) {
                shell->assist_service_.navigation().OpenWorkspaceSymbol(path, line, static_cast<int>(character));
              },
          .set_language = [shell](const std::string& language) { shell->ApplyLanguageModeToActiveEditor(language); },
          .activate_tab =
              [shell](std::uint64_t stable_id) {
                const EditorGroup& group = shell->context_.current_project_state.focused_group();
                for (std::size_t i = 0; i < group.open_tabs.size(); ++i) {
                  if (group.open_tabs[i].stable_id == stable_id) {
                    shell->MakeEditorTabService().Activate(i);
                    shell->RequestWindowRedraw();
                    return;
                  }
                }
              },
          .run_command_line =
              [shell](const std::string& command_line) {
                const ParsedCommandLine parsed = ParseCommandLine(command_line);
                if (parsed.tokens.empty()) {
                  return;
                }
                std::vector<std::string> args;
                for (std::size_t i = 1; i < parsed.tokens.size(); ++i) {
                  args.push_back(parsed.tokens[i].text);
                }
                (void)shell->ExecuteCommandName(parsed.tokens.front().text, args, ActionSource::Command);
              },
      });
}

void WorkspaceShell::Bootstrapper::WireTabStrip() const {
  WorkspaceShell* const shell = &shell_;
  shell->tab_strip_chrome_.Configure(
      shell->context_, shell->tab_strip_service_, shell->layout_mode_service_, shell->output_channels_,
      WorkspaceTabStripChrome::Operations{
          .project_tab_display_title =
              [shell](std::size_t index) { return shell->ProjectTabDisplayTitle(index); },
          .project_tab_tooltip_label =
              [shell](std::size_t index) { return shell->ProjectTabTooltipLabel(index); },
          .project_catalog_entry =
              [shell](std::size_t index) -> const ProjectWorkspaceState* {
                return shell->ProjectCatalogEntry(index);
              },
          .project_catalog_root =
              [shell](std::size_t index) -> const std::filesystem::path& {
                return shell->ProjectCatalogRoot(index);
              },
          .editor_tab_display_title =
              [shell](std::size_t group_index, std::size_t index) {
                return shell->TabDisplayTitle(group_index, index);
              },
          .editor_tab_tooltip_label =
              [shell](std::size_t group_index, std::size_t index) {
                return shell->TabTooltipLabel(group_index, index);
              },
          .current_window_rect = [shell]() { return shell->CurrentWindowRect(); },
          .editor_group_tab_strip_width =
              [shell](std::size_t group_index) { return shell->EditorGroupTabStripWidth(group_index); },
          .measure_width =
              [shell](std::string_view text) { return shell->text_renderer_.MeasureWidth(text); },
          .ensure_output_channel_tab_open =
              [shell](std::string_view id) { shell->EnsureOutputChannelTabOpen(id); },
          .close_output_channel_tab =
              [shell](std::string_view id) { shell->CloseOutputChannelTab(id); },
          .close_terminal_tab =
              [shell](std::size_t index) { shell->MakeTerminalPanelService().CloseTerminalTab(index); },
          .request_bottom_panel_redraw = [shell]() { shell->RequestBottomPanelRedraw(); },
      });
}

void WorkspaceShell::Bootstrapper::WireLsp() const {
  WorkspaceShell* const shell = &shell_;
  shell->lsp_service_.Configure(
      shell->context_, shell->completion_registry_, shell->code_action_registry_,
      LspService::Operations{
          .active_editable_viewport = [shell]() { return shell->ActiveEditableViewport(); },
          .refresh_problems_sidebar = [shell]() { shell->RefreshProblemsSidebar(); },
          .request_editor_surface_redraw = [shell]() { shell->RequestEditorSurfaceRedraw(); },
          .request_chrome_redraw = [shell]() { shell->RequestChromeRedraw(); },
          .request_bottom_panel_redraw = [shell]() { shell->RequestBottomPanelRedraw(); },
          .apply_server_workspace_edit =
              [shell](const LspClient& client, LspClient::WorkspaceEdit edit) {
                return shell->workspace_edit_service_.ApplyServerEdit(client, std::move(edit));
              },
          .get_setting_value = [shell](std::string_view id) { return shell->GetSettingValue(id); },
          .plugin_has_definition_provider =
              [shell](std::string_view language_id) {
                return shell->plugin_runtime_.Host().HasLanguageProvider(
                    plugin::PluginHost::ContributedLanguageProvider::Kind::Definition,
                    language_id);
              },
          .plugin_has_references_provider =
              [shell](std::string_view language_id) {
                return shell->plugin_runtime_.Host().HasLanguageProvider(
                    plugin::PluginHost::ContributedLanguageProvider::Kind::References,
                    language_id);
              },
      });
  // Live theme pointer for baking semantic-token recolor decorations (theme_'s
  // address is stable; a theme switch mutates it in place).
  shell->lsp_service_.SetTheme(&shell->theme_);
}

void WorkspaceShell::Bootstrapper::WireDebug() const {
  WorkspaceShell* const shell = &shell_;
  shell->debug_service_.Configure(
      shell->context_,
      DebugService::Operations{
          .append_console_output =
              [shell](int session_id, const std::string& label,
                     const dap_protocol::DapOutputEvent& output) {
                shell->AppendDebugConsoleOutput(session_id, label, output);
                shell->control_channel_service_.OnDebugOutput(output.category, output.output);
              },
          .show_debug_console =
              [shell](int session_id, const std::string& label) {
                shell->ShowDebugConsole(session_id, label);
              },
          .remove_debug_console = [shell](int session_id) { shell->RemoveDebugConsole(session_id); },
          .notify_session_state_changed =
              [shell](DebugSession::State /*state*/) { shell->RequestChromeRedraw(); },
          // A terminal end (clean exit OR a crash/kill/launch-rejection) mirrors to
          // the control channel here, so an observer is never stranded waiting on a
          // `terminated` that a non-clean death never sent.
          .notify_session_terminated =
              [shell](int session_id, bool failed, const std::string& reason) {
                shell->control_channel_service_.OnDebugTerminated(session_id, reason);
                // A non-clean end (crash / kill / launch rejection) is easy to miss:
                // the session row + transient views disappear. Surface an error toast
                // so the user knows the debugger died and why, not just that it
                // silently vanished. A clean exit needs no toast (it was expected).
                if (failed) {
                  shell->Notify(NotificationService::Tone::Error,
                         reason.empty() ? std::string("Debug adapter exited unexpectedly")
                                        : reason);
                }
              },
          .notify_stop_began =
              [shell](const std::string& reason, int thread_id) {
                shell->control_channel_service_.OnDebugStopBegan(reason, thread_id);
              },
          .notify_stop_resolved =
              [shell]() { shell->control_channel_service_.OnDebugStopped(); },
          .request_chrome_redraw = [shell]() { shell->RequestChromeRedraw(); },
          .request_debug_pane_redraw = [shell]() { shell->RequestDebugPaneRedraw(); },
          .request_editor_redraw = [shell]() { shell->RequestEditorSurfaceRedraw(); },
          .queue_editor_hover_refresh = [shell]() { shell->QueueEditorHoverRefresh(); },
          .focus_source_location =
              [shell](const std::filesystem::path& path, std::size_t line) {
                shell->OpenFile(path);
                if (editor::TextViewport* viewport = shell->ActiveEditorViewport();
                    viewport != nullptr) {
                  viewport->JumpCursorTo(line, 0);
                }
                shell->context_.current_project_state.surface.focus = FocusTarget::Editor;
                shell->RequestEditorSurfaceRedraw();
              },
          .show_call_stack_panel =
              [shell]() {
                // Auto-open the right-side debug pane on the first stop (Call Stack),
                // but don't yank the user off another surface on every subsequent step.
                shell->OpenDebugPaneOnStop();
              },
      });
}

void WorkspaceShell::Bootstrapper::WireControlChannel() const {
  WorkspaceShell* const shell = &shell_;
  shell->control_channel_service_.Configure(
      shell->context_,
      ControlChannelService::Operations{
          .execute_command_line =
              [shell](const std::string& line) { return shell->ExecuteControlCommand(line); },
          .emit_jsonl = [](const std::string& line) { std::cout << line << '\n' << std::flush; },
          .adapters =
              [shell]() {
                std::vector<ControlAdapterInfo> adapters;
                for (const auto& detail : shell->CurrentDapManager().AdapterDetails()) {
                  adapters.push_back(ControlAdapterInfo{detail.type, detail.command});
                }
                return adapters;
              },
          .ensure_debugger_enabled = [shell]() { shell->EnsureDebuggerEnabledTransiently(); },
          .notifications = &shell->notification_service_,
          .remote_project =
              [shell](const std::filesystem::path& root) -> std::optional<ControlRemoteProjectInfo> {
                const auto snapshot = shell->MakeRemoteHostService().SnapshotOf(root);
                if (!snapshot.has_value()) {
                  return std::nullopt;
                }
                using State = project::remote::RemoteHostSession::State;
                const auto& sync = snapshot->sync;
                std::string state;
                switch (snapshot->state) {
                  case State::Disconnected: state = "disconnected"; break;
                  case State::Connecting: state = "connecting"; break;
                  case State::NeedsAuth: state = "needs-auth"; break;
                  case State::StartingServer: state = "starting"; break;
                  case State::Installing: state = "installing"; break;
                  case State::Reconnecting: state = "reconnecting"; break;
                  case State::Offline: state = "offline"; break;
                  case State::Ready:
                    state = (sync.syncing || !sync.synced_once) ? "syncing" : "ready";
                    break;
                }
                const auto count = [](std::size_t n) { return static_cast<std::int64_t>(n); };
                return ControlRemoteProjectInfo{
                    .host = snapshot->host,
                    .host_root = snapshot->host_root,
                    .state = std::move(state),
                    .files = count(sync.files),
                    .files_pending = count(std::min(sync.absent + sync.stale, sync.files)),
                    .unsent_pushes = count(sync.dirty),
                    .conflicts = count(sync.conflicts),
                    .held_deletes = count(sync.held_deletes),
                };
              },
          .keybindings = [shell]() -> const std::vector<ResolvedKeybinding>& {
            return shell->ResolvedKeybindings();
          },
          .action_enabled = [shell](ActionId id) { return shell->MakeActionAvailability().IsEnabled(id); },
          .ui = shell->MakeControlUiOperations(),
          .window_hit_region = [shell](float x, float y) -> std::string_view {
            const auto window = shell->CurrentWindowRect();
            if (!window.has_value() || x < 0.0f || y < 0.0f || x >= window->w || y >= window->h) {
              return "outside";
            }
            switch (shell->WindowHitTest(x, y)) {
              case SDL_HITTEST_NORMAL:
                return "normal";
              case SDL_HITTEST_DRAGGABLE:
                return "draggable";
              default:
                return "resize";
            }
          },
          .active_buffer = [shell]() -> ControlActiveBuffer {
            const editor::TextViewport* viewport = shell->ActiveNavigableViewport();
            if (viewport == nullptr || viewport->is_placeholder()) {
              return {};
            }
            return ControlActiveBuffer{
                .viewport = viewport,
                .kind = shell->ActiveMergeTab() != nullptr     ? "merge"
                        : shell->ActiveCompareTab() != nullptr ? "compare"
                                                        : "editor",
            };
          },
          .output_channels = &shell->output_channels_,
          .plugin_command_names = [shell]() { return shell->plugin_runtime_.Host().CommandNames(); },
          .settings =
              [shell]() { return AllSettingInfos(shell->plugin_runtime_.Host(), shell->available_colorscheme_names_); },
          .setting_value = [shell](std::string_view id) { return shell->GetSettingValue(id); },
      });
}

void WorkspaceShell::Bootstrapper::WireWorkspaceEdits() const {
  WorkspaceShell* const shell = &shell_;
  shell->workspace_edit_service_.Configure(
      shell->context_, shell->lsp_service_,
      WorkspaceEditService::Operations{
          .active_editable_viewport = [shell]() { return shell->ActiveEditableViewport(); },
          .reset_caret_blink = [shell]() { shell->ResetCaretBlink(); },
          .request_active_editable_last_change_redraw =
              [shell]() { shell->RequestActiveEditableLastChangeRedraw(); },
          .request_chrome_redraw = [shell]() { shell->RequestChromeRedraw(); },
          .request_focused_editor_redraw = [shell]() { shell->RequestFocusedEditorRedraw(); },
          .request_active_editable_line_range_redraw =
              [shell](std::size_t start_line, std::size_t end_line) {
                if (shell->ActiveTabIsCompare()) {
                  shell->RequestCompareRightLineRangeRedraw(start_line, end_line);
                } else if (shell->ActiveTabIsMerge()) {
                  shell->RequestMergeResultLineRangeRedraw(start_line, end_line);
                } else {
                  shell->RequestEditorLineRangeRedraw(start_line, end_line);
                }
              },
          .request_active_tab_redraw =
              [shell](bool include_tree_sidebar) { shell->RequestActiveTabRedraw(include_tree_sidebar); },
          .confirm_rename_save =
              [shell](std::string detail) {
                shell->OpenPromptSurface(PromptSurfaceState::Action::ConfirmRenameSave,
                                  PromptSurfaceState::Kind::Confirm, std::filesystem::path{},
                                  std::string{});
                shell->context_.prompts.surface.detail = std::move(detail);
                shell->RequestChromeRedraw();
              },
          .notify_error =
              [shell](std::string message) {
                shell->Notify(NotificationService::Tone::Error, std::move(message));
              },
          .notify_buffer_saved =
              [shell](const std::filesystem::path& path) { shell->NotifyPluginBufferSave(path); },
          .run_closed_file_edits_async =
              [shell](std::vector<ClosedFileEditBucket> buckets) {
                // Load/apply/save the server rename's closed-file targets off the
                // shell thread; the file-index watcher reflects the on-disk changes
                // (TD-2026-07-17-011). The task captures only the buckets by value.
                project::FileWriteGate* gate = &shell->context_.current_project_state.write_gate();
                shell->project_background_executor_.Post(
                    [gate, buckets = std::move(buckets)]() mutable {
                      WorkspaceEditService::RunClosedFileEdits(buckets, *gate);
                    });
              },
          .reconcile_tabs_after_resource_rename =
              [shell](const std::filesystem::path& old_path, const std::filesystem::path& new_path) {
                EditorTabService& editor_tabs = shell->MakeEditorTabService();
                PromptSurfaceService& prompt_surfaces = shell->MakePromptSurfaceService();
                shell->MakePathMutationCoordinator(editor_tabs, prompt_surfaces)
                    .ReconcileAfterExternalRename(old_path, new_path);
              },
          .reconcile_tabs_after_resource_delete =
              [shell](const std::filesystem::path& path) {
                EditorTabService& editor_tabs = shell->MakeEditorTabService();
                PromptSurfaceService& prompt_surfaces = shell->MakePromptSurfaceService();
                shell->MakePathMutationCoordinator(editor_tabs, prompt_surfaces)
                    .ReconcileAfterExternalDelete(path);
              },
          .refresh_views_after_resource_ops =
              [shell](const std::filesystem::path& preferred_tree_path) {
                EditorTabService& editor_tabs = shell->MakeEditorTabService();
                PromptSurfaceService& prompt_surfaces = shell->MakePromptSurfaceService();
                shell->MakePathMutationCoordinator(editor_tabs, prompt_surfaces)
                    .RefreshViewsAfterExternalMutation(preferred_tree_path);
              },
          .dispose_staged_paths_async =
              [shell](std::vector<std::filesystem::path> staged) {
                // Removing a staged (already-renamed-aside) backup can be slow for
                // directories; do it off the shell thread. Failure is harmless — a
                // leftover hidden staging entry, never user data.
                // Through the project's gate (captured now: the worker must not read
                // shell state), which is where a remote project's host removes them.
                project::FileWriteGate* gate = &shell->context_.current_project_state.write_gate();
                shell->project_background_executor_.Post([gate, staged = std::move(staged)]() {
                  gate->DisposeStaged(staged);
                });
              },
      });
}

void WorkspaceShell::Bootstrapper::WireAssist() const {
  WorkspaceShell* const shell = &shell_;
  shell->assist_service_.Configure(
      shell->context_, shell->plugin_runtime_, shell->output_channels_, shell->language_contract_, shell->workspace_edit_service_,
      AssistService::Operations{
          .get_setting_value =
              [shell](std::string_view id) { return shell->GetSettingValue(id); },
          .active_editable_viewport = [shell]() { return shell->ActiveEditableViewport(); },
          .active_editor_viewport = [shell]() { return shell->ActiveEditorViewport(); },
          .active_editor_tab = [shell]() { return shell->ActiveEditorTab(); },
          .active_compare_tab = [shell]() { return shell->ActiveCompareTab(); },
          .active_merge_tab = [shell]() { return shell->ActiveMergeTab(); },
          .lsp_client_for_viewport =
              [shell](const editor::TextViewport& viewport, std::string* language_id) {
                return shell->LspClientForViewport(viewport, language_id);
              },
          .current_lsp_manager = [shell]() -> LspManager& { return shell->CurrentLspManager(); },
          .ensure_lsp_document_open =
              [shell](const editor::TextViewport& viewport,
                     LspClient& client,
                     std::string_view language_id) {
                shell->EnsureLspDocumentOpen(viewport, client, language_id);
              },
          .begin_tracked_lsp_request = [shell]() { shell->BeginTrackedLspRequest(); },
          .finish_tracked_lsp_request = [shell]() { shell->FinishTrackedLspRequest(); },
          .show_overlay = [shell](OverlayMode mode) { shell->ShowOverlay(mode); },
          .dismiss_overlay = [shell](bool focus_editor) { shell->DismissOverlay(focus_editor); },
          .request_overlay_redraw = [shell]() { shell->RequestOverlayRedraw(); },
          .reset_caret_blink = [shell]() { shell->ResetCaretBlink(); },
          .request_focused_editor_redraw = [shell]() { shell->RequestFocusedEditorRedraw(); },
          .request_active_editable_last_change_redraw =
              [shell]() { shell->RequestActiveEditableLastChangeRedraw(); },
          .request_active_editable_blame_neighborhood_redraw =
              [shell](std::size_t before_line, std::size_t after_line) {
                shell->RequestActiveEditableBlameNeighborhoodRedraw(before_line, after_line);
              },
          .refresh_compare_tab_derived_state =
              [shell](CompareTabState& tab) { shell->RefreshCompareTabDerivedState(tab); },
          .sync_compare_selection_from_viewport =
              [shell](CompareTabState& tab, bool keep_top_line) {
                shell->SyncCompareSelectionFromViewport(tab, keep_top_line);
              },
          .update_merge_tracking_after_viewport_edit =
              [shell](MergeTabState& tab) {
                shell->UpdateMergeTrackingAfterViewportEdit(tab);
              },
          .notify_info = [shell](std::string message) { shell->Notify(NotificationService::Tone::Info, message); },
      },
      AssistService::Ports{
          .completion =
              {
                  .has_completion_provider =
                      [shell](std::string_view language_id) {
                        return !language_id.empty() &&
                               (FindProvider(shell->completion_registry_, std::string(language_id)) != nullptr ||
                                shell->CurrentLspManager().HasServer(std::string(language_id)));
                      },
                  .plugin_completion_trigger_characters =
                      [shell](std::string_view language_id) {
                        const CompletionProviderSpec* provider =
                            language_id.empty() ? nullptr
                                                : FindProvider(shell->completion_registry_, language_id);
                        return provider != nullptr ? provider->trigger_characters : std::string{};
                      },
              },
          .code_actions =
              {
                  .execute_command_name =
                      [shell](std::string_view command_name,
                             const std::vector<std::string>& args,
                             std::string* error_message) {
                        return shell->ExecuteCommandName(command_name, args, ActionSource::Menu,
                                                  error_message);
                      },
                  .collect_lsp_context_diagnostics =
                      [shell](const editor::TextViewport& viewport,
                             const editor::SelectionRange& range,
                             lsp_encoding::PositionEncoding encoding) {
                        return shell->CollectLspContextDiagnostics(viewport, range, encoding);
                      },
              },
          .navigation =
              {
                  .open_file_in_new_tab =
                      [shell](const std::filesystem::path& path) { return shell->OpenFileInNewTab(path); },
                  .open_file_then =
                      [shell](const std::filesystem::path& path, std::function<void()> then) {
                        if (shell->glue_->remote_host_service != nullptr &&
                            shell->glue_->remote_host_service->OpenWhenFetched(
                                path, [shell, then](const std::filesystem::path& fetched) {
                                  if (shell->OpenFileInNewTab(fetched)) {
                                    then();
                                  }
                                })) {
                          return true;
                        }
                        if (!shell->OpenFileInNewTab(path)) {
                          return false;
                        }
                        then();
                        return true;
                      },
              },
          .signature_help =
              {
                  .show_signature_help =
                      [shell](std::string signature, std::string documentation) {
                        shell->ShowSignatureHelpPopup(std::move(signature), std::move(documentation));
                      },
                  .signature_help_showing = [shell]() { return shell->active_signature_help_.has_value(); },
                  .anchor_signature_help_at_caret =
                      [shell]() {
                        const editor::TextViewport* viewport = shell->ActiveEditableViewport();
                        if (shell->active_signature_help_.has_value() && viewport != nullptr) {
                          shell->active_signature_help_->cursor_line = viewport->cursor_line();
                          shell->active_signature_help_->cursor_column = viewport->cursor_column();
                          shell->RequestEditorSurfaceRedraw();
                        }
                      },
              },
      });
}

void WorkspaceShell::Bootstrapper::WirePluginRuntime() const {
  WorkspaceShell* const shell = &shell_;
  shell->plugin_runtime_.SetCallbacks(plugin::PluginHost::Callbacks{
      .is_command_name_available =
          [](std::string_view name) { return FindWorkspaceActionByCommand(name) == nullptr; },
      .open_file =
          [shell](const plugin::PluginHost::OpenFileRequest& request) {
            const std::filesystem::path normalized_path = util::NormalizedPath(request.path);
            const std::string requested = request.path.generic_string();
            const bool is_virtual = IsVirtualDocumentUri(requested);
            const bool opened = is_virtual ? shell->OpenVirtualDocumentInNewTab(requested)
                                           : shell->OpenFileInNewTab(normalized_path);
            if (!opened) {
              // Say why on the Plugin Errors channel rather than failing mutely.
              // The bool is not a usable signal: PluginHostCallbacks routes
              // open_file through ApplyHostMutation on the non-direct path and
              // returns true unconditionally there, so a plugin never observes
              // this false at all (TD-2026-07-27-001).
              //
              // The virtual-document case is worth naming specifically: the
              // consumer side is complete, but no plugin API exists to REGISTER
              // a virtual document yet, so every virtual:// open fails and the
              // reason is not discoverable from the plugin side.
              std::string reason =
                  is_virtual
                      ? "open_file: no virtual document is registered for '" + requested +
                            "' (no plugin API contributes virtual documents yet)"
                      : "open_file: could not open '" + normalized_path.generic_string() + "'";
              shell->output_channels_.AppendLine("plugins.error", "Plugin Errors", reason);
              shell->plugin_runtime_.AppendError(std::move(reason));
              return false;
            }
            if (request.line > 0) {
              const std::size_t target_line = request.line - 1;
              const std::size_t target_column = request.column > 0 ? request.column - 1 : 0;
              if (editor::TextViewport* viewport = shell->ActiveEditorViewport(); viewport != nullptr) {
                viewport->JumpCursorTo(target_line, target_column);
              }
            }
            return true;
          },
      .active_buffer =
          [shell]() -> std::optional<plugin::PluginHost::ActiveBuffer> {
            const editor::TextViewport* viewport = shell->ActiveEditableViewport();
            if (viewport == nullptr || viewport->path().empty()) {
              return std::nullopt;
            }
            return plugin::PluginHost::ActiveBuffer{
                .path = util::NormalizedPath(viewport->path()),
                .line = viewport->cursor_line() + 1,
                .column = viewport->cursor_column() + 1,
                .content_revision = viewport->content_revision(),
            };
          },
      .live_buffer_text =
          [shell](const std::filesystem::path& path) -> std::shared_ptr<const std::string> {
            // The buffer under the query is nearly always the one in front (a
            // compare's or a merge's editable side included); otherwise any open
            // editor tab on the path.
            const editor::TextViewport* viewport = shell->ActiveEditableViewport();
            if (viewport == nullptr || !util::SameAsNormalizedPath(viewport->path(), path)) {
              viewport = OpenEditorViewOfPath(shell->context_.current_project_state.editor_groups,
                                              path.lexically_normal());
            }
            // A tab still reading its file has no text to give yet.
            return viewport != nullptr && !viewport->is_placeholder()
                       ? shell->plugin_runtime_.LiveBufferText(*viewport)
                       : nullptr;
          },
      .show_sidebar =
          [shell](std::string_view id) {
            return ActionCoordinator(shell->MakeActionContext()).Execute(ActionId::SidebarShow, {std::string(id)},
                                                    ActionSource::Shortcut);
          },
      .publish_diagnostics =
          [shell](std::string_view owner,
                 const std::filesystem::path& path,
                 std::vector<editor::Diagnostic> diagnostics) {
            if (shell->context_.current_project_state.diagnostics_store.ReplaceForOwnerFile(owner, path, std::move(diagnostics))) {
              shell->RefreshProblemsSidebar();
              shell->RequestEditorSurfaceRedraw();
            }
          },
      .clear_file_diagnostics =
          [shell](std::string_view owner, const std::filesystem::path& path) {
            if (shell->context_.current_project_state.diagnostics_store.ClearOwnerFile(owner, path)) {
              shell->RefreshProblemsSidebar();
              shell->RequestEditorSurfaceRedraw();
            }
          },
      .clear_owner_diagnostics =
          [shell](std::string_view owner) {
            if (shell->context_.current_project_state.diagnostics_store.ClearOwner(owner)) {
              shell->RefreshProblemsSidebar();
              shell->RequestEditorSurfaceRedraw();
            }
          },
      .publish_decorations =
          [shell](std::string_view owner,
                 const std::filesystem::path& path,
                 editor::PluginDecorationData data) {
            if (shell->context_.current_project_state.EnsurePluginPresentation()
                    .decorations.ReplaceForOwnerFile(owner, path, std::move(data))) {
              shell->RequestEditorSurfaceRedraw();
            }
          },
      .clear_file_decorations =
          [shell](std::string_view owner, const std::filesystem::path& path) {
            auto& state = shell->context_.current_project_state;
            auto* pres = state.plugin_presentation.get();
            if (pres != nullptr && pres->decorations.ClearOwnerFile(owner, path)) {
              shell->RequestEditorSurfaceRedraw();
              state.MaybeReleasePluginPresentation();
            }
          },
      .clear_owner_decorations =
          [shell](std::string_view owner) {
            auto& state = shell->context_.current_project_state;
            auto* pres = state.plugin_presentation.get();
            if (pres != nullptr && pres->decorations.ClearOwner(owner)) {
              shell->RequestEditorSurfaceRedraw();
              state.MaybeReleasePluginPresentation();
            }
          },
      .publish_surface =
          [shell](std::string_view owner, std::string_view surface_id,
                 editor::SurfaceContent content) {
            const editor::SurfacePreviewSlot slot = content.preview;
            ApplyKnownRasterSize(content, shell->surface_texture_cache_);
            if (shell->context_.current_project_state.EnsurePluginPresentation()
                    .surfaces.ReplaceForOwnerSurface(owner, surface_id, std::move(content))) {
              if (slot != editor::SurfacePreviewSlot::None) {
                shell->ActivatePluginSurfacePreview(owner, surface_id, slot);
              }
              shell->RequestEditorSurfaceRedraw();
            }
          },
      .clear_surface =
          [shell](std::string_view owner, std::string_view surface_id) {
            auto& state = shell->context_.current_project_state;
            auto* pres = state.plugin_presentation.get();
            if (pres != nullptr && pres->surfaces.ClearOwnerSurface(owner, surface_id)) {
              shell->SyncPluginSurfacePreviewClosed();
              shell->RequestEditorSurfaceRedraw();
              state.MaybeReleasePluginPresentation();
            }
          },
      .clear_owner_surfaces =
          [shell](std::string_view owner) {
            auto& state = shell->context_.current_project_state;
            auto* pres = state.plugin_presentation.get();
            if (pres != nullptr && pres->surfaces.ClearOwner(owner)) {
              shell->SyncPluginSurfacePreviewClosed();
              shell->RequestEditorSurfaceRedraw();
              state.MaybeReleasePluginPresentation();
            }
          },
      .decode_raster =
          [shell](std::uint64_t hash, int format, std::vector<std::byte> bytes, int width,
                 int height) {
            shell->surface_texture_cache_.Request(
                hash,
                format == 0 ? render::SurfaceTextureCache::RasterFormat::Png
                            : render::SurfaceTextureCache::RasterFormat::Rgba8,
                std::move(bytes), width, height);
          },
      .apply_workspace_edit =
          [shell](std::string_view /*owner*/,
                 const plugin::PluginHost::WorkspaceEditRequest& request) {
            return shell->workspace_edit_service_.ApplyPluginEdit(request);
          },
      .publish_ghost_text =
          [shell](std::string_view owner,
                 const plugin::PluginHost::GhostTextRequest& request) {
            shell->PublishPluginGhostText(owner, request);
          },
      .clear_ghost_text =
          [shell](std::string_view owner) { shell->ClearPluginGhostText(owner); },
      .error_sink =
          [shell](const std::string& text) {
            shell->output_channels_.AppendLine("plugins.error", "Plugin Errors", text);
            shell->plugin_runtime_.AppendError(text);
          },
      .log_sink =
          [shell](const std::string& text) {
            shell->output_channels_.AppendLine("plugins.log", "Plugin Log", text);
            shell->plugin_runtime_.AppendLog(text);
          },
      .get_setting =
          [shell](std::string_view id) {
            return shell->GetSettingValue(id);
          },
      .settings_revision =
          [shell]() {
            return shell->settings_store_.Revision();
          },
      // Plugin status items are drawn in the breadcrumb row, not the menu bar the
      // chrome redraw covers (QA plugins-ui: an updated item kept its old text).
      .request_status_redraw =
          [shell]() {
            shell->RequestBreadcrumbRedraw();
          },
      .show_notification =
          [shell](const std::string& level, const std::string& message) {
            shell->Notify(NotificationService::ToneFromLevel(level), message);
          },
  });
}

}  // namespace microide::workspace
