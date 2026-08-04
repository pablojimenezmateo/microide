#include "workspace/shell/WorkspaceShellBootstrapper.h"

#include <utility>

#include "workspace/services/TerminalPanelService.h"
#include "workspace/actions/WorkspaceActionCoordinator.h"
#include "workspace/coordinators/WorkspaceCommandLineCoordinator.h"
#include "workspace/coordinators/WorkspaceKeyInputCoordinator.h"
#include "workspace/shell/WorkspaceShell.h"
#include "workspace/WorkspaceFileDrop.h"
#include "workspace/coordinators/WorkspaceTextInputCoordinator.h"

namespace microide::workspace {

WorkspaceShell::EventResult WorkspaceShell::HandleEvent(const SDL_Event& event) {
  const FocusTarget focus_before = context_.current_project_state.surface.focus;
  EventResult result = Bootstrapper(*this).BuildEventDispatcher().Handle(event);
  // The focus ring is drawn per-surface from each surface's render path. When focus moves
  // between surfaces, both the previously- and newly-focused surfaces must repaint so the
  // old ring is erased and the new one drawn. This is the single chokepoint for that
  // invalidation, independent of which coordinator mutated the focus field.
  const FocusTarget focus_after = context_.current_project_state.surface.focus;
  if (result.handled && !result.redraw.full && focus_after != focus_before) {
    if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
      const auto add_focus_surface_redraw = [&](FocusTarget focus) {
        if (const auto rect = FocusSurfaceRect(*layout, focus); rect.has_value()) {
          result.redraw.rects.push_back(*rect);
        }
      };
      add_focus_surface_redraw(focus_before);
      add_focus_surface_redraw(focus_after);
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
  return result;
}

TerminalPanelService WorkspaceShell::MakeTerminalPanelService() {
  return TerminalPanelService(TerminalPanelService::Operations{
      .read_primary_selection_text = [this]() { return ReadPrimarySelectionText(); },
      .clear_terminal_selection = [this]() { ClearTerminalSelection(); },
      .append_terminal_pending_input =
          [this](std::string_view input) { AppendTerminalPendingInput(input); },
      .terminal_url_at_point = [this](float x, float y) { return TerminalUrlAtPoint(x, y); },
      .open_external_url = [this](std::string_view url) { return OpenExternalUrl(url); },
      .sync_primary_selection_with_terminal_selection =
          [this]() { SyncPrimarySelectionWithTerminalSelection(); },
      .open_terminal =
          [this](std::string command, bool focus_terminal, bool log_feedback) {
            OpenTerminal(std::move(command), focus_terminal, log_feedback);
          },
      .close_terminal_tab = [this](std::size_t index) { CloseTerminalTab(index); },
      .move_active_terminal_tab_to =
          [this](std::size_t index) { return MoveActiveTerminalTabTo(index); },
  });
}

CommandLineCoordinator WorkspaceShell::MakeCommandLineCoordinator() {
  return CommandLineCoordinator(
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
}

WorkspaceShell::Bootstrapper::Bootstrapper(WorkspaceShell& shell) : shell_(shell) {}

ActionAvailability WorkspaceShell::Bootstrapper::BuildActionAvailability() const {
  auto* shell = &shell_;
  return ActionAvailability(
      shell->context_,
      ActionAvailability::Operations{
          .selected_tree_target_kind = [shell]() { return shell->SelectedTreeTargetKind(); },
          .resolve_tree_action_path =
              [shell](WorkspaceShell::ActionSource source) {
                return shell->ResolveTreeActionPath(source);
              },
          .row_context_menu_path =
              [shell]() {
                const auto& menu = shell->context_.menu_state.tree_context_menu;
                return menu.open ? menu.path.lexically_normal() : std::filesystem::path{};
              },
          .active_tab_path = [shell]() { return shell->ActiveTabPath(); },
          .active_navigable_viewport = [shell]() { return shell->ActiveNavigableViewport(); },
          .active_editable_viewport = [shell]() { return shell->ActiveEditableViewport(); },
          .current_text_input_surface = [shell]() { return shell->CurrentTextInputSurface(); },
          .active_single_line_text_has_selection =
              [shell]() {
                return shell->MakeTextInputCoordinator().HasSelectionAtActiveSingleLineSurface();
              },
          .active_terminal_tab = [shell]() { return shell->ActiveTerminalTab(); },
          .has_last_terminal_command = [shell]() { return shell->HasLastTerminalCommand(); },
          .terminal_has_selection = [shell]() { return shell->TerminalHasSelection(); },
          .active_tab_is_editor = [shell]() { return shell->ActiveTabIsEditor(); },
          .editor_group_count = [shell]() { return shell->EditorGroupCount(); },
          .active_tab_is_compare = [shell]() { return shell->ActiveTabIsCompare(); },
          .active_tab_is_merge = [shell]() { return shell->ActiveTabIsMerge(); },
          .active_compare_tab = [shell]() { return shell->ActiveCompareTab(); },
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
          .debug_session_active = [shell]() { return shell->IsDebugSessionActive(); },
          .debug_session_stopped = [shell]() { return shell->IsDebugSessionStopped(); },
          .debug_supports_reverse = [shell]() { return shell->DebugSupportsReverse(); },
          .debug_session_count = [shell]() { return shell->CurrentDapManager().SessionCount(); },
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
      },
      WorkspaceEventDispatcher::State{
          .window_has_input_focus = shell->context_.interaction_state.window_has_input_focus,
      },
      WorkspaceEventDispatcher::Operations{
          .sync_terminal_focus = [shell]() { shell->SyncTerminalFocusState(); },
          .consume_pending_render_invalidation =
              [shell]() { return shell->ConsumePendingRenderInvalidation(); },
          .consume_pending_project_open_dialog_result =
              [shell]() { shell->ConsumePendingProjectOpenDialogResult(); },
          .consume_pending_font_file_dialog_result =
              [shell]() { shell->ConsumePendingFontFileDialogResult(); },
          .reload_plugins_if_assets_changed =
              [shell](bool force_check) {
                return shell->ReloadPluginsIfPluginAssetsChanged(force_check);
              },
          .plugin_runtime_consume_wake_event =
              [shell](Uint32 type) { return shell->plugin_runtime_.ConsumeWakeEvent(type); },
          .project_file_monitor_consume_wake_event =
              [shell](Uint32 type) { return shell->project_file_monitor_.ConsumeWakeEvent(type); },
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
          .consume_highlight_prefetch_results =
              [shell]() { shell->ConsumeHighlightPrefetchResults(); },
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
          .request_window_redraw = [shell]() { shell->RequestWindowRedraw(); },
          .handle_key_down =
              [shell](const SDL_KeyboardEvent& event) {
                return shell->MakeKeyInputCoordinator().HandleKeyDown(event);
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

WorkspaceRootView WorkspaceShell::Bootstrapper::BuildRootView() const {
  auto* shell = &shell_;
  return WorkspaceRootView(
      WorkspaceRootView::FrameOperations{
          .prepare_render_frame =
              [shell](SDL_Renderer* renderer, int width, int height) {
                shell->PrepareRenderFrame(renderer, width, height);
              },
          .compute_layout =
              [shell](int width, int height) {
                return ComputeLayout(static_cast<float>(width), static_cast<float>(height),
                                     shell->context_.current_project_state.sidebar.visible,
                                     shell->BottomPanelVisible(),
                                     shell->context_.current_project_state.sidebar.width,
                                     shell->context_.current_project_state.panel.height,
                                     shell->layout_mode_service_.SnapshotInputs(),
                                     shell->layout_mode_service_.StatusBarVisible(),
                                     shell->context_.current_project_state.debug_pane.visible,
                                     shell->context_.current_project_state.debug_pane.width,
                                     shell->ProjectTabStripVisible());
              },
          .reset_visible_editor_blame_overlay =
              [shell]() { shell->editor_blame_overlay_service_.ClearVisibleOverlay(); },
          .should_draw_editor_caret =
              [shell]() {
                return shell->CaretVisibleNow() &&
                       !(shell->CurrentTextInputSurface() == WorkspaceShell::TextInputSurface::Editor &&
                         !shell->context_.text_input.composition.text.empty());
              },
          .render_frame_base =
              [shell](SDL_Renderer* renderer, const WorkspaceLayout& layout) {
                shell->RootViewRenderFrameBase(renderer, layout);
              },
      },
      WorkspaceRootView::Views{
          .active_surface = WorkspaceActiveSurfaceView(
              WorkspaceActiveSurfaceView::Operations{
                  .render =
                      [shell](SDL_Renderer* renderer,
                             const WorkspaceLayout& layout,
                             bool draw_editor_caret,
                             std::optional<SDL_FRect>* active_editor_pane_rect) {
                        shell->RootViewRenderActiveWorkspaceSurface(
                            renderer, layout, WorkspaceShell::FrameToken{}, draw_editor_caret,
                            active_editor_pane_rect);
                      },
                  .refresh_hover_if_needed =
                      [shell]() {
                        if (shell->editor_hover_refresh_pending_ &&
                            shell->last_mouse_position_valid_) {
                          shell->UpdateEditorHover(shell->last_mouse_x_, shell->last_mouse_y_);
                          shell->editor_hover_refresh_pending_ = false;
                        }
                      },
                  .render_hover_popup =
                      [shell](SDL_Renderer* renderer) {
                        shell->RenderEditorHoverPopup(renderer);
                      },
              }),
          .chrome = WorkspaceChromeView(
              [shell](SDL_Renderer* renderer, const WorkspaceLayout& layout) {
                shell->RenderWindowChrome(renderer, layout);
              }),
          .sidebar = WorkspaceSidebarView(
              [shell](SDL_Renderer* renderer, const WorkspaceLayout& layout) {
                shell->RenderSidebarSurface(renderer, layout);
              }),
          .overlay = WorkspaceOverlayView(
              [shell](SDL_Renderer* renderer, const WorkspaceLayout& layout) {
                shell->RootViewRenderOverlaySurface(renderer, layout);
              }),
          .panel = WorkspacePanelView(
              WorkspacePanelView::Operations{
                  .active_terminal_line_count =
                      [shell]() {
                        return shell->ActiveTerminalTab() != nullptr
                                   ? shell->ActiveTerminalTab()->session.LineCount()
                                   : std::size_t{0};
                      },
                  .render =
                      [shell](SDL_Renderer* renderer,
                             const WorkspaceLayout& layout,
                             std::size_t terminal_line_count) {
                        shell->RenderBottomPanelSurface(renderer, layout, terminal_line_count);
                      },
              }),
          .menu = WorkspaceMenuView(
              [shell](SDL_Renderer* renderer, const WorkspaceLayout& layout) {
                shell->RenderHoverTooltip(renderer, layout);
                shell->RenderMenuPopups(renderer, layout);
              }),
          .prompt = WorkspacePromptView(
              [shell](SDL_Renderer* renderer,
                     const WorkspaceLayout& layout,
                     const std::optional<SDL_FRect>& active_editor_pane_rect) {
                SDL_Window* render_window = SDL_GetRenderWindow(renderer);
                const auto active_text_input_visual =
                    shell->BuildActiveTextInputVisual(layout, active_editor_pane_rect);
                shell->RenderPromptSurface(renderer, layout, active_text_input_visual);
                shell->RenderSingleLineTextSelection(renderer, active_text_input_visual);
                shell->RenderActiveTextInputCaret(renderer, active_text_input_visual);
                shell->RenderTextComposition(renderer, active_text_input_visual);
                shell->UpdateTextInputArea(renderer, render_window, active_text_input_visual);
                shell->RenderDirtyPromptSurface(renderer, layout);
              }),
      });
}

}  // namespace microide::workspace
