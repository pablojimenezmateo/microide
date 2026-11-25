#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/SingleLineEditor.h"
#include "editor/TextViewport.h"
#include "workspace/state/WorkspaceMenuState.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/state/WorkspacePromptState.h"
#include "workspace/state/WorkspaceTextInputState.h"

namespace microide::workspace {

class CompareMergeService;
class QuickPickService;
class AssistService;
class TerminalPanelService;

class TextInputCoordinator {
 public:
  struct Services {
    CompareMergeService& compare_merge;
    QuickPickService& quick_pick;
    AssistService& assist;
    TerminalPanelService& terminal_panel;
  };

  struct Operations {
    std::function<TextInputSurface()> current_text_input_surface;
    std::function<void()> request_frame;
    std::function<void()> refresh_command_palette;
    std::function<void()> refresh_buffer_search;
    std::function<void()> refresh_project_search;
    std::function<void()> reset_overlay_scroll;
    std::function<bool(std::string_view)> switch_quick_open_mode;
    std::function<editor::SingleLineEditor*()> settings_query_editor;
    std::function<editor::SingleLineEditor*()> settings_value_editor;
    std::function<editor::SingleLineEditor*()> terminal_find_query_editor;
    std::function<void()> refresh_terminal_find;
    std::function<void()> refresh_settings_overlay;
    std::function<editor::TextViewport*()> active_editable_viewport;
    std::function<CompareTabState*()> active_compare_tab;
    std::function<void(CompareTabState&)> refresh_compare_tab_derived_state;
    std::function<void(CompareTabState&, bool)> sync_compare_selection_from_viewport;
    std::function<MergeTabState*()> active_merge_tab;
    std::function<void(MergeTabState&)> update_merge_tracking_after_viewport_edit;
    std::function<void()> reset_caret_blink;
    std::function<void()> mark_active_editor_folding_dirty;
    std::function<void()> note_active_editable_edit;
    std::function<void(const std::vector<std::string>&, const std::vector<std::string>&)>
        request_active_editable_change_redraw;
    std::function<TerminalPaneState*()> active_terminal_pane;
    std::function<void()> clear_terminal_selection;
    std::function<void(std::string_view)> append_terminal_pending_input;
    std::function<bool()> terminal_has_selection;
    std::function<std::string()> selected_terminal_text;
    std::function<bool(std::string_view)> write_clipboard_text;
    std::function<bool(std::string_view)> write_primary_selection_text;
    std::function<std::optional<std::string>()> read_clipboard_text;
    std::function<std::optional<std::filesystem::path>()> store_clipboard_image;
    std::function<bool(TerminalPaneState&, const std::filesystem::path&)> upload_file_to_host_terminal;
    std::function<std::string(const TerminalPaneState&, const std::filesystem::path&)> dropped_path_text;
    std::function<void()> submit_terminal_pending_input;
    std::function<void()> erase_last_terminal_pending_input_codepoint;
    std::function<char(SDL_Keycode, SDL_Keymod)> keycode_to_ascii;
  };

  TextInputCoordinator(ProjectWorkspaceState& state,
                       PromptState& prompts,
                       MenuSurfaceState& menu_state,
                       TextInputState& text_input_state,
                       Services services,
                       Operations operations);

  void SyncTextInputSurface(SDL_Window* window);
  bool CompositionConsumesKey(SDL_Keycode key, SDL_Keymod modifiers) const;
  bool HandleTextEditing(const SDL_TextEditingEvent& event);
  bool HandleTextInput(const SDL_TextInputEvent& event);
  bool HandleSingleLineKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandleTerminalKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool PasteClipboardIntoTerminal();
  bool PasteTextIntoTerminal(std::string text);
  bool TypeFileIntoTerminal(TerminalPaneState& pane, const std::filesystem::path& local_path);
  bool InsertTextAtActiveSurface(std::string_view input, bool distribute_across_carets = false);
  bool HasSelectionAtActiveSingleLineSurface() const;
  bool HasActiveSingleLineTextSurface() const;
  std::string SelectedTextAtActiveSingleLineSurface() const;
  bool SelectAllAtActiveSingleLineSurface();
  bool CutSelectionAtActiveSingleLineSurface();

 private:
  void RequestSurfaceRedraw(TextInputSurface surface);
  editor::SingleLineEditor* ActiveSingleLineTextState();
  const editor::SingleLineEditor* ActiveSingleLineTextState() const;
  void RequestSingleLineTextRedraw(TextInputSurface surface, bool text_changed);

  ProjectWorkspaceState& state_;
  PromptState& prompts_;
  MenuSurfaceState& menu_state_;
  TextInputState& text_input_state_;
  Services services_;
  Operations operations_;
};

}
