#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

#include "workspace/WorkspaceActionTypes.h"
#include "workspace/WorkspaceKeybindingRegistry.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceMenuState.h"
#include "workspace/WorkspaceProjectState.h"
#include "workspace/WorkspacePromptState.h"
#include "workspace/WorkspaceTextInputState.h"

namespace microide::workspace {

class KeyInputCoordinator {
 public:
  struct Operations {
    std::function<bool()> has_pending_redraw;
    std::function<void()> request_prompt_redraw;
    std::function<void()> request_chrome_redraw;
    std::function<void()> request_window_redraw;
    std::function<void()> request_overlay_redraw;
    std::function<void()> request_sidebar_redraw;
    std::function<void()> request_bottom_panel_command_redraw;
    std::function<void()> request_bottom_panel_content_redraw;
    std::function<void()> request_focused_editor_redraw;
    std::function<bool(const SDL_KeyboardEvent&)> command_prompt_handle_key_down;
    std::function<bool(SDL_Keycode, SDL_Keymod)> text_input_composition_consumes_key;
    std::function<TextInputSurface()> current_text_input_surface;
    std::function<bool(const SDL_KeyboardEvent&, SDL_Keymod)> text_input_handle_single_line_key_down;
    std::function<bool(const SDL_KeyboardEvent&, SDL_Keymod)> text_input_handle_terminal_key_down;
    std::function<void()> confirm_dirty_prompt;
    std::function<char(SDL_Keycode, SDL_Keymod)> keycode_to_ascii;
    std::function<void()> close_tree_context_menu;
    std::function<int(int, int)> next_enabled_tree_context_menu_item_index;
    std::function<bool(std::size_t)> execute_tree_context_menu_item;
    std::function<void()> close_menu_bar;
    std::function<bool(int)> switch_menu_bar_menu;
    std::function<bool(int)> move_active_menu_item;
    std::function<bool(MenuId, std::size_t)> execute_menu_item;
    std::function<void(bool)> dismiss_prompt_surface;
    std::function<void()> confirm_prompt_surface;
    std::function<bool(ActionId, const std::vector<std::string>&, ActionSource)> execute_action;
    std::function<bool(std::string_view, const std::vector<std::string>&, ActionSource)>
        execute_command_name;
    std::function<std::vector<ResolvedKeybinding>()> resolved_keybindings;
    std::function<bool()> open_untitled_tab;
    std::function<bool()> active_tab_is_compare;
    std::function<bool()> active_tab_is_merge;
    std::function<editor::TextViewport*()> active_navigable_viewport;
    std::function<editor::TextViewport*()> active_editable_viewport;
    std::function<TerminalTabState*()> active_terminal_tab;
    std::function<void(bool)> dismiss_overlay;
    std::function<bool()> settings_overlay_visible;
    std::function<void()> close_settings_overlay;
    std::function<void()> close_sidebar;
    std::function<SidebarMode()> active_sidebar_mode;
    std::function<void()> activate_overlay_selection;
    std::function<void(int)> move_compare_picker_selection;
    std::function<void()> refresh_compare_picker;
    std::function<std::optional<WorkspaceLayout>()> current_workspace_layout;
    std::function<SDL_FRect(const SDL_FRect&)> compute_overlay_rect;
    std::function<void(const SDL_FRect&)> reveal_overlay_selection;
    std::function<void(int)> move_buffer_search_selection;
    std::function<void()> refresh_buffer_search;
    std::function<void(int)> move_project_search_selection;
    std::function<void()> refresh_project_search;
    std::function<void()> replace_all_buffer_search_matches;
    std::function<void()> replace_current_buffer_search_match;
    std::function<void(int)> move_file_finder_selection;
    std::function<void()> reset_overlay_scroll;
    std::function<void(ProjectSearchEditField)> begin_project_search_edit;
    std::function<void()> commit_project_search_edit;
    std::function<void()> cancel_project_search_edit;
    std::function<void()> replace_all_project_search_matches;
    std::function<void()> toggle_project_search_pattern_mode;
    std::function<void()> cycle_project_search_case_mode;
    std::function<void()> toggle_project_search_hidden_files;
    std::function<void(const std::filesystem::path&)> open_file;
    std::function<editor::TextViewport*()> active_editor_viewport;
    std::function<void()> restore_previous_sidebar;
    std::function<void(int)> move_git_sidebar_selection;
    std::function<void()> reveal_selected_git_sidebar_line;
    std::function<bool(std::size_t)> open_git_sidebar_entry;
    std::function<bool(std::size_t)> stage_git_sidebar_entry;
    std::function<bool(std::size_t)> unstage_git_sidebar_entry;
    std::function<bool(std::size_t)> discard_git_sidebar_entry;
    std::function<void(int)> move_problems_sidebar_selection;
    std::function<void()> reveal_selected_problems_sidebar_line;
    std::function<bool()> open_selected_problem_sidebar_item;
    std::function<bool()> refresh_problems_sidebar;
    std::function<void(int)> move_tests_sidebar_selection;
    std::function<void()> reveal_selected_tests_sidebar_line;
    std::function<bool()> open_selected_test_sidebar_item;
    std::function<bool()> run_selected_test_sidebar_item;
    std::function<bool()> refresh_tests_sidebar;
    std::function<void(int)> move_plugin_sidebar_selection;
    std::function<void()> reveal_selected_plugin_sidebar_line;
    std::function<bool()> open_selected_plugin_sidebar_item;
    std::function<bool()> refresh_plugin_sidebar;
    std::function<void()> reveal_selected_tree_sidebar_line;
    std::function<void()> refresh_project_files;
    std::function<void()> open_compare_picker;
    std::function<void(int)> move_compare_selection;
    std::function<void(int)> jump_compare_hunk;
    std::function<void()> open_working_file_from_compare;
    std::function<CompareTabState*()> active_compare_tab;
    std::function<void(CompareTabState&)> refresh_compare_tab_derived_state;
    std::function<void(CompareTabState&, bool)> sync_compare_selection_from_viewport;
    std::function<void()> reset_caret_blink;
    std::function<void()> request_active_editable_last_change_redraw;
    std::function<void(const std::vector<std::string>&, const std::vector<std::string>&)>
        request_active_editable_change_redraw;
    std::function<void(std::size_t, std::size_t)> request_active_editable_blame_neighborhood_redraw;
    std::function<void()> request_tab_strip_redraw;
    std::function<void(std::size_t, std::size_t)> request_compare_row_range_redraw;
    std::function<void()> request_close_active_tab;
    std::function<void()> reveal_active_compare_selection;
    std::function<bool(std::string*)> show_completion_overlay;
    std::function<bool()> apply_selected_completion;
    std::function<bool(std::string*)> show_code_actions_overlay;
    std::function<bool()> execute_selected_code_action;
    std::function<bool(std::string*)> request_inline_completion;
    std::function<bool()> accept_inline_completion;
    std::function<void()> dismiss_inline_completion;
    std::function<MergeTabState*()> active_merge_tab;
    std::function<void(MergeTabState&,
                       const std::vector<std::string>&,
                       std::optional<editor::SelectionRange>,
                       editor::TextPosition)>
        update_merge_tracking_after_viewport_edit;
    std::function<void(int)> move_merge_selection;
    std::function<void(compare::MergeChoice)> apply_merge_choice;
    std::function<void()> open_merge_result_file;
  };

  KeyInputCoordinator(ProjectWorkspaceState& state,
                      PromptState& prompts,
                      MenuSurfaceState& menu_state,
                      Operations operations);

  bool HandleKeyDown(const SDL_KeyboardEvent& event);

 private:
  bool HandleDirtyPromptKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandleTreeContextMenuKeyDown(const SDL_KeyboardEvent& event);
  bool HandleMenuBarKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandlePromptSurfaceKeyDown(const SDL_KeyboardEvent& event);
  bool HandleGlobalKeyDown(const SDL_KeyboardEvent& event,
                           SDL_Keymod modifiers,
                           bool active_compare_tab,
                           bool active_merge_tab);
  KeybindingContext ActiveKeybindingContext() const;
  bool DispatchResolvedKeybinding(const ResolvedKeybinding& binding, ActionSource source);
  bool HandleSurfaceNavigationKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandleOverlayKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandleSidebarKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandleCompareKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandleMergeKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandleDefaultEditorKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);

  ProjectWorkspaceState& state_;
  PromptState& prompts_;
  MenuSurfaceState& menu_state_;
  Operations operations_;
};

}  // namespace microide::workspace
