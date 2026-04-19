#include "workspace/WorkspaceShell.h"

#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace microide::workspace {

void WorkspaceShell::MoveFileFinderSelection(int delta) {
  file_finder_.MoveSelection(delta);
  if (surface_.overlay_visible) {
    const auto layout = CurrentWorkspaceLayout();
    if (!layout.has_value()) {
      return;
    }
    RevealOverlaySelection(ComputeOverlayRect(layout->editor_area));
  }
  RequestOverlayRedraw();
}

WorkspaceShell::TextInputSurface WorkspaceShell::CurrentTextInputSurface() const {
  if (prompts_.dirty_visible) {
    return TextInputSurface::None;
  }

  if (prompts_.surface_visible) {
    return prompts_.surface.kind == PromptSurfaceState::Kind::TextInput
               ? TextInputSurface::PromptInput
               : TextInputSurface::None;
  }

  if (menu_state_.menu_bar_open || menu_state_.tree_context_menu.open) {
    return TextInputSurface::None;
  }

  if (surface_.command_mode) {
    return TextInputSurface::Command;
  }

  if (surface_.overlay_visible) {
    switch (surface_.overlay_mode) {
      case OverlayMode::BufferSearch:
        return TextInputSurface::BufferSearch;
      case OverlayMode::BufferReplace:
        return surface_.buffer_search_field == BufferSearchField::Search
                   ? TextInputSurface::BufferReplaceSearch
                   : TextInputSurface::BufferReplaceReplace;
      case OverlayMode::ProjectSearch:
        return TextInputSurface::ProjectSearchOverlay;
      case OverlayMode::CommitPicker:
        return TextInputSurface::CommitPicker;
      case OverlayMode::FileFinder:
      default:
        return TextInputSurface::FileFinder;
    }
  }

  if (surface_.focus == FocusTarget::Sidebar && surface_.sidebar_visible &&
      surface_.sidebar_mode == SidebarMode::Search &&
      overlay_workflow_.project_search.editing) {
    return overlay_workflow_.project_search.edit_field == ProjectSearchEditField::Query
               ? TextInputSurface::SidebarSearchQuery
               : TextInputSurface::SidebarSearchReplace;
  }

  if (surface_.focus == FocusTarget::Editor && ActiveEditableViewport() != nullptr) {
    return TextInputSurface::Editor;
  }

  if (surface_.focus == FocusTarget::Panel && ActiveTerminalTab() != nullptr) {
    return TextInputSurface::Terminal;
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
  const editor::TextViewport* viewport = ActiveEditableViewport();
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
  const editor::TextViewport* viewport = ActiveEditableViewport();
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
  if (!project_root_.empty() && !path.empty()) {
    std::error_code error;
    const std::filesystem::path relative = std::filesystem::relative(path, project_root_, error);
    if (!error && !relative.empty() && relative.native().rfind("..", 0) != 0) {
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
