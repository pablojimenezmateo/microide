#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "util/StringUtil.h"
#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceCommandRegistry.h"


namespace microide::workspace {

namespace {
// How many recent files to surface at the top of an empty file finder.
constexpr std::size_t kFileFinderRecentLimit = 8;
}  // namespace

WorkspaceShell::FocusTarget WorkspaceShell::PrimarySurfaceFocusTarget() const {
  return PrimarySurfaceFocus(context_.current_project_state);
}

void WorkspaceShell::ShowOverlay(OverlayMode mode) {
  RequestOverlayRedraw();
  InvalidateCursorKindFingerprint();
  // An opening overlay (completion, code actions, pickers) supersedes the
  // caret-anchored signature popup; drop it so the two never stack.
  active_signature_help_.reset();
  context_.current_project_state.overlay.visible = true;
  context_.current_project_state.overlay.mode = mode;
  if (mode == OverlayMode::Completion || mode == OverlayMode::CodeActions) {
    // Anchor the compact popup to the caret so it appears next to the code being
    // completed instead of as a centered modal that hides it.
    if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
      context_.current_project_state.overlay.caret_anchor = ActiveEditorCaretRect(*layout);
    } else {
      context_.current_project_state.overlay.caret_anchor.reset();
    }
  } else {
    context_.current_project_state.overlay.caret_anchor.reset();
  }
  if (mode == OverlayMode::FileFinder) {
    // Seed the finder with the active project's recent files so an empty query leads
    // with them. Convert the stored absolute paths to project-relative form lexically
    // (no filesystem access) to match the index's path strings.
    const std::filesystem::path& root = context_.current_project_state.root;
    std::vector<std::filesystem::path> recent_relative;
    if (!root.empty()) {
      for (const std::filesystem::path& absolute :
           recents_service_.RecentFilesFor(root, kFileFinderRecentLimit)) {
        std::filesystem::path relative = absolute.lexically_relative(root);
        if (!relative.empty() && relative.native()[0] != '.') {
          recent_relative.push_back(std::move(relative));
        }
      }
    }
    context_.current_project_state.file_finder.SetRecentRelativePaths(std::move(recent_relative));
  }
  context_.current_project_state.surface.focus = FocusTarget::Overlay;
  ResetOverlayScroll();
  RequestOverlayRedraw();
}

void WorkspaceShell::DismissOverlay(bool focus_editor) {
  RequestOverlayRedraw();
  InvalidateCursorKindFingerprint();
  const bool preserve_buffer_search_expansions =
      context_.current_project_state.overlay.mode == OverlayMode::BufferSearch &&
      focus_editor;
  if (context_.current_project_state.overlay.mode == OverlayMode::BufferSearch ||
      context_.current_project_state.overlay.mode == OverlayMode::BufferReplace) {
    ResetBufferSearchFoldRevealState(preserve_buffer_search_expansions);
  }
  context_.current_project_state.overlay.visible = false;
  context_.current_project_state.surface.focus = focus_editor ? FocusTarget::Editor : PrimarySurfaceFocusTarget();
  RequestOverlayRedraw();
}

std::string WorkspaceShell::SingleLineEditorSelectionSeed() const {
  // VSCode-style: a non-empty single-line editor selection seeds the find query.
  const editor::TextViewport* viewport = ActiveEditorViewport();
  if (viewport == nullptr) {
    return {};
  }
  const auto selection = viewport->selection_range();
  if (!selection.has_value() || selection->start.line != selection->end.line) {
    return {};
  }
  std::string text = viewport->SelectedText();
  if (text.empty() || text.find('\n') != std::string::npos) {
    return {};
  }
  return text;
}

void WorkspaceShell::OpenBufferSearchSurface(OverlayMode mode) {
  auto& buffer_search = context_.current_project_state.overlay.workflow.buffer_search;
  // Re-invoking find while the widget already floats keeps the existing query and
  // simply re-focuses + selects it (so the next keystroke replaces it). Otherwise
  // seed from the editor selection when there is one, else reuse the last term.
  const bool already_open = context_.current_project_state.overlay.visible &&
                            (context_.current_project_state.overlay.mode == OverlayMode::BufferSearch ||
                             context_.current_project_state.overlay.mode == OverlayMode::BufferReplace);
  std::string seed = SingleLineEditorSelectionSeed();
  if (!already_open) {
    ResetBufferSearchFoldRevealState(false);
  }

  ShowOverlay(mode);
  context_.current_project_state.overlay.buffer_search_field = BufferSearchField::Search;
  if (!seed.empty()) {
    buffer_search.query.SetText(std::move(seed));
  }
  buffer_search.query.SelectAll();
  RefreshBufferSearch();
}

void WorkspaceShell::OpenBufferSearch() { OpenBufferSearchSurface(OverlayMode::BufferSearch); }

void WorkspaceShell::OpenBufferReplace() { OpenBufferSearchSurface(OverlayMode::BufferReplace); }

void WorkspaceShell::OpenProjectSearch() {
  if (context_.current_project_state.root.empty()) {
    return;
  }
  context_.current_project_state.overlay.workflow.project_search.query.SetText("");
  context_.current_project_state.overlay.workflow.project_search.results.clear();
  context_.current_project_state.overlay.workflow.project_search.selected_index = 0;
  context_.current_project_state.overlay.workflow.project_search.replace_text.SetText("");
  ResetOverlayScroll();
  ShowSearchSidebar("", true);
}

void WorkspaceShell::ResetOverlayScroll() {
  context_.current_project_state.overlay.scroll_row = 0;
  RequestOverlayRedraw();
}

float WorkspaceShell::OverlayListStartOffset() const {
  switch (context_.current_project_state.overlay.mode) {
    case OverlayMode::FileFinder:
      return 74.0f;
    case OverlayMode::BufferReplace:
      return 106.0f;
    case OverlayMode::Completion:
    case OverlayMode::CodeActions:
      // Caret-anchored popups render header-less, so the list starts near the top.
      return 8.0f;
    case OverlayMode::CommitPicker:
    case OverlayMode::LaunchConfigPicker:
    case OverlayMode::CommandPalette:
      // Picker carries a richer header (title + context subtitle + query field +
      // result/hint line) so the list starts lower than the search overlays.
      return 108.0f;
    case OverlayMode::BufferSearch:
    case OverlayMode::ProjectSearch:
    default:
      return 86.0f;
  }
}

ScrollableListLayout WorkspaceShell::ComputeOverlayListLayout(const SDL_FRect& overlay) const {
  return ComputeScrollableListLayout(overlay, overlay.y + OverlayListStartOffset(),
                                     OverlayItemCount(), context_.current_project_state.overlay.scroll_row, 18.0f,
                                     22.0f, 18.0f, 16.0f, 8.0f);
}

int WorkspaceShell::OverlayVisibleRows(const SDL_FRect& overlay) const {
  return ComputeOverlayListLayout(overlay).visible_rows;
}

std::size_t WorkspaceShell::OverlayItemCount() const {
  switch (context_.current_project_state.overlay.mode) {
    case OverlayMode::CommitPicker:
      return context_.current_project_state.overlay.workflow.compare_picker.matches.size();
    case OverlayMode::LaunchConfigPicker:
      return context_.current_project_state.overlay.workflow.launch_config_picker.matches.size();
    case OverlayMode::CommandPalette:
      return context_.current_project_state.overlay.workflow.command_palette.matches.size();
    case OverlayMode::BufferSearch:
    case OverlayMode::BufferReplace:
      return context_.current_project_state.overlay.workflow.buffer_search.matches.size();
    case OverlayMode::ProjectSearch:
      return context_.current_project_state.overlay.workflow.project_search.results.size();
    case OverlayMode::Completion:
      return context_.current_project_state.overlay.workflow.completion.items.size();
    case OverlayMode::CodeActions:
      return context_.current_project_state.overlay.workflow.code_actions.items.size();
    case OverlayMode::FileFinder:
    default:
      return context_.current_project_state.file_finder.results().size();
  }
}

std::size_t WorkspaceShell::OverlaySelectedIndex() const {
  switch (context_.current_project_state.overlay.mode) {
    case OverlayMode::CommitPicker:
      return context_.current_project_state.overlay.workflow.compare_picker.selected_index;
    case OverlayMode::LaunchConfigPicker:
      return context_.current_project_state.overlay.workflow.launch_config_picker.selected_index;
    case OverlayMode::CommandPalette:
      return context_.current_project_state.overlay.workflow.command_palette.selected_index;
    case OverlayMode::BufferSearch:
    case OverlayMode::BufferReplace:
      return context_.current_project_state.overlay.workflow.buffer_search.selected_index;
    case OverlayMode::ProjectSearch:
      return context_.current_project_state.overlay.workflow.project_search.selected_index;
    case OverlayMode::Completion:
      return context_.current_project_state.overlay.workflow.completion.selected_index;
    case OverlayMode::CodeActions:
      return context_.current_project_state.overlay.workflow.code_actions.selected_index;
    case OverlayMode::FileFinder:
    default:
      return context_.current_project_state.file_finder.selected_index();
  }
}

void WorkspaceShell::SetOverlaySelectedIndex(std::size_t index) {
  const std::size_t item_count = OverlayItemCount();
  if (item_count == 0) {
    return;
  }
  const std::size_t clamped_index = std::min(index, item_count - 1);
  switch (context_.current_project_state.overlay.mode) {
    case OverlayMode::CommitPicker:
      context_.current_project_state.overlay.workflow.compare_picker.selected_index = clamped_index;
      break;
    case OverlayMode::LaunchConfigPicker:
      context_.current_project_state.overlay.workflow.launch_config_picker.selected_index =
          clamped_index;
      break;
    case OverlayMode::CommandPalette:
      context_.current_project_state.overlay.workflow.command_palette.selected_index =
          clamped_index;
      break;
    case OverlayMode::BufferSearch:
    case OverlayMode::BufferReplace:
      context_.current_project_state.overlay.workflow.buffer_search.selected_index = clamped_index;
      if (!context_.current_project_state.overlay.workflow.buffer_search.matches.empty()) {
        const auto& match = context_.current_project_state.overlay.workflow.buffer_search.matches[context_.current_project_state.overlay.workflow.buffer_search.selected_index];
        if (editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
          viewport->MoveCursorTo(match.start.line, match.start.column);
        }
      }
      break;
    case OverlayMode::ProjectSearch:
      context_.current_project_state.overlay.workflow.project_search.selected_index = clamped_index;
      break;
    case OverlayMode::Completion:
      context_.current_project_state.overlay.workflow.completion.selected_index = clamped_index;
      break;
    case OverlayMode::CodeActions:
      context_.current_project_state.overlay.workflow.code_actions.selected_index = clamped_index;
      break;
    case OverlayMode::FileFinder:
    default: {
      const std::size_t current_index = context_.current_project_state.file_finder.selected_index();
      context_.current_project_state.file_finder.MoveSelection(static_cast<int>(clamped_index) - static_cast<int>(current_index));
      break;
    }
  }
  RequestOverlayRedraw();
  if (context_.current_project_state.overlay.mode == OverlayMode::BufferSearch ||
      context_.current_project_state.overlay.mode == OverlayMode::BufferReplace) {
    RequestEditorSurfaceRedraw();
  }
}

void WorkspaceShell::ClampOverlayScrollRow(const SDL_FRect& overlay) {
  context_.current_project_state.overlay.scroll_row = ComputeOverlayListLayout(overlay).scroll_row;
}

void WorkspaceShell::RevealOverlaySelection(const SDL_FRect& overlay) {
  ClampOverlayScrollRow(overlay);
  if (OverlayItemCount() == 0) {
    return;
  }

  const auto layout = ComputeOverlayListLayout(overlay);
  const int selected = static_cast<int>(std::min(OverlaySelectedIndex(), OverlayItemCount() - 1));
  context_.current_project_state.overlay.scroll_row = RevealScrollableListIndex(layout, selected);
  RequestOverlayRedraw();
}

bool WorkspaceShell::ActivateOverlaySelection() {
  switch (context_.current_project_state.overlay.mode) {
    case OverlayMode::CommitPicker:
      OpenSelectedCompareCommit();
      // Dismiss the picker so it does not stay painted on top of the comparison it
      // just opened (matches the FileFinder / BufferSearch activation paths below).
      DismissOverlay(true);
      return true;
    case OverlayMode::LaunchConfigPicker:
      ConfirmLaunchConfigSelection();
      DismissOverlay(true);
      return true;
    case OverlayMode::CommandPalette:
      // ConfirmCommandPaletteSelection dismisses before dispatching (the action may
      // open its own overlay), so do not dismiss again here.
      ConfirmCommandPaletteSelection();
      return true;
    case OverlayMode::BufferSearch:
      if (!context_.current_project_state.overlay.workflow.buffer_search.matches.empty()) {
        const auto& match = context_.current_project_state.overlay.workflow.buffer_search.matches[context_.current_project_state.overlay.workflow.buffer_search.selected_index];
        if (editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
          viewport->MoveCursorTo(match.start.line, match.start.column);
        }
      }
      DismissOverlay(true);
      return true;
    case OverlayMode::BufferReplace:
      ReplaceCurrentBufferSearchMatch();
      return true;
    case OverlayMode::ProjectSearch:
      if (!context_.current_project_state.overlay.workflow.project_search.results.empty() &&
          context_.current_project_state.overlay.workflow.project_search.selected_index <
              context_.current_project_state.overlay.workflow.project_search.results.size()) {
        const auto& result =
            context_.current_project_state.overlay.workflow.project_search.results[context_.current_project_state.overlay.workflow.project_search.selected_index];
        OpenFile(context_.current_project_state.root / result.relative_path);
        if (editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
          viewport->MoveCursorTo(result.line, result.column);
        }
        OpenBufferSearchFromProjectSearchResult();
      }
      return true;
    case OverlayMode::Completion:
      return assist_service_.ApplySelectedCompletion();
    case OverlayMode::CodeActions:
      return assist_service_.ExecuteSelectedCodeAction();
    case OverlayMode::FileFinder:
    default:
      if (const auto selected = context_.current_project_state.file_finder.SelectedPath(); selected.has_value()) {
        OpenFile(context_.current_project_state.root / *selected);
      }
      DismissOverlay(true);
      return true;
  }
}

void WorkspaceShell::OpenCommandPalette() {
  CommandPaletteState& palette = context_.current_project_state.overlay.workflow.command_palette;
  palette.query.SetText("");
  palette.items.clear();

  // Built-in commands: every action that carries a human label. The label and key
  // chord come straight from the command registry (the same source the menus use),
  // so the palette can never drift from the real bindings.
  const std::span<const ActionSpec> specs = WorkspaceCommandSpecs();
  palette.items.reserve(specs.size());
  for (const ActionSpec& spec : specs) {
    if (spec.label.empty() || spec.id == ActionId::OpenCommandPalette) {
      continue;
    }
    palette.items.push_back(CommandPaletteItem{
        .primary_label = std::string(spec.label),
        .secondary_label = std::string(spec.accelerator),
        .action = spec.id,
        .command_token = {},
        .is_plugin = false,
    });
  }

  // Plugin-contributed commands, dispatched by their command token.
  for (const std::string& name : plugin_runtime_.Host().CommandNames()) {
    if (name.empty()) {
      continue;
    }
    palette.items.push_back(CommandPaletteItem{
        .primary_label = name,
        .secondary_label = {},
        .action = ActionId::CodeActions,
        .command_token = name,
        .is_plugin = true,
    });
  }

  RefreshCommandPalette();
  ShowOverlay(OverlayMode::CommandPalette);
}

void WorkspaceShell::RefreshCommandPalette() {
  CommandPaletteState& palette = context_.current_project_state.overlay.workflow.command_palette;
  palette.matches.clear();
  palette.selected_index = 0;
  const std::string query = util::ToLowerAscii(palette.query.text());
  for (const CommandPaletteItem& item : palette.items) {
    if (!query.empty()) {
      const std::string haystack =
          util::ToLowerAscii(item.primary_label + " " + item.secondary_label);
      if (haystack.find(query) == std::string::npos) {
        continue;
      }
    }
    palette.matches.push_back(item);
  }
  palette.summary_line =
      std::to_string(palette.matches.size()) + " of " + std::to_string(palette.items.size());
  ResetOverlayScroll();
  RequestOverlayRedraw();
}

void WorkspaceShell::ConfirmCommandPaletteSelection() {
  CommandPaletteState& palette = context_.current_project_state.overlay.workflow.command_palette;
  if (palette.matches.empty() || palette.selected_index >= palette.matches.size()) {
    return;
  }
  // Copy the selected item before dismissing: the dispatched action may itself open
  // another overlay (e.g. Find File, Settings), so the palette must be gone first and
  // we must not hold a reference into state that the action could mutate.
  const CommandPaletteItem selected = palette.matches[palette.selected_index];
  DismissOverlay(true);
  if (selected.is_plugin) {
    ExecuteCommandName(selected.command_token, {}, ActionSource::Menu, nullptr);
    return;
  }
  ActionCoordinator(MakeActionContext()).Execute(selected.action, {}, ActionSource::Menu);
}

}  // namespace microide::workspace
