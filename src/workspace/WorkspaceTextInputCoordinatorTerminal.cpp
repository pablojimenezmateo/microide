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

  if (modifiers & SDL_KMOD_CTRL) {
    if (event.key >= SDLK_A && event.key <= SDLK_Z) {
      const char control = static_cast<char>(1 + (event.key - SDLK_A));
      follow_terminal_tail();
      terminal_tab->session.SendBytes(std::string(1, control));
      return handled_with_panel_redraw();
    }
    switch (event.key) {
      case SDLK_LEFTBRACKET:
        follow_terminal_tail();
        terminal_tab->session.SendBytes("\x1b");
        return handled_with_panel_redraw();
      case SDLK_BACKSLASH:
        follow_terminal_tail();
        terminal_tab->session.SendBytes("\x1c");
        return handled_with_panel_redraw();
      case SDLK_RIGHTBRACKET:
        follow_terminal_tail();
        terminal_tab->session.SendBytes("\x1d");
        return handled_with_panel_redraw();
      case SDLK_SPACE:
        follow_terminal_tail();
        terminal_tab->session.SendBytes(std::string(1, '\0'));
        return handled_with_panel_redraw();
      default:
        break;
    }
  }

  if (modifiers & SDL_KMOD_ALT) {
    const char input_character = operations_.keycode_to_ascii(event.key, modifiers);
    if (input_character != '\0') {
      std::string bytes(1, '\x1b');
      bytes.push_back(input_character);
      follow_terminal_tail();
      terminal_tab->session.SendBytes(bytes);
      return handled_with_panel_redraw();
    }
  }

  switch (event.key) {
    case SDLK_ESCAPE:
      follow_terminal_tail();
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Escape);
      return handled_with_panel_redraw();
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      operations_.submit_terminal_pending_input();
      follow_terminal_tail();
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Enter);
      return handled_with_panel_redraw();
    case SDLK_BACKSPACE:
      operations_.erase_last_terminal_pending_input_codepoint();
      follow_terminal_tail();
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Backspace);
      return handled_with_panel_redraw();
    case SDLK_TAB:
      follow_terminal_tail();
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Tab);
      return handled_with_panel_redraw();
    case SDLK_UP:
      follow_terminal_tail();
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Up);
      return handled_with_panel_redraw();
    case SDLK_DOWN:
      follow_terminal_tail();
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Down);
      return handled_with_panel_redraw();
    case SDLK_RIGHT:
      follow_terminal_tail();
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Right);
      return handled_with_panel_redraw();
    case SDLK_LEFT:
      follow_terminal_tail();
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Left);
      return handled_with_panel_redraw();
    case SDLK_HOME:
      follow_terminal_tail();
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Home);
      return handled_with_panel_redraw();
    case SDLK_END:
      follow_terminal_tail();
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::End);
      return handled_with_panel_redraw();
    case SDLK_PAGEUP:
      follow_terminal_tail();
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::PageUp);
      return handled_with_panel_redraw();
    case SDLK_PAGEDOWN:
      follow_terminal_tail();
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::PageDown);
      return handled_with_panel_redraw();
    case SDLK_INSERT:
      follow_terminal_tail();
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Insert);
      return handled_with_panel_redraw();
    case SDLK_DELETE:
      follow_terminal_tail();
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Delete);
      return handled_with_panel_redraw();
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
      });
}


}  // namespace microide::workspace
