#include "workspace/WorkspaceShellBootstrapper.h"

#include "workspace/WorkspaceKeyInputCoordinator.h"
#include "workspace/WorkspaceShell.h"
#include "workspace/WorkspaceTextInputCoordinator.h"

namespace microide::workspace {

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
          .active_tab_path = [shell]() { return shell->ActiveTabPath(); },
          .active_navigable_viewport = [shell]() { return shell->ActiveNavigableViewport(); },
          .active_editable_viewport = [shell]() { return shell->ActiveEditableViewport(); },
          .current_text_input_surface = [shell]() { return shell->CurrentTextInputSurface(); },
          .active_single_line_text_has_selection =
              [shell]() {
                return shell->MakeTextInputCoordinator().HasSelectionAtActiveSingleLineSurface();
              },
          .active_terminal_tab = [shell]() { return shell->ActiveTerminalTab(); },
          .last_terminal_command_text = [shell]() { return shell->LastTerminalCommandText(); },
          .terminal_has_selection = [shell]() { return shell->TerminalHasSelection(); },
          .active_tab_is_editor = [shell]() { return shell->ActiveTabIsEditor(); },
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
      });
}

WorkspaceEventDispatcher WorkspaceShell::Bootstrapper::BuildEventDispatcher() const {
  auto* shell = &shell_;
  return WorkspaceEventDispatcher(
      WorkspaceEventDispatcher::Runtime{
          .project_open_dialog_event_type = shell->project_open_dialog_event_type_,
          .git_blame_event_type = shell->git_blame_event_type_,
          .terminal_event_type = shell->terminal_event_type_,
          .project_file_event_type = shell->project_file_event_type_,
          .lsp_event_type = shell->lsp_event_type_,
          .plugin_async_process_event_type = shell->plugin_async_process_event_type_,
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
          .task_runtime_handles_event =
              [shell](Uint32 type) { return shell->task_runtime_.HandlesEvent(type); },
          .consume_task_runtime_updates =
              [shell]() { shell->ConsumeTaskRuntimeUpdates(); },
          .provider_bridge_handles_event =
              [shell](Uint32 type) { return shell->provider_bridge_manager_.HandlesEvent(type); },
          .consume_provider_bridge_updates =
              [shell]() { shell->ConsumeProviderBridgeUpdates(); },
          .request_focused_editor_redraw =
              [shell]() { shell->RequestFocusedEditorRedraw(); },
          .consume_lsp_callbacks =
              [shell]() { shell->ConsumeLspCallbacks(); },
          .consume_plugin_async_process_callbacks =
              [shell]() { shell->ConsumePluginAsyncProcessCallbacks(); },
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
          .request_window_redraw = [shell]() { shell->RequestWindowRedraw(); },
          .handle_key_down =
              [shell](const SDL_KeyboardEvent& event) {
                return shell->MakeKeyInputCoordinator().HandleKeyDown(event);
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
          .should_blink_caret = [shell]() { return shell->ShouldBlinkCaret(); },
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
                                     shell->context_.current_project_state.panel.height);
              },
          .reset_visible_editor_blame_overlay =
              [shell]() { shell->visible_editor_blame_overlay_.reset(); },
          .should_draw_editor_caret =
              [shell]() {
                return shell->CaretVisibleNow() &&
                       !(shell->CurrentTextInputSurface() == WorkspaceShell::TextInputSurface::Editor &&
                         !shell->context_.text_input.composition.text.empty());
              },
          .render_frame_base =
              [shell](SDL_Renderer* renderer, const WorkspaceLayout& layout) {
                shell->RenderFrameBase(renderer, layout);
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
                        shell->RenderActiveWorkspaceSurface(renderer, layout, draw_editor_caret,
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
                shell->RenderOverlaySurface(renderer, layout);
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
                shell->RenderChromeTooltips(renderer, layout);
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
