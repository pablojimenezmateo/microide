#include "workspace/shell/WorkspaceShell.h"

#include <algorithm>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "util/StringUtil.h"
#include "workspace/BufferFindScope.h"
#include "workspace/actions/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/WorkspaceUiText.h"
#include "workspace/coordinators/WorkspaceCommandLineCoordinator.h"
#include "workspace/registries/WorkspaceCommandRegistry.h"
#include "workspace/services/CompareMergeService.h"


namespace microide::workspace {

namespace {
constexpr std::size_t kFileFinderRecentLimit = 8;
}

void WorkspaceShell::ShowOverlay(OverlayMode mode) {
  RequestPreparedFrame();
  InvalidateCursorKindFingerprint();
  active_signature_help_.reset();
  if (context_.current_project_state.surface.focus != FocusTarget::Overlay) {
    context_.current_project_state.overlay.opened_from = context_.current_project_state.surface.focus;
  }
  if (context_.current_project_state.overlay.visible &&
      context_.current_project_state.overlay.mode == OverlayMode::QuickPick && mode != OverlayMode::QuickPick) {
    quick_pick_service_.Cancel();
  }
  context_.current_project_state.overlay.visible = true;
  context_.current_project_state.overlay.mode = mode;
  if (mode == OverlayMode::Completion) {
    if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
      context_.current_project_state.overlay.caret_anchor = ActiveEditorCaretRect(*layout);
    } else {
      context_.current_project_state.overlay.caret_anchor.reset();
    }
  } else {
    context_.current_project_state.overlay.caret_anchor.reset();
  }
  if (mode == OverlayMode::FileFinder) {
    const std::filesystem::path& root = context_.current_project_state.root;
    std::vector<std::filesystem::path> recent_relative;
    if (!root.empty()) {
      for (const std::filesystem::path& absolute :
           recents_service_.RecentFilesFor(root, kFileFinderRecentLimit)) {
        std::filesystem::path relative = absolute.lexically_relative(root);
        if (!relative.empty() && *relative.begin() != "..") {
          recent_relative.push_back(std::move(relative));
        }
      }
    }
    context_.current_project_state.file_finder.SetRecentRelativePaths(std::move(recent_relative));
  }
  context_.current_project_state.surface.focus = FocusTarget::Overlay;
  ResetOverlayScroll();
  RequestPreparedFrame();
}

void WorkspaceShell::DismissOverlay(bool focus_editor) {
  RequestPreparedFrame();
  InvalidateCursorKindFingerprint();
  if (context_.current_project_state.overlay.mode == OverlayMode::QuickPick) {
    quick_pick_service_.Cancel();
  }
  context_.current_project_state.overlay.workflow.file_finder_compare_left.clear();
  context_.current_project_state.overlay.workflow.file_finder_compare_title.clear();
  const bool preserve_buffer_search_expansions =
      context_.current_project_state.overlay.mode == OverlayMode::BufferSearch &&
      focus_editor;
  if (context_.current_project_state.overlay.mode == OverlayMode::BufferSearch ||
      context_.current_project_state.overlay.mode == OverlayMode::BufferReplace) {
    ResetBufferSearchFoldRevealState(preserve_buffer_search_expansions);
  }
  context_.current_project_state.overlay.visible = false;
  context_.current_project_state.surface.focus =
      focus_editor ? FocusTarget::Editor : FocusAfterOverlay(context_.current_project_state);
  RequestPreparedFrame();
}

std::string WorkspaceShell::SingleLineEditorSelectionSeed() const {
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
  const bool already_open = context_.current_project_state.overlay.visible &&
                            (context_.current_project_state.overlay.mode == OverlayMode::BufferSearch ||
                             context_.current_project_state.overlay.mode == OverlayMode::BufferReplace);
  std::string seed = SingleLineEditorSelectionSeed();
  if (already_open && !buffer_search.matches.empty() &&
      buffer_search.selected_index < buffer_search.matches.size()) {
    if (const editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
      const auto selection = viewport->selection_range();
      const editor::SelectionRange& current = buffer_search.matches[buffer_search.selected_index];
      if (selection.has_value() && selection->start.line == current.start.line &&
          selection->start.column == current.start.column && selection->end.line == current.end.line &&
          selection->end.column == current.end.column) {
        seed.clear();
      }
    }
  }
  if (!already_open) {
    ResetBufferSearchFoldRevealState(false);
    buffer_search.scope.reset();
    buffer_search.opened_over.reset();
    if (const editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
      buffer_search.opened_over = FindScopeAt(*viewport);
    }
    if (const editor::TextViewport* viewport = ActiveEditorViewport();
        seed.empty() && viewport != nullptr && !viewport->selection_range().has_value()) {
      if (const auto word = viewport->WordRangeAt({viewport->cursor_line(), viewport->cursor_column()});
          word.has_value() && word->start.line == word->end.line) {
        seed = std::string(viewport->lines().LineView(word->start.line)
                               .substr(word->start.column, word->end.column - word->start.column));
      }
    }
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

void WorkspaceShell::ResetOverlayScroll() {
  context_.current_project_state.overlay.scroll_row = 0;
  RequestPreparedFrame();
}

float WorkspaceShell::OverlayListStartOffset() const {
  return workspace::OverlayListStartOffset(context_.current_project_state.overlay.mode);
}

ScrollableListLayout WorkspaceShell::ComputeOverlayListLayout(const SDL_FRect& overlay) const {
  return ComputeScrollableListLayout(overlay, overlay.y + OverlayListStartOffset(),
                                     OverlayItemCount(), context_.current_project_state.overlay.scroll_row, 18.0f,
                                     22.0f, 18.0f, 16.0f, 8.0f);
}

std::size_t WorkspaceShell::OverlayItemCount() const {
  switch (context_.current_project_state.overlay.mode) {
    case OverlayMode::CommitPicker:
      return context_.current_project_state.overlay.workflow.compare_picker.matches.size();
    case OverlayMode::QuickPick:
      return context_.current_project_state.overlay.workflow.quick_pick.matches.size();
    case OverlayMode::CommandPalette:
      return context_.current_project_state.overlay.workflow.command_palette.matches.size();
    case OverlayMode::BufferSearch:
    case OverlayMode::BufferReplace:
      return context_.current_project_state.overlay.workflow.buffer_search.matches.size();
    case OverlayMode::Completion:
      return context_.current_project_state.overlay.workflow.completion.VisibleCount();
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
    case OverlayMode::QuickPick:
      return context_.current_project_state.overlay.workflow.quick_pick.selected_index;
    case OverlayMode::CommandPalette:
      return context_.current_project_state.overlay.workflow.command_palette.selected_index;
    case OverlayMode::BufferSearch:
    case OverlayMode::BufferReplace:
      return context_.current_project_state.overlay.workflow.buffer_search.selected_index;
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
    case OverlayMode::QuickPick:
      context_.current_project_state.overlay.workflow.quick_pick.selected_index =
          clamped_index;
      quick_pick_service_.SelectionChanged();
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
          viewport->JumpCursorTo(match.start.line, match.start.column);
        }
      }
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
  RequestPreparedFrame();
  if (context_.current_project_state.overlay.mode == OverlayMode::BufferSearch ||
      context_.current_project_state.overlay.mode == OverlayMode::BufferReplace) {
    RequestPreparedFrame();
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
  RequestPreparedFrame();
}

bool WorkspaceShell::ActivateOverlaySelection() {
  switch (context_.current_project_state.overlay.mode) {
    case OverlayMode::CommitPicker:
      MakeCompareMergeService().interactions().OpenSelectedCommit();
      DismissOverlay(true);
      return true;
    case OverlayMode::QuickPick: {
      const std::uint64_t opened = quick_pick_service_.open_count();
      quick_pick_service_.Accept();
      if (context_.current_project_state.overlay.visible &&
          context_.current_project_state.overlay.mode == OverlayMode::QuickPick &&
          quick_pick_service_.open_count() == opened) {
        DismissOverlay(true);
      }
      return true;
    }
    case OverlayMode::CommandPalette:
      ConfirmCommandPaletteSelection();
      return true;
    case OverlayMode::BufferSearch:
      if (!context_.current_project_state.overlay.workflow.buffer_search.matches.empty()) {
        const auto& match = context_.current_project_state.overlay.workflow.buffer_search.matches[context_.current_project_state.overlay.workflow.buffer_search.selected_index];
        if (editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
          viewport->JumpCursorTo(match.start.line, match.start.column);
        }
      }
      DismissOverlay(true);
      return true;
    case OverlayMode::BufferReplace:
      ReplaceCurrentBufferSearchMatch();
      return true;
    case OverlayMode::Completion:
      return assist_service_.completion().ApplySelectedCompletion();
    case OverlayMode::CodeActions:
      return assist_service_.code_actions().ExecuteSelected();
    case OverlayMode::FileFinder:
    default:
      if (const auto selected = context_.current_project_state.file_finder.SelectedPath(); selected.has_value()) {
        const std::filesystem::path path = context_.current_project_state.root / *selected;
        OverlayWorkflowState& workflow = context_.current_project_state.overlay.workflow;
        if (!workflow.file_finder_compare_left.empty()) {
          const std::filesystem::path left = std::exchange(workflow.file_finder_compare_left, {});
          workflow.file_finder_compare_title.clear();
          DismissOverlay(true);
          (void)ExecuteCommandName("compare-files", {left.string(), path.string()}, ActionSource::Command, nullptr);
          return true;
        }
        if (const auto target = context_.current_project_state.file_finder.line_target();
            target.has_value()) {
          DismissOverlay(true);
          (void)ExecuteCommandName("reveal",
                                   {path.string(), std::to_string(target->line) +
                                                       (target->column > 0 ? ":" + std::to_string(target->column)
                                                                           : std::string{})},
                                   ActionSource::Command, nullptr);
          return true;
        }
        OpenFile(path);
      }
      DismissOverlay(true);
      return true;
  }
}

void WorkspaceShell::OpenCommandPalette(std::string seed) {
  CommandPaletteState& palette = context_.current_project_state.overlay.workflow.command_palette;
  palette.query.SetText(seed);
  palette.items.clear();

  const std::span<const ActionSpec> specs = WorkspaceCommandSpecs();
  palette.items.reserve(specs.size());
  for (const ActionSpec& spec : specs) {
    if (spec.label.empty() || spec.id == ActionId::OpenCommandPalette) {
      continue;
    }
    palette.items.push_back(CommandPaletteItem{
        .primary_label = std::string(spec.label),
        .secondary_label = std::string(spec.accelerator),
        .search_text = util::ToLowerAscii(std::string(spec.command_name) + " " +
                                          std::string(spec.label) + " " +
                                          std::string(spec.accelerator)),
        .command_name = util::ToLowerAscii(spec.command_name),
        .action = spec.id,
        .command_token = {},
        .is_plugin = false,
    });
  }

  for (const std::string& name : plugin_runtime_.Host().CommandNames()) {
    if (name.empty()) {
      continue;
    }
    palette.items.push_back(CommandPaletteItem{
        .primary_label = name,
        .secondary_label = {},
        .search_text = util::ToLowerAscii(name),
        .command_name = util::ToLowerAscii(name),
        .action = ActionId::CodeActions,
        .command_token = name,
        .is_plugin = true,
    });
  }

  RefreshCommandPalette();
  ShowOverlay(OverlayMode::CommandPalette);
}

namespace {
std::string_view PaletteQueryText(const CommandPaletteState& palette) {
  std::string_view text = palette.query.text();
  if (text.starts_with('>')) {
    text.remove_prefix(1);
    while (!text.empty() && text.front() == ' ') {
      text.remove_prefix(1);
    }
  }
  return text;
}
}

void WorkspaceShell::RefreshCommandPalette() {
  CommandPaletteState& palette = context_.current_project_state.overlay.workflow.command_palette;
  palette.matches.clear();
  palette.selected_index = 0;
  const std::string query = util::ToLowerAscii(PaletteQueryText(palette));
  const std::vector<std::string_view> query_words = util::SplitAsciiWhitespace(query);
  for (std::size_t i = 0; i < palette.items.size(); ++i) {
    const std::string& text = palette.items[i].search_text;
    bool every_word_found = true;
    for (const std::string_view word : query_words) {
      if (text.find(word) == std::string::npos) {
        every_word_found = false;
        break;
      }
    }
    if (!every_word_found) {
      continue;
    }
    palette.matches.push_back(i);
  }
  const auto recency = [&palette](std::size_t index) {
    const std::string& key = CommandPaletteRecentKey(palette.items[index]);
    const auto found = std::find(palette.recent.begin(), palette.recent.end(), key);
    return static_cast<std::size_t>(found - palette.recent.begin());
  };
  if (query.empty() && !palette.recent.empty()) {
    std::stable_sort(palette.matches.begin(), palette.matches.end(),
                     [&](std::size_t lhs, std::size_t rhs) { return recency(lhs) < recency(rhs); });
  }
  if (!query.empty()) {
    const auto rank = [&](std::size_t index) {
      const CommandPaletteItem& item = palette.items[index];
      if (item.command_name == query) {
        return 0;
      }
      if (item.command_name.rfind(query, 0) == 0) {
        return 1;
      }
      if (item.search_text.rfind(query, 0) == 0) {
        return 2;
      }
      return item.search_text.find(query) != std::string::npos ? 3 : 4;
    };
    std::stable_sort(palette.matches.begin(), palette.matches.end(), [&](std::size_t lhs, std::size_t rhs) {
      const int lhs_rank = rank(lhs);
      const int rhs_rank = rank(rhs);
      return lhs_rank != rhs_rank ? lhs_rank < rhs_rank : recency(lhs) < recency(rhs);
    });
  }
  palette.summary_line =
      BuildFilteredCountSummary(palette.matches.size(), palette.items.size(), "commands");
  ResetOverlayScroll();
  RequestPreparedFrame();
}

bool WorkspaceShell::CommandPaletteQueryIsCommandLine() const {
  const CommandPaletteState& palette =
      context_.current_project_state.overlay.workflow.command_palette;
  const ParsedCommandLine parsed = ParseCommandLine(PaletteQueryText(palette));
  if (parsed.tokens.empty() || parsed.open_quote != '\0') {
    return false;
  }
  return parsed.tokens.size() > 1 || palette.matches.empty();
}

void WorkspaceShell::ConfirmCommandPaletteSelection() {
  CommandPaletteState& palette = context_.current_project_state.overlay.workflow.command_palette;
  bool verb_with_arguments = false;
  {
    const ParsedCommandLine parsed = ParseCommandLine(PaletteQueryText(palette));
    if (parsed.tokens.size() > 1 && parsed.open_quote == '\0') {
      const std::string verb = util::ToLowerAscii(parsed.tokens.front().text);
      for (const CommandPaletteItem& item : palette.items) {
        if (item.command_name == verb) {
          verb_with_arguments = true;
          break;
        }
      }
    }
    if (verb_with_arguments && palette.selected_index < palette.matches.size()) {
      const std::string& label = palette.items[palette.matches[palette.selected_index]].primary_label;
      const std::string query = util::ToLowerAscii(PaletteQueryText(palette));
      verb_with_arguments = util::ToLowerAscii(label).rfind(query, 0) != 0;
    }
  }
  if (!verb_with_arguments && !palette.matches.empty() &&
      palette.selected_index < palette.matches.size()) {
    const CommandPaletteItem selected = palette.items[palette.matches[palette.selected_index]];
    {
      const std::string key = CommandPaletteRecentKey(selected);
      std::erase(palette.recent, key);
      palette.recent.insert(palette.recent.begin(), key);
      if (palette.recent.size() > kCommandPaletteRecentCap) {
        palette.recent.resize(kCommandPaletteRecentCap);
      }
    }
    DismissOverlay(true);
    if (selected.is_plugin) {
      ExecuteCommandName(selected.command_token, {}, ActionSource::Menu, nullptr);
      return;
    }
    ActionCoordinator(MakeActionContext()).Execute(selected.action, {}, ActionSource::Menu);
    return;
  }

  if (CommandPaletteQueryIsCommandLine()) {
    const std::string command_line(PaletteQueryText(palette));
    DismissOverlay(true);
    const bool ok = MakeCommandLineCoordinator().ExecuteCommandLine(command_line);
    if (const std::string& feedback = context_.current_project_state.panel.feedback.text;
        !feedback.empty()) {
      Notify(ok ? NotificationService::Tone::Info : NotificationService::Tone::Error, feedback);
    }
  }
}

void WorkspaceShell::CompleteCommandPaletteQuery() {
  CommandPaletteState& palette = context_.current_project_state.overlay.workflow.command_palette;
  MakeCommandLineCoordinator().CompleteInput(palette.query);
  RefreshCommandPalette();
}

}
