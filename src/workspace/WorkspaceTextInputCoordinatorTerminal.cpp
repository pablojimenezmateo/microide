#include "workspace/WorkspaceTextInputCoordinator.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspaceCommandPromptCoordinator.h"
#include "workspace/WorkspaceShell.h"

namespace microide::workspace {
bool TextInputCoordinator::HandleTerminalKeyDown(const SDL_KeyboardEvent& event,
                                                 SDL_Keymod modifiers) {
  auto* terminal_tab = operations_.active_terminal_tab();
  if (terminal_tab == nullptr) {
    return false;
  }
  const auto follow_terminal_tail = [&]() { terminal_tab->follow_tail = true; };
  const auto handled_with_panel_redraw = [this]() {
    operations_.request_bottom_panel_content_redraw();
    return true;
  };

  using KeyPress = terminal::TerminalSession::KeyPress;
  const auto make_key_press = [&](KeyPress::Key key, char32_t codepoint = 0) {
    KeyPress press;
    press.key = key;
    press.codepoint = codepoint;
    press.shift = (modifiers & SDL_KMOD_SHIFT) != 0;
    press.alt = (modifiers & SDL_KMOD_ALT) != 0;
    press.ctrl = (modifiers & SDL_KMOD_CTRL) != 0;
    press.super = (modifiers & SDL_KMOD_GUI) != 0;
    return press;
  };
  const auto send_key_press = [&](KeyPress::Key key, char32_t codepoint = 0) {
    follow_terminal_tail();
    terminal_tab->session.SendKeyPress(make_key_press(key, codepoint));
    return handled_with_panel_redraw();
  };

  if ((modifiers & SDL_KMOD_CTRL) && event.key == SDLK_C && operations_.terminal_has_selection()) {
    const std::string text = operations_.selected_terminal_text();
    if (!text.empty() && operations_.write_clipboard_text(text)) {
      operations_.write_primary_selection_text(text);
    }
    return true;
  }

  if (event.key == SDLK_ESCAPE && operations_.terminal_has_selection()) {
    operations_.clear_terminal_selection();
    return handled_with_panel_redraw();
  }

  if ((modifiers & SDL_KMOD_CTRL) && (modifiers & SDL_KMOD_SHIFT) && event.key == SDLK_V) {
    return PasteClipboardIntoTerminal();
  }

  if ((modifiers & SDL_KMOD_SHIFT) && event.key == SDLK_INSERT) {
    return PasteClipboardIntoTerminal();
  }

  // Control combinations that map to C0 control bytes (or CSI-u under Kitty).
  if (modifiers & SDL_KMOD_CTRL) {
    if ((event.key >= SDLK_A && event.key <= SDLK_Z) || event.key == SDLK_LEFTBRACKET ||
        event.key == SDLK_BACKSLASH || event.key == SDLK_RIGHTBRACKET ||
        event.key == SDLK_SPACE) {
      return send_key_press(KeyPress::Key::Char, static_cast<char32_t>(event.key));
    }
  }

  // Alt+printable sends ESC-prefixed text (or CSI-u under Kitty).
  if (modifiers & SDL_KMOD_ALT) {
    const char input_character = operations_.keycode_to_ascii(event.key, modifiers);
    if (input_character != '\0') {
      return send_key_press(KeyPress::Key::Char, static_cast<char32_t>(input_character));
    }
  }

  switch (event.key) {
    case SDLK_ESCAPE:
      return send_key_press(KeyPress::Key::Escape);
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      operations_.submit_terminal_pending_input();
      return send_key_press(KeyPress::Key::Enter);
    case SDLK_BACKSPACE:
      operations_.erase_last_terminal_pending_input_codepoint();
      return send_key_press(KeyPress::Key::Backspace);
    case SDLK_TAB:
      return send_key_press(KeyPress::Key::Tab);
    case SDLK_UP:
      return send_key_press(KeyPress::Key::Up);
    case SDLK_DOWN:
      return send_key_press(KeyPress::Key::Down);
    case SDLK_RIGHT:
      return send_key_press(KeyPress::Key::Right);
    case SDLK_LEFT:
      return send_key_press(KeyPress::Key::Left);
    case SDLK_HOME:
      return send_key_press(KeyPress::Key::Home);
    case SDLK_END:
      return send_key_press(KeyPress::Key::End);
    case SDLK_PAGEUP:
      return send_key_press(KeyPress::Key::PageUp);
    case SDLK_PAGEDOWN:
      return send_key_press(KeyPress::Key::PageDown);
    case SDLK_INSERT:
      return send_key_press(KeyPress::Key::Insert);
    case SDLK_DELETE:
      return send_key_press(KeyPress::Key::Delete);
    case SDLK_F1:
      return send_key_press(KeyPress::Key::F1);
    case SDLK_F2:
      return send_key_press(KeyPress::Key::F2);
    case SDLK_F3:
      return send_key_press(KeyPress::Key::F3);
    case SDLK_F4:
      return send_key_press(KeyPress::Key::F4);
    case SDLK_F5:
      return send_key_press(KeyPress::Key::F5);
    case SDLK_F6:
      return send_key_press(KeyPress::Key::F6);
    case SDLK_F7:
      return send_key_press(KeyPress::Key::F7);
    case SDLK_F8:
      return send_key_press(KeyPress::Key::F8);
    case SDLK_F9:
      return send_key_press(KeyPress::Key::F9);
    case SDLK_F10:
      return send_key_press(KeyPress::Key::F10);
    case SDLK_F11:
      return send_key_press(KeyPress::Key::F11);
    case SDLK_F12:
      return send_key_press(KeyPress::Key::F12);
    default:
      break;
  }

  return false;
}

bool TextInputCoordinator::PasteClipboardIntoTerminal() {
  auto* terminal_tab = operations_.active_terminal_tab();
  if (terminal_tab == nullptr) {
    return false;
  }

  const std::optional<std::string> clipboard_text = operations_.read_clipboard_text();
  if (!clipboard_text.has_value()) {
    return true;
  }

  operations_.clear_terminal_selection();
  if (clipboard_text->find_first_of("\r\n") == std::string::npos) {
    operations_.append_terminal_pending_input(*clipboard_text);
  }
  terminal_tab->follow_tail = true;
  terminal_tab->session.PasteText(*clipboard_text);
  operations_.request_bottom_panel_content_redraw();
  return true;
}

TextInputCoordinator WorkspaceShell::MakeTextInputCoordinator() {
  return TextInputCoordinator(
      context_.current_project_state, context_.prompts, context_.menu_state,
      context_.text_input,
      TextInputCoordinator::Operations{
          .current_text_input_surface = [this]() { return CurrentTextInputSurface(); },
          .request_prompt_redraw = [this]() { RequestPromptRedraw(); },
          .request_bottom_panel_command_redraw =
              [this]() { RequestBottomPanelCommandRedraw(); },
          .request_sidebar_redraw = [this]() { RequestSidebarRedraw(); },
          .active_sidebar_mode = [this]() { return ActiveSidebarMode(); },
          .request_overlay_redraw = [this]() { RequestOverlayRedraw(); },
          .request_focused_editor_redraw = [this]() { RequestFocusedEditorRedraw(); },
          .request_window_redraw = [this]() { RequestWindowRedraw(); },
          .command_prompt_append_input =
              [this](std::string_view input) {
                MakeCommandPromptCoordinator().AppendInput(input);
              },
          .refresh_compare_picker = [this]() { RefreshComparePicker(); },
          .refresh_buffer_search = [this]() { RefreshBufferSearch(); },
          .refresh_project_search = [this]() { RefreshProjectSearch(); },
          .reset_overlay_scroll = [this]() { ResetOverlayScroll(); },
          .active_editable_viewport = [this]() { return ActiveEditableViewport(); },
          .active_compare_tab = [this]() { return ActiveCompareTab(); },
          .refresh_compare_tab_derived_state =
              [this](CompareTabState& compare_tab) { RefreshCompareTabDerivedState(compare_tab); },
          .sync_compare_selection_from_viewport =
              [this](CompareTabState& compare_tab, bool reveal_selection) {
                SyncCompareSelectionFromViewport(compare_tab, reveal_selection);
              },
          .active_merge_tab = [this]() { return ActiveMergeTab(); },
          .update_merge_tracking_after_viewport_edit =
              [this](MergeTabState& merge_tab,
                     const std::vector<std::string>& before_lines,
                     std::optional<editor::SelectionRange> selection_before,
                     editor::TextPosition cursor_before) {
                UpdateMergeTrackingAfterViewportEdit(merge_tab, before_lines, selection_before,
                                                     cursor_before);
              },
          .reset_caret_blink = [this]() { ResetCaretBlink(); },
          .mark_active_editor_folding_dirty =
              [this]() {
                if (auto* editor_tab = ActiveEditorTab(); editor_tab != nullptr) {
                  editor_tab->folding_model->MarkDirty();
                }
              },
          .request_active_editable_last_change_redraw =
              [this]() { RequestActiveEditableLastChangeRedraw(); },
          .request_active_editable_change_redraw =
              [this](const std::vector<std::string>& before_lines,
                     const std::vector<std::string>& after_lines) {
                RequestActiveEditableChangeRedraw(before_lines, after_lines);
              },
          .request_active_editable_blame_neighborhood_redraw =
              [this](std::size_t first_line, std::size_t last_line) {
                RequestActiveEditableBlameNeighborhoodRedraw(first_line, last_line);
              },
          .request_tab_strip_redraw = [this]() { RequestTabStripRedraw(); },
          .active_terminal_tab = [this]() { return ActiveTerminalTab(); },
          .clear_terminal_selection = [this]() { ClearTerminalSelection(); },
          .append_terminal_pending_input =
              [this](std::string_view input) { AppendTerminalPendingInput(input); },
          .request_bottom_panel_content_redraw =
              [this]() { RequestBottomPanelContentRedraw(); },
          .terminal_has_selection = [this]() { return TerminalHasSelection(); },
          .selected_terminal_text = [this]() { return SelectedTerminalText(); },
          .write_clipboard_text =
              [this](std::string_view text) { return WriteClipboardText(text); },
          .write_primary_selection_text =
              [this](std::string_view text) { return WritePrimarySelectionText(text); },
          .read_clipboard_text = [this]() { return ReadClipboardText(); },
          .submit_terminal_pending_input = [this]() { SubmitTerminalPendingInput(); },
          .erase_last_terminal_pending_input_codepoint =
              [this]() { EraseLastTerminalPendingInputCodepoint(); },
          .read_primary_selection_text = [this]() { return ReadPrimarySelectionText(); },
          .keycode_to_ascii =
              [](SDL_Keycode key, SDL_Keymod modifiers) {
                return WorkspaceShell::KeycodeToAscii(key, modifiers);
              },
          .try_editor_snippet_insert_text =
              [this](editor::TextViewport* viewport, std::string_view text) {
                return assist_service_.TrySnippetInsertTextInEditor(viewport, text);
              },
      });
}


}  // namespace microide::workspace
