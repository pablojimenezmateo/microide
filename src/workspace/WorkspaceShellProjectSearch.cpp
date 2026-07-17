#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <vector>

#include "workspace/WorkspaceProjectSearchPresentation.h"
#include "workspace/WorkspaceShellRenderPrimitives.h"
#include "workspace/WorkspaceTextSearch.h"
#include "util/PerformanceCounters.h"
#include "util/TextFileIO.h"

namespace microide::workspace {

using project::kMaxProjectSearchResults;

void WorkspaceShell::RefreshProjectSearch() {
  StopProjectSearch();
  // Starting (or clearing) a search invalidates the cached-results marker; it is
  // re-armed in ConsumeProjectSearchUpdates once this run reports `finished`.
  context_.current_project_state.overlay.workflow.project_search.searched_query.clear();
  context_.current_project_state.overlay.workflow.project_search.results.clear();
  context_.current_project_state.overlay.workflow.project_search.selected_index = 0;
  context_.current_project_state.overlay.workflow.project_search.truncated = false;
  context_.current_project_state.overlay.workflow.project_search.error.clear();
  context_.current_project_state.overlay.workflow.project_search.searched_files = 0;
  context_.current_project_state.overlay.workflow.project_search.total_files = 0;
  context_.current_project_state.overlay.workflow.project_search.total_matches = 0;
  // A fresh search resets the selection to the top of the list.
  context_.current_project_state.sidebar.scroll_row = 0;

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
                              file_snapshot.files ? file_snapshot.files->size() : 0);
  project_search_runtime_.Start(context_.current_project_state.root,
                                context_.current_project_state.overlay.workflow.project_search.query.text(),
                                context_.current_project_state.overlay.workflow.project_search.options,
                                file_snapshot.files);
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

  auto& search = context_.current_project_state.overlay.workflow.project_search;
  auto& shell_results = search.results;

  // Workers publish out of order, so remember which result is selected (by its
  // stable position identity) and restore it after the re-sort below, keeping the
  // highlighted row from jumping as later batches stream in.
  const bool had_selection = search.selected_index < shell_results.size();
  const std::size_t selected_file_index =
      had_selection ? shell_results[search.selected_index].file_index : 0;
  const std::size_t selected_line =
      had_selection ? shell_results[search.selected_index].line : 0;
  const std::size_t selected_column =
      had_selection ? shell_results[search.selected_index].column : 0;

  for (auto& result : update.results) {
    if (shell_results.size() >= kMaxProjectSearchResults) {
      break;
    }
    shell_results.push_back(std::move(result));
  }

  // Restore deterministic, file-grouped display order (the engine streams matches
  // in whatever order parallel workers find them).
  std::sort(shell_results.begin(), shell_results.end(),
            [](const project::ProjectSearchResult& lhs, const project::ProjectSearchResult& rhs) {
              if (lhs.file_index != rhs.file_index) {
                return lhs.file_index < rhs.file_index;
              }
              if (lhs.line != rhs.line) {
                return lhs.line < rhs.line;
              }
              return lhs.column < rhs.column;
            });

  if (had_selection) {
    for (std::size_t i = 0; i < shell_results.size(); ++i) {
      if (shell_results[i].file_index == selected_file_index &&
          shell_results[i].line == selected_line &&
          shell_results[i].column == selected_column) {
        search.selected_index = i;
        break;
      }
    }
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
  if (update.total_matches > 0) {
    context_.current_project_state.overlay.workflow.project_search.total_matches =
        update.total_matches;
  }
  if (!update.error.empty()) {
    context_.current_project_state.overlay.workflow.project_search.error = std::move(update.error);
  }
  if (update.finished) {
    context_.current_project_state.overlay.workflow.project_search.running = false;
    // Arm the results cache so returning to the search sidebar with this same
    // query reuses the results instead of re-running the search.
    context_.current_project_state.overlay.workflow.project_search.searched_query =
        context_.current_project_state.overlay.workflow.project_search.query.text();
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
  // The button highlights when active; "Hidden" reads as "include hidden files"
  // far more clearly than the old "Hide+/Hide-" polarity.
  return "Hidden";
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

  // Clear any prior abort/search error so it does not linger past a fresh run.
  context_.current_project_state.overlay.workflow.project_search.error.clear();

  const bool case_sensitive = ProjectSearchReplaceCaseSensitive();
  struct PendingProjectReplace {
    std::filesystem::path relative_path;
    std::filesystem::path absolute_path;
    std::string content;
    std::size_t replacements = 0;
  };

  std::vector<PendingProjectReplace> pending;

  // Replace-all validates all target files (no dirty open tab) before committing
  // any write, so it must buffer the modified contents. Bound that aggregate: a
  // project with many large matching files would otherwise hold multiple GB of
  // new content at once -> OOM. Above the ceiling we abort the whole operation
  // (no writes), consistent with the other pre-commit abort paths here.
  const std::size_t kMaxAggregateReplaceBytes = replace_all_aggregate_cap_bytes_;
  std::size_t aggregate_bytes = 0;

  // Fast path: when the just-completed search's cached results provably cover every
  // matching file, replace-all only needs to touch those files. Every other project
  // file contains zero matches of the current query, so `ReplaceLiteralMatchesInText`
  // would return 0 and skip it anyway -- re-reading and re-scanning the whole project
  // (potentially thousands of files) to rediscover the same subset is pure wasted I/O
  // on the shell thread. Eligibility requires the results to be authoritative and
  // complete: the search finished (`!running`), still describes the current query
  // (`searched_query == query`), and was neither truncated nor capped (the worker
  // flags `truncated` on any cap hit, and the consumer stops storing at
  // `kMaxProjectSearchResults`, so a capped set silently omits whole files). Outside
  // that window the cached results are an incomplete subset, so fall back to the
  // authoritative whole-project scan below.
  const ProjectSearchState& search_state =
      context_.current_project_state.overlay.workflow.project_search;
  const bool results_cover_all_matches =
      !search_state.running && !search_state.truncated &&
      search_state.results.size() < kMaxProjectSearchResults &&
      search_state.searched_query == search_state.query.text();

  std::vector<std::filesystem::path> matched_relative_paths;
  if (results_cover_all_matches) {
    // results are sorted by (file_index, line, column), so all matches for one file
    // are adjacent; collapse to one relative path per file (results hold one entry
    // per match).
    matched_relative_paths.reserve(search_state.results.size());
    for (const auto& result : search_state.results) {
      if (matched_relative_paths.empty() ||
          matched_relative_paths.back() != result.relative_path) {
        matched_relative_paths.push_back(result.relative_path);
      }
    }
  }

  // TD-2026-07-17-094: use the shared-pointer snapshot rather than SnapshotPaths(),
  // which deep-copies the entire catalog path vector on the shell thread before the
  // per-file read/replace/write loop even starts. On large projects that copy alone
  // froze the UI; SnapshotPathsWithVersion() hands back a SharedPathList (shared with
  // the index cache) that we iterate without copying. Only materialized on the
  // whole-project fallback -- the fast path above never scans the full catalog.
  project::FilePathSnapshot path_snapshot;
  if (!results_cover_all_matches) {
    path_snapshot = context_.current_project_state.file_index.SnapshotPathsWithVersion(
        search_state.options.show_hidden ? project::ProjectFileScanMode::IncludeHidden
                                         : project::ProjectFileScanMode::ExcludeHidden);
  }
  static const std::vector<std::filesystem::path> kEmptyPaths;
  const std::vector<std::filesystem::path>& files =
      results_cover_all_matches ? matched_relative_paths
      : path_snapshot.files     ? *path_snapshot.files
                                : kEmptyPaths;
  for (const auto& relative_path : files) {
    const std::filesystem::path absolute_path = context_.current_project_state.root / relative_path;
    const std::filesystem::path normalized_absolute = absolute_path.lexically_normal();

    std::string updated_content;
    // Uses the same search cap as the finder (default kMaxSearchFileBytes): a file
    // too large to search shows no matches, so replace-all must not silently rewrite
    // it either. Keeping both on one cap keeps replace scope aligned with results.
    if (!util::ReadFileForTextSearch(absolute_path, updated_content)) {
      continue;
    }

    const std::size_t replacements = ReplaceLiteralMatchesInText(
        updated_content, context_.current_project_state.overlay.workflow.project_search.query.text(),
        context_.current_project_state.overlay.workflow.project_search.replace_text.text(), case_sensitive);
    if (replacements == 0) {
      continue;
    }

    // Scan EVERY editor group, not just the focused split: a file open dirty in the
    // other split must still block replace-all, otherwise replace-all would write a
    // new copy to disk underneath that split's unsaved edits. On a blocker, surface
    // a precise error (naming the file) rather than a bare silent return.
    bool blocked_by_dirty = false;
    for (const EditorGroup& group : context_.current_project_state.editor_groups) {
      for (const TabEntry& tab : group.open_tabs) {
        if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value() &&
            tab.path.lexically_normal() == normalized_absolute &&
            tab.editor_state->viewport.dirty()) {
          blocked_by_dirty = true;
          break;
        }
      }
      if (blocked_by_dirty) {
        break;
      }
    }
    if (blocked_by_dirty) {
      context_.current_project_state.overlay.workflow.project_search.error =
          "Replace-all aborted: save open changes to " + relative_path.generic_string() + " first.";
      RequestSidebarRedraw();
      return;
    }
    aggregate_bytes += updated_content.size();
    if (aggregate_bytes > kMaxAggregateReplaceBytes) {
      // Too much modified content to buffer safely: abort without writing any
      // file rather than risk exhausting memory mid-operation. Surface it so the
      // user does not believe the replacement silently succeeded.
      context_.current_project_state.overlay.workflow.project_search.error =
          "Replace-all aborted: too much content to modify at once.";
      RequestSidebarRedraw();
      return;
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

  std::size_t failed_write_count = 0;
  for (const auto& change : pending) {
    // Atomic temp-file + rename, never an in-place truncating write: a failed write
    // (disk full, I/O error) must leave the original file intact rather than emptied
    // or half-written. On failure keep going and collect it — a bare early return
    // here left earlier files already overwritten, the current file corrupt, no error
    // surfaced, and the search state unrefreshed.
    if (!util::WriteTextFileAtomically(change.absolute_path, change.content)) {
      ++failed_write_count;
      continue;
    }

    // Refresh every clean open view of the rewritten file across ALL editor groups,
    // not just the focused split: a file open clean in the other split must not keep
    // rendering the pre-replace buffer after the on-disk write.
    for (std::size_t group_index = 0;
         group_index < context_.current_project_state.editor_groups.size(); ++group_index) {
      EditorGroup& group = context_.current_project_state.editor_groups[group_index];
      const bool is_focused_group =
          group_index == context_.current_project_state.focused_group_index;
      for (std::size_t i = 0; i < group.open_tabs.size(); ++i) {
        auto& tab = group.open_tabs[i];
        if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
          continue;
        }
        auto& editor_state = *tab.editor_state;
        if (EditorViewPath(editor_state) != change.absolute_path) {
          continue;
        }

        editor::TextViewport reopened_view;
        if (!reopened_view.OpenFile(change.absolute_path)) {
          continue;
        }
        ApplyEditorPreferences(reopened_view);
        ApplyDetectedIndentOnOpen(reopened_view);
        editor_state.viewport = reopened_view;
        editor_state.restored_path = change.absolute_path;
        editor_state.restored_cursor_line = reopened_view.cursor_line();
        editor_state.restored_cursor_column = reopened_view.cursor_column();
        editor_state.restored_scroll_line = reopened_view.scroll_line();
        editor_state.restored_horizontal_scroll = reopened_view.horizontal_scroll();
        editor_state.needs_restore = false;
        if (is_focused_group && i == group.active_tab_index) {
          SyncActiveEditorTabMetadata();
        }
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
  if (failed_write_count > 0) {
    // Surface a precise partial-apply result: some files were rewritten, some were
    // not. The successfully written files above are already reflected in the index /
    // reopened tabs; this tells the user the operation did not fully complete.
    context_.current_project_state.overlay.workflow.project_search.error =
        "Replace-all wrote " + std::to_string(pending.size() - failed_write_count) + " of " +
        std::to_string(pending.size()) + " files; " + std::to_string(failed_write_count) +
        " could not be written.";
    RequestSidebarRedraw();
  }
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
  if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
    // Pull the selected row into view in the sidebar list (mirrors the
    // git/problems/tests sidebars' RevealSelected* helpers). Only navigation
    // moves the scroll; mouse-wheel/scrollbar scrolling is left untouched so the
    // user can scroll freely past the selection.
    if (ActiveSidebarMode() == SidebarMode::Search && layout->sidebar.h > 0.0f) {
      const auto line_map = BuildProjectSearchLineMap();
      const auto list_layout =
          ComputeProjectSearchSidebarListLayout(layout->sidebar, line_map.size());
      const int selected_line = ProjectSearchLineForResult(
          context_.current_project_state.overlay.workflow.project_search.selected_index);
      context_.current_project_state.sidebar.scroll_row =
          RevealScrollableListIndex(list_layout, selected_line);
    }
    if (context_.current_project_state.overlay.visible) {
      RevealOverlaySelection(ComputeOverlayRect(layout->editor_area));
    }
  }
  RequestSidebarRedraw();
}

}  // namespace microide::workspace
