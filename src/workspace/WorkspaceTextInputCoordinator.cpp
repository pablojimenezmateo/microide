#include "workspace/WorkspaceTextInputCoordinator.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspaceCommandPromptCoordinator.h"

namespace microide::workspace {

WorkspaceShell::TextInputCoordinator::TextInputCoordinator(WorkspaceShell& shell) : shell_(shell) {}

void WorkspaceShell::TextInputCoordinator::SyncTextInputSurface(SDL_Window* window) {
  const TextInputSurface current_surface = shell_.CurrentTextInputSurface();
  if (current_surface == shell_.active_text_input_surface_) {
    return;
  }

  shell_.active_text_input_surface_ = current_surface;
  shell_.text_composition_ = TextCompositionState{};
  SDL_Window* target_window = window != nullptr ? window : SDL_GetKeyboardFocus();
  if (target_window != nullptr) {
    SDL_ClearComposition(target_window);
  }
}

bool WorkspaceShell::TextInputCoordinator::CompositionConsumesKey(SDL_Keycode key,
                                                                  SDL_Keymod modifiers) const {
  if (shell_.text_composition_.text.empty() ||
      shell_.text_composition_.surface != shell_.CurrentTextInputSurface() ||
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

void WorkspaceShell::TextInputCoordinator::RequestCompositionRedraw(TextInputSurface surface) {
  switch (surface) {
    case TextInputSurface::PromptInput:
      shell_.RequestPromptRedraw();
      break;
    case TextInputSurface::Command:
      shell_.RequestBottomPanelCommandRedraw();
      break;
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace:
      shell_.RequestSidebarRedraw();
      break;
    case TextInputSurface::FileFinder:
    case TextInputSurface::BufferSearch:
    case TextInputSurface::BufferReplaceSearch:
    case TextInputSurface::BufferReplaceReplace:
    case TextInputSurface::ProjectSearchOverlay:
    case TextInputSurface::CommitPicker:
      shell_.RequestOverlayRedraw();
      break;
    case TextInputSurface::Editor:
      shell_.RequestFocusedEditorRedraw();
      break;
    case TextInputSurface::None:
    case TextInputSurface::Terminal:
      break;
  }
}

bool WorkspaceShell::TextInputCoordinator::HandleTextEditing(const SDL_TextEditingEvent& event) {
  if (shell_.surface_.menu_bar_open || shell_.surface_.tree_context_menu.open) {
    if (!shell_.text_composition_.text.empty()) {
      shell_.RequestWindowRedraw();
    }
    shell_.text_composition_ = TextCompositionState{};
    return true;
  }

  SyncTextInputSurface(nullptr);
  const TextInputSurface surface = shell_.CurrentTextInputSurface();
  if (surface == TextInputSurface::None || surface == TextInputSurface::Terminal) {
    if (!shell_.text_composition_.text.empty()) {
      shell_.RequestWindowRedraw();
    }
    shell_.text_composition_ = TextCompositionState{};
    return false;
  }

  if (event.text == nullptr || event.text[0] == '\0') {
    if (!shell_.text_composition_.text.empty()) {
      RequestCompositionRedraw(surface);
    }
    shell_.text_composition_ = TextCompositionState{};
    return true;
  }

  shell_.text_composition_.surface = surface;
  shell_.text_composition_.text = event.text;
  shell_.text_composition_.start = event.start;
  shell_.text_composition_.length = event.length;
  RequestCompositionRedraw(surface);
  return true;
}

bool WorkspaceShell::TextInputCoordinator::HandleTextInput(const SDL_TextInputEvent& event) {
  if (shell_.surface_.menu_bar_open || shell_.surface_.tree_context_menu.open) {
    return true;
  }
  if (event.text == nullptr || event.text[0] == '\0' || shell_.prompts_.dirty_visible) {
    return false;
  }

  SyncTextInputSurface(nullptr);
  shell_.text_composition_ = TextCompositionState{};
  const std::string_view input(event.text);
  if (shell_.prompts_.surface_visible &&
      shell_.prompts_.surface.kind == PromptSurfaceState::Kind::TextInput) {
    shell_.prompts_.surface.input.append(input);
    shell_.RequestPromptRedraw();
    return true;
  }
  if (shell_.surface_.command_mode) {
    CommandPromptCoordinator(shell_).AppendInput(input);
    shell_.RequestBottomPanelCommandRedraw();
    return true;
  }

  if (shell_.surface_.overlay_visible) {
    switch (shell_.surface_.overlay_mode) {
      case OverlayMode::CommitPicker:
        shell_.overlay_workflow_.compare_picker.query.append(input);
        shell_.RefreshComparePicker();
        return true;
      case OverlayMode::BufferSearch:
        shell_.overlay_workflow_.buffer_search.query.append(input);
        shell_.RefreshBufferSearch();
        return true;
      case OverlayMode::BufferReplace:
        if (shell_.surface_.buffer_search_field == BufferSearchField::Search) {
          shell_.overlay_workflow_.buffer_search.query.append(input);
          shell_.RefreshBufferSearch();
        } else {
          shell_.overlay_workflow_.buffer_search.replace_text.append(input);
          shell_.RequestOverlayRedraw();
        }
        return true;
      case OverlayMode::ProjectSearch:
        shell_.overlay_workflow_.project_search.query.append(input);
        shell_.RefreshProjectSearch();
        return true;
      case OverlayMode::FileFinder:
      default:
        shell_.file_finder_.AppendQueryText(input);
        shell_.ResetOverlayScroll();
        return true;
    }
  }

  if (shell_.surface_.focus == FocusTarget::Sidebar && shell_.surface_.sidebar_visible &&
      shell_.surface_.sidebar_mode == SidebarMode::Search &&
      shell_.overlay_workflow_.project_search.editing) {
    shell_.overlay_workflow_.project_search.edit_buffer.append(input);
    shell_.RequestSidebarRedraw();
    return true;
  }

  if (shell_.surface_.focus == FocusTarget::Editor && shell_.ActiveEditableViewport() != nullptr) {
    editor::TextViewport* viewport = shell_.ActiveEditableViewport();
    if (viewport == nullptr) {
      return false;
    }
    const bool was_dirty = viewport->dirty();
    const std::vector<std::string> before_lines = viewport->lines();
    const std::optional<editor::SelectionRange> selection_before = viewport->selection_range();
    const editor::TextPosition cursor_before{viewport->cursor_line(), viewport->cursor_column()};
    viewport->InsertText(input);
    if (auto* compare_tab = shell_.ActiveCompareTab();
        compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
      shell_.RefreshCompareTabDerivedState(*compare_tab);
      shell_.SyncCompareSelectionFromViewport(*compare_tab, true);
    }
    if (auto* merge_tab = shell_.ActiveMergeTab();
        merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
      shell_.UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before,
                                                  cursor_before);
    }
    shell_.ResetCaretBlink();
    shell_.RequestActiveEditableChangeRedraw(before_lines, viewport->lines());
    if (viewport->dirty() != was_dirty) {
      shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before.line,
                                                          viewport->cursor_line());
      shell_.RequestTabStripRedraw();
    }
    return true;
  }

  if (shell_.surface_.focus == FocusTarget::Panel && shell_.ActiveTerminalTab() != nullptr) {
    shell_.ClearTerminalSelection();
    if (auto* terminal_tab = shell_.ActiveTerminalTab(); terminal_tab != nullptr) {
      terminal_tab->follow_tail = true;
      shell_.AppendTerminalPendingInput(input);
      terminal_tab->session.SendBytes(input);
    }
    shell_.RequestBottomPanelContentRedraw();
    return true;
  }

  return false;
}

bool WorkspaceShell::TextInputCoordinator::HandleTerminalKeyDown(const SDL_KeyboardEvent& event,
                                                                 SDL_Keymod modifiers) {
  auto* terminal_tab = shell_.ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return false;
  }
  const auto follow_terminal_tail = [&]() { terminal_tab->follow_tail = true; };
  const auto handled_with_panel_redraw = [this]() {
    shell_.RequestBottomPanelContentRedraw();
    return true;
  };

  if ((modifiers & SDL_KMOD_CTRL) && event.key == SDLK_C && shell_.TerminalHasSelection()) {
    const std::string text = shell_.SelectedTerminalText();
    if (!text.empty() && shell_.WriteClipboardText(text)) {
      shell_.WritePrimarySelectionText(text);
    }
    return true;
  }

  if (event.key == SDLK_ESCAPE && shell_.TerminalHasSelection()) {
    shell_.ClearTerminalSelection();
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
    const char input_character = KeycodeToAscii(event.key, modifiers);
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
      shell_.SubmitTerminalPendingInput();
      follow_terminal_tail();
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Enter);
      return handled_with_panel_redraw();
    case SDLK_BACKSPACE:
      shell_.EraseLastTerminalPendingInputCodepoint();
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

bool WorkspaceShell::TextInputCoordinator::PasteClipboardIntoTerminal() {
  auto* terminal_tab = shell_.ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return false;
  }

  const std::optional<std::string> clipboard_text = shell_.ReadClipboardText();
  if (!clipboard_text.has_value()) {
    return true;
  }

  shell_.ClearTerminalSelection();
  if (clipboard_text->find_first_of("\r\n") == std::string::npos) {
    shell_.AppendTerminalPendingInput(*clipboard_text);
  }
  terminal_tab->follow_tail = true;
  terminal_tab->session.PasteText(*clipboard_text);
  shell_.RequestBottomPanelContentRedraw();
  return true;
}

}  // namespace microide::workspace
