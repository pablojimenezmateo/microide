#include "workspace/WorkspaceShell.h"

#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

struct ScopeExit {
  std::function<void()> on_exit;

  ~ScopeExit() {
    if (on_exit) {
      on_exit();
    }
  }
};

}  // namespace

bool WorkspaceShell::HandleEvent(const SDL_Event& event) {
  const ScopeExit sync_terminal_focus{[this]() { SyncTerminalFocusState(); }};

  if (project_open_dialog_event_type_ != 0 && event.type == project_open_dialog_event_type_) {
    ConsumePendingProjectOpenDialogResult();
    return true;
  }
  if (project_search_runtime_.HandlesEvent(event.type)) {
    ConsumeProjectSearchUpdates();
    return true;
  }
  if (git_blame_event_type_ != 0 && event.type == git_blame_event_type_) {
    return true;
  }
  if (terminal_event_type_ != 0 && event.type == terminal_event_type_) {
    ConsumeTerminalSessionUpdates();
    return true;
  }

  SyncTextInputSurface(nullptr);

  switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      return HandleMouseButtonDown(event);
    case SDL_EVENT_MOUSE_BUTTON_UP:
      return HandleMouseButtonUp(event);
    case SDL_EVENT_MOUSE_MOTION:
      return HandleMouseMotion(event);
    case SDL_EVENT_MOUSE_WHEEL:
      return HandleMouseWheel(event);
    case SDL_EVENT_TEXT_EDITING:
      return HandleTextEditing(event.edit);
    case SDL_EVENT_TEXT_INPUT:
      return HandleTextInput(event.text);
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      surface_.window_has_input_focus = true;
      return true;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      surface_.window_has_input_focus = false;
      return true;
    case SDL_EVENT_KEY_DOWN:
      break;
    default:
      return false;
  }

  const SDL_Keymod modifiers =
      event.key.mod != SDL_KMOD_NONE ? event.key.mod : SDL_GetModState();
  if (prompts_.dirty_visible) {
    return HandleDirtyPromptKeyDown(event.key, modifiers);
  }
  if (surface_.tree_context_menu.open) {
    return HandleTreeContextMenuKeyDown(event.key);
  }
  if (surface_.menu_bar_open) {
    return HandleMenuBarKeyDown(event.key, modifiers);
  }
  if (CompositionConsumesKey(event.key.key, modifiers)) {
    return true;
  }
  if (prompts_.surface_visible) {
    return HandlePromptSurfaceKeyDown(event.key);
  }
  const bool active_compare_tab = ActiveTabIsCompare();
  const bool active_merge_tab = ActiveTabIsMerge();
  if (HandleGlobalKeyDown(event.key, modifiers, active_compare_tab, active_merge_tab)) {
    return true;
  }
  if (surface_.command_mode) {
    return HandleCommandKeyDown(event.key);
  }
  if (HandleSurfaceNavigationKeyDown(event.key, modifiers)) {
    return true;
  }
  if (surface_.focus == FocusTarget::Overlay) {
    return HandleOverlayKeyDown(event.key, modifiers);
  }
  if (surface_.focus == FocusTarget::Sidebar && surface_.sidebar_visible) {
    return HandleSidebarKeyDown(event.key, modifiers);
  }

  if (surface_.focus == FocusTarget::Panel && ActiveTerminalTab() != nullptr) {
    return HandleTerminalKeyDown(event.key, modifiers);
  }

  if (surface_.focus == FocusTarget::Editor && active_compare_tab) {
    return HandleCompareKeyDown(event.key, modifiers);
  }

  if (surface_.focus == FocusTarget::Editor && active_merge_tab) {
    return HandleMergeKeyDown(event.key, modifiers);
  }

  return HandleDefaultEditorKeyDown(event.key, modifiers);
}

void WorkspaceShell::SyncTextInputSurface(SDL_Window* window) {
  const TextInputSurface current_surface = CurrentTextInputSurface();
  if (current_surface == active_text_input_surface_) {
    return;
  }

  active_text_input_surface_ = current_surface;
  text_composition_ = TextCompositionState{};
  SDL_Window* target_window = window != nullptr ? window : SDL_GetKeyboardFocus();
  if (target_window != nullptr) {
    SDL_ClearComposition(target_window);
  }
}

bool WorkspaceShell::CompositionConsumesKey(SDL_Keycode key, SDL_Keymod modifiers) const {
  if (text_composition_.text.empty() || text_composition_.surface != CurrentTextInputSurface() ||
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

bool WorkspaceShell::HandleTextEditing(const SDL_TextEditingEvent& event) {
  if (surface_.menu_bar_open || surface_.tree_context_menu.open) {
    text_composition_ = TextCompositionState{};
    return true;
  }
  SyncTextInputSurface(nullptr);
  const TextInputSurface surface = CurrentTextInputSurface();
  if (surface == TextInputSurface::None || surface == TextInputSurface::Terminal) {
    text_composition_ = TextCompositionState{};
    return false;
  }

  if (event.text == nullptr || event.text[0] == '\0') {
    text_composition_ = TextCompositionState{};
    return true;
  }

  text_composition_.surface = surface;
  text_composition_.text = event.text;
  text_composition_.start = event.start;
  text_composition_.length = event.length;
  return true;
}

bool WorkspaceShell::HandleTextInput(const SDL_TextInputEvent& event) {
  if (surface_.menu_bar_open || surface_.tree_context_menu.open) {
    return true;
  }
  if (event.text == nullptr || event.text[0] == '\0' || prompts_.dirty_visible) {
    return false;
  }

  SyncTextInputSurface(nullptr);
  text_composition_ = TextCompositionState{};
  const std::string_view input(event.text);
  if (prompts_.surface_visible &&
      prompts_.surface.kind == PromptSurfaceState::Kind::TextInput) {
    prompts_.surface.input.append(input);
    return true;
  }
  if (surface_.command_mode) {
    command_.input.append(input);
    command_.history_index.reset();
    command_.history_pending_input.clear();
    ClearCommandFeedback();
    return true;
  }

  if (surface_.overlay_visible) {
    switch (surface_.overlay_mode) {
      case OverlayMode::CommitPicker:
        overlay_workflow_.compare_picker.query.append(input);
        RefreshComparePicker();
        return true;
      case OverlayMode::BufferSearch:
        overlay_workflow_.buffer_search.query.append(input);
        RefreshBufferSearch();
        return true;
      case OverlayMode::BufferReplace:
        if (surface_.buffer_search_field == BufferSearchField::Search) {
          overlay_workflow_.buffer_search.query.append(input);
          RefreshBufferSearch();
        } else {
          overlay_workflow_.buffer_search.replace_text.append(input);
        }
        return true;
      case OverlayMode::ProjectSearch:
        overlay_workflow_.project_search.query.append(input);
        RefreshProjectSearch();
        return true;
      case OverlayMode::FileFinder:
      default:
        file_finder_.AppendQueryText(input);
        ResetOverlayScroll();
        return true;
    }
  }

  if (surface_.focus == FocusTarget::Sidebar && surface_.sidebar_visible && surface_.sidebar_mode == SidebarMode::Search &&
      overlay_workflow_.project_search.editing) {
    overlay_workflow_.project_search.edit_buffer.append(input);
    return true;
  }

  if (surface_.focus == FocusTarget::Editor && ActiveEditableViewport() != nullptr) {
    editor::TextViewport* viewport = ActiveEditableViewport();
    if (viewport == nullptr) {
      return false;
    }
    const std::vector<std::string> before_lines = viewport->lines();
    const std::optional<editor::SelectionRange> selection_before = viewport->selection_range();
    const editor::TextPosition cursor_before{viewport->cursor_line(), viewport->cursor_column()};
    viewport->InsertText(input);
    if (auto* compare_tab = ActiveCompareTab(); compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
      RefreshCompareTabDerivedState(*compare_tab);
      SyncCompareSelectionFromViewport(*compare_tab, true);
    }
    if (auto* merge_tab = ActiveMergeTab(); merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
      UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before, cursor_before);
    }
    ResetCaretBlink();
    return true;
  }

  if (surface_.focus == FocusTarget::Panel && ActiveTerminalTab() != nullptr) {
    ClearTerminalSelection();
    if (auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr) {
      AppendTerminalPendingInput(input);
      terminal_tab->session.SendBytes(input);
    }
    return true;
  }

  return false;
}

bool WorkspaceShell::HandleTerminalKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers) {
  auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return false;
  }

  if ((modifiers & SDL_KMOD_CTRL) && event.key == SDLK_C && TerminalHasSelection()) {
    const std::string text = SelectedTerminalText(terminal_tab->session.SnapshotLines());
    if (!text.empty() && WriteClipboardText(text)) {
    }
    return true;
  }

  if (event.key == SDLK_ESCAPE && TerminalHasSelection()) {
    ClearTerminalSelection();
    return true;
  }

  if ((modifiers & SDL_KMOD_CTRL) && (modifiers & SDL_KMOD_SHIFT) && event.key == SDLK_V) {
    return PasteClipboardIntoTerminal();
  }

  if ((modifiers & SDL_KMOD_SHIFT) && event.key == SDLK_INSERT) {
    return PasteClipboardIntoTerminal();
  }

  if (modifiers & SDL_KMOD_CTRL) {
    if (event.key >= SDLK_A && event.key <= SDLK_Z) {
      const char control =
          static_cast<char>(1 + (event.key - SDLK_A));
      terminal_tab->session.SendBytes(std::string(1, control));
      return true;
    }
    switch (event.key) {
      case SDLK_LEFTBRACKET:
        terminal_tab->session.SendBytes("\x1b");
        return true;
      case SDLK_BACKSLASH:
        terminal_tab->session.SendBytes("\x1c");
        return true;
      case SDLK_RIGHTBRACKET:
        terminal_tab->session.SendBytes("\x1d");
        return true;
      case SDLK_SPACE:
        terminal_tab->session.SendBytes(std::string(1, '\0'));
        return true;
      default:
        break;
    }
  }

  if (modifiers & SDL_KMOD_ALT) {
    const char input_character = KeycodeToAscii(event.key, modifiers);
    if (input_character != '\0') {
      std::string bytes(1, '\x1b');
      bytes.push_back(input_character);
      terminal_tab->session.SendBytes(bytes);
      return true;
    }
  }

  switch (event.key) {
    case SDLK_ESCAPE:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Escape);
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      SubmitTerminalPendingInput();
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Enter);
      return true;
    case SDLK_BACKSPACE:
      EraseLastTerminalPendingInputCodepoint();
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Backspace);
      return true;
    case SDLK_TAB:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Tab);
      return true;
    case SDLK_UP:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Up);
      return true;
    case SDLK_DOWN:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Down);
      return true;
    case SDLK_RIGHT:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Right);
      return true;
    case SDLK_LEFT:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Left);
      return true;
    case SDLK_HOME:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Home);
      return true;
    case SDLK_END:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::End);
      return true;
    case SDLK_PAGEUP:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::PageUp);
      return true;
    case SDLK_PAGEDOWN:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::PageDown);
      return true;
    case SDLK_INSERT:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Insert);
      return true;
    case SDLK_DELETE:
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Delete);
      return true;
    default:
      break;
  }

  return false;
}

bool WorkspaceShell::PasteClipboardIntoTerminal() {
  auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return false;
  }

  const std::optional<std::string> clipboard_text = ReadClipboardText();
  if (!clipboard_text.has_value()) {
    return true;
  }

  ClearTerminalSelection();
  if (clipboard_text->find_first_of("\r\n") == std::string::npos) {
    AppendTerminalPendingInput(*clipboard_text);
  }
  terminal_tab->session.PasteText(*clipboard_text);
  return true;
}

}  // namespace microide::workspace
