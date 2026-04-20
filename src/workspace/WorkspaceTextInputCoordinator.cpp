#include "workspace/WorkspaceTextInputCoordinator.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspaceCommandPromptCoordinator.h"
#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

TextInputCoordinator::TextInputCoordinator(ProjectWorkspaceState& state,
                                           PromptState& prompts,
                                           MenuSurfaceState& menu_state,
                                           TextInputState& text_input_state,
                                           Operations operations)
    : state_(state),
      prompts_(prompts),
      menu_state_(menu_state),
      text_input_state_(text_input_state),
      operations_(std::move(operations)) {}

void TextInputCoordinator::SyncTextInputSurface(SDL_Window* window) {
  const TextInputSurface current_surface = operations_.current_text_input_surface();
  if (current_surface == text_input_state_.active_surface) {
    return;
  }

  text_input_state_.active_surface = current_surface;
  text_input_state_.composition = TextCompositionState{};
  SDL_Window* target_window = window != nullptr ? window : SDL_GetKeyboardFocus();
  if (target_window != nullptr) {
    SDL_ClearComposition(target_window);
  }
}

bool TextInputCoordinator::CompositionConsumesKey(SDL_Keycode key, SDL_Keymod modifiers) const {
  if (text_input_state_.composition.text.empty() ||
      text_input_state_.composition.surface != operations_.current_text_input_surface() ||
      (modifiers & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0) {
    return false;
  }

  switch (key) {
    case SDLK_BACKSPACE:
    case SDLK_DELETE:
    case SDLK_ESCAPE:
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_TAB:
    case SDLK_UP:
    case SDLK_DOWN:
    case SDLK_LEFT:
    case SDLK_RIGHT:
    case SDLK_HOME:
    case SDLK_END:
    case SDLK_PAGEUP:
    case SDLK_PAGEDOWN:
      return true;
    default:
      return false;
  }
}

void TextInputCoordinator::RequestCompositionRedraw(TextInputSurface surface) {
  switch (surface) {
    case TextInputSurface::PromptInput:
      operations_.request_prompt_redraw();
      break;
    case TextInputSurface::Command:
      operations_.request_bottom_panel_command_redraw();
      break;
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace:
      operations_.request_sidebar_redraw();
      break;
    case TextInputSurface::FileFinder:
    case TextInputSurface::BufferSearch:
    case TextInputSurface::BufferReplaceSearch:
    case TextInputSurface::BufferReplaceReplace:
    case TextInputSurface::ProjectSearchOverlay:
    case TextInputSurface::CommitPicker:
      operations_.request_overlay_redraw();
      break;
    case TextInputSurface::Editor:
      operations_.request_focused_editor_redraw();
      break;
    case TextInputSurface::None:
    case TextInputSurface::Terminal:
      break;
  }
}

bool TextInputCoordinator::HandleTextEditing(const SDL_TextEditingEvent& event) {
  if (menu_state_.menu_bar_open || menu_state_.tree_context_menu.open) {
    if (!text_input_state_.composition.text.empty()) {
      operations_.request_window_redraw();
    }
    text_input_state_.composition = TextCompositionState{};
    return true;
  }

  SyncTextInputSurface(nullptr);
  const TextInputSurface surface = operations_.current_text_input_surface();
  if (surface == TextInputSurface::None || surface == TextInputSurface::Terminal) {
    if (!text_input_state_.composition.text.empty()) {
      operations_.request_window_redraw();
    }
    text_input_state_.composition = TextCompositionState{};
    return false;
  }

  if (event.text == nullptr || event.text[0] == '\0') {
    if (!text_input_state_.composition.text.empty()) {
      RequestCompositionRedraw(surface);
    }
    text_input_state_.composition = TextCompositionState{};
    return true;
  }

  text_input_state_.composition.surface = surface;
  text_input_state_.composition.text = event.text;
  text_input_state_.composition.start = event.start;
  text_input_state_.composition.length = event.length;
  RequestCompositionRedraw(surface);
  return true;
}

bool TextInputCoordinator::HandleTextInput(const SDL_TextInputEvent& event) {
  if (menu_state_.menu_bar_open || menu_state_.tree_context_menu.open) {
    return true;
  }
  if (event.text == nullptr || event.text[0] == '\0' || prompts_.dirty_visible) {
    return false;
  }

  SyncTextInputSurface(nullptr);
  text_input_state_.composition = TextCompositionState{};
  const std::string_view input(event.text);
  if (prompts_.surface_visible && prompts_.surface.kind == PromptSurfaceState::Kind::TextInput) {
    prompts_.surface.input.append(input);
    operations_.request_prompt_redraw();
    return true;
  }
  if (state_.panel.command_mode) {
    operations_.command_prompt_append_input(input);
    operations_.request_bottom_panel_command_redraw();
    return true;
  }

  if (state_.overlay.visible) {
    switch (state_.overlay.mode) {
      case OverlayMode::CommitPicker:
        state_.overlay.workflow.compare_picker.query.append(input);
        operations_.refresh_compare_picker();
        return true;
      case OverlayMode::BufferSearch:
        state_.overlay.workflow.buffer_search.query.append(input);
        operations_.refresh_buffer_search();
        return true;
      case OverlayMode::BufferReplace:
        if (state_.overlay.buffer_search_field == BufferSearchField::Search) {
          state_.overlay.workflow.buffer_search.query.append(input);
          operations_.refresh_buffer_search();
        } else {
          state_.overlay.workflow.buffer_search.replace_text.append(input);
          operations_.request_overlay_redraw();
        }
        return true;
      case OverlayMode::ProjectSearch:
        state_.overlay.workflow.project_search.query.append(input);
        operations_.refresh_project_search();
        return true;
      case OverlayMode::FileFinder:
      default:
        state_.file_finder.AppendQueryText(input);
        operations_.reset_overlay_scroll();
        return true;
    }
  }

  if (state_.surface.focus == FocusTarget::Sidebar && state_.sidebar.visible &&
      (operations_.current_text_input_surface() == TextInputSurface::SidebarSearchQuery ||
       operations_.current_text_input_surface() == TextInputSurface::SidebarSearchReplace)) {
    state_.overlay.workflow.project_search.edit_buffer.append(input);
    operations_.request_sidebar_redraw();
    return true;
  }

  if (state_.surface.focus == FocusTarget::Editor &&
      operations_.active_editable_viewport() != nullptr) {
    editor::TextViewport* viewport = operations_.active_editable_viewport();
    if (viewport == nullptr) {
      return false;
    }
    const bool was_dirty = viewport->dirty();
    const std::vector<std::string> before_lines = viewport->lines();
    const std::optional<editor::SelectionRange> selection_before = viewport->selection_range();
    const editor::TextPosition cursor_before{viewport->cursor_line(), viewport->cursor_column()};
    viewport->InsertText(input);
    if (auto* compare_tab = operations_.active_compare_tab();
        compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
      operations_.refresh_compare_tab_derived_state(*compare_tab);
      operations_.sync_compare_selection_from_viewport(*compare_tab, true);
    }
    if (auto* merge_tab = operations_.active_merge_tab();
        merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
      operations_.update_merge_tracking_after_viewport_edit(*merge_tab, before_lines,
                                                            selection_before, cursor_before);
    }
    operations_.reset_caret_blink();
    operations_.request_active_editable_change_redraw(before_lines, viewport->lines());
    if (viewport->dirty() != was_dirty) {
      operations_.request_active_editable_blame_neighborhood_redraw(cursor_before.line,
                                                                    viewport->cursor_line());
      operations_.request_tab_strip_redraw();
    }
    return true;
  }

  if (state_.surface.focus == FocusTarget::Panel && operations_.active_terminal_tab() != nullptr) {
    operations_.clear_terminal_selection();
    if (auto* terminal_tab = operations_.active_terminal_tab(); terminal_tab != nullptr) {
      terminal_tab->follow_tail = true;
      operations_.append_terminal_pending_input(input);
      terminal_tab->session.SendBytes(input);
    }
    operations_.request_bottom_panel_content_redraw();
    return true;
  }

  return false;
}

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
      current_project_state_, prompts_, menu_state_, text_input_state_,
      TextInputCoordinator::Operations{
          .current_text_input_surface = [this]() { return CurrentTextInputSurface(); },
          .request_prompt_redraw = [this]() { RequestPromptRedraw(); },
          .request_bottom_panel_command_redraw =
              [this]() { RequestBottomPanelCommandRedraw(); },
          .request_sidebar_redraw = [this]() { RequestSidebarRedraw(); },
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
