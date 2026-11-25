#include "workspace/coordinators/WorkspaceKeyInputCoordinator.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

#include "workspace/KeycodeText.h"
#include "workspace/ListSelection.h"
#include "workspace/actions/WorkspaceActionCoordinator.h"
#include "workspace/coordinators/WorkspaceCommandLineCoordinator.h"
#include "workspace/coordinators/WorkspaceMenuCoordinator.h"
#include "workspace/coordinators/WorkspacePanelMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceSidebarMouseCoordinator.h"
#include "workspace/services/CompareMergeService.h"
#include "workspace/services/SidebarService.h"
#include "workspace/coordinators/WorkspaceTextInputCoordinator.h"
#include "workspace/shell/WorkspaceShell.h"
#include "util/InvokeIfSet.h"
#include "workspace/actions/WorkspaceActionAvailability.h"
#include "workspace/debug/DebugService.h"
#include "workspace/services/SettingsOverlayService.h"

namespace microide::workspace {

KeyInputCoordinator::KeyInputCoordinator(ProjectWorkspaceState& state, PromptState& prompts,
                                         MenuSurfaceState& menu_state,
                                         NotificationService& notifications,
                                         Services services,
                                         Operations operations)
    : state_(state),
      prompts_(prompts),
      menu_state_(menu_state),
      notifications_(notifications),
      services_(services),
      operations_(std::move(operations)) {}

std::function<bool(ActionId)> KeyInputCoordinator::ActionEnabledPredicate() const {
  return [this](ActionId id) { return services_.availability.IsEnabled(id); };
}

bool KeyInputCoordinator::HandleKeyDown(const SDL_KeyboardEvent& event) {
  key_damage_mark_ = operations_.damage_generation();
  dispatch_probe_.last = nullptr;
  if (operations_.set_swallow_next_text_input) {
    operations_.set_swallow_next_text_input(false);
  }

  const SDL_Keymod modifiers = event.mod;
  const TextInputSurface surface_before = operations_.current_text_input_surface
                                              ? operations_.current_text_input_surface()
                                              : TextInputSurface::None;
  const bool handled = DispatchKeyDown(event, modifiers);
  const bool printable = (modifiers & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0 &&
                         operations_.keycode_to_ascii &&
                         operations_.keycode_to_ascii(event.key, modifiers) != '\0';
  if (handled && printable && operations_.set_swallow_next_text_input &&
      operations_.current_text_input_surface &&
      operations_.current_text_input_surface() != surface_before) {
    operations_.set_swallow_next_text_input(true);
  }
  return handled;
}

bool KeyInputCoordinator::DispatchKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers) {

  const bool clears_pending_chord =
      prompts_.dirty_visible || menu_state_.tree_context_menu.open ||
      menu_state_.menu_bar_open || prompts_.surface_visible || state_.overlay.visible ||
      ActiveKeybindingContext() == KeybindingContext::Terminal;
  if (clears_pending_chord) {
    state_.surface.pending_chord.reset();
  }

  if (prompts_.dirty_visible) {
    const bool handled = HandleDirtyPromptKeyDown(event, modifiers);
    if (handled && prompts_.dirty_visible) {
      operations_.request_frame();
    } else if (handled) {
      EnsureRedraw([this]() { operations_.request_frame(); });
    }
    return handled;
  }
  const auto menu_surface_key = [&](bool handled) {
    if (handled) {
      EnsureRedraw([this]() { operations_.request_frame(); });
    }
    return handled;
  };
  if (menu_state_.tree_context_menu.open) {
    return menu_surface_key(HandleTreeContextMenuKeyDown(event));
  }
  if (menu_state_.menu_bar_open) {
    return menu_surface_key(HandleMenuBarKeyDown(event, modifiers));
  }
  if (menu_state_.overflow_popup_open) {
    return menu_surface_key(HandleMenuOverflowPopupKeyDown(event, modifiers));
  }
  if (services_.settings_overlay.Visible()) {
    return HandleSettingsOverlayKeyDown(event, modifiers);
  }
  if (services_.text_input.CompositionConsumesKey(event.key, modifiers)) {
    return true;
  }
  if (prompts_.surface_visible) {
    const bool handled = HandlePromptSurfaceKeyDown(event);
    if (handled) {
      EnsureRedraw([this]() { operations_.request_frame(); });
    }
    return handled;
  }

  if (HandleNotificationFocusKeyDown(event, modifiers)) {
    return true;
  }

  if (state_.surface.focus == FocusTarget::DebugPane &&
      state_.debug_pane.mode == DebugPaneMode::Variables) {
    if (HandleDebugVariablesKeyDown(event, modifiers)) {
      EnsureRedraw([this]() { operations_.request_frame(); });
      return true;
    }
  }

  if (state_.surface.focus == FocusTarget::DebugPane &&
      state_.debug_pane.mode == DebugPaneMode::Watch) {
    if (HandleDebugWatchKeyDown(event, modifiers)) {
      EnsureRedraw([this]() { operations_.request_frame(); });
      return true;
    }
  }

  if (state_.surface.focus == FocusTarget::DebugPane &&
      state_.debug_pane.mode == DebugPaneMode::CallStack) {
    if (HandleDebugCallStackKeyDown(event)) {
      EnsureRedraw([this]() { operations_.request_frame(); });
      return true;
    }
  }
  if (state_.surface.focus == FocusTarget::DebugPane &&
      state_.debug_pane.mode == DebugPaneMode::Breakpoints) {
    if (HandleDebugBreakpointsKeyDown(event)) {
      EnsureRedraw([this]() { operations_.request_frame(); });
      return true;
    }
  }

  const bool active_compare_tab = operations_.active_tab_is_compare();
  const bool active_merge_tab = operations_.active_tab_is_merge();
  if (HandleGlobalKeyDown(event, modifiers, active_merge_tab)) {
    return true;
  }
  if (HandleSurfaceNavigationKeyDown(event, modifiers)) {
    EnsureRedraw([this]() { operations_.request_frame(); });
    return true;
  }
  if (state_.surface.focus == FocusTarget::DebugPane && state_.debug_pane.visible) {
    return false;
  }
  if (state_.surface.focus == FocusTarget::Overlay) {
    const bool handled = HandleOverlayKeyDown(event, modifiers);
    if (handled) {
      EnsureRedraw([this]() { operations_.request_frame(); });
    }
    return handled;
  }
  if (state_.surface.focus == FocusTarget::Sidebar && state_.sidebar.visible) {
    const bool handled = HandleSidebarKeyDown(event, modifiers);
    if (handled) {
      EnsureRedraw([this]() { operations_.request_frame(); });
    }
    return handled;
  }
  if (state_.surface.focus == FocusTarget::Panel &&
      state_.panel.content == PanelContentKind::Terminal &&
      operations_.active_terminal_pane() != nullptr) {
    if ((modifiers & SDL_KMOD_SHIFT) != 0 &&
        (modifiers & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0 &&
        (event.key == SDLK_PAGEUP || event.key == SDLK_PAGEDOWN) &&
        !operations_.active_terminal_pane()->session.using_alternate_screen() &&
        operations_.scroll_bottom_panel_rows && operations_.bottom_panel_visible_rows) {
      const int page = std::max(1, operations_.bottom_panel_visible_rows() - 1);
      operations_.scroll_bottom_panel_rows(event.key == SDLK_PAGEUP ? -page : page);
      EnsureRedraw([this]() { operations_.request_frame(); });
      return true;
    }
    const bool handled = operations_.text_input_handle_terminal_key_down(event, modifiers);
    if (handled) {
      EnsureRedraw([this]() { operations_.request_frame(); });
    }
    return handled;
  }
  if (state_.surface.focus == FocusTarget::Panel &&
      state_.panel.content != PanelContentKind::None) {
    const bool handled = HandleBottomPanelKeyDown(event);
    if (handled) {
      EnsureRedraw([this]() { operations_.request_frame(); });
    }
    return handled;
  }
  if (state_.surface.focus == FocusTarget::Editor && active_compare_tab) {
    const bool handled = HandleCompareKeyDown(event, modifiers);
    if (handled) {
      EnsureRedraw([this]() { operations_.request_frame(); });
    }
    return handled;
  }
  if (state_.surface.focus == FocusTarget::Editor && active_merge_tab) {
    const bool handled = HandleMergeKeyDown(event, modifiers);
    if (handled) {
      EnsureRedraw([this]() { operations_.request_frame(); });
    }
    return handled;
  }

  const bool handled = HandleDefaultEditorKeyDown(event, modifiers);
  if (handled) {
    EnsureRedraw([this]() { operations_.request_frame(); });
  }
  return handled;
}

bool KeyInputCoordinator::HandleDebugVariablesKeyDown(const SDL_KeyboardEvent& event,
                                                      SDL_Keymod modifiers) {
  DebugVariablesModel& model = state_.debug_variables;
  if (model.IsEditing()) {
    switch (event.key) {
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        services_.debug.CommitVariableEdit();
        return true;
      case SDLK_ESCAPE:
        services_.debug.CancelVariableEdit();
        return true;
      default:
        services_.text_input.HandleSingleLineKeyDown(event, modifiers);
        return true;
    }
  }

  const std::vector<DebugVariableRowView>& rows = model.Rows();
  const std::size_t selected = model.SelectedRow();
  const DebugVariableRowView* row = selected < rows.size() ? &rows[selected] : nullptr;
  if (const auto delta = ListNavigationKeyDelta(event.key, rows.size()); delta.has_value()) {
    model.MoveSelection(*delta);
    operations_.reveal_debug_pane_selection();
    return true;
  }
  switch (event.key) {
    case SDLK_RIGHT:
      if (row != nullptr && row->has_children && !row->expanded) {
        services_.debug.ToggleVariableRow(selected);
        operations_.reveal_debug_pane_selection();
      }
      return true;
    case SDLK_LEFT:
      if (row != nullptr && row->has_children && row->expanded) {
        services_.debug.ToggleVariableRow(selected);
      }
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      if (row != nullptr) {
        if (row->has_children || row->is_show_more) {
          services_.debug.ToggleVariableRow(selected);
        } else if (row->editable) {
          services_.debug.BeginVariableEdit(selected);
        }
      }
      return true;
    default:
      return false;
  }
}

bool KeyInputCoordinator::HandleDebugCallStackKeyDown(const SDL_KeyboardEvent& event) {
  const DebugExecutionView& exec = state_.debug_execution;
  const std::size_t row_count = exec.PanelRowCount();
  if (row_count == 0 || !operations_.focus_debug_call_stack_row) {
    return false;
  }
  const auto delta = ListNavigationKeyDelta(event.key, row_count);
  const bool is_enter = event.key == SDLK_RETURN || event.key == SDLK_KP_ENTER;
  if (!delta.has_value() && !is_enter) {
    return false;
  }
  const auto current = static_cast<int>(exec.FocusedPanelRow());
  const auto max_row = static_cast<int>(row_count) - 1;
  const int target = delta.has_value() ? std::clamp(current + *delta, 0, max_row) : current;
  operations_.focus_debug_call_stack_row(static_cast<std::size_t>(target));
  operations_.reveal_debug_pane_selection();
  return true;
}

bool KeyInputCoordinator::HandleBottomPanelKeyDown(const SDL_KeyboardEvent& event) {
  if (!operations_.scroll_bottom_panel_rows || !operations_.bottom_panel_scroll_span_rows) {
    return false;
  }
  const auto span = static_cast<std::size_t>(
      std::max(0, operations_.bottom_panel_scroll_span_rows()));
  if (span == 0) {
    return false;
  }
  const auto delta = ListNavigationKeyDelta(event.key, span);
  if (!delta.has_value()) {
    return false;
  }
  operations_.scroll_bottom_panel_rows(*delta);
  return true;
}

bool KeyInputCoordinator::HandleDebugBreakpointsKeyDown(const SDL_KeyboardEvent& event) {
  const std::size_t row_count = state_.debug_breakpoints_panel.Rows().size();
  if (row_count == 0) {
    return false;
  }
  int& selected = state_.debug_pane.breakpoints_selected_row;
  const auto max_row = static_cast<int>(row_count) - 1;
  selected = std::clamp(selected, 0, max_row);
  if (const auto delta = ListNavigationKeyDelta(event.key, row_count); delta.has_value()) {
    selected = std::clamp(selected + *delta, 0, max_row);
    operations_.reveal_debug_pane_selection();
    return true;
  }
  if (event.key == SDLK_DELETE || event.key == SDLK_BACKSPACE) {
    const DebugBreakpointRowView& row =
        state_.debug_breakpoints_panel.Rows()[static_cast<std::size_t>(selected)];
    if (row.kind == DebugBreakpointRowView::Kind::Breakpoint && !row.path.empty()) {
      (void)operations_.execute_action(ActionId::BreakpointRemove,
                                       {row.path.string(), std::to_string(row.line + 1)},
                                       ActionSource::Shortcut);
    } else if (row.kind == DebugBreakpointRowView::Kind::FunctionBreakpoint) {
      (void)operations_.execute_action(ActionId::BreakpointFunctionRemove, {row.function_name},
                                       ActionSource::Shortcut);
    }
    return true;
  }
  const bool is_enter = event.key == SDLK_RETURN || event.key == SDLK_KP_ENTER;
  if ((is_enter || event.key == SDLK_SPACE) && operations_.activate_debug_breakpoint_row) {
    operations_.activate_debug_breakpoint_row(static_cast<std::size_t>(selected),
                                              event.key == SDLK_SPACE);
    return true;
  }
  return false;
}

bool KeyInputCoordinator::HandleDebugWatchKeyDown(const SDL_KeyboardEvent& event,
                                                  SDL_Keymod modifiers) {
  DebugWatchModel& model = state_.debug_watch;
  if (model.IsEditing()) {
    switch (event.key) {
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        services_.debug.CommitWatchEdit();
        return true;
      case SDLK_ESCAPE:
        services_.debug.CancelWatchEdit();
        return true;
      default:
        services_.text_input.HandleSingleLineKeyDown(event, modifiers);
        return true;
    }
  }

  const std::vector<DebugVariableRowView>& rows = model.Rows();
  const std::size_t selected = model.SelectedRow();
  const DebugVariableRowView* row = selected < rows.size() ? &rows[selected] : nullptr;
  std::optional<std::size_t> expr_index;
  if (row != nullptr) {
    expr_index = model.ExpressionIndexForRow(selected);
  }
  if (const auto delta = ListNavigationKeyDelta(event.key, rows.size()); delta.has_value()) {
    model.MoveSelection(*delta);
    operations_.reveal_debug_pane_selection();
    return true;
  }
  switch (event.key) {
    case SDLK_RIGHT:
      if (row != nullptr && row->has_children && !row->expanded) {
        services_.debug.ToggleWatchRow(selected);
      }
      return true;
    case SDLK_LEFT:
      if (row != nullptr && row->has_children && row->expanded) {
        services_.debug.ToggleWatchRow(selected);
      }
      return true;
    case SDLK_INSERT:
      util::InvokeIfSet(operations_.add_debug_watch_expression);
      return true;
    case SDLK_DELETE:
      if (expr_index.has_value()) {
        services_.debug.RemoveWatch(*expr_index);
      }
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      if (expr_index.has_value()) {
        util::InvokeIfSet(operations_.edit_debug_watch_expression, *expr_index);
      } else if (row != nullptr) {
        if (row->has_children) {
          services_.debug.ToggleWatchRow(selected);
        } else if (row->editable) {
          services_.debug.BeginWatchEdit(selected);
        }
      }
      return true;
    default:
      return false;
  }
}

bool KeyInputCoordinator::HandleSurfaceNavigationKeyDown(const SDL_KeyboardEvent& event,
                                                         SDL_Keymod modifiers) {
  switch (event.key) {
    case SDLK_TAB:
      if ((modifiers & SDL_KMOD_CTRL) != 0 && operations_.step_recent_editors) {
        return operations_.step_recent_editors((modifiers & SDL_KMOD_SHIFT) != 0);
      }
      break;
    case SDLK_ESCAPE:
      if (state_.overlay.visible) {
        if (state_.overlay.mode == OverlayMode::BufferSearch ||
            state_.overlay.mode == OverlayMode::BufferReplace) {
          break;
        }
        operations_.dismiss_overlay(false);
        return true;
      }
      break;
    default:
      break;
  }

  return false;
}

KeyInputCoordinator& WorkspaceShell::MakeKeyInputCoordinator() {
  if (glue_->key_input_coordinator != nullptr) {
    return *glue_->key_input_coordinator;
  }
  glue_->key_input_coordinator = std::make_unique<KeyInputCoordinator>(
      context_.current_project_state, context_.prompts, context_.menu_state,
      notification_service_,
      KeyInputCoordinator::Services{
          .sidebar = MakeSidebarService(),
          .menus = MakeMenuCoordinator(),
          .debug = debug_service_,
          .assist = assist_service_,
          .settings_overlay = settings_overlay_service_,
          .compare_merge = MakeCompareMergeService(),
          .text_input = MakeTextInputCoordinator(),
          .quick_pick = quick_pick_service_,
          .clipboard = clipboard_,
          .availability = MakeActionAvailability(),
      },
      KeyInputCoordinator::Operations{
          .damage_generation = [this]() { return MarkDamage(); },
          .scroll_bottom_panel_rows =
              [this](int row_delta) {
                if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
                  MakePanelMouseCoordinator().ScrollPanelRows(*layout, row_delta);
                }
              },
          .bottom_panel_scroll_span_rows =
              [this]() {
                const auto layout = CurrentWorkspaceLayout();
                return layout.has_value()
                           ? MakePanelMouseCoordinator().ScrollSpanRows(*layout)
                           : 0;
              },
          .bottom_panel_visible_rows =
              [this]() {
                const auto layout = CurrentWorkspaceLayout();
                return layout.has_value() ? BottomPanelVisibleRows(layout->bottom_panel.h) : 0;
              },
          .request_frame = [this]() { RequestPreparedFrame(); },
          .current_text_input_surface =
              [this]() { return CurrentTextInputSurface(); },
          .text_input_handle_terminal_key_down =
              [this](const SDL_KeyboardEvent& event, SDL_Keymod modifiers) {
                if (HandleTerminalFindKeyDown(event, modifiers)) {
                  return true;
                }
                return MakeTextInputCoordinator().HandleTerminalKeyDown(event, modifiers);
              },
          .set_swallow_next_text_input =
              [this](bool swallow) { context_.text_input.swallow_next_text_input = swallow; },
          .confirm_dirty_prompt = [this]() { ConfirmDirtyPrompt(); },
          .keycode_to_ascii =
              [](SDL_Keycode key, SDL_Keymod modifiers) {
                return KeycodeToAscii(key, modifiers);
              },
          .dismiss_prompt_surface = [this](bool restore_focus) { DismissPromptSurface(restore_focus); },
          .confirm_prompt_surface = [this]() { ConfirmPromptSurface(); },
          .execute_action =
              [this](ActionId id, const std::vector<std::string>& args, ActionSource source) {
                return ActionCoordinator(MakeActionContext()).Execute(id, args, source);
              },
          .execute_command_name =
              [this](std::string_view command_name,
                     const std::vector<std::string>& args,
                     ActionSource source) {
                return ExecuteCommandName(command_name, args, source);
              },
          .resolved_keybindings = [this]() -> const std::vector<ResolvedKeybinding>& {
            return ResolvedKeybindings();
          },
          .active_tab_is_compare = [this]() { return ActiveTabIsCompare(); },
          .active_tab_is_merge = [this]() { return ActiveTabIsMerge(); },
          .active_navigable_viewport = [this]() { return ActiveNavigableViewport(); },
          .active_editable_viewport = [this]() { return ActiveEditableViewport(); },
          .active_terminal_pane = [this]() { return ActiveTerminalPane(); },
          .dismiss_overlay = [this](bool focus_editor) { DismissOverlay(focus_editor); },
          .close_settings_overlay = [this]() { CloseSettingsOverlay(); },
          .settings_focused_pane =
              [this]() { return static_cast<int>(settings_overlay_service_.FocusedPane()); },
          .settings_focus_pane =
              [this](int pane) {
                settings_overlay_service_.SetFocusedPane(static_cast<SettingsPane>(pane));
                InvalidateCursorKindFingerprint();
              },
          .settings_cycle_focus =
              [this](int delta) {
                settings_overlay_service_.CycleFocusedPane(delta);
                InvalidateCursorKindFingerprint();
              },
          .settings_move_category =
              [this](int delta) {
                settings_overlay_service_.MoveCategory(delta);
                EnsureSettingsSelectionVisible();
              },
          .settings_move_row =
              [this](int delta) {
                settings_overlay_service_.MoveRow(delta);
                EnsureSettingsSelectionVisible();
              },
          .settings_pane_item_count =
              [this](int pane) -> std::size_t {
                switch (static_cast<SettingsPane>(pane)) {
                  case SettingsPane::Categories:
                    return settings_overlay_service_.Categories().size();
                  case SettingsPane::Values:
                    return settings_overlay_service_.Mode() == SettingsOverlayMode::Settings
                               ? settings_overlay_service_.RowCountInSelectedCategory()
                               : settings_overlay_service_.VisibleRowCount();
                  default:
                    return 0;
                }
              },
          .settings_scroll_rows = [this](int delta) { ScrollSettingsOverlayRows(delta); },
          .settings_step_selected =
              [this](int direction) {
                if (const SettingsOverlayRow* row = settings_overlay_service_.SelectedSettingRow();
                    row != nullptr) {
                  StepSetting(row->id, direction >= 0);
                  RefreshSettingsOverlayCatalog();
                }
              },
          .settings_toggle_or_activate_selected =
              [this]() {
                if (const SettingsOverlayRow* row = settings_overlay_service_.SelectedSettingRow();
                    row != nullptr) {
                  if (row->control_kind == SettingsControlKind::TextEdit) {
                    BeginSettingValueEdit(row->id);
                  } else {
                    StepSetting(row->id, true);
                    RefreshSettingsOverlayCatalog();
                  }
                }
              },
          .settings_reset_selected =
              [this]() {
                if (const SettingsOverlayRow* row = settings_overlay_service_.SelectedSettingRow();
                    row != nullptr && row->resettable) {
                  ResetSettingValue(row->id);
                  RefreshSettingsOverlayCatalog();
                }
              },
          .settings_commit_value_edit = [this]() { CommitSettingValueEdit(); },
          .settings_cancel_value_edit = [this]() { CancelSettingValueEdit(); },
          .settings_picker_move = [this](int delta) { MoveSettingsFontPicker(delta); },
          .active_sidebar_mode = [this]() { return ActiveSidebarMode(); },
          .activate_overlay_selection =
              [this]() {
                ActivateOverlaySelection();
              },
          .step_recent_editors =
              [this](bool back) {
                QuickPickState& pick = context_.current_project_state.overlay.workflow.quick_pick;
                if (context_.current_project_state.overlay.visible &&
                    context_.current_project_state.overlay.mode == OverlayMode::QuickPick &&
                    pick.kind == QuickPickKind::RecentEditor) {
                  if (!pick.matches.empty()) {
                    const std::size_t n = pick.matches.size();
                    SetOverlaySelectedIndex(back ? (pick.selected_index + n - 1) % n : (pick.selected_index + 1) % n);
                  }
                  return true;
                }
                if (context_.current_project_state.overlay.visible) {
                  return false;
                }
                EditorGroup& group = context_.current_project_state.focused_group();
                if (group.open_tabs.empty()) {
                  return false;
                }
                for (TabEntry& tab : group.open_tabs) {
                  (void)EnsureTabStableId(tab);
                }
                quick_pick_service_.Open(QuickPickService::RecentEditors(group, context_.current_project_state.root, back));
                return true;
              },
          .open_sidebar_selection_context_menu =
              [this]() {
                const auto layout = CurrentWorkspaceLayout();
                return layout.has_value() && MakeSidebarMouseCoordinator().OpenSelectionContextMenu(*layout);
              },
          .complete_command_palette_query = [this]() { CompleteCommandPaletteQuery(); },
          .current_workspace_layout = [this]() { return CurrentWorkspaceLayout(); },
          .compute_overlay_rect = [this](const SDL_FRect& rect) { return ComputeOverlayRect(rect); },
          .reveal_overlay_selection = [this](const SDL_FRect& rect) { RevealOverlaySelection(rect); },
          .reveal_debug_pane_selection = [this]() { RevealDebugPaneSelection(); },
          .focus_debug_call_stack_row =
              [this](std::size_t row) { FocusDebugCallStackRow(row); },
          .activate_debug_breakpoint_row =
              [this](std::size_t row, bool toggle) { ActivateDebugBreakpointRow(row, toggle); },
          .move_buffer_search_selection = [this](int delta) { MoveBufferSearchSelection(delta); },
          .toggle_buffer_search_option =
              [this](BufferFindToggle toggle) { ToggleBufferSearchOption(toggle); },
          .move_project_search_selection = [this](int delta) { MoveProjectSearchSelection(delta); },
          .toggle_project_search_pattern_mode = [this]() { ToggleProjectSearchPatternMode(); },
          .cycle_project_search_case_mode = [this]() { CycleProjectSearchCaseMode(); },
          .toggle_project_search_hidden_files = [this]() { ToggleProjectSearchHiddenFiles(); },
          .replace_all_buffer_search_matches = [this]() { ReplaceAllBufferSearchMatches(); },
          .replace_current_buffer_search_match = [this]() { ReplaceCurrentBufferSearchMatch(); },
          .move_file_finder_selection = [this](int delta) { MoveFileFinderSelection(delta); },
          .begin_project_search_edit =
              [this](ProjectSearchEditField field) { BeginProjectSearchEdit(field); },
          .commit_project_search_edit = [this]() { CommitProjectSearchEdit(); },
          .cancel_project_search_edit = [this]() { CancelProjectSearchEdit(); },
          .replace_all_project_search_matches = [this]() { RequestProjectSearchReplaceAll(); },
          .open_file = [this](const std::filesystem::path& path) { OpenFile(path); },
          .active_editor_viewport = [this]() { return ActiveEditorViewport(); },
          .seed_buffer_search_from_project_search =
              [this]() { OpenBufferSearchFromProjectSearchResult(); },
          .dispatch_git_sidebar_action =
              [this](GitSidebarActionId action, std::size_t index) {
                return DispatchGitSidebarAction(action, index);
              },
          .close_commit_workflow = [this]() { CloseCommitWorkflow(); },
          .request_commit_workflow_commit =
              [this]() {
                InitializeCommitWorkflowService();
                return commit_workflow_service_.RequestCommit(
                    context_.current_project_state.sidebar.git.commit_workflow,
                    project::CommitOperationKind::Create);
              },
          .set_commit_workflow_focus_field =
              [this](CommitWorkflowFocusField field) {
                auto& workflow = context_.current_project_state.sidebar.git.commit_workflow;
                workflow.focus_field = field;
                InitializeCommitWorkflowService();
                commit_workflow_service_.OnDraftEdited(workflow);
                ResetCaretBlink();
                RequestPreparedFrame();
              },
          .commit_body_write_clipboard_text =
              [this](std::string_view text) { return WriteClipboardText(text); },
          .refresh_problems_sidebar = [this]() { return RefreshProblemsSidebar(); },
          .open_selected_plugin_sidebar_item = [this]() { return OpenSelectedPluginSidebarItem(); },
          .toggle_plugin_sidebar_item = [this]() { return ToggleSelectedPluginSidebarItem(); },
          .jump_compare_hunk = [this](int delta) { JumpCompareHunk(delta); },
          .stage_compare_hunk = [this]() { StageCompareHunk(); },
          .stage_compare_selected_lines = [this]() { StageCompareSelectedLines(); },
          .unstage_compare_hunk = [this]() { UnstageCompareHunk(); },
          .unstage_compare_selected_lines = [this]() { UnstageCompareSelectedLines(); },
          .open_discard_compare_hunk_prompt = [this]() { OpenDiscardCompareHunkPrompt(); },
          .open_discard_compare_selected_lines_prompt =
              [this]() { OpenDiscardCompareSelectedLinesPrompt(); },
          .active_compare_tab = [this]() { return ActiveCompareTab(); },
          .refresh_compare_tab_derived_state =
              [this](CompareTabState& compare_tab) { RefreshCompareTabDerivedState(compare_tab); },
          .sync_compare_selection_from_viewport =
              [this](CompareTabState& compare_tab, bool reveal_selection) {
                SyncCompareSelectionFromViewport(compare_tab, reveal_selection);
              },
          .reset_caret_blink = [this]() { ResetCaretBlink(); },
          .mark_active_editor_folding_dirty =
              [this]() {
                if (auto* editor_tab = ActiveEditorTab(); editor_tab != nullptr) {
                  editor_tab->folding_model->MarkDirty();
                }
              },
          .note_active_editable_edit =
              [this]() { NoteActiveEditableEdit(); },
          .request_active_editable_change_redraw =
              [this](const std::vector<std::string>& before_lines,
                     const std::vector<std::string>& after_lines) {
                RequestActiveEditableChangeRedraw(before_lines, after_lines);
              },
          .request_close_active_tab =
              [this]() { RequestCloseTab(context_.current_project_state.focused_group().active_tab_index); },
          .reveal_active_compare_selection = [this]() { RevealActiveCompareSelection(); },
          .accept_inline_completion = [this]() { return AcceptGhostText(); },
          .dismiss_inline_completion = [this]() { DismissGhostText(); },
          .get_setting_value =
              [this](std::string_view id) { return GetSettingValue(id); },
          .active_merge_tab = [this]() { return ActiveMergeTab(); },
          .update_merge_tracking_after_viewport_edit =
              [this](MergeTabState& merge_tab) {
                UpdateMergeTrackingAfterViewportEdit(merge_tab);
              },
          .move_merge_selection = [this](int delta) { MoveMergeSelection(delta); },
          .apply_merge_choice = [this](compare::MergeChoice choice) { ApplyMergeChoice(choice); },
          .add_debug_watch_expression = [this]() { OpenWatchExpressionPrompt(std::nullopt); },
          .edit_debug_watch_expression =
              [this](std::size_t index) { OpenWatchExpressionPrompt(index); },
      });
  return *glue_->key_input_coordinator;
}

}
