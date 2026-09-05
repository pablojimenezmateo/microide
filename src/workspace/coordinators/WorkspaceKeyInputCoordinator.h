#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "editor/TextViewport.h"
#include "workspace/git/CommitWorkflowState.h"
#include "workspace/actions/WorkspaceActionTypes.h"
#include "workspace/registries/WorkspaceKeybindingRegistry.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/state/WorkspaceMenuState.h"
#include "workspace/git/GitSidebarCommandCenter.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/state/WorkspacePromptState.h"
#include "workspace/state/WorkspaceTextInputState.h"

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
    std::function<void()> request_bottom_panel_content_redraw;
    // Bottom-panel scroll for the keyboard. The Output channel and the plugin
    // surface preview are focusable and scrollable but had no key handling at all,
    // so arrowing in a focused panel scrolled the editor behind it instead. Both
    // route through PanelMouseCoordinator::ScrollPanelRows, the same clamped
    // scroll model the wheel uses; the span only sizes the Home/End jump.
    std::function<void(int)> scroll_bottom_panel_rows;
    std::function<int()> bottom_panel_scroll_span_rows;
    // Rows the bottom panel shows at once; a terminal Shift+PageUp/PageDown pages
    // by this rather than the list widgets' fixed eight-row step.
    std::function<int()> bottom_panel_visible_rows;
    std::function<void()> request_focused_editor_redraw;
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
    std::function<const std::vector<ResolvedKeybinding>&()> resolved_keybindings;
    std::function<bool()> active_tab_is_compare;
    std::function<bool()> active_tab_is_merge;
    std::function<editor::TextViewport*()> active_navigable_viewport;
    std::function<editor::TextViewport*()> active_editable_viewport;
    std::function<TerminalTabState*()> active_terminal_tab;
    std::function<void(bool)> dismiss_overlay;
    std::function<bool()> settings_overlay_visible;
    std::function<void()> close_settings_overlay;
    // Settings two-pane keyboard navigation. Panes are encoded as ints to keep
    // this header free of the SettingsOverlayService enum (0=Filter,1=Categories,2=Values).
    std::function<bool()> settings_overlay_is_settings_mode;
    std::function<int()> settings_focused_pane;
    std::function<void(int)> settings_focus_pane;
    std::function<void(int)> settings_cycle_focus;
    std::function<void(int)> settings_move_category;
    std::function<void(int)> settings_move_row;
    // Item count of a pane, used to size the Home/End jump so the overlay lists
    // answer the same navigation contract as every other list in the shell.
    std::function<std::size_t(int)> settings_pane_item_count;
    // Scrolls the row list by whole entries, clamped against the live layout.
    // Help/About has no selection to move, so this is how it navigates.
    std::function<void(int)> settings_scroll_rows;
    std::function<void(int)> settings_step_selected;
    std::function<void()> settings_toggle_or_activate_selected;
    std::function<void()> settings_reset_selected;
    // Inline String-value editing in the Settings overlay.
    std::function<bool()> settings_value_edit_active;
    std::function<void()> settings_commit_value_edit;
    std::function<void()> settings_cancel_value_edit;
    // Font-picker specialization of value editing: Up/Down move the dropdown
    // highlight; typing (any other key) re-filters and resets the highlight.
    std::function<bool()> settings_value_edit_is_picker;
    std::function<void(int)> settings_picker_move;
    std::function<void()> settings_picker_reset_highlight;
    std::function<void()> close_sidebar;
    std::function<SidebarMode()> active_sidebar_mode;
    std::function<void()> activate_overlay_selection;
    std::function<void()> complete_command_palette_query;
    std::function<void(int)> move_compare_picker_selection;
    std::function<std::optional<WorkspaceLayout>()> current_workspace_layout;
    std::function<SDL_FRect(const SDL_FRect&)> compute_overlay_rect;
    std::function<void(const SDL_FRect&)> reveal_overlay_selection;
    // Keeps the debug pane's selection on screen after a move or an expand, like
    // every other list's reveal.
    std::function<void()> reveal_debug_pane_selection;
    // Focuses the session/thread/frame a Call Stack row names, running the same
    // path a click on that row does (minus handing focus to the editor).
    std::function<void(std::size_t)> focus_debug_call_stack_row;
    // Enter (navigate) / Space (toggle) on a Breakpoints row, running the same
    // path the mouse's single/double click does.
    std::function<void(std::size_t, bool)> activate_debug_breakpoint_row;
    std::function<void(int)> move_buffer_search_selection;
    std::function<void(BufferFindToggle)> toggle_buffer_search_option;
    std::function<void(int)> move_project_search_selection;
    std::function<void()> refresh_project_search;
    // Alt+R / Alt+C / Alt+H in the search panel — the same three toggles the
    // panel's buttons drive (VSCode binds the equivalent chords in its search
    // box). Each re-runs the search itself.
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
    std::function<void()> restore_previous_sidebar;
    std::function<void()> seed_buffer_search_from_project_search;
    std::function<void(int)> move_git_sidebar_selection;
    std::function<bool(GitSidebarActionId, std::size_t)> dispatch_git_sidebar_action;
    std::function<void()> close_commit_workflow;
    std::function<bool()> request_commit_workflow_commit;
    // Move keyboard focus between the commit subject and body fields, then refresh the
    // commit pre-checks/draft (this is the coarse refresh point, not every keystroke).
    std::function<void(CommitWorkflowFocusField)> set_commit_workflow_focus_field;
    std::function<bool(std::string_view)> commit_body_write_clipboard_text;
    std::function<std::optional<std::string>()> commit_body_read_clipboard_text;
    std::function<void(int)> move_problems_sidebar_selection;
    std::function<bool()> open_selected_problem_sidebar_item;
    std::function<bool()> refresh_problems_sidebar;
    std::function<void(int)> move_tests_sidebar_selection;
    std::function<bool()> open_selected_test_sidebar_item;
    std::function<bool()> run_selected_test_sidebar_item;
    std::function<bool()> refresh_tests_sidebar;
    std::function<void(int)> move_plugin_sidebar_selection;
    std::function<bool()> open_selected_plugin_sidebar_item;
    std::function<bool()> toggle_plugin_sidebar_item;
    std::function<bool()> refresh_plugin_sidebar;
    std::function<void()> reveal_selected_tree_sidebar_line;
    std::function<void()> refresh_project_files;
    std::function<void()> open_compare_picker;
    std::function<void(int)> move_compare_selection;
    std::function<void(int)> jump_compare_hunk;
    std::function<void()> stage_compare_hunk;
    std::function<void()> stage_compare_selected_lines;
    std::function<void()> unstage_compare_hunk;
    std::function<void()> unstage_compare_selected_lines;
    std::function<void()> open_discard_compare_hunk_prompt;
    std::function<void()> open_discard_compare_selected_lines_prompt;
    std::function<void()> open_working_file_from_compare;
    std::function<CompareTabState*()> active_compare_tab;
    std::function<void(CompareTabState&)> refresh_compare_tab_derived_state;
    std::function<void(CompareTabState&, bool)> sync_compare_selection_from_viewport;
    std::function<void()> reset_caret_blink;
    std::function<void()> mark_active_editor_folding_dirty;
    std::function<void()> request_active_editable_last_change_redraw;
    std::function<void(const std::vector<std::string>&, const std::vector<std::string>&)>
        request_active_editable_change_redraw;
    std::function<void(std::size_t, std::size_t)> request_active_editable_blame_neighborhood_redraw;
    std::function<void()> request_tab_strip_redraw;
    std::function<void(std::size_t, std::size_t)> request_compare_row_range_redraw;
    std::function<void()> request_close_active_tab;
    std::function<void()> reveal_active_compare_selection;
    std::function<bool()> accept_inline_completion;
    std::function<void()> dismiss_inline_completion;
    std::function<bool(bool)> try_snippet_tab_in_editor;
    std::function<bool()> try_snippet_escape_in_editor;
    std::function<bool(editor::TextViewport*)> try_snippet_backspace_in_editor;
    std::function<bool(editor::TextViewport*)> try_snippet_delete_forward_in_editor;
    std::function<void()> notify_snippet_session_caret_moved;
    std::function<std::optional<std::string>(std::string_view)> get_setting_value;
    std::function<MergeTabState*()> active_merge_tab;
    std::function<void(MergeTabState&,
                       std::optional<editor::SelectionRange>,
                       editor::TextPosition)>
        update_merge_tracking_after_viewport_edit;
    std::function<void(int)> move_merge_selection;
    std::function<void(compare::MergeChoice)> apply_merge_choice;
    std::function<void()> open_merge_result_file;
    // Debug Variables panel (Phase 4) keyboard ops: expand/collapse a tree row and
    // enter/commit/cancel an inline value edit (selection movement is pure state,
    // handled directly on the model).
    std::function<void(std::size_t)> toggle_debug_variable_row;
    std::function<void(std::size_t)> begin_debug_variable_edit;
    std::function<void()> commit_debug_variable_edit;
    std::function<void()> cancel_debug_variable_edit;
    // Watch panel (Phase 6): tree nav/expand + inline child setVariable edit, plus
    // add/edit/remove of the watch expression strings (add/edit route to a prompt).
    std::function<void(std::size_t)> toggle_debug_watch_row;
    std::function<void(std::size_t)> begin_debug_watch_edit;
    std::function<void()> commit_debug_watch_edit;
    std::function<void()> cancel_debug_watch_edit;
    std::function<void()> add_debug_watch_expression;
    std::function<void(std::size_t)> edit_debug_watch_expression;
    std::function<void(std::size_t)> remove_debug_watch_expression;
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
  // Match navigation + Alt+R regex toggle, shared by the find and
  // find-and-replace widgets (see the definition for why it is factored out).
  bool HandleSharedBufferSearchKey(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  // Clamped selection move + reveal for the overlay lists whose selection is a plain
  // index into a match vector (command palette, launch picker, completion, code
  // actions). The pickers with extra reveal side effects keep their own helpers.
  void MoveOverlayListSelection(std::size_t& selected_index, std::size_t item_count, int delta);
  // Requests a redraw only when none is already pending. A member rather than a
  // local lambda so the key handlers that live in their own translation units
  // (settings, sidebar) coalesce redraws the same way HandleKeyDown does.
  template <typename Fn>
  void EnsureRedraw(Fn&& request_redraw) {
    if (!operations_.has_pending_redraw()) {
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

  ProjectWorkspaceState& state_;
  PromptState& prompts_;
  MenuSurfaceState& menu_state_;
  Operations operations_;
};

}  // namespace microide::workspace
