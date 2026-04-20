#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>

#include "project/ProjectFileScanner.h"
#include "workspace/WorkspaceProjectSearchPresentation.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

constexpr std::size_t kMaxProjectSearchResults = 200;

}  // namespace

void WorkspaceShell::RefreshProjectSearch() {
  StopProjectSearch();
  context_.current_project_state.overlay.workflow.project_search.results.clear();
  context_.current_project_state.overlay.workflow.project_search.selected_index = 0;
  context_.current_project_state.overlay.workflow.project_search.truncated = false;
  context_.current_project_state.overlay.workflow.project_search.error.clear();

  if (context_.current_project_state.root.empty() || context_.current_project_state.overlay.workflow.project_search.query.empty()) {
    ResetOverlayScroll();
    RequestSidebarRedraw();
    return;
  }

  context_.current_project_state.overlay.workflow.project_search.running = true;
  const project::ProjectFileScanMode scan_mode =
      context_.current_project_state.overlay.workflow.project_search.options.show_hidden ? project::ProjectFileScanMode::IncludeHidden
                                                           : project::ProjectFileScanMode::ExcludeHidden;
  project_search_runtime_.Start(context_.current_project_state.root, context_.current_project_state.overlay.workflow.project_search.query,
                                context_.current_project_state.overlay.workflow.project_search.options,
                                context_.current_project_state.file_index.files(scan_mode));
  ResetOverlayScroll();
  RequestSidebarRedraw();
}

void WorkspaceShell::StopProjectSearch() {
  project_search_runtime_.Stop();
  context_.current_project_state.overlay.workflow.project_search.running = false;
}

void WorkspaceShell::ConsumeProjectSearchUpdates() {
  const std::optional<project::ProjectSearchUpdate> maybe_update =
      project_search_runtime_.ConsumeActiveUpdate();
  if (!maybe_update.has_value()) {
    return;
  }
  auto update = *maybe_update;

  for (auto& result : update.results) {
    if (context_.current_project_state.overlay.workflow.project_search.results.size() >= kMaxProjectSearchResults) {
      context_.current_project_state.overlay.workflow.project_search.truncated = true;
      StopProjectSearch();
      break;
    }
    context_.current_project_state.overlay.workflow.project_search.results.push_back(std::move(result));
  }

  context_.current_project_state.overlay.workflow.project_search.truncated =
      context_.current_project_state.overlay.workflow.project_search.truncated || update.truncated;
  if (!update.error.empty()) {
    context_.current_project_state.overlay.workflow.project_search.error = std::move(update.error);
  }
  if (update.finished) {
    context_.current_project_state.overlay.workflow.project_search.running = false;
  }
  if (context_.current_project_state.overlay.visible && context_.current_project_state.overlay.mode == OverlayMode::ProjectSearch) {
    if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
      RevealOverlaySelection(ComputeOverlayRect(layout->editor_area));
    }
  }
  RequestSidebarRedraw();
}

void WorkspaceShell::BeginProjectSearchEdit(ProjectSearchEditField field) {
  context_.current_project_state.overlay.workflow.project_search.edit_field = field;
  context_.current_project_state.overlay.workflow.project_search.edit_buffer =
      field == ProjectSearchEditField::Query ? context_.current_project_state.overlay.workflow.project_search.query
                                             : context_.current_project_state.overlay.workflow.project_search.replace_text;
  context_.current_project_state.overlay.workflow.project_search.editing = true;
  RequestSidebarRedraw();
}

void WorkspaceShell::CommitProjectSearchEdit() {
  context_.current_project_state.overlay.workflow.project_search.editing = false;
  if (context_.current_project_state.overlay.workflow.project_search.edit_field == ProjectSearchEditField::Query) {
    context_.current_project_state.overlay.workflow.project_search.query = context_.current_project_state.overlay.workflow.project_search.edit_buffer;
    RefreshProjectSearch();
    return;
  }

  context_.current_project_state.overlay.workflow.project_search.replace_text = context_.current_project_state.overlay.workflow.project_search.edit_buffer;
  RequestSidebarRedraw();
}

void WorkspaceShell::CancelProjectSearchEdit() {
  context_.current_project_state.overlay.workflow.project_search.edit_buffer =
      context_.current_project_state.overlay.workflow.project_search.edit_field == ProjectSearchEditField::Query
          ? context_.current_project_state.overlay.workflow.project_search.query
          : context_.current_project_state.overlay.workflow.project_search.replace_text;
  context_.current_project_state.overlay.workflow.project_search.editing = false;
  RequestSidebarRedraw();
}

SDL_FRect WorkspaceShell::ProjectSearchQueryRect(const SDL_FRect& sidebar_rect) const {
  return MakeRect(sidebar_rect.x + 10.0f, sidebar_rect.y + kProjectSearchQueryTop,
                  std::max(0.0f, sidebar_rect.w - 20.0f), 14.0f);
}

SDL_FRect WorkspaceShell::ProjectSearchReplaceRect(const SDL_FRect& sidebar_rect) const {
  return MakeRect(sidebar_rect.x + 10.0f, sidebar_rect.y + kProjectSearchReplaceTop,
                  std::max(0.0f, sidebar_rect.w - 20.0f), 14.0f);
}

SDL_FRect WorkspaceShell::ProjectSearchModeButtonRect(const SDL_FRect& sidebar_rect) const {
  const float gap = 4.0f;
  const float available_width = std::max(0.0f, sidebar_rect.w - 20.0f - gap * 2.0f);
  const float mode_width = std::floor(available_width * 0.28f);
  return MakeRect(sidebar_rect.x + 10.0f, sidebar_rect.y + kProjectSearchButtonTop, mode_width,
                  kProjectSearchButtonHeight);
}

SDL_FRect WorkspaceShell::ProjectSearchCaseButtonRect(const SDL_FRect& sidebar_rect) const {
  const float gap = 4.0f;
  const float available_width = std::max(0.0f, sidebar_rect.w - 20.0f - gap * 2.0f);
  const float mode_width = std::floor(available_width * 0.28f);
  const float case_width = std::floor(available_width * 0.38f);
  const SDL_FRect mode_rect = ProjectSearchModeButtonRect(sidebar_rect);
  return MakeRect(mode_rect.x + mode_width + gap, mode_rect.y, case_width,
                  kProjectSearchButtonHeight);
}

SDL_FRect WorkspaceShell::ProjectSearchHiddenButtonRect(const SDL_FRect& sidebar_rect) const {
  const float gap = 4.0f;
  const float available_width = std::max(0.0f, sidebar_rect.w - 20.0f - gap * 2.0f);
  const float mode_width = std::floor(available_width * 0.28f);
  const float case_width = std::floor(available_width * 0.38f);
  const SDL_FRect case_rect = ProjectSearchCaseButtonRect(sidebar_rect);
  const float hidden_width = std::max(0.0f, available_width - mode_width - case_width);
  return MakeRect(case_rect.x + case_width + gap, case_rect.y, hidden_width,
                  kProjectSearchButtonHeight);
}

std::string WorkspaceShell::ProjectSearchModeButtonLabel() const {
  return context_.current_project_state.overlay.workflow.project_search.options.pattern_mode ==
                 project::ProjectSearchPatternMode::Regex
             ? "Rx"
             : "Lit";
}

std::string WorkspaceShell::ProjectSearchCaseButtonLabel() const {
  switch (context_.current_project_state.overlay.workflow.project_search.options.case_mode) {
    case project::ProjectSearchCaseMode::Sensitive:
      return "Case";
    case project::ProjectSearchCaseMode::Insensitive:
      return "NoCase";
    case project::ProjectSearchCaseMode::Smart:
    default:
      return "Smart";
  }
}

std::string WorkspaceShell::ProjectSearchHiddenButtonLabel() const {
  return context_.current_project_state.overlay.workflow.project_search.options.show_hidden ? "Hide+" : "Hide-";
}

bool WorkspaceShell::ProjectSearchCanReplaceAll() const {
  return context_.current_project_state.overlay.workflow.project_search.options.pattern_mode ==
             project::ProjectSearchPatternMode::Literal &&
         !context_.current_project_state.overlay.workflow.project_search.query.empty();
}

bool WorkspaceShell::ProjectSearchReplaceCaseSensitive() const {
  switch (context_.current_project_state.overlay.workflow.project_search.options.case_mode) {
    case project::ProjectSearchCaseMode::Sensitive:
      return true;
    case project::ProjectSearchCaseMode::Insensitive:
      return false;
    case project::ProjectSearchCaseMode::Smart:
    default:
      return UsesCaseSensitiveLiteralMatch(context_.current_project_state.overlay.workflow.project_search.query);
  }
}

void WorkspaceShell::ToggleProjectSearchPatternMode() {
  context_.current_project_state.overlay.workflow.project_search.options.pattern_mode =
      context_.current_project_state.overlay.workflow.project_search.options.pattern_mode ==
              project::ProjectSearchPatternMode::Literal
          ? project::ProjectSearchPatternMode::Regex
          : project::ProjectSearchPatternMode::Literal;
  RefreshProjectSearch();
  RequestSidebarRedraw();
}

void WorkspaceShell::CycleProjectSearchCaseMode() {
  switch (context_.current_project_state.overlay.workflow.project_search.options.case_mode) {
    case project::ProjectSearchCaseMode::Smart:
      context_.current_project_state.overlay.workflow.project_search.options.case_mode =
          project::ProjectSearchCaseMode::Sensitive;
      break;
    case project::ProjectSearchCaseMode::Sensitive:
      context_.current_project_state.overlay.workflow.project_search.options.case_mode =
          project::ProjectSearchCaseMode::Insensitive;
      break;
    case project::ProjectSearchCaseMode::Insensitive:
      context_.current_project_state.overlay.workflow.project_search.options.case_mode = project::ProjectSearchCaseMode::Smart;
      break;
  }
  RefreshProjectSearch();
  RequestSidebarRedraw();
}

void WorkspaceShell::ToggleProjectSearchHiddenFiles() {
  context_.current_project_state.overlay.workflow.project_search.options.show_hidden =
      !context_.current_project_state.overlay.workflow.project_search.options.show_hidden;
  RefreshProjectSearch();
  RequestSidebarRedraw();
}

void WorkspaceShell::ReplaceAllProjectSearchMatches() {
  if (context_.current_project_state.overlay.workflow.project_search.query.empty()) {
    return;
  }

  if (!ProjectSearchCanReplaceAll()) {
    return;
  }

  const bool case_sensitive = ProjectSearchReplaceCaseSensitive();
  struct PendingProjectReplace {
    std::filesystem::path relative_path;
    std::filesystem::path absolute_path;
    std::string content;
    std::size_t replacements = 0;
  };

  std::vector<PendingProjectReplace> pending;

  const std::vector<std::filesystem::path> files = project::CollectProjectFiles(
      context_.current_project_state.root, context_.current_project_state.overlay.workflow.project_search.options.show_hidden
                         ? project::ProjectFileScanMode::IncludeHidden
                         : project::ProjectFileScanMode::ExcludeHidden);
  for (const auto& relative_path : files) {
    const std::filesystem::path absolute_path = context_.current_project_state.root / relative_path;
    const std::filesystem::path normalized_absolute = absolute_path.lexically_normal();

    std::ifstream input(absolute_path, std::ios::binary);
    if (!input) {
      continue;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    std::string content = buffer.str();
    if (content.find('\0') != std::string::npos) {
      continue;
    }

    std::string updated_content = content;
    const std::size_t replacements = ReplaceLiteralMatchesInText(
        updated_content, context_.current_project_state.overlay.workflow.project_search.query,
        context_.current_project_state.overlay.workflow.project_search.replace_text, case_sensitive);
    if (replacements == 0) {
      continue;
    }

    for (std::size_t i = 0; i < context_.current_project_state.open_tabs.size(); ++i) {
      if (context_.current_project_state.open_tabs[i].kind == TabEntry::Kind::Editor &&
          context_.current_project_state.open_tabs[i].path.lexically_normal() == normalized_absolute && TabIsDirty(i)) {
        return;
      }
    }
    pending.push_back(PendingProjectReplace{
        .relative_path = relative_path,
        .absolute_path = normalized_absolute,
        .content = std::move(updated_content),
        .replacements = replacements,
    });
  }

  if (pending.empty()) {
    return;
  }

  for (const auto& change : pending) {
    std::ofstream output(change.absolute_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      return;
    }
    output.write(change.content.data(), static_cast<std::streamsize>(change.content.size()));
    if (!output.good()) {
      return;
    }

    for (std::size_t i = 0; i < context_.current_project_state.open_tabs.size(); ++i) {
      auto& tab = context_.current_project_state.open_tabs[i];
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
          tab.editor_state->views.empty()) {
        continue;
      }

      editor::TextViewport reopened_view;
      if (!reopened_view.OpenFile(change.absolute_path)) {
        continue;
      }
      ApplyEditorPreferences(reopened_view);
      bool reloaded_any = false;
      for (auto& view : tab.editor_state->views) {
        const bool active_view =
            i == context_.current_project_state.active_tab_index && view.leaf_id == tab.editor_state->active_leaf_id &&
            !view.needs_restore;
        const std::filesystem::path current_path =
            active_view ? ActiveEditorViewport()->path().lexically_normal() : EditorViewPath(view);
        if (current_path != change.absolute_path) {
          continue;
        }
        view.viewport = reopened_view;
        view.restored_path = change.absolute_path;
        view.restored_cursor_line = reopened_view.cursor_line();
        view.restored_cursor_column = reopened_view.cursor_column();
        view.restored_scroll_line = reopened_view.scroll_line();
        view.restored_horizontal_scroll = reopened_view.horizontal_scroll();
        view.needs_restore = false;
        if (active_view) {
          context_.current_project_state.text_viewport = reopened_view;
        }
        reloaded_any = true;
      }
      if (reloaded_any && i == context_.current_project_state.active_tab_index) {
        NormalizeEditorSplitTree(*tab.editor_state);
        SyncActiveEditorTabMetadata();
      }
    }
  }

  RefreshProjectFiles();
  RefreshProjectSearch();
}

std::vector<int> WorkspaceShell::BuildProjectSearchLineMap() const {
  return BuildProjectSearchResultLineMap(context_.current_project_state.overlay.workflow.project_search.results);
}

int WorkspaceShell::ProjectSearchLineForResult(std::size_t index) const {
  return FindProjectSearchResultLine(BuildProjectSearchLineMap(), index);
}

void WorkspaceShell::MoveProjectSearchSelection(int delta) {
  if (context_.current_project_state.overlay.workflow.project_search.results.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(context_.current_project_state.overlay.workflow.project_search.selected_index);
  const int max_index = static_cast<int>(context_.current_project_state.overlay.workflow.project_search.results.size()) - 1;
  context_.current_project_state.overlay.workflow.project_search.selected_index =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  if (context_.current_project_state.overlay.visible) {
    if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
      RevealOverlaySelection(ComputeOverlayRect(layout->editor_area));
    }
  }
  RequestSidebarRedraw();
}

}  // namespace microide::workspace
