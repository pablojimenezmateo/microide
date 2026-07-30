#include "workspace/WorkspaceShell.h"

#include "workspace/ProjectSearchPanelLayout.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

#include "project/GlobMatch.h"
#include "workspace/WorkspaceProjectSearchPresentation.h"
#include "workspace/WorkspaceShellRenderPrimitives.h"
#include "workspace/WorkspaceTextSearch.h"
#include "util/PerformanceCounters.h"
#include "util/TextFileIO.h"

namespace microide::workspace {

using project::kMaxProjectSearchResults;

namespace {

// Pure read/replace/buffer + atomic-write pass for a project-wide replace-all, run
// on the background executor (touches only its arguments + the filesystem, never
// shell state). Buffers every modified file's new content before committing any
// write so an aggregate-cap or open-dirty-file abort leaves the tree untouched
// (matching the old synchronous behaviour). TD-2026-07-17-021 / TD-2026-07-16-21.
ProjectReplaceOutcome RunProjectReplace(const std::filesystem::path& root,
                                        const std::vector<std::filesystem::path>& files,
                                        const std::unordered_set<std::string>& dirty_open,
                                        const std::string& query,
                                        const std::string& replace_text, bool case_sensitive,
                                        const util::CompiledRegex* regex,
                                        std::size_t aggregate_cap_bytes) {
  ProjectReplaceOutcome outcome;
  struct Buffered {
    std::filesystem::path relative_path;
    std::filesystem::path absolute_path;
    std::string content;
    std::size_t replacements = 0;
  };
  std::vector<Buffered> pending;
  std::size_t aggregate_bytes = 0;

  for (const auto& relative_path : files) {
    const std::filesystem::path absolute_path = (root / relative_path).lexically_normal();
    std::string content;
    // Same read cap as the finder/search: a file too large to search shows no
    // matches, so replace-all must not silently rewrite it either.
    if (!util::ReadFileForTextSearch(absolute_path, content)) {
      continue;
    }
    std::size_t replacements = 0;
    if (regex != nullptr) {
      // Regex mode: per-line substitution with capture-group expansion. A negative
      // result means an invalid replacement escape / match-limit hit — deterministic
      // across every file, so abort the whole operation with a precise message.
      const std::optional<std::size_t> count =
          ReplaceRegexMatchesInText(content, *regex, replace_text);
      if (!count.has_value()) {
        outcome.status = ProjectReplaceOutcome::Status::InvalidReplacement;
        return outcome;
      }
      replacements = *count;
    } else {
      replacements = ReplaceLiteralMatchesInText(content, query, replace_text, case_sensitive);
    }
    if (replacements == 0) {
      continue;
    }
    // Refuse to overwrite a file open with unsaved edits (snapshot taken on the main
    // thread before dispatch). Abort the whole operation — no writes — as before.
    if (dirty_open.count(absolute_path.string()) != 0) {
      outcome.status = ProjectReplaceOutcome::Status::BlockedByDirty;
      outcome.blocked_relative_path = relative_path.generic_string();
      return outcome;
    }
    aggregate_bytes += content.size();
    if (aggregate_bytes > aggregate_cap_bytes) {
      outcome.status = ProjectReplaceOutcome::Status::CapExceeded;
      return outcome;
    }
    pending.push_back(Buffered{.relative_path = relative_path,
                               .absolute_path = absolute_path,
                               .content = std::move(content),
                               .replacements = replacements});
  }

  if (pending.empty()) {
    outcome.status = ProjectReplaceOutcome::Status::NothingToDo;
    return outcome;
  }

  outcome.status = ProjectReplaceOutcome::Status::Applied;
  outcome.written.reserve(pending.size());
  for (auto& buffered : pending) {
    // Atomic temp-file + rename: a failed write leaves the original intact. Keep
    // going and collect failures rather than leaving a partial half-applied set.
    if (!util::WriteTextFileAtomically(buffered.absolute_path, buffered.content)) {
      ++outcome.failed_write_count;
      continue;
    }
    outcome.written.push_back(ProjectReplaceOutcome::WrittenFile{
        .relative_path = std::move(buffered.relative_path),
        .absolute_path = std::move(buffered.absolute_path),
        .replacements = buffered.replacements});
  }
  return outcome;
}

}  // namespace

void WorkspaceShell::RefreshProjectSearch() {
  StopProjectSearch();
  // Starting (or clearing) a search invalidates the cached-results marker; it is
  // re-armed in ConsumeProjectSearchUpdates once this run reports `finished`.
  context_.current_project_state.overlay.workflow.project_search.searched_query.clear();
  context_.current_project_state.overlay.workflow.project_search.results.clear();
  ++context_.current_project_state.overlay.workflow.project_search.results_revision;
  context_.current_project_state.overlay.workflow.project_search.selected_index = 0;
  context_.current_project_state.overlay.workflow.project_search.truncated = false;
  context_.current_project_state.overlay.workflow.project_search.index_incomplete = false;
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
  // Pin the catalog's completeness at search start: if the index is only a prefix
  // of the tree, the search can't have seen every file (TD-2026-07-17-008/033).
  context_.current_project_state.overlay.workflow.project_search.index_incomplete =
      context_.current_project_state.file_index.truncated();
  util::AddPerformanceCounter(util::PerfCounterId::SearchProjectCandidateFilesFromIndex,
                              file_snapshot.files ? file_snapshot.files->size() : 0);
  // Mirror the committed scope editors into the options the worker sees. Held on
  // the options (not re-read from the editors) so an in-flight run keeps the scope
  // it started with.
  project::ProjectSearchOptions options =
      context_.current_project_state.overlay.workflow.project_search.options;
  options.include_globs =
      context_.current_project_state.overlay.workflow.project_search.include_globs.text();
  options.exclude_globs =
      context_.current_project_state.overlay.workflow.project_search.exclude_globs.text();
  project_search_runtime_.Start(context_.current_project_state.root,
                                context_.current_project_state.overlay.workflow.project_search.query.text(),
                                std::move(options), file_snapshot.files);
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
  // Results just changed (appended + re-sorted); invalidate the cached line map.
  ++search.results_revision;

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
  auto& search = context_.current_project_state.overlay.workflow.project_search;
  search.edit_field = field;
  search.edit_buffer.SetText(ProjectSearchFieldEditor(search, field).text());
  search.editing = true;
  RequestSidebarRedraw();
}

void WorkspaceShell::CommitProjectSearchEdit() {
  auto& search = context_.current_project_state.overlay.workflow.project_search;
  search.editing = false;
  const ProjectSearchEditField field = search.edit_field;
  ProjectSearchFieldEditor(search, field).SetText(search.edit_buffer.text());
  // Query and scope globs all change WHICH files match, so committing any of them
  // re-runs the search. Replace text only affects a later replace-all.
  if (field == ProjectSearchEditField::Replace) {
    RequestSidebarRedraw();
    return;
  }
  RefreshProjectSearch();
}

void WorkspaceShell::CancelProjectSearchEdit() {
  auto& search = context_.current_project_state.overlay.workflow.project_search;
  search.edit_buffer.SetText(ProjectSearchFieldEditor(search, search.edit_field).text());
  search.editing = false;
  RequestSidebarRedraw();
}

bool WorkspaceShell::ProjectSearchScopeExpanded() const {
  return context_.current_project_state.overlay.workflow.project_search.scope_expanded;
}

void WorkspaceShell::ToggleProjectSearchScopeExpanded() {
  auto& search = context_.current_project_state.overlay.workflow.project_search;
  search.scope_expanded = !search.scope_expanded;
  if (!search.scope_expanded && search.editing &&
      (search.edit_field == ProjectSearchEditField::Include ||
       search.edit_field == ProjectSearchEditField::Exclude)) {
    // Collapsing while a scope field is focused would strand the caret on a
    // surface that is no longer drawn; commit the edit so focus returns to the
    // panel and any glob change takes effect.
    CommitProjectSearchEdit();
  }
  RequestSidebarRedraw();
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

bool WorkspaceShell::ProjectSearchCanReplaceAll() const {
  // Both literal and regex modes support replace-all (regex substitutes with
  // capture-group expansion). The only gate is a non-empty query.
  return !context_.current_project_state.overlay.workflow.project_search.query.text().empty();
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
  ProjectSearchState& ps = context_.current_project_state.overlay.workflow.project_search;
  if (ps.query.text().empty() || !ProjectSearchCanReplaceAll()) {
    return;
  }
  // Clear any prior abort/search error so it does not linger past a fresh run.
  ps.error.clear();

  const std::filesystem::path root = context_.current_project_state.root;
  if (root.empty()) {
    return;
  }

  // Determine the target file set on the main thread. Fast path: when the just-
  // completed search's cached results provably cover every matching file, only those
  // files can contain a match, so touch just them (see TD-2026-07-16-21's candidate-
  // set reduction). Eligibility requires an authoritative, complete result set:
  // finished (!running), same query, and neither truncated nor capped. Otherwise fall
  // back to the whole indexed catalog. Materialize a value the background job owns.
  const bool results_cover_all_matches =
      !ps.running && !ps.truncated && ps.results.size() < kMaxProjectSearchResults &&
      ps.searched_query == ps.query.text();
  std::vector<std::filesystem::path> files;
  if (results_cover_all_matches) {
    files.reserve(ps.results.size());
    for (const auto& result : ps.results) {
      if (files.empty() || files.back() != result.relative_path) {
        files.push_back(result.relative_path);
      }
    }
  } else {
    const project::FilePathSnapshot snapshot =
        context_.current_project_state.file_index.SnapshotPathsWithVersion(
            ps.options.show_hidden ? project::ProjectFileScanMode::IncludeHidden
                                   : project::ProjectFileScanMode::ExcludeHidden);
    if (snapshot.files) {
      files = *snapshot.files;  // copy out: the shared cache pointer must not cross threads
    }
  }
  // Replace-all must honor the same scope the search did. The fast path above is
  // already scoped (it is built from scoped results), but the whole-catalog
  // fallback is not — without this, replace-all would rewrite exactly the files
  // the user excluded and whose matches were never shown.
  const project::GlobSet include_globs = project::GlobSet::Parse(ps.include_globs.text());
  const project::GlobSet exclude_globs = project::GlobSet::Parse(ps.exclude_globs.text());
  if (!include_globs.empty() || !exclude_globs.empty()) {
    std::erase_if(files, [&](const std::filesystem::path& relative_path) {
      const std::string text = relative_path.generic_string();
      return (!include_globs.empty() && !include_globs.Matches(text)) ||
             (!exclude_globs.empty() && exclude_globs.Matches(text));
    });
  }
  if (files.empty()) {
    return;
  }

  // Snapshot the open dirty files (absolute, normalized) so the background can refuse
  // to overwrite unsaved edits without reading editor state off-thread. Scans EVERY
  // editor group, not just the focused split.
  std::unordered_set<std::string> dirty_open;
  for (const EditorGroup& group : context_.current_project_state.editor_groups) {
    for (const TabEntry& tab : group.open_tabs) {
      if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value() &&
          tab.editor_state->viewport.dirty()) {
        dirty_open.insert(tab.path.lexically_normal().string());
      }
    }
  }

  const std::string query = ps.query.text();
  const std::string replace_text = ps.replace_text.text();
  const bool case_sensitive = ProjectSearchReplaceCaseSensitive();
  const bool regex_mode =
      ps.options.pattern_mode == project::ProjectSearchPatternMode::Regex;

  // Regex mode: compile once on the main thread so an invalid pattern surfaces
  // immediately (before dispatch). The compiled pattern is a shared_ptr-backed,
  // thread-safe-const value shared read-only by the background job.
  std::shared_ptr<util::CompiledRegex> regex;
  if (regex_mode) {
    const uint32_t regex_options = util::SearchRegexCompileOptions(query, case_sensitive);
    regex = std::make_shared<util::CompiledRegex>(query, regex_options,
                                                  "Invalid project search pattern");
    if (!regex->valid()) {
      ps.error = regex->error();
      RequestSidebarRedraw();
      return;
    }
  }

  const std::size_t aggregate_cap = replace_all_aggregate_cap_bytes_;
  const std::uint64_t generation = ++project_replace_generation_;

  // Run the read/replace/buffer/atomic-write off the shell thread; apply the outcome
  // (reload clean tabs, refresh index/tree/finder, status) back on the main thread.
  // PostLatest so a burst of replace-all clicks only runs the newest queued job.
  project_background_executor_.PostLatest(
      "project-replace-all",
      [this, files = std::move(files), dirty_open = std::move(dirty_open), query, replace_text,
       case_sensitive, regex, root, aggregate_cap, generation]() mutable {
        ProjectReplaceOutcome outcome =
            RunProjectReplace(root, files, dirty_open, query, replace_text, case_sensitive,
                              regex.get(), aggregate_cap);
        outcome.project_root = root;
        outcome.generation = generation;
        project_replace_mailbox_.Post([this, outcome = std::move(outcome)]() mutable {
          ApplyProjectReplaceOutcome(std::move(outcome));
        });
      });
}

void WorkspaceShell::ApplyProjectReplaceOutcome(ProjectReplaceOutcome outcome) {
  // Drop a stale apply: the project was switched away, or a newer replace-all
  // superseded this one. The on-disk writes already happened; only the UI
  // reconciliation is gated (a newer run will reconcile its own writes).
  if (outcome.generation != project_replace_generation_ ||
      outcome.project_root != context_.current_project_state.root) {
    return;
  }

  ProjectSearchState& ps = context_.current_project_state.overlay.workflow.project_search;
  switch (outcome.status) {
    case ProjectReplaceOutcome::Status::NothingToDo:
      return;
    case ProjectReplaceOutcome::Status::BlockedByDirty:
      ps.error =
          "Replace-all aborted: save open changes to " + outcome.blocked_relative_path + " first.";
      RequestSidebarRedraw();
      return;
    case ProjectReplaceOutcome::Status::CapExceeded:
      ps.error = "Replace-all aborted: too much content to modify at once.";
      RequestSidebarRedraw();
      return;
    case ProjectReplaceOutcome::Status::InvalidReplacement:
      ps.error = "Replace-all aborted: invalid replacement pattern.";
      RequestSidebarRedraw();
      return;
    case ProjectReplaceOutcome::Status::Applied:
      break;
  }

  for (const auto& change : outcome.written) {
    // Refresh every clean open view of the rewritten file across ALL editor groups.
    // A tab that became dirty AFTER the pre-dispatch snapshot is left alone (its
    // buffer keeps the user's unsaved edits; the on-disk replace is overridden when
    // they next save) rather than clobbered by a reload.
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
        if (editor_state.viewport.dirty()) {
          continue;  // became dirty mid-flight: keep the user's edits
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
  metadata_updates.changes.reserve(outcome.written.size());
  for (const auto& change : outcome.written) {
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
  if (outcome.failed_write_count > 0) {
    const std::size_t attempted = outcome.written.size() + outcome.failed_write_count;
    ps.error = "Replace-all wrote " + std::to_string(outcome.written.size()) + " of " +
               std::to_string(attempted) + " files; " + std::to_string(outcome.failed_write_count) +
               " could not be written.";
    RequestSidebarRedraw();
  }
}

std::vector<int> WorkspaceShell::BuildProjectSearchLineMap() const {
  const auto& search = context_.current_project_state.overlay.workflow.project_search;
  // Rebuild only when `results` actually changed (revision mismatch), so a
  // hover/scroll/keystroke repaint reuses the prior grouped line map instead of
  // re-walking every result and re-copying the group-boundary paths.
  if (!search.cached_line_map_valid ||
      search.cached_line_map_revision != search.results_revision) {
    search.cached_line_map = BuildProjectSearchResultLineMap(search.results);
    search.cached_line_map_revision = search.results_revision;
    search.cached_line_map_valid = true;
  }
  return search.cached_line_map;
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
