#include "workspace/WorkspaceTextInputCoordinator.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util/SingleLineText.h"
#include "workspace/WorkspaceCommandPromptCoordinator.h"
#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

namespace {

void SyncChatDraft(ProjectWorkspaceState& state) {
  if (Conversation* conversation = state.conversations.GetConversation(state.panel.chat.conversation_id);
      conversation != nullptr) {
    conversation->draft = util::SerializeLines(state.panel.chat.composer.lines(), util::LineEnding::LF);
  }
}

}  // namespace

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
    case TextInputSurface::ChatComposer:
      if (operations_.active_sidebar_mode() == SidebarMode::Chat &&
          state_.surface.focus == FocusTarget::Sidebar && state_.sidebar.visible) {
        operations_.request_sidebar_redraw();
      } else {
        operations_.request_bottom_panel_command_redraw();
      }
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

util::SingleLineTextState* TextInputCoordinator::ActiveSingleLineTextState() {
  switch (operations_.current_text_input_surface()) {
    case TextInputSurface::PromptInput:
      return &prompts_.surface.input;
    case TextInputSurface::Command:
      return &state_.panel.command.input;
    case TextInputSurface::ChatComposer:
      return nullptr;
    case TextInputSurface::CommitPicker:
      return &state_.overlay.workflow.compare_picker.query;
    case TextInputSurface::BufferSearch:
    case TextInputSurface::BufferReplaceSearch:
      return &state_.overlay.workflow.buffer_search.query;
    case TextInputSurface::BufferReplaceReplace:
      return &state_.overlay.workflow.buffer_search.replace_text;
    case TextInputSurface::ProjectSearchOverlay:
      return &state_.overlay.workflow.project_search.query;
    case TextInputSurface::FileFinder:
      return &state_.file_finder.query_state();
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace:
      return &state_.overlay.workflow.project_search.edit_buffer;
    case TextInputSurface::None:
    case TextInputSurface::Editor:
    case TextInputSurface::Terminal:
      return nullptr;
  }
  return nullptr;
}

const util::SingleLineTextState* TextInputCoordinator::ActiveSingleLineTextState() const {
  return const_cast<TextInputCoordinator*>(this)->ActiveSingleLineTextState();
}

void TextInputCoordinator::DidMutateCommandInputText() {
  state_.panel.command.history_index.reset();
  state_.panel.command.history_pending_input.clear();
  state_.panel.command.feedback_text.clear();
}

void TextInputCoordinator::RequestSingleLineTextRedraw(TextInputSurface surface,
                                                       bool text_changed) {
  switch (surface) {
    case TextInputSurface::PromptInput:
      operations_.request_prompt_redraw();
      break;
    case TextInputSurface::Command:
      if (text_changed) {
        DidMutateCommandInputText();
      }
      operations_.request_bottom_panel_command_redraw();
      break;
    case TextInputSurface::ChatComposer:
      if (operations_.active_sidebar_mode() == SidebarMode::Chat &&
          state_.surface.focus == FocusTarget::Sidebar && state_.sidebar.visible) {
        operations_.request_sidebar_redraw();
      } else {
        operations_.request_bottom_panel_command_redraw();
      }
      break;
    case TextInputSurface::CommitPicker:
      if (text_changed) {
        operations_.refresh_compare_picker();
      } else {
        operations_.request_overlay_redraw();
      }
      break;
    case TextInputSurface::BufferSearch:
    case TextInputSurface::BufferReplaceSearch:
      if (text_changed) {
        operations_.refresh_buffer_search();
      } else {
        operations_.request_overlay_redraw();
      }
      break;
    case TextInputSurface::BufferReplaceReplace:
      operations_.request_overlay_redraw();
      break;
    case TextInputSurface::ProjectSearchOverlay:
      if (text_changed) {
        operations_.refresh_project_search();
      } else {
        operations_.request_overlay_redraw();
      }
      break;
    case TextInputSurface::FileFinder:
      if (text_changed) {
        state_.file_finder.Refresh();
        operations_.reset_overlay_scroll();
      } else {
        operations_.request_overlay_redraw();
      }
      break;
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace:
      operations_.request_sidebar_redraw();
      break;
    case TextInputSurface::None:
    case TextInputSurface::Editor:
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
  return InsertTextAtActiveSurface(event.text);
}

bool TextInputCoordinator::InsertTextAtActiveSurface(std::string_view input) {
  if (input.empty()) {
    return false;
  }

  SyncTextInputSurface(nullptr);
  text_input_state_.composition = TextCompositionState{};
  const TextInputSurface surface = operations_.current_text_input_surface();
  if (surface == TextInputSurface::ChatComposer) {
    state_.panel.chat.composer.InsertText(input);
    SyncChatDraft(state_);
    RequestSingleLineTextRedraw(surface, true);
    return true;
  }
  if (util::SingleLineTextState* text_state = ActiveSingleLineTextState();
      text_state != nullptr) {
    if (!util::InsertSingleLineText(text_state, input)) {
      return false;
    }
    RequestSingleLineTextRedraw(surface, true);
    return true;
  }
  switch (surface) {
    case TextInputSurface::Command:
    case TextInputSurface::PromptInput:
    case TextInputSurface::FileFinder:
    case TextInputSurface::BufferSearch:
    case TextInputSurface::BufferReplaceSearch:
    case TextInputSurface::BufferReplaceReplace:
    case TextInputSurface::ProjectSearchOverlay:
    case TextInputSurface::CommitPicker:
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace:
    case TextInputSurface::ChatComposer:
      return false;
    case TextInputSurface::Editor:
      if (operations_.active_editable_viewport() == nullptr) {
        return false;
      }
      {
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
    case TextInputSurface::Terminal:
      if (auto* terminal_tab = operations_.active_terminal_tab(); terminal_tab != nullptr) {
        operations_.clear_terminal_selection();
        terminal_tab->follow_tail = true;
        operations_.append_terminal_pending_input(input);
        terminal_tab->session.SendBytes(input);
        operations_.request_bottom_panel_content_redraw();
        return true;
      }
      return false;
    case TextInputSurface::None:
      return false;
  }
  return false;
}

bool TextInputCoordinator::HandleSingleLineKeyDown(const SDL_KeyboardEvent& event,
                                                   SDL_Keymod modifiers) {
  if (operations_.current_text_input_surface() == TextInputSurface::ChatComposer) {
    editor::TextViewport& composer = state_.panel.chat.composer;
    const bool extend_selection = (modifiers & SDL_KMOD_SHIFT) != 0;
    bool handled = false;
    if ((modifiers & SDL_KMOD_CTRL) != 0) {
      switch (event.key) {
        case SDLK_A:
          composer.SelectAll();
          handled = true;
          break;
        case SDLK_LEFT:
        case SDLK_HOME:
          composer.MoveCursorLineStart(extend_selection);
          handled = true;
          break;
        case SDLK_RIGHT:
        case SDLK_END:
          composer.MoveCursorLineEnd(extend_selection);
          handled = true;
          break;
        default:
          break;
      }
    } else {
      switch (event.key) {
        case SDLK_BACKSPACE:
          composer.Backspace();
          handled = true;
          break;
        case SDLK_DELETE:
          composer.DeleteForward();
          handled = true;
          break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
          composer.InsertNewline();
          handled = true;
          break;
        case SDLK_LEFT:
          composer.MoveCursorHorizontal(-1, extend_selection);
          handled = true;
          break;
        case SDLK_RIGHT:
          composer.MoveCursorHorizontal(1, extend_selection);
          handled = true;
          break;
        case SDLK_UP:
          composer.MoveCursorVertical(-1, extend_selection);
          handled = true;
          break;
        case SDLK_DOWN:
          composer.MoveCursorVertical(1, extend_selection);
          handled = true;
          break;
        case SDLK_HOME:
          composer.MoveCursorLineStart(extend_selection);
          handled = true;
          break;
        case SDLK_END:
          composer.MoveCursorLineEnd(extend_selection);
          handled = true;
          break;
        case SDLK_PAGEUP:
          composer.Page(-1);
          handled = true;
          break;
        case SDLK_PAGEDOWN:
          composer.Page(1);
          handled = true;
          break;
        default:
          break;
      }
    }
    if (!handled) {
      return false;
    }
    SyncChatDraft(state_);
    RequestSingleLineTextRedraw(TextInputSurface::ChatComposer, true);
    if (composer.has_selection()) {
      operations_.write_primary_selection_text(composer.SelectedText());
    }
    return true;
  }

  util::SingleLineTextState* text_state = ActiveSingleLineTextState();
  if (text_state == nullptr) {
    return false;
  }

  const TextInputSurface surface = operations_.current_text_input_surface();
  const bool extend_selection = (modifiers & SDL_KMOD_SHIFT) != 0;
  bool handled = false;
  bool text_changed = false;

  if ((modifiers & SDL_KMOD_CTRL) != 0) {
    switch (event.key) {
      case SDLK_A:
        handled = util::SelectAllSingleLineText(text_state);
        break;
      case SDLK_LEFT:
      case SDLK_HOME:
        handled = util::MoveSingleLineCursorHome(text_state, extend_selection);
        break;
      case SDLK_RIGHT:
      case SDLK_END:
        handled = util::MoveSingleLineCursorEnd(text_state, extend_selection);
        break;
      default:
        return false;
    }
  } else {
    switch (event.key) {
      case SDLK_BACKSPACE:
        handled = util::BackspaceSingleLineText(text_state);
        text_changed = handled;
        break;
      case SDLK_DELETE:
        handled = util::DeleteForwardSingleLineText(text_state);
        text_changed = handled;
        break;
      case SDLK_LEFT:
        handled = util::MoveSingleLineCursorLeft(text_state, extend_selection);
        break;
      case SDLK_RIGHT:
        handled = util::MoveSingleLineCursorRight(text_state, extend_selection);
        break;
      case SDLK_HOME:
        handled = util::MoveSingleLineCursorHome(text_state, extend_selection);
        break;
      case SDLK_END:
        handled = util::MoveSingleLineCursorEnd(text_state, extend_selection);
        break;
      default:
        return false;
    }
  }

  if (!handled) {
    return false;
  }
  RequestSingleLineTextRedraw(surface, text_changed);
  if (util::HasSingleLineSelection(*text_state)) {
    operations_.write_primary_selection_text(util::SelectedSingleLineText(*text_state));
  }
  return true;
}

bool TextInputCoordinator::HasSelectionAtActiveSingleLineSurface() const {
  if (operations_.current_text_input_surface() == TextInputSurface::ChatComposer) {
    return state_.panel.chat.composer.has_selection();
  }
  const util::SingleLineTextState* text_state = ActiveSingleLineTextState();
  return text_state != nullptr && util::HasSingleLineSelection(*text_state);
}

std::string TextInputCoordinator::SelectedTextAtActiveSingleLineSurface() const {
  if (operations_.current_text_input_surface() == TextInputSurface::ChatComposer) {
    return state_.panel.chat.composer.SelectedText();
  }
  const util::SingleLineTextState* text_state = ActiveSingleLineTextState();
  return text_state != nullptr ? util::SelectedSingleLineText(*text_state) : std::string{};
}

bool TextInputCoordinator::SelectAllAtActiveSingleLineSurface() {
  if (operations_.current_text_input_surface() == TextInputSurface::ChatComposer) {
    state_.panel.chat.composer.SelectAll();
    RequestSingleLineTextRedraw(TextInputSurface::ChatComposer, false);
    return true;
  }
  util::SingleLineTextState* text_state = ActiveSingleLineTextState();
  if (text_state == nullptr) {
    return false;
  }
  if (!util::SelectAllSingleLineText(text_state)) {
    RequestSingleLineTextRedraw(operations_.current_text_input_surface(), false);
    return false;
  }
  RequestSingleLineTextRedraw(operations_.current_text_input_surface(), false);
  return true;
}

bool TextInputCoordinator::CutSelectionAtActiveSingleLineSurface() {
  if (operations_.current_text_input_surface() == TextInputSurface::ChatComposer) {
    editor::TextViewport& composer = state_.panel.chat.composer;
    const std::string selected = composer.SelectedText();
    if (selected.empty() || !operations_.write_clipboard_text(selected)) {
      return false;
    }
    operations_.write_primary_selection_text(selected);
    if (!composer.DeleteSelectedText()) {
      return false;
    }
    SyncChatDraft(state_);
    RequestSingleLineTextRedraw(TextInputSurface::ChatComposer, true);
    return true;
  }
  util::SingleLineTextState* text_state = ActiveSingleLineTextState();
  if (text_state == nullptr) {
    return false;
  }
  const std::string selected = util::SelectedSingleLineText(*text_state);
  if (selected.empty() || !operations_.write_clipboard_text(selected)) {
    return false;
  }
  operations_.write_primary_selection_text(selected);
  if (!util::DeleteSelectedSingleLineText(text_state)) {
    return false;
  }
  RequestSingleLineTextRedraw(operations_.current_text_input_surface(), true);
  return true;
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
