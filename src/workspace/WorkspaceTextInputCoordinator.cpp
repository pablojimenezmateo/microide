#include "workspace/WorkspaceTextInputCoordinator.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/SingleLineKeyHandler.h"
#include "workspace/WorkspaceCommandPromptCoordinator.h"
#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

namespace {
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
    case TextInputSurface::DebugVariableEdit:
      // The Variables inline edit now lives in the right-side debug pane.
      operations_.request_window_redraw();
      break;
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace:
    case TextInputSurface::CommitSubject:
    case TextInputSurface::CommitBody:
      operations_.request_sidebar_redraw();
      break;
    case TextInputSurface::FileFinder:
    case TextInputSurface::BufferSearch:
    case TextInputSurface::BufferReplaceSearch:
    case TextInputSurface::BufferReplaceReplace:
    case TextInputSurface::ProjectSearchOverlay:
    case TextInputSurface::CommitPicker:
    case TextInputSurface::LaunchConfigPicker:
    case TextInputSurface::CommandPalette:
    case TextInputSurface::SettingsQuery:
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

editor::SingleLineEditor* TextInputCoordinator::ActiveSingleLineTextState() {
  switch (operations_.current_text_input_surface()) {
    case TextInputSurface::PromptInput:
      return &prompts_.surface.input;
    case TextInputSurface::CommitPicker:
      return &state_.overlay.workflow.compare_picker.query;
    case TextInputSurface::LaunchConfigPicker:
      return &state_.overlay.workflow.launch_config_picker.query;
    case TextInputSurface::CommandPalette:
      return &state_.overlay.workflow.command_palette.query;
    case TextInputSurface::BufferSearch:
    case TextInputSurface::BufferReplaceSearch:
      return &state_.overlay.workflow.buffer_search.query;
    case TextInputSurface::BufferReplaceReplace:
      return &state_.overlay.workflow.buffer_search.replace_text;
    case TextInputSurface::ProjectSearchOverlay:
      return &state_.overlay.workflow.project_search.query;
    case TextInputSurface::FileFinder:
      return &state_.file_finder.query_state();
    case TextInputSurface::SettingsQuery:
      return operations_.settings_query_editor ? operations_.settings_query_editor() : nullptr;
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace:
      return &state_.overlay.workflow.project_search.edit_buffer;
    case TextInputSurface::DebugVariableEdit:
      return &state_.debug_variables.EditBuffer();
    case TextInputSurface::CommitSubject:
      return &state_.sidebar.git.commit_workflow.subject;
    case TextInputSurface::CommitBody:
    case TextInputSurface::None:
    case TextInputSurface::Editor:
    case TextInputSurface::Terminal:
      return nullptr;
  }
  return nullptr;
}

const editor::SingleLineEditor* TextInputCoordinator::ActiveSingleLineTextState() const {
  return const_cast<TextInputCoordinator*>(this)->ActiveSingleLineTextState();
}

void TextInputCoordinator::RequestSingleLineTextRedraw(TextInputSurface surface,
                                                       bool text_changed) {
  switch (surface) {
    case TextInputSurface::PromptInput:
      operations_.request_prompt_redraw();
      break;
    case TextInputSurface::CommitPicker:
      if (text_changed) {
        operations_.refresh_compare_picker();
      } else {
        operations_.request_overlay_redraw();
      }
      break;
    case TextInputSurface::LaunchConfigPicker:
      if (text_changed) {
        operations_.refresh_launch_config_picker();
      } else {
        operations_.request_overlay_redraw();
      }
      break;
    case TextInputSurface::CommandPalette:
      if (text_changed) {
        operations_.refresh_command_palette();
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
    case TextInputSurface::SettingsQuery:
      if (text_changed && operations_.refresh_settings_overlay) {
        operations_.refresh_settings_overlay();
      } else {
        operations_.request_overlay_redraw();
      }
      break;
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace:
    case TextInputSurface::CommitSubject:
    case TextInputSurface::CommitBody:
      // Typing in the commit subject/body only repaints the sidebar; the (heavier)
      // pre-check + draft-persist refresh happens on coarser events (field switch,
      // close) so each keystroke stays cheap.
      operations_.request_sidebar_redraw();
      break;
    case TextInputSurface::DebugVariableEdit:
      // The Variables inline edit now lives in the right-side debug pane.
      operations_.request_window_redraw();
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
  if (editor::SingleLineEditor* text_state = ActiveSingleLineTextState();
      text_state != nullptr) {
    if (!text_state->Insert(input)) {
      return false;
    }
    RequestSingleLineTextRedraw(surface, true);
    return true;
  }
  switch (surface) {
    case TextInputSurface::PromptInput:
    case TextInputSurface::FileFinder:
    case TextInputSurface::BufferSearch:
    case TextInputSurface::BufferReplaceSearch:
    case TextInputSurface::BufferReplaceReplace:
    case TextInputSurface::ProjectSearchOverlay:
    case TextInputSurface::CommitPicker:
    case TextInputSurface::LaunchConfigPicker:
    case TextInputSurface::CommandPalette:
    case TextInputSurface::SettingsQuery:
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace:
    case TextInputSurface::DebugVariableEdit:
    case TextInputSurface::CommitSubject:
      // These are single-line editors handled by ActiveSingleLineTextState above;
      // reaching here means it had no backing state, so there is nothing to insert.
      return false;
    case TextInputSurface::CommitBody: {
      editor::TextViewport& body = state_.sidebar.git.commit_workflow.body;
      body.InsertText(input);
      operations_.reset_caret_blink();
      operations_.request_sidebar_redraw();
      return true;
    }
    case TextInputSurface::Editor:
      if (operations_.active_editable_viewport() == nullptr) {
        return false;
      }
      {
        editor::TextViewport* viewport = operations_.active_editable_viewport();
        if (viewport == nullptr) {
          return false;
        }
        if (operations_.try_editor_snippet_insert_text &&
            operations_.try_editor_snippet_insert_text(viewport, input)) {
          return true;
        }
        const bool was_dirty = viewport->dirty();
        const std::size_t cursor_before_line = viewport->cursor_line();
        std::vector<std::string> before_lines;
        std::optional<editor::SelectionRange> selection_before;
        std::optional<editor::TextPosition> cursor_before;
        if (auto* merge_tab = operations_.active_merge_tab();
            merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
          before_lines = viewport->lines();
          selection_before = viewport->selection_range();
          cursor_before = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()};
        }
        viewport->InsertText(input);
        if (operations_.mark_active_editor_folding_dirty) {
          operations_.mark_active_editor_folding_dirty();
        }
        if (auto* compare_tab = operations_.active_compare_tab();
            compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
          operations_.refresh_compare_tab_derived_state(*compare_tab);
          operations_.sync_compare_selection_from_viewport(*compare_tab, true);
        }
        if (auto* merge_tab = operations_.active_merge_tab();
            merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
          operations_.update_merge_tracking_after_viewport_edit(*merge_tab, before_lines,
                                                                selection_before, *cursor_before);
        }
        operations_.reset_caret_blink();
        operations_.request_active_editable_last_change_redraw();
        if (viewport->dirty() != was_dirty) {
          operations_.request_active_editable_blame_neighborhood_redraw(cursor_before_line,
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
  editor::SingleLineEditor* text_state = ActiveSingleLineTextState();
  if (text_state == nullptr) {
    return false;
  }

  const std::string before_text = text_state->text();
  const TextInputSurface surface = operations_.current_text_input_surface();
  const bool handled = editor::SingleLineKeyHandler::HandleKeyDown(
      *text_state, event.key, modifiers,
      editor::SingleLineKeyHandler::Clipboard{
          .write_text = [this](const std::string& text) {
            (void)operations_.write_clipboard_text(text);
          },
          .read_text = [this]() { return operations_.read_clipboard_text(); },
      });

  if (!handled) {
    return false;
  }
  const bool text_changed = text_state->text() != before_text;
  RequestSingleLineTextRedraw(surface, text_changed);
  if (text_state->HasSelection()) {
    operations_.write_primary_selection_text(text_state->SelectedText());
  }
  return true;
}

bool TextInputCoordinator::HasSelectionAtActiveSingleLineSurface() const {
  const editor::SingleLineEditor* text_state = ActiveSingleLineTextState();
  return text_state != nullptr && text_state->HasSelection();
}

std::string TextInputCoordinator::SelectedTextAtActiveSingleLineSurface() const {
  const editor::SingleLineEditor* text_state = ActiveSingleLineTextState();
  return text_state != nullptr ? text_state->SelectedText() : std::string{};
}

bool TextInputCoordinator::SelectAllAtActiveSingleLineSurface() {
  editor::SingleLineEditor* text_state = ActiveSingleLineTextState();
  if (text_state == nullptr) {
    return false;
  }
  if (!text_state->SelectAll()) {
    RequestSingleLineTextRedraw(operations_.current_text_input_surface(), false);
    return false;
  }
  RequestSingleLineTextRedraw(operations_.current_text_input_surface(), false);
  return true;
}

bool TextInputCoordinator::CutSelectionAtActiveSingleLineSurface() {
  editor::SingleLineEditor* text_state = ActiveSingleLineTextState();
  if (text_state == nullptr) {
    return false;
  }
  const std::string selected = text_state->SelectedText();
  if (selected.empty() || !operations_.write_clipboard_text(selected)) {
    return false;
  }
  operations_.write_primary_selection_text(selected);
  if (!text_state->DeleteSelection()) {
    return false;
  }
  RequestSingleLineTextRedraw(operations_.current_text_input_surface(), true);
  return true;
}


}  // namespace microide::workspace
