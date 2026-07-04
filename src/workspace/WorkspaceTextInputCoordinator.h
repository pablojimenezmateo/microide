#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/SingleLineEditor.h"
#include "editor/TextViewport.h"
#include "workspace/WorkspaceMenuState.h"
#include "workspace/WorkspaceProjectState.h"
#include "workspace/WorkspacePromptState.h"
#include "workspace/WorkspaceTextInputState.h"

namespace microide::workspace {

class TextInputCoordinator {
 public:
  struct Operations {
    std::function<TextInputSurface()> current_text_input_surface;
    std::function<void()> request_prompt_redraw;
    std::function<void()> request_sidebar_redraw;
    std::function<SidebarMode()> active_sidebar_mode;
    std::function<void()> request_overlay_redraw;
    std::function<void()> request_focused_editor_redraw;
    std::function<void()> request_window_redraw;
    std::function<void()> refresh_compare_picker;
    std::function<void()> refresh_launch_config_picker;
    std::function<void()> refresh_command_palette;
    std::function<void()> refresh_buffer_search;
    std::function<void()> refresh_project_search;
    std::function<void()> reset_overlay_scroll;
    std::function<editor::SingleLineEditor*()> settings_query_editor;
    std::function<editor::SingleLineEditor*()> settings_value_editor;
    std::function<void()> refresh_settings_overlay;
    std::function<editor::TextViewport*()> active_editable_viewport;
    std::function<CompareTabState*()> active_compare_tab;
    std::function<void(CompareTabState&)> refresh_compare_tab_derived_state;
    std::function<void(CompareTabState&, bool)> sync_compare_selection_from_viewport;
    std::function<MergeTabState*()> active_merge_tab;
    std::function<void(MergeTabState&,
                       const std::vector<std::string>&,
                       std::optional<editor::SelectionRange>,
                       editor::TextPosition)>
        update_merge_tracking_after_viewport_edit;
    std::function<void()> reset_caret_blink;
    std::function<void()> mark_active_editor_folding_dirty;
    std::function<void()> request_active_editable_last_change_redraw;
    std::function<void(const std::vector<std::string>&, const std::vector<std::string>&)>
        request_active_editable_change_redraw;
    std::function<void(std::size_t, std::size_t)> request_active_editable_blame_neighborhood_redraw;
    std::function<void()> request_tab_strip_redraw;
    std::function<TerminalTabState*()> active_terminal_tab;
    std::function<void()> clear_terminal_selection;
    std::function<void(std::string_view)> append_terminal_pending_input;
    std::function<void()> request_bottom_panel_content_redraw;
    std::function<bool()> terminal_has_selection;
    std::function<std::string()> selected_terminal_text;
    std::function<bool(std::string_view)> write_clipboard_text;
    std::function<bool(std::string_view)> write_primary_selection_text;
    std::function<std::optional<std::string>()> read_clipboard_text;
    std::function<void()> submit_terminal_pending_input;
    std::function<void()> erase_last_terminal_pending_input_codepoint;
    std::function<std::optional<std::string>()> read_primary_selection_text;
    std::function<char(SDL_Keycode, SDL_Keymod)> keycode_to_ascii;
    std::function<bool(editor::TextViewport*, std::string_view)> try_editor_snippet_insert_text;
  };

  TextInputCoordinator(ProjectWorkspaceState& state,
                       PromptState& prompts,
                       MenuSurfaceState& menu_state,
                       TextInputState& text_input_state,
                       Operations operations);

  void SyncTextInputSurface(SDL_Window* window);
  bool CompositionConsumesKey(SDL_Keycode key, SDL_Keymod modifiers) const;
  bool HandleTextEditing(const SDL_TextEditingEvent& event);
  bool HandleTextInput(const SDL_TextInputEvent& event);
  bool HandleSingleLineKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandleTerminalKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool PasteClipboardIntoTerminal();
  bool PasteTextIntoTerminal(std::string text);
  bool InsertTextAtActiveSurface(std::string_view input);
  bool HasSelectionAtActiveSingleLineSurface() const;
  std::string SelectedTextAtActiveSingleLineSurface() const;
  bool SelectAllAtActiveSingleLineSurface();
  bool CutSelectionAtActiveSingleLineSurface();

 private:
  void RequestCompositionRedraw(TextInputSurface surface);
  editor::SingleLineEditor* ActiveSingleLineTextState();
  const editor::SingleLineEditor* ActiveSingleLineTextState() const;
  void RequestSingleLineTextRedraw(TextInputSurface surface, bool text_changed);

  ProjectWorkspaceState& state_;
  PromptState& prompts_;
  MenuSurfaceState& menu_state_;
  TextInputState& text_input_state_;
  Operations operations_;
};

}  // namespace microide::workspace
