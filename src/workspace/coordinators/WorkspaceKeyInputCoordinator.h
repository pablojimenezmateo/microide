#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "editor/TextViewport.h"
#include "workspace/git/CommitWorkflowState.h"
#include "workspace/WorkspaceEventResult.h"
#include "workspace/actions/WorkspaceActionTypes.h"
#include "workspace/registries/WorkspaceKeybindingRegistry.h"
#include "workspace/services/NotificationService.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/state/WorkspaceMenuState.h"
#include "workspace/git/GitSidebarCommandCenter.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/state/WorkspacePromptState.h"
#include "workspace/state/WorkspaceTextInputState.h"

namespace microide::workspace {

class ActionAvailability;
class AssistService;
class CompareMergeService;
class DebugService;
class MenuCoordinator;
class QuickPickService;
class SettingsOverlayService;
class SidebarCoordinator;
class SystemClipboard;
class TextInputCoordinator;

class KeyInputCoordinator {
 public:
  struct Services {
    SidebarCoordinator& sidebar;
    MenuCoordinator& menus;
    DebugService& debug;
    AssistService& assist;
    SettingsOverlayService& settings_overlay;
    CompareMergeService& compare_merge;
    TextInputCoordinator& text_input;
    QuickPickService& quick_pick;
    SystemClipboard& clipboard;
    const ActionAvailability& availability;
  };

  struct Operations {
    std::function<DamageMark()> damage_generation;
    std::function<void(int)> scroll_bottom_panel_rows;
    std::function<int()> bottom_panel_scroll_span_rows;
    std::function<int()> bottom_panel_visible_rows;
    std::function<void()> request_frame;
    std::function<TextInputSurface()> current_text_input_surface;
    std::function<bool(const SDL_KeyboardEvent&, SDL_Keymod)> text_input_handle_terminal_key_down;
    std::function<void(bool)> set_swallow_next_text_input;
    std::function<void()> confirm_dirty_prompt;
    std::function<char(SDL_Keycode, SDL_Keymod)> keycode_to_ascii;
    std::function<void(bool)> dismiss_prompt_surface;
    std::function<void()> confirm_prompt_surface;
    std::function<bool(ActionId, const std::vector<std::string>&, ActionSource)> execute_action;
    std::function<bool(std::string_view, const std::vector<std::string>&, ActionSource)>
        execute_command_name;
    std::function<const std::vector<ResolvedKeybinding>&()> resolved_keybindings;
    std::function<bool()> active_tab_is_compare;
    std::function<bool()> active_tab_is_merge;
    std::function<editor::TextViewport*()> active_navigable_viewport;
    std::function<editor::TextViewport*()> active_editable_viewport;
    std::function<TerminalPaneState*()> active_terminal_pane;
    std::function<void(bool)> dismiss_overlay;
    std::function<void()> close_settings_overlay;
    std::function<int()> settings_focused_pane;
    std::function<void(int)> settings_focus_pane;
    std::function<void(int)> settings_cycle_focus;
    std::function<void(int)> settings_move_category;
    std::function<void(int)> settings_move_row;
    std::function<std::size_t(int)> settings_pane_item_count;
    std::function<void(int)> settings_scroll_rows;
    std::function<void(int)> settings_step_selected;
    std::function<void()> settings_toggle_or_activate_selected;
    std::function<void()> settings_reset_selected;
    std::function<void()> settings_commit_value_edit;
    std::function<void()> settings_cancel_value_edit;
    std::function<void(int)> settings_picker_move;
    std::function<SidebarMode()> active_sidebar_mode;
    std::function<void()> activate_overlay_selection;
    std::function<bool(bool back)> step_recent_editors;
    std::function<bool()> open_sidebar_selection_context_menu;
    std::function<void()> complete_command_palette_query;
    std::function<std::optional<WorkspaceLayout>()> current_workspace_layout;
    std::function<SDL_FRect(const SDL_FRect&)> compute_overlay_rect;
    std::function<void(const SDL_FRect&)> reveal_overlay_selection;
    std::function<void()> reveal_debug_pane_selection;
    std::function<void(std::size_t)> focus_debug_call_stack_row;
    std::function<void(std::size_t, bool)> activate_debug_breakpoint_row;
    std::function<void(int)> move_buffer_search_selection;
    std::function<void(BufferFindToggle)> toggle_buffer_search_option;
    std::function<void(int)> move_project_search_selection;
    std::function<void()> toggle_project_search_pattern_mode;
    std::function<void()> cycle_project_search_case_mode;
    std::function<void()> toggle_project_search_hidden_files;
    std::function<void()> replace_all_buffer_search_matches;
    std::function<void()> replace_current_buffer_search_match;
    std::function<void(int)> move_file_finder_selection;
    std::function<void(ProjectSearchEditField)> begin_project_search_edit;
    std::function<void()> commit_project_search_edit;
    std::function<void()> cancel_project_search_edit;
    std::function<void()> replace_all_project_search_matches;
    std::function<void(const std::filesystem::path&)> open_file;
    std::function<editor::TextViewport*()> active_editor_viewport;
    std::function<void()> seed_buffer_search_from_project_search;
    std::function<bool(GitSidebarActionId, std::size_t)> dispatch_git_sidebar_action;
    std::function<void()> close_commit_workflow;
    std::function<bool()> request_commit_workflow_commit;
    std::function<void(CommitWorkflowFocusField)> set_commit_workflow_focus_field;
    std::function<bool(std::string_view)> commit_body_write_clipboard_text;
    std::function<bool()> refresh_problems_sidebar;
    std::function<bool()> open_selected_plugin_sidebar_item;
    std::function<bool()> toggle_plugin_sidebar_item;
    std::function<void(int)> jump_compare_hunk;
    std::function<void()> stage_compare_hunk;
    std::function<void()> stage_compare_selected_lines;
    std::function<void()> unstage_compare_hunk;
    std::function<void()> unstage_compare_selected_lines;
    std::function<void()> open_discard_compare_hunk_prompt;
    std::function<void()> open_discard_compare_selected_lines_prompt;
    std::function<CompareTabState*()> active_compare_tab;
    std::function<void(CompareTabState&)> refresh_compare_tab_derived_state;
    std::function<void(CompareTabState&, bool)> sync_compare_selection_from_viewport;
    std::function<void()> reset_caret_blink;
    std::function<void()> mark_active_editor_folding_dirty;
    std::function<void()> note_active_editable_edit;
    std::function<void(const std::vector<std::string>&, const std::vector<std::string>&)>
        request_active_editable_change_redraw;
    std::function<void()> request_close_active_tab;
    std::function<void()> reveal_active_compare_selection;
    std::function<bool()> accept_inline_completion;
    std::function<void()> dismiss_inline_completion;
    std::function<std::optional<std::string>(std::string_view)> get_setting_value;
    std::function<MergeTabState*()> active_merge_tab;
    std::function<void(MergeTabState&)> update_merge_tracking_after_viewport_edit;
    std::function<void(int)> move_merge_selection;
    std::function<void(compare::MergeChoice)> apply_merge_choice;
    std::function<void()> add_debug_watch_expression;
    std::function<void(std::size_t)> edit_debug_watch_expression;
  };

  KeyInputCoordinator(ProjectWorkspaceState& state,
                      PromptState& prompts,
                      MenuSurfaceState& menu_state,
                      NotificationService& notifications,
                      Services services,
                      Operations operations);

  bool HandleKeyDown(const SDL_KeyboardEvent& event);

  struct DispatchProbe {
    const ResolvedKeybinding* last = nullptr;
    bool record_only = false;
  };
  DispatchProbe& dispatch_probe() { return dispatch_probe_; }

 private:
  bool HandleDirtyPromptKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandleTreeContextMenuKeyDown(const SDL_KeyboardEvent& event);
  bool HandleMenuBarKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandleMenuOverflowPopupKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandlePromptSurfaceKeyDown(const SDL_KeyboardEvent& event);
  bool HandleGlobalKeyDown(const SDL_KeyboardEvent& event,
                           SDL_Keymod modifiers,
                           bool active_merge_tab);
  std::optional<bool> HandleKeyChord(const SDL_KeyboardEvent& event,
                                     SDL_Keymod modifiers,
                                     KeybindingContext key_ctx);
  KeybindingContext ActiveKeybindingContext() const;
  std::function<bool(ActionId)> ActionEnabledPredicate() const;
  bool DispatchResolvedKeybinding(const ResolvedKeybinding& binding, ActionSource source);
  bool HandleSurfaceNavigationKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandleOverlayKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandleSharedBufferSearchKey(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool DispatchKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  void MoveOverlayListSelection(std::size_t& selected_index, std::size_t item_count, int delta);
  template <typename Fn>
  void EnsureRedraw(Fn&& request_redraw) {
    if (operations_.damage_generation() == key_damage_mark_) {
      request_redraw();
    }
  }
  bool HandleSettingsOverlayKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandleSidebarKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandleBottomPanelKeyDown(const SDL_KeyboardEvent& event);
  bool HandleCommitBodyKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandleCompareKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandleMergeKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandleDebugVariablesKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandleDebugWatchKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandleDebugCallStackKeyDown(const SDL_KeyboardEvent& event);
  bool HandleDebugBreakpointsKeyDown(const SDL_KeyboardEvent& event);
  bool HandleDefaultEditorKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandleNotificationFocusKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);

  ProjectWorkspaceState& state_;
  PromptState& prompts_;
  MenuSurfaceState& menu_state_;
  NotificationService& notifications_;
  Services services_;
  Operations operations_;
  DispatchProbe dispatch_probe_;
  DamageMark key_damage_mark_ = 0;
};

}
