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

WorkspaceShell::EventResult WorkspaceShell::HandleEvent(const SDL_Event& event) {
  const ScopeExit sync_terminal_focus{[this]() { SyncTerminalFocusState(); }};
  const auto finish = [this](bool handled) {
    return EventResult{
        .handled = handled,
        .redraw = ConsumePendingRenderInvalidation(),
    };
  };
  const auto ensure_redraw = [this](auto request_redraw) {
    if (!pending_render_invalidation_.HasAnyRedraw()) {
      request_redraw();
    }
  };

  if (project_open_dialog_event_type_ != 0 && event.type == project_open_dialog_event_type_) {
    ConsumePendingProjectOpenDialogResult();
    return finish(true);
  }
  if (project_search_runtime_.HandlesEvent(event.type)) {
    ConsumeProjectSearchUpdates();
    return finish(true);
  }
  if (git_blame_event_type_ != 0 && event.type == git_blame_event_type_) {
    RequestFocusedEditorRedraw();
    return finish(true);
  }
  if (terminal_event_type_ != 0 && event.type == terminal_event_type_) {
    ConsumeTerminalSessionUpdates();
    return finish(true);
  }

  SyncTextInputSurface(nullptr);

  switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      return finish(HandleMouseButtonDown(event));
    case SDL_EVENT_MOUSE_BUTTON_UP:
      return finish(HandleMouseButtonUp(event));
    case SDL_EVENT_MOUSE_MOTION:
      return finish(HandleMouseMotion(event));
    case SDL_EVENT_MOUSE_WHEEL:
      return finish(HandleMouseWheel(event));
    case SDL_EVENT_TEXT_EDITING:
      return finish(HandleTextEditing(event.edit));
    case SDL_EVENT_TEXT_INPUT:
      return finish(HandleTextInput(event.text));
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      surface_.window_has_input_focus = true;
      RequestWindowRedraw();
      return finish(true);
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      surface_.window_has_input_focus = false;
      RequestWindowRedraw();
      return finish(true);
    case SDL_EVENT_KEY_DOWN:
      break;
    default:
      return finish(false);
  }

  const SDL_Keymod modifiers =
      event.key.mod != SDL_KMOD_NONE ? event.key.mod : SDL_GetModState();
  if (prompts_.dirty_visible) {
    const bool handled = HandleDirtyPromptKeyDown(event.key, modifiers);
    if (handled) {
      ensure_redraw([this]() { RequestPromptRedraw(); });
    }
    return finish(handled);
  }
  if (surface_.tree_context_menu.open) {
    const bool handled = HandleTreeContextMenuKeyDown(event.key);
    if (handled) {
      ensure_redraw([this]() { RequestChromeRedraw(); });
    }
    return finish(handled);
  }
  if (surface_.menu_bar_open) {
    const bool handled = HandleMenuBarKeyDown(event.key, modifiers);
    if (handled) {
      ensure_redraw([this]() { RequestChromeRedraw(); });
    }
    return finish(handled);
  }
  if (CompositionConsumesKey(event.key.key, modifiers)) {
    return finish(true);
  }
  if (prompts_.surface_visible) {
    const bool handled = HandlePromptSurfaceKeyDown(event.key);
    if (handled) {
      ensure_redraw([this]() { RequestPromptRedraw(); });
    }
    return finish(handled);
  }
  const bool active_compare_tab = ActiveTabIsCompare();
  const bool active_merge_tab = ActiveTabIsMerge();
  if (HandleGlobalKeyDown(event.key, modifiers, active_compare_tab, active_merge_tab)) {
    return finish(true);
  }
  if (surface_.command_mode) {
    const bool handled = HandleCommandKeyDown(event.key);
    if (handled) {
      ensure_redraw([this]() { RequestBottomPanelCommandRedraw(); });
    }
    return finish(handled);
  }
  if (HandleSurfaceNavigationKeyDown(event.key, modifiers)) {
    ensure_redraw([this]() { RequestWindowRedraw(); });
    return finish(true);
  }
  if (surface_.focus == FocusTarget::Overlay) {
    const bool handled = HandleOverlayKeyDown(event.key, modifiers);
    if (handled) {
      ensure_redraw([this]() { RequestOverlayRedraw(); });
    }
    return finish(handled);
  }
  if (surface_.focus == FocusTarget::Sidebar && surface_.sidebar_visible) {
    const bool handled = HandleSidebarKeyDown(event.key, modifiers);
    if (handled) {
      ensure_redraw([this]() { RequestSidebarRedraw(); });
    }
    return finish(handled);
  }

  if (surface_.focus == FocusTarget::Panel && ActiveTerminalTab() != nullptr) {
    const bool handled = HandleTerminalKeyDown(event.key, modifiers);
    if (handled) {
      ensure_redraw([this]() { RequestBottomPanelContentRedraw(); });
    }
    return finish(handled);
  }

  if (surface_.focus == FocusTarget::Editor && active_compare_tab) {
    const bool handled = HandleCompareKeyDown(event.key, modifiers);
    if (handled) {
      ensure_redraw([this]() { RequestFocusedEditorRedraw(); });
    }
    return finish(handled);
  }

  if (surface_.focus == FocusTarget::Editor && active_merge_tab) {
    const bool handled = HandleMergeKeyDown(event.key, modifiers);
    if (handled) {
      ensure_redraw([this]() { RequestFocusedEditorRedraw(); });
    }
    return finish(handled);
  }

  const bool handled = HandleDefaultEditorKeyDown(event.key, modifiers);
  if (handled) {
    ensure_redraw([this]() { RequestFocusedEditorRedraw(); });
  }
  return finish(handled);
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
    if (!text_composition_.text.empty()) {
      RequestWindowRedraw();
    }
    text_composition_ = TextCompositionState{};
    return true;
  }
  SyncTextInputSurface(nullptr);
  const TextInputSurface surface = CurrentTextInputSurface();
  if (surface == TextInputSurface::None || surface == TextInputSurface::Terminal) {
    if (!text_composition_.text.empty()) {
      RequestWindowRedraw();
    }
    text_composition_ = TextCompositionState{};
    return false;
  }

  if (event.text == nullptr || event.text[0] == '\0') {
    if (!text_composition_.text.empty()) {
      switch (surface) {
        case TextInputSurface::PromptInput:
          RequestPromptRedraw();
          break;
        case TextInputSurface::Command:
          RequestBottomPanelCommandRedraw();
          break;
        case TextInputSurface::SidebarSearchQuery:
        case TextInputSurface::SidebarSearchReplace:
          RequestSidebarRedraw();
          break;
        case TextInputSurface::FileFinder:
        case TextInputSurface::BufferSearch:
        case TextInputSurface::BufferReplaceSearch:
        case TextInputSurface::BufferReplaceReplace:
        case TextInputSurface::ProjectSearchOverlay:
        case TextInputSurface::CommitPicker:
          RequestOverlayRedraw();
          break;
        case TextInputSurface::Editor:
          RequestFocusedEditorRedraw();
          break;
        case TextInputSurface::None:
        case TextInputSurface::Terminal:
          break;
      }
    }
    text_composition_ = TextCompositionState{};
    return true;
  }

  text_composition_.surface = surface;
  text_composition_.text = event.text;
  text_composition_.start = event.start;
  text_composition_.length = event.length;
  switch (surface) {
    case TextInputSurface::PromptInput:
      RequestPromptRedraw();
      break;
    case TextInputSurface::Command:
      RequestBottomPanelCommandRedraw();
      break;
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace:
      RequestSidebarRedraw();
      break;
    case TextInputSurface::FileFinder:
    case TextInputSurface::BufferSearch:
    case TextInputSurface::BufferReplaceSearch:
    case TextInputSurface::BufferReplaceReplace:
    case TextInputSurface::ProjectSearchOverlay:
    case TextInputSurface::CommitPicker:
      RequestOverlayRedraw();
      break;
    case TextInputSurface::Editor:
      RequestFocusedEditorRedraw();
      break;
    case TextInputSurface::None:
    case TextInputSurface::Terminal:
      break;
  }
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
    RequestPromptRedraw();
    return true;
  }
  if (surface_.command_mode) {
    command_.input.append(input);
    command_.history_index.reset();
    command_.history_pending_input.clear();
    ClearCommandFeedback();
    RequestBottomPanelCommandRedraw();
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
          RequestOverlayRedraw();
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
    RequestSidebarRedraw();
    return true;
  }

  if (surface_.focus == FocusTarget::Editor && ActiveEditableViewport() != nullptr) {
    editor::TextViewport* viewport = ActiveEditableViewport();
    if (viewport == nullptr) {
      return false;
    }
    const bool was_dirty = viewport->dirty();
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
    RequestActiveEditableChangeRedraw(before_lines, viewport->lines());
    if (viewport->dirty() != was_dirty) {
      RequestActiveEditableBlameNeighborhoodRedraw(cursor_before.line, viewport->cursor_line());
      RequestTabStripRedraw();
    }
    return true;
  }

  if (surface_.focus == FocusTarget::Panel && ActiveTerminalTab() != nullptr) {
    ClearTerminalSelection();
    if (auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr) {
      terminal_tab->follow_tail = true;
      AppendTerminalPendingInput(input);
      terminal_tab->session.SendBytes(input);
    }
    RequestBottomPanelContentRedraw();
    return true;
  }

  return false;
}

bool WorkspaceShell::HandleTerminalKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers) {
  auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return false;
  }
  const auto follow_terminal_tail = [&]() { terminal_tab->follow_tail = true; };
  const auto handled_with_panel_redraw = [this]() {
    RequestBottomPanelContentRedraw();
    return true;
  };

  if ((modifiers & SDL_KMOD_CTRL) && event.key == SDLK_C && TerminalHasSelection()) {
    const std::string text = SelectedTerminalText();
    if (!text.empty() && WriteClipboardText(text)) {
      WritePrimarySelectionText(text);
    }
    return true;
  }

  if (event.key == SDLK_ESCAPE && TerminalHasSelection()) {
    ClearTerminalSelection();
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
      const char control =
          static_cast<char>(1 + (event.key - SDLK_A));
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
      SubmitTerminalPendingInput();
      follow_terminal_tail();
      terminal_tab->session.SendKey(terminal::TerminalSession::Key::Enter);
      return handled_with_panel_redraw();
    case SDLK_BACKSPACE:
      EraseLastTerminalPendingInputCodepoint();
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
  terminal_tab->follow_tail = true;
  terminal_tab->session.PasteText(*clipboard_text);
  RequestBottomPanelContentRedraw();
  return true;
}

}  // namespace microide::workspace
