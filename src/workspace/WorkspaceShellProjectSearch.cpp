#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>

#include "workspace/WorkspaceProjectSearchPresentation.h"
#include "workspace/WorkspaceShellRenderPrimitives.h"
#include "workspace/WorkspaceTextSearch.h"
#include "util/PerformanceCounters.h"

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
  context_.current_project_state.overlay.workflow.project_search.searched_files = 0;
  context_.current_project_state.overlay.workflow.project_search.total_files = 0;

  if (context_.current_project_state.root.empty() ||
      context_.current_project_state.overlay.workflow.project_search.query.text().empty()) {
    ResetOverlayScroll();
    RequestSidebarRedraw();
    return;
  }

  context_.current_project_state.overlay.workflow.project_search.running = true;
  const project::ProjectFileScanMode scan_mode =
      context_.current_project_state.overlay.workflow.project_search.options.show_hidden ? project::ProjectFileScanMode::IncludeHidden
                                                           : project::ProjectFileScanMode::ExcludeHidden;
  const project::FilePathSnapshot file_snapshot =
      context_.current_project_state.file_index.SnapshotPathsWithVersion(scan_mode);
  util::AddPerformanceCounter(util::PerfCounterId::SearchProjectCandidateFilesFromIndex,
                              file_snapshot.files.size());
  project_search_runtime_.Start(context_.current_project_state.root,
                                context_.current_project_state.overlay.workflow.project_search.query.text(),
                                context_.current_project_state.overlay.workflow.project_search.options,
                                std::move(file_snapshot.files));
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

  context_.current_project_state.overlay.workflow.project_search.results = std::move(update.results);
  if (context_.current_project_state.overlay.workflow.project_search.results.size() >
      kMaxProjectSearchResults) {
    context_.current_project_state.overlay.workflow.project_search.results.resize(
        kMaxProjectSearchResults);
  }

  context_.current_project_state.overlay.workflow.project_search.truncated =
      context_.current_project_state.overlay.workflow.project_search.truncated || update.truncated;
  if (update.total_files > 0) {
    // The service always publishes the latest counters on each update; reflect
    // them as-is so "Searching X matches (Y of Z files)" stays current.
    context_.current_project_state.overlay.workflow.project_search.searched_files =
        update.searched_files;
    context_.current_project_state.overlay.workflow.project_search.total_files =
        update.total_files;
  }
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
  context_.current_project_state.overlay.workflow.project_search.edit_buffer.SetText(
      field == ProjectSearchEditField::Query
          ? context_.current_project_state.overlay.workflow.project_search.query.text()
          : context_.current_project_state.overlay.workflow.project_search.replace_text.text());
  context_.current_project_state.overlay.workflow.project_search.editing = true;
  RequestSidebarRedraw();
}

void WorkspaceShell::CommitProjectSearchEdit() {
  context_.current_project_state.overlay.workflow.project_search.editing = false;
  if (context_.current_project_state.overlay.workflow.project_search.edit_field == ProjectSearchEditField::Query) {
    context_.current_project_state.overlay.workflow.project_search.query.SetText(
        context_.current_project_state.overlay.workflow.project_search.edit_buffer.text());
    RefreshProjectSearch();
    return;
  }

  context_.current_project_state.overlay.workflow.project_search.replace_text.SetText(
      context_.current_project_state.overlay.workflow.project_search.edit_buffer.text());
  RequestSidebarRedraw();
}

void WorkspaceShell::CancelProjectSearchEdit() {
  context_.current_project_state.overlay.workflow.project_search.edit_buffer.SetText(
      context_.current_project_state.overlay.workflow.project_search.edit_field ==
              ProjectSearchEditField::Query
          ? context_.current_project_state.overlay.workflow.project_search.query.text()
          : context_.current_project_state.overlay.workflow.project_search.replace_text.text());
  context_.current_project_state.overlay.workflow.project_search.editing = false;
  RequestSidebarRedraw();
}

SDL_FRect WorkspaceShell::ProjectSearchQueryRect(const SDL_FRect& sidebar_rect) const {
  return MakeRect(sidebar_rect.x + 10.0f, sidebar_rect.y + kProjectSearchQueryTop,
                  std::max(0.0f, sidebar_rect.w - 20.0f), 20.0f);
}

SDL_FRect WorkspaceShell::ProjectSearchReplaceRect(const SDL_FRect& sidebar_rect) const {
  return MakeRect(sidebar_rect.x + 10.0f, sidebar_rect.y + kProjectSearchReplaceTop,
                  std::max(0.0f, sidebar_rect.w - 20.0f), 20.0f);
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

std::string WorkspaceShell::HoveredSidebarSearchTooltipLabel(
    const SDL_FRect& sidebar_rect) const {
  if (!last_mouse_position_valid_ || !context_.current_project_state.sidebar.visible ||
      ActiveSidebarMode() != SidebarMode::Search || MenuSurfaceCapturingMouse() ||
      !Contains(sidebar_rect, last_mouse_x_, last_mouse_y_)) {
    return {};
  }

  const auto& options =
      context_.current_project_state.overlay.workflow.project_search.options;
  if (Contains(ProjectSearchModeButtonRect(sidebar_rect), last_mouse_x_, last_mouse_y_)) {
    return options.pattern_mode == project::ProjectSearchPatternMode::Regex
               ? "Pattern: regex (click for literal)"
               : "Pattern: literal (click for regex)";
  }
  if (Contains(ProjectSearchCaseButtonRect(sidebar_rect), last_mouse_x_, last_mouse_y_)) {
    switch (options.case_mode) {
      case project::ProjectSearchCaseMode::Sensitive:
        return "Case: sensitive (click to cycle)";
      case project::ProjectSearchCaseMode::Insensitive:
        return "Case: insensitive (click to cycle)";
      case project::ProjectSearchCaseMode::Smart:
      default:
        return "Case: smart (click to cycle)";
    }
  }
  if (Contains(ProjectSearchHiddenButtonRect(sidebar_rect), last_mouse_x_, last_mouse_y_)) {
    return options.show_hidden ? "Searching hidden files (click to skip)"
                               : "Skipping hidden files (click to include)";
  }
  return {};
}

std::optional<SDL_FRect> WorkspaceShell::HoveredSidebarSearchTooltipRect(
    const WorkspaceLayout& layout) const {
  const std::string label = HoveredSidebarSearchTooltipLabel(layout.sidebar);
  if (label.empty()) {
    return std::nullopt;
  }
  const auto tooltip = detail::BuildTooltipLayout(
      text_renderer_, label, std::max(180.0f, layout.full.w - layout.sidebar.w - 24.0f));
  const float tooltip_x =
      std::clamp(last_mouse_x_ + 12.0f, layout.full.x + 8.0f,
                 layout.full.x + layout.full.w - tooltip.rect.w - 8.0f);
  const float tooltip_y =
      last_mouse_y_ - tooltip.rect.h - 10.0f >= layout.full.y + 8.0f
          ? last_mouse_y_ - tooltip.rect.h - 10.0f
          : std::clamp(last_mouse_y_ + 14.0f, layout.full.y + 8.0f,
                       layout.full.y + layout.full.h - tooltip.rect.h - 8.0f);
  return MakeRect(tooltip_x, tooltip_y, tooltip.rect.w, tooltip.rect.h);
}

bool WorkspaceShell::ProjectSearchCanReplaceAll() const {
  return context_.current_project_state.overlay.workflow.project_search.options.pattern_mode ==
             project::ProjectSearchPatternMode::Literal &&
         !context_.current_project_state.overlay.workflow.project_search.query.text().empty();
}

bool WorkspaceShell::ProjectSearchReplaceCaseSensitive() const {
  switch (context_.current_project_state.overlay.workflow.project_search.options.case_mode) {
    case project::ProjectSearchCaseMode::Sensitive:
      return true;
    case project::ProjectSearchCaseMode::Insensitive:
      return false;
    case project::ProjectSearchCaseMode::Smart:
    default:
      return UsesCaseSensitiveLiteralMatch(context_.current_project_state.overlay.workflow.project_search.query.text());
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
  if (context_.current_project_state.overlay.workflow.project_search.query.text().empty()) {
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

  const std::vector<std::filesystem::path> files = context_.current_project_state.file_index.SnapshotPaths(
      context_.current_project_state.overlay.workflow.project_search.options.show_hidden
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
        updated_content, context_.current_project_state.overlay.workflow.project_search.query.text(),
        context_.current_project_state.overlay.workflow.project_search.replace_text.text(), case_sensitive);
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
      ApplyDetectedIndentOnOpen(reopened_view);
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
          context_.current_project_state.welcome_surface.viewport = reopened_view;
        }
        reloaded_any = true;
      }
      if (reloaded_any && i == context_.current_project_state.active_tab_index) {
        NormalizeEditorSplitTree(*tab.editor_state);
        SyncActiveEditorTabMetadata();
      }
    }
  }

  platform::IndexUpdateBatch metadata_updates;
  metadata_updates.is_initial = false;
  metadata_updates.changes.reserve(pending.size());
  for (const auto& change : pending) {
    std::error_code status_error;
    const auto status = std::filesystem::status(change.absolute_path, status_error);
    if (status_error || !std::filesystem::is_regular_file(status)) {
      continue;
    }
    std::error_code mtime_error;
    const auto mtime = std::filesystem::last_write_time(change.absolute_path, mtime_error);
    std::error_code size_error;
    const auto size = std::filesystem::file_size(change.absolute_path, size_error);
    metadata_updates.changes.push_back(platform::IndexUpdateBatch::Change{
        .kind = platform::IndexUpdateBatch::Kind::CreatedOrModified,
        .entry = platform::IndexFileEntry{
            .relative_path = change.relative_path,
            .mtime = mtime_error ? std::filesystem::file_time_type{} : mtime,
            .size = size_error ? 0 : size,
        },
    });
  }
  if (!metadata_updates.changes.empty() &&
      context_.current_project_state.file_index.ApplyBatch(metadata_updates)) {
    file_index_has_pending_changes_.store(true, std::memory_order_release);
  }
  context_.current_project_state.directory_tree.Refresh();
  context_.current_project_state.file_finder.InvalidateIndexCache();
  if (context_.current_project_state.overlay.visible &&
      context_.current_project_state.overlay.mode == OverlayMode::FileFinder) {
    context_.current_project_state.file_finder.Refresh();
  }
  RequestAutomaticGitSidebarRefresh();
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
