#include "workspace/WorkspaceShell.h"

#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "editor/FoldingModel.h"
#include "editor/LineSpan.h"
#include "editor/TextViewport.h"
#include "util/StringUtil.h"

namespace microide::workspace {

namespace {

bool IsBlankOrWhitespace(std::string_view text) {
  for (const char ch : text) {
    if (std::isspace(static_cast<unsigned char>(ch)) == 0) {
      return false;
    }
  }
  return true;
}

std::string JoinLineRange(editor::LineSpan lines,
                          std::size_t first,
                          std::size_t last_inclusive) {
  std::string joined;
  const std::size_t clamped_last = std::min(last_inclusive, lines.empty() ? 0 : lines.size() - 1);
  for (std::size_t line = first; line <= clamped_last && line < lines.size(); ++line) {
    if (line > first) {
      joined.push_back('\n');
    }
    joined += lines[line];
  }
  return joined;
}

// Renders the project-relative label for a buffer path, or an empty string
// when the path falls outside the project root (or there is no root).
std::string RelativePathLabel(const std::filesystem::path& path,
                              const std::filesystem::path& project_root) {
  if (project_root.empty() || path.empty()) {
    return {};
  }
  const std::filesystem::path relative = path.lexically_relative(project_root);
  const bool starts_with_parent =
      relative.begin() != relative.end() && *relative.begin() == std::filesystem::path("..");
  if (relative.empty() || starts_with_parent) {
    return {};
  }
  return relative.generic_string();
}

}  // namespace

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

  // The settings overlay is a host-owned modal layered above everything else. It
  // only claims text input when its filter pane holds focus, mirroring the
  // "focused-only" rule the buffer-search surfaces use below.
  if (settings_overlay_service_.Visible()) {
    if (settings_overlay_service_.Mode() != SettingsOverlayMode::Settings) {
      return TextInputSurface::None;
    }
    // An active inline value edit claims text input regardless of focused pane so
    // typing lands in the value editor; otherwise the filter pane owns input.
    if (settings_overlay_service_.EditingValue()) {
      return TextInputSurface::SettingsValueEdit;
    }
    if (settings_overlay_service_.FocusedPane() == SettingsPane::Filter) {
      return TextInputSurface::SettingsQuery;
    }
    return TextInputSurface::None;
  }

  if (context_.prompts.surface_visible) {
    return context_.prompts.surface.kind == PromptSurfaceState::Kind::TextInput
               ? TextInputSurface::PromptInput
               : TextInputSurface::None;
  }

  if (MenuSurfaceCapturingMouse()) {
    return TextInputSurface::None;
  }

  if (context_.current_project_state.overlay.visible) {
    // The find/replace widget is non-modal: while it floats, focus may be on the
    // editor (so the user can keep editing). Only claim the buffer-search input
    // surfaces when the widget actually holds focus; otherwise fall through so
    // typing and the caret belong to the editor underneath.
    const bool overlay_focused =
        context_.current_project_state.surface.focus == FocusTarget::Overlay;
    switch (context_.current_project_state.overlay.mode) {
      case OverlayMode::BufferSearch:
        if (overlay_focused) {
          return TextInputSurface::BufferSearch;
        }
        break;
      case OverlayMode::BufferReplace:
        if (overlay_focused) {
          return context_.current_project_state.overlay.buffer_search_field ==
                         BufferSearchField::Search
                     ? TextInputSurface::BufferReplaceSearch
                     : TextInputSurface::BufferReplaceReplace;
        }
        break;
      case OverlayMode::ProjectSearch:
        return TextInputSurface::ProjectSearchOverlay;
      case OverlayMode::CommitPicker:
        return TextInputSurface::CommitPicker;
      case OverlayMode::LaunchConfigPicker:
        return TextInputSurface::LaunchConfigPicker;
      case OverlayMode::CommandPalette:
        return TextInputSurface::CommandPalette;
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

  if (context_.current_project_state.surface.focus == FocusTarget::Sidebar &&
      context_.current_project_state.sidebar.visible && ActiveSidebarMode() == SidebarMode::Git &&
      context_.current_project_state.sidebar.git.commit_workflow.open) {
    return context_.current_project_state.sidebar.git.commit_workflow.focus_field ==
                   CommitWorkflowFocusField::Subject
               ? TextInputSurface::CommitSubject
               : TextInputSurface::CommitBody;
  }

  if (context_.current_project_state.surface.focus == FocusTarget::Editor && ActiveEditableViewport() != nullptr) {
    return TextInputSurface::Editor;
  }

  if (context_.current_project_state.surface.focus == FocusTarget::DebugPane &&
      context_.current_project_state.debug_pane.mode == DebugPaneMode::Variables &&
      context_.current_project_state.debug_variables.IsEditing()) {
    return TextInputSurface::DebugVariableEdit;
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

std::optional<std::string> WorkspaceShell::SelectionTextWithContext() {
  editor::TextViewport* viewport = ActiveNavigableViewport();
  if (viewport == nullptr) {
    return std::nullopt;
  }

  // Resolve the copied body and its 0-indexed line span. With a selection we
  // honour it verbatim; otherwise we fall back to the current line, expanding a
  // blank line out to its enclosing fold (method/block) so right-clicking an
  // empty line still yields useful context.
  std::string body;
  std::size_t start_line = 0;
  std::size_t end_line = 0;

  const std::optional<editor::SelectionRange> range = viewport->selection_range();
  if (range.has_value() && viewport->has_selection()) {
    body = viewport->SelectedText();
    if (body.empty()) {
      return std::nullopt;
    }
    start_line = range->start.line;
    end_line = range->end.line;
    if (range->end.column == 0 && range->end.line > range->start.line) {
      --end_line;
    }
  } else {
    // No selection: read the live buffer zero-copy (LineView) instead of materializing
    // every line with Snapshot() just to return the current line or a small enclosing
    // fold (TD-2026-07-17A-035).
    const editor::LineSpan lines = viewport->lines();
    const std::size_t cursor = viewport->cursor_line();
    if (cursor >= lines.size()) {
      return std::nullopt;
    }
    start_line = cursor;
    end_line = cursor;
    if (IsBlankOrWhitespace(lines[cursor])) {
      const editor::FoldingModel* folding_model = EnsureActiveFoldingModelFresh();
      const std::optional<editor::FoldRange> fold =
          folding_model != nullptr ? folding_model->InnermostFoldContaining(cursor)
                                    : std::nullopt;
      if (fold.has_value()) {
        start_line = fold->opener_line;
        end_line = std::min(fold->closer_line, lines.empty() ? 0 : lines.size() - 1);
      }
    }
    body = JoinLineRange(lines, start_line, end_line);
  }

  const std::filesystem::path path = viewport->path().lexically_normal();
  std::string path_label = RelativePathLabel(path, context_.current_project_state.root);
  if (path_label.empty()) {
    path_label = path.empty() ? "untitled" : path.string();
  }

  // Prepend the enclosing fold opener (function/method/block header) as a
  // neutral context comment so the snippet pastes into an LLM with scope.
  std::string result;
  if (const editor::FoldingModel* folding_model = EnsureActiveFoldingModelFresh();
      folding_model != nullptr) {
    const std::optional<editor::FoldRange> enclosing =
        folding_model->InnermostFoldContaining(start_line);
    if (enclosing.has_value() && enclosing->opener_line < start_line &&
        enclosing->opener_line < viewport->lines().size()) {
      const std::string opener =
          util::TrimAsciiWhitespace(viewport->lines()[enclosing->opener_line]);
      if (!opener.empty()) {
        result += "// context: ";
        result += opener;
        result.push_back('\n');
      }
    }
  }

  result += path_label;
  result.push_back(':');
  result += std::to_string(start_line + 1);
  if (end_line > start_line) {
    result.push_back('-');
    result += std::to_string(end_line + 1);
  }
  result.push_back('\n');
  result += body;
  return result;
}

}  // namespace microide::workspace
