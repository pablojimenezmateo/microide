#include "workspace/WorkspaceShell.h"

#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace microide::workspace {

void WorkspaceShell::MoveFileFinderSelection(int delta) {
  context_.current_project_state.file_finder.MoveSelection(delta);
  if (context_.current_project_state.overlay.visible) {
    const auto layout = CurrentWorkspaceLayout();
    if (!layout.has_value()) {
      return;
    }
    RevealOverlaySelection(ComputeOverlayRect(layout->editor_area));
  }
  RequestOverlayRedraw();
}

WorkspaceShell::TextInputSurface WorkspaceShell::CurrentTextInputSurface() const {
  if (context_.prompts.dirty_visible) {
    return TextInputSurface::None;
  }

  if (context_.prompts.surface_visible) {
    return context_.prompts.surface.kind == PromptSurfaceState::Kind::TextInput
               ? TextInputSurface::PromptInput
               : TextInputSurface::None;
  }

  if (context_.menu_state.menu_bar_open || context_.menu_state.tree_context_menu.open) {
    return TextInputSurface::None;
  }

  if (context_.current_project_state.panel.command_mode) {
    return TextInputSurface::Command;
  }

  if (context_.current_project_state.overlay.visible) {
    switch (context_.current_project_state.overlay.mode) {
      case OverlayMode::BufferSearch:
        return TextInputSurface::BufferSearch;
      case OverlayMode::BufferReplace:
        return context_.current_project_state.overlay.buffer_search_field == BufferSearchField::Search
                   ? TextInputSurface::BufferReplaceSearch
                   : TextInputSurface::BufferReplaceReplace;
      case OverlayMode::ProjectSearch:
        return TextInputSurface::ProjectSearchOverlay;
      case OverlayMode::CommitPicker:
        return TextInputSurface::CommitPicker;
      case OverlayMode::Completion:
      case OverlayMode::CodeActions:
        return TextInputSurface::None;
      case OverlayMode::FileFinder:
      default:
        return TextInputSurface::FileFinder;
    }
  }

  if (context_.current_project_state.surface.focus == FocusTarget::Sidebar && context_.current_project_state.sidebar.visible &&
      ActiveSidebarMode() == SidebarMode::Search &&
      context_.current_project_state.overlay.workflow.project_search.editing) {
    return context_.current_project_state.overlay.workflow.project_search.edit_field == ProjectSearchEditField::Query
               ? TextInputSurface::SidebarSearchQuery
               : TextInputSurface::SidebarSearchReplace;
  }

  if (context_.current_project_state.surface.focus == FocusTarget::Editor && ActiveEditableViewport() != nullptr) {
    return TextInputSurface::Editor;
  }

  if (context_.current_project_state.surface.focus == FocusTarget::Panel) {
    if (BottomPanelShowsTerminal() && ActiveTerminalTab() != nullptr) {
      return TextInputSurface::Terminal;
    }
  }

  return TextInputSurface::None;
}

bool WorkspaceShell::WriteClipboardText(std::string_view text) const {
  if (text.empty()) {
    return false;
  }
  if (clipboard_text_writer_) {
    return clipboard_text_writer_(text);
  }
  return SDL_SetClipboardText(std::string(text).c_str());
}

std::optional<std::string> WorkspaceShell::ReadClipboardText() const {
  if (clipboard_text_reader_) {
    return clipboard_text_reader_();
  }

  char* clipboard_text = SDL_GetClipboardText();
  if (clipboard_text == nullptr) {
    return std::nullopt;
  }

  std::string copied_text(clipboard_text);
  SDL_free(clipboard_text);
  return copied_text;
}

bool WorkspaceShell::WritePrimarySelectionText(std::string_view text) const {
  if (text.empty()) {
    return false;
  }
  if (primary_selection_text_writer_) {
    return primary_selection_text_writer_(text);
  }
  return SDL_SetPrimarySelectionText(std::string(text).c_str());
}

std::optional<std::string> WorkspaceShell::ReadPrimarySelectionText() const {
  if (primary_selection_text_reader_) {
    return primary_selection_text_reader_();
  }

  char* selection_text = SDL_GetPrimarySelectionText();
  if (selection_text == nullptr) {
    return std::nullopt;
  }

  std::string copied_text(selection_text);
  SDL_free(selection_text);
  return copied_text;
}

void WorkspaceShell::SyncPrimarySelectionWithActiveEditor() {
  const editor::TextViewport* viewport = ActiveNavigableViewport();
  if (viewport == nullptr || !viewport->has_selection()) {
    return;
  }

  const std::string text = viewport->SelectedText();
  if (!text.empty()) {
    WritePrimarySelectionText(text);
  }
}

void WorkspaceShell::SyncPrimarySelectionWithTerminalSelection() {
  if (!TerminalHasSelection()) {
    return;
  }

  const std::string text = SelectedTerminalText();
  if (!text.empty()) {
    WritePrimarySelectionText(text);
  }
}

std::optional<std::string> WorkspaceShell::SelectionTextWithContext() const {
  const editor::TextViewport* viewport = ActiveNavigableViewport();
  if (viewport == nullptr) {
    return std::nullopt;
  }

  const std::optional<editor::SelectionRange> range = viewport->selection_range();
  if (!range.has_value()) {
    return std::nullopt;
  }

  const std::string text = viewport->SelectedText();
  if (text.empty()) {
    return std::nullopt;
  }

  const std::filesystem::path path = viewport->path().lexically_normal();
  std::string path_label;
  if (!context_.current_project_state.root.empty() && !path.empty()) {
    std::error_code error;
    const std::filesystem::path relative = std::filesystem::relative(path, context_.current_project_state.root, error);
    const bool starts_with_parent =
        relative.begin() != relative.end() &&
        *relative.begin() == std::filesystem::path("..");
    if (!error && !relative.empty() && !starts_with_parent) {
      path_label = relative.generic_string();
    }
  }
  if (path_label.empty()) {
    path_label = path.empty() ? "untitled" : path.string();
  }

  const std::size_t start_line = range->start.line + 1;
  std::size_t end_line = range->end.line + 1;
  if (range->end.column == 0 && range->end.line > range->start.line) {
    end_line = range->end.line;
  }

  std::string header = path_label + ":" + std::to_string(start_line);
  if (end_line > start_line) {
    header += "-" + std::to_string(end_line);
  }
  header += "\n";
  header += text;
  return header;
}

}  // namespace microide::workspace
