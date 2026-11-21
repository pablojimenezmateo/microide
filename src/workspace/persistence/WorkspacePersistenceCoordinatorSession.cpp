#include "compare/CompareReviewTypes.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <unordered_map>

#include "util/JsonValue.h"
#include "util/PathMatch.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "util/StartupTrace.h"
#include "util/TextFileIO.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "workspace/git/MergeResultValidation.h"
#include "workspace/persistence/WorkspacePersistenceFormat.h"
#include "workspace/WorkspaceProjectPresentation.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

// The disk signature a restored dirty snapshot compares the file against before
// its save (TextViewport::DetectDiskConflict). Its stat as the buffer last saw it,
// and its content hash when the snapshot recorded one under the same hash function
// (the probe matches): then a touch while microide was down -- the stat moved, the
// bytes did not -- saves. Without one it reads as a change and asks, the safe way
// to be wrong.
// A buffer of a file that did not exist yet (never saved) gets a baseline no real
// file matches once one appears: a file someone created at that path meanwhile is
// a change to show, not one to overwrite.
util::FileSignature RestoredDiskBaseline(const PersistedEditorTabState::DiskBaseline& baseline,
                                         const std::filesystem::path& path) {
  util::FileSignature signature;
  if (baseline.exists) {
    signature.exists = true;
    signature.mtime_ticks = baseline.mtime_ticks;
    signature.size = static_cast<std::uintmax_t>(baseline.size);
    if (baseline.content_hash.has_value() && baseline.hash_probe == util::QuickContentHashProbe()) {
      signature.has_content_hash = true;
      signature.content_hash = static_cast<std::size_t>(*baseline.content_hash);
    }
    return signature;
  }
  std::error_code error;
  if (std::filesystem::exists(path, error) || error) {
    signature.exists = true;  // mtime 0, size max: nothing on disk matches it
    signature.size = static_cast<std::uintmax_t>(-1);
  }
  return signature;
}

}  // namespace

namespace {

// Dirty-buffer snapshot byte budget (mirrors the reader's kMaxSessionBufferBytes);
// process-wide, overridable for tests via SetMaxDirtySnapshotBytesForTesting.
std::size_t& MaxDirtySnapshotBytesStorage() {
  static std::size_t bytes = 64u * 1024 * 1024;  // 64 MiB
  return bytes;
}

std::size_t MaxDirtySnapshotBytes() { return MaxDirtySnapshotBytesStorage(); }

// Whether a dirty buffer fits the reader's snapshot budget. Checked BEFORE
// snapshotting: without it a huge dirty generated file materializes its whole
// buffer into a second vector (Snapshot) and gets encoded into a session file the
// reader would then reject at kMaxSessionBufferBytes / kMaxSessionBufferLines.
// Sized from the live LineSpan (O(lines), no allocation, early-out); over budget,
// the record carries no snapshot and its tab restores by reopening from disk
// (matching the reader's rejection outcome, minus the wasted snapshot/encode/write).
// TD-2026-07-17A-083.
bool FitsDirtySnapshotBudget(const editor::TextViewport& view) {
  constexpr std::size_t kMaxPersistedBufferLines = 4'000'000;  // mirrors reader
  const std::size_t max_buffer_bytes = MaxDirtySnapshotBytes();
  const editor::LineSpan span = view.lines();
  if (span.size() > kMaxPersistedBufferLines) {
    return false;
  }
  std::size_t total_bytes = 0;
  for (std::size_t i = 0; i < span.size(); ++i) {
    total_bytes += span[i].size() + 1;  // + newline
    if (total_bytes > max_buffer_bytes) {
      return false;
    }
  }
  return true;
}

// Stages `view`'s unsaved text in a tab's record: the snapshot, its line ending
// and mark, and -- for the buffer of a file, `normalized_path` -- the file as the
// buffer last saw it. One definition for every record that carries a buffer: an
// editor tab's, and a compare tab's for its editable side.
void StageDirtyBuffer(const editor::TextViewport& view, const std::filesystem::path& normalized_path,
                      PersistedEditorTabState& out) {
  out.dirty_snapshot = true;
  out.line_ending = view.line_ending();
  out.utf8_bom = view.has_utf8_bom();
  // Shares the piece tree's buffers: no copy of the text here, on the shell
  // thread; the encode on the persistence worker reads it (TD-2026-11-02-429.3).
  out.buffer = view.lines().SnapshotDocument();
  // The restore snapshot is one of the moments a buffer's bytes are written
  // somewhere: a kill -9 finding then says whether the lost buffer was staged.
  view.TraceBufferIo("session-stage");
  if (!normalized_path.empty()) {
    const util::FileSignature& seen = view.disk_signature();
    out.disk_baseline = PersistedEditorTabState::DiskBaseline{
        .exists = seen.exists, .mtime_ticks = seen.mtime_ticks, .size = static_cast<std::uint64_t>(seen.size)};
    if (seen.exists && seen.has_content_hash) {
      out.disk_baseline->content_hash = static_cast<std::uint64_t>(seen.content_hash);
      out.disk_baseline->hash_probe = util::QuickContentHashProbe();
    }
  }
}

// The result file as a merge's result buffer last saw it, for a staged typed
// result -- what a save after a restore compares the file against. The buffer's
// own stat once it has saved; before that, the file now, when it still holds
// what the merge read (MergeResultChangedOnDisk); and when it does not, a
// baseline no file matches, so the restored result's save asks rather than
// writing over the change it never saw.
std::optional<PersistedEditorTabState::DiskBaseline> MergeResultDiskBaseline(const MergeTabState& merge) {
  const util::FileSignature& seen = merge.result_viewport.disk_signature();
  if (!seen.exists && MergeResultChangedOnDisk(merge)) {
    return PersistedEditorTabState::DiskBaseline{
        .exists = true, .mtime_ticks = 0, .size = static_cast<std::uint64_t>(-1)};
  }
  const util::FileSignature signature = seen.exists ? seen : util::StatFileSignature(merge.output_path);
  PersistedEditorTabState::DiskBaseline baseline{.exists = signature.exists,
                                                 .mtime_ticks = signature.mtime_ticks,
                                                 .size = static_cast<std::uint64_t>(signature.size)};
  if (signature.exists && signature.has_content_hash) {
    baseline.content_hash = static_cast<std::uint64_t>(signature.content_hash);
    baseline.hash_probe = util::QuickContentHashProbe();
  }
  return baseline;
}

// A merge's typed result, staged in its record with the conflicts where the tab
// tracks them in that text (TD-2026-11-19-453): an edit to a merge result was
// staged nowhere, and a crash lost it.
void StageTypedMergeResult(const MergeTabState& merge, PersistedEditorTabState& out) {
  const editor::TextViewport& result = merge.result_viewport;
  if (!result.dirty() || merge.output_path.empty()) {
    return;
  }
  if (!FitsDirtySnapshotBudget(result)) {
    result.TraceBufferIo("session-stage-skipped-over-budget");
    return;
  }
  out.path = merge.output_path.lexically_normal();
  StageDirtyBuffer(result, out.path, out);
  out.disk_baseline = MergeResultDiskBaseline(merge);
  out.merge_conflict_spans.reserve(merge.conflicts.size());
  for (const MergeTrackedConflict& conflict : merge.conflicts) {
    out.merge_conflict_spans.push_back(PersistedMergeConflictSpan{
        .hunk_index = conflict.hunk_index,
        .start_line = conflict.start_line,
        .end_line = conflict.end_line,
        .last_choice = compare::MergeChoiceLabel(conflict.last_choice),
        .valid = conflict.valid,
        .resolved = conflict.resolved,
    });
  }
}

// A view (an editor tab's or a closed editor's) in its persisted form, and back.
// One pair, so a tab and a closed editor save and restore the same things.
void WriteViewState(const DeferredTabHandle& view, PersistedEditorTabState& out) {
  out.cursor_line = view.cursor_line;
  out.cursor_column = view.cursor_column;
  out.scroll_line = view.scroll_line;
  out.horizontal_scroll = view.horizontal_scroll;
  out.collapsed_fold_openers = view.collapsed_fold_openers;
}

DeferredTabHandle ReadViewState(const PersistedEditorTabState& tab, std::filesystem::path path) {
  return DeferredTabHandle{
      .path = std::move(path),
      .cursor_line = tab.cursor_line,
      .cursor_column = tab.cursor_column,
      .scroll_line = tab.scroll_line,
      .horizontal_scroll = tab.horizontal_scroll,
      .selection = std::nullopt,
      .collapsed_fold_openers = tab.collapsed_fold_openers,
  };
}

// A persisted path made absolute against the project root and normalized; empty
// stays empty.
std::filesystem::path ResolveSessionPath(const std::filesystem::path& root,
                                         std::filesystem::path path) {
  if (path.empty()) {
    return path;
  }
  if (path.is_relative()) {
    path = root / path;
  }
  return path.lexically_normal();
}

// A persisted scroll row as the int the tab state holds.
int ScrollRowFromSession(std::size_t row) {
  return static_cast<int>(
      std::min<std::size_t>(row, static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

}  // namespace

void PersistenceCoordinator::SetMaxDirtySnapshotBytesForTesting(std::size_t bytes) {
  MaxDirtySnapshotBytesStorage() = bytes == 0 ? 64u * 1024 * 1024 : bytes;
}

std::filesystem::path PersistenceCoordinator::SessionStatePath() const {
  return CurrentProjectState().root.empty() ? std::filesystem::path{}
                                            : operations_.project_state_directory() / "session";
}

std::filesystem::path PersistenceCoordinator::DebugStatePath() const {
  return CurrentProjectState().root.empty() ? std::filesystem::path{}
                                            : operations_.project_state_directory() / "debug";
}

void PersistenceCoordinator::SaveDebugState() {
  if (operations_.persistence_service == nullptr) {
    return;
  }
  const std::filesystem::path debug_path = DebugStatePath();
  if (debug_path.empty()) {
    return;
  }
  const ProjectWorkspaceState& state = CurrentProjectState();

  PersistedDebugState persisted;
  // Views, not SnapshotAll. The header classes session persistence as a caller
  // that KEEPS the snapshot across store mutations; it does not -- the loop below
  // copies each breakpoint into the persisted struct and drops the snapshot before
  // returning, with nothing mutating the store in between. SnapshotAll therefore
  // deep-copied every file's whole breakpoint vector, on every session save, i.e.
  // on every project switch, to be read once.
  std::vector<editor::BreakpointStore::FileBreakpointsView> breakpoint_files;
  state.breakpoint_store.FillSortedFileViews(&breakpoint_files);
  for (const auto& file : breakpoint_files) {
    PersistedFileBreakpoints persisted_file;
    persisted_file.path = *file.path;
    for (const editor::Breakpoint& breakpoint : file.breakpoints) {
      persisted_file.breakpoints.push_back(PersistedBreakpoint{
          .line = breakpoint.line,
          .enabled = breakpoint.enabled,
          .condition = breakpoint.condition,
          .hit_condition = breakpoint.hit_condition,
          .log_message = breakpoint.log_message,
      });
    }
    persisted.files.push_back(std::move(persisted_file));
  }
  for (const LaunchConfig& config : state.launch_configs) {
    persisted.launch_configs.push_back(PersistedLaunchConfig{
        .name = config.name,
        .type = config.type,
        .request = config.request,
        .arguments_json =
            config.arguments.IsNull() ? std::string{} : util::SerializeJson(config.arguments),
    });
  }
  for (const editor::FunctionBreakpoint& fn : state.function_breakpoint_store.All()) {
    persisted.function_breakpoints.push_back(PersistedFunctionBreakpoint{
        .name = fn.name,
        .enabled = fn.enabled,
        .condition = fn.condition,
        .hit_condition = fn.hit_condition,
    });
  }
  persisted.selected_launch_config_index = state.selected_launch_config_index;
  persisted.watch_expressions = state.debug_watch.Expressions();
  persisted.enabled_exception_filters = state.debug_breakpoints_panel.EnabledFilterIds();
  persisted.exception_filters_seeded = state.debug_breakpoints_panel.Seeded();
  persisted.exception_filter_conditions = state.debug_breakpoints_panel.FilterConditions();

  // Avoid leaving stale state when there is nothing to persist. Delete BOTH the
  // primary file and its backup through the service — removing only the primary
  // would let the reader fall back to `debug.bak` and resurrect cleared
  // breakpoints / launch configs / watches on the next restore.
  if (persisted.files.empty() && persisted.launch_configs.empty() &&
      persisted.watch_expressions.empty() && persisted.enabled_exception_filters.empty() &&
      persisted.function_breakpoints.empty() && persisted.exception_filter_conditions.empty() &&
      !persisted.exception_filters_seeded) {
    operations_.persistence_service->DeleteState(debug_path);
    return;
  }
  operations_.persistence_service->SaveDebugState(debug_path, persisted);
}

void PersistenceCoordinator::RestoreDebugState() {
  if (operations_.persistence_service == nullptr) {
    return;
  }
  const std::filesystem::path debug_path = DebugStatePath();
  if (debug_path.empty()) {
    return;
  }
  PersistedDebugState persisted;
  if (!operations_.persistence_service->LoadDebugState(debug_path, &persisted)) {
    return;
  }

  ProjectWorkspaceState& state = CurrentProjectState();
  std::vector<editor::BreakpointStore::FileBreakpoints> files;
  files.reserve(persisted.files.size());
  for (const PersistedFileBreakpoints& persisted_file : persisted.files) {
    editor::BreakpointStore::FileBreakpoints file;
    file.path = persisted_file.path;
    for (const PersistedBreakpoint& breakpoint : persisted_file.breakpoints) {
      file.breakpoints.push_back(editor::Breakpoint{
          .line = breakpoint.line,
          .enabled = breakpoint.enabled,
          .condition = breakpoint.condition,
          .hit_condition = breakpoint.hit_condition,
          .log_message = breakpoint.log_message,
      });
    }
    files.push_back(std::move(file));
  }
  state.breakpoint_store.ReplaceAll(std::move(files));

  std::vector<editor::FunctionBreakpoint> function_breakpoints;
  function_breakpoints.reserve(persisted.function_breakpoints.size());
  for (const PersistedFunctionBreakpoint& fn : persisted.function_breakpoints) {
    function_breakpoints.push_back(editor::FunctionBreakpoint{
        .name = fn.name,
        .enabled = fn.enabled,
        .condition = fn.condition,
        .hit_condition = fn.hit_condition,
    });
  }
  state.function_breakpoint_store.ReplaceAll(std::move(function_breakpoints));

  state.launch_configs.clear();
  for (const PersistedLaunchConfig& persisted_config : persisted.launch_configs) {
    LaunchConfig config;
    config.name = persisted_config.name;
    config.type = persisted_config.type;
    config.request = persisted_config.request;
    // A corrupt arguments string must not nuke the rest; fall back to Null.
    if (!persisted_config.arguments_json.empty()) {
      if (auto parsed = util::ParseJson(persisted_config.arguments_json); parsed.has_value()) {
        config.arguments = std::move(*parsed);
      }
    }
    state.launch_configs.push_back(std::move(config));
  }
  // A persisted index can be stale or out of range relative to the rebuilt
  // config list. An out-of-range selection makes StartDebuggingWithDefaultConfig
  // ignore every launch config, so clamp/reset it to a valid slot immediately.
  if (state.launch_configs.empty() ||
      persisted.selected_launch_config_index >= state.launch_configs.size()) {
    state.selected_launch_config_index = 0;
  } else {
    state.selected_launch_config_index = persisted.selected_launch_config_index;
  }
  state.debug_watch.SetExpressions(std::move(persisted.watch_expressions));
  state.debug_breakpoints_panel.SetEnabledFilterIds(std::move(persisted.enabled_exception_filters),
                                                    persisted.exception_filters_seeded);
  state.debug_breakpoints_panel.SetFilterConditions(
      std::move(persisted.exception_filter_conditions));
  state.debug_breakpoints_panel.Rebuild(state.breakpoint_store, state.function_breakpoint_store);
}

std::optional<TabEntry> PersistenceCoordinator::RestoreCompareTab(
    const PersistedEditorTabState& persisted_tab, const std::filesystem::path& root,
    const editor::TextViewport* unsaved_right) {
  const std::filesystem::path compare_path = ResolveSessionPath(root, persisted_tab.compare_path);
  if (compare_path.empty() || persisted_tab.compare_commit_hash.empty() ||
      persisted_tab.compare_commit_short_hash.empty()) {
    return std::nullopt;
  }

  std::optional<TabEntry> compare_tab;
  if (!persisted_tab.compare_right_ref.empty()) {
    const auto side_path = [&](const std::filesystem::path& path) {
      return path.empty() ? compare_path : ResolveSessionPath(root, path);
    };
    CompareTabState compare_state;
    compare_state.path = compare_path;
    compare_state.left_path = side_path(persisted_tab.compare_left_path);
    compare_state.right_path = side_path(persisted_tab.compare_right_path);
    compare_state.commit_hash = persisted_tab.compare_commit_hash;
    compare_state.right_ref = persisted_tab.compare_right_ref;
    compare_state.left_label = persisted_tab.compare_commit_short_hash;
    compare_state.right_label =
        persisted_tab.compare_right_label.empty()
            ? (persisted_tab.compare_right_ref == "WORKTREE" ? "Working tree"
                                                             : persisted_tab.compare_right_ref)
            : persisted_tab.compare_right_label;
    compare_state.selected_row = persisted_tab.compare_selected_row;
    compare_state.scroll_row = ScrollRowFromSession(persisted_tab.compare_scroll_row);
    compare_state.horizontal_scroll = persisted_tab.compare_horizontal_scroll;
    if (!persisted_tab.compare_review_mode.empty()) {
      compare_state.review_mode = compare::ParseCompareReviewMode(persisted_tab.compare_review_mode);
    }
    if (!persisted_tab.compare_staging_view.empty()) {
      compare_state.staging_view =
          compare::ParseWorkingTreeStagingView(persisted_tab.compare_staging_view);
    }
    compare_state.divider_fraction = persisted_tab.compare_divider_fraction;
    compare_state.persistable = true;
    if (unsaved_right != nullptr) {
      // The diff is built of the unsaved text, as a rebuild of a live dirty diff
      // is: the file on disk is not what this side holds, and need not be readable.
      compare_state.right_viewport = *unsaved_right;
    }
    compare_tab = operations_.build_compare_tab_from_state(compare_path, compare_state);
  } else {
    const project::GitCommitEntry commit{
        .hash = persisted_tab.compare_commit_hash,
        .short_hash = persisted_tab.compare_commit_short_hash,
        .subject = {},
        .author = {},
        .relative_date = {},
    };
    compare_tab = operations_.build_compare_tab_from_commit(compare_path, commit,
                                                            persisted_tab.compare_selected_row);
  }
  if (!compare_tab.has_value() || !compare_tab->compare.has_value()) {
    return std::nullopt;
  }
  CompareTabState& compare = *compare_tab->compare;
  compare.scroll_row = ScrollRowFromSession(persisted_tab.compare_scroll_row);
  compare.horizontal_scroll = persisted_tab.compare_horizontal_scroll;
  compare.divider_fraction = persisted_tab.compare_divider_fraction;
  if (unsaved_right != nullptr) {
    // The restored buffer itself, not a copy of its text: unsaved, with the mark,
    // line ending and disk baseline the snapshot kept, and shared with every other
    // tab the restore puts on this file. A diff that cannot hold it (no editable
    // side on this file any more) is not the tab to bring it back.
    if (!compare.right_editable ||
        !util::SameAsNormalizedPath(compare.right_viewport.path(), unsaved_right->path())) {
      return std::nullopt;
    }
    RepointCompareRightSideToReloadedBuffer(compare, *unsaved_right);
  }
  return compare_tab;
}

// A typed result the record staged, laid over the merge the restore rebuilt from
// its inputs and choices: the text as typed, unsaved, with the file's baseline,
// and the conflicts where the tab tracked them in it. Spans that no longer fit
// the rebuilt model (an input changed while microide was down) are placed again
// by matching the model against the text, as a result read from disk is.
void PersistenceCoordinator::RestoreTypedMergeResult(const PersistedEditorTabState& persisted_tab,
                                                     const std::filesystem::path& output_path,
                                                     MergeTabState& merge) {
  editor::TextViewport result;
  result.LoadLines(persisted_tab.buffer.ToLines(), output_path, persisted_tab.line_ending);
  result.SetUtf8Bom(persisted_tab.utf8_bom);
  if (persisted_tab.disk_baseline.has_value()) {
    result.RebindToPath(output_path, RestoredDiskBaseline(*persisted_tab.disk_baseline, output_path));
  }
  operations_.apply_editor_preferences(result);
  merge.result_line_ending = persisted_tab.line_ending;
  merge.result_viewport = std::move(result);
  const std::vector<PersistedMergeConflictSpan>& spans = persisted_tab.merge_conflict_spans;
  const std::size_t line_count = merge.result_viewport.line_count();
  const bool spans_fit =
      spans.size() == merge.conflicts.size() &&
      std::equal(spans.begin(), spans.end(), merge.conflicts.begin(),
                 [line_count](const PersistedMergeConflictSpan& span, const MergeTrackedConflict& rebuilt) {
                   return span.hunk_index == rebuilt.hunk_index && span.start_line <= span.end_line &&
                          span.end_line <= line_count;
                 });
  if (spans_fit) {
    for (std::size_t i = 0; i < spans.size(); ++i) {
      MergeTrackedConflict& conflict = merge.conflicts[i];
      conflict.start_line = spans[i].start_line;
      conflict.end_line = spans[i].end_line;
      conflict.last_choice = compare::ParseMergeChoice(spans[i].last_choice);
      conflict.valid = spans[i].valid;
      conflict.resolved = spans[i].resolved;
    }
  } else if (operations_.merge_conflicts_for_result) {
    merge.conflicts = operations_.merge_conflicts_for_result(merge.model, merge.result_viewport.lines().Snapshot());
  }
  merge.visual_conflicts_valid = false;
  ++merge.model_revision;
  merge.max_visual_columns = std::max(merge.max_visual_columns, merge.result_viewport.max_visual_columns());
}

std::optional<TabEntry> PersistenceCoordinator::RestoreMergeTab(
    const PersistedEditorTabState& persisted_tab, const std::filesystem::path& root) {
  const std::filesystem::path merge_base = ResolveSessionPath(root, persisted_tab.merge_base_path);
  const std::filesystem::path merge_incoming =
      ResolveSessionPath(root, persisted_tab.merge_incoming_path);
  const std::filesystem::path merge_current =
      ResolveSessionPath(root, persisted_tab.merge_current_path);
  const std::filesystem::path merge_output =
      ResolveSessionPath(root, persisted_tab.merge_output_path);
  if (merge_base.empty() || merge_incoming.empty() || merge_current.empty() ||
      merge_output.empty()) {
    return std::nullopt;
  }

  auto merge_tab =
      operations_.build_merge_tab_entry(merge_base, merge_incoming, merge_current, merge_output);
  if (!merge_tab.has_value() || !merge_tab->merge.has_value()) {
    return std::nullopt;
  }

  auto& merge_state = merge_tab->merge.value();
  for (std::size_t i = 0;
       i < merge_state.model.hunks.size() && i < persisted_tab.merge_hunk_choices.size(); ++i) {
    if (!persisted_tab.merge_hunk_choices[i].empty()) {
      merge_state.model.hunks[i].choice = compare::ParseMergeChoice(persisted_tab.merge_hunk_choices[i]);
    }
  }
  merge_state.left_divider_fraction = persisted_tab.merge_left_divider_fraction;
  merge_state.right_divider_fraction = persisted_tab.merge_right_divider_fraction;
  operations_.refresh_merge_tab_derived_state(merge_state);
  if (persisted_tab.dirty_snapshot) {
    RestoreTypedMergeResult(persisted_tab, merge_output, merge_state);
  }
  merge_state.selected_hunk =
      merge_state.conflicts.empty()
          ? 0
          : std::min(persisted_tab.merge_selected_hunk, merge_state.conflicts.size() - 1);
  merge_state.scroll_row = ScrollRowFromSession(persisted_tab.merge_scroll_row);
  merge_state.horizontal_scroll = persisted_tab.merge_horizontal_scroll;
  merge_state.result_viewport.SetScrollLine(
      static_cast<std::size_t>(std::max(0, merge_state.scroll_row)));
  merge_state.result_viewport.SetHorizontalScroll(merge_state.horizontal_scroll);
  merge_state.scroll_row = static_cast<int>(merge_state.result_viewport.scroll_line());
  return merge_tab;
}

bool PersistenceCoordinator::RestoreSessionState() {
  util::StartupTrace::Scope trace_scope("WorkspaceShell::RestoreSessionState");
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::RestoreSessionState");
  const std::filesystem::path session_path = SessionStatePath();
  if (session_path.empty() || operations_.persistence_service == nullptr) {
    return false;
  }

  PersistedProjectSessionState persisted_session;
  persisted_session.sidebar_visible = CurrentProjectState().sidebar.visible;
  persisted_session.sidebar_width = CurrentProjectState().sidebar.width;
  persisted_session.bottom_panel_height = CurrentProjectState().panel.height;
  persisted_session.outgoing_base_choice = CurrentProjectState().sidebar.git.outgoing_base_choice;
  persisted_session.focused_group_index = CurrentProjectState().focused_group_index;
  persisted_session.right_pane_visible = CurrentProjectState().debug_pane.visible;
  persisted_session.right_pane_width = CurrentProjectState().debug_pane.width;
  persisted_session.right_pane_mode =
      static_cast<std::uint8_t>(CurrentProjectState().debug_pane.mode);
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::RestoreSessionState::ParseSessionFile");
    if (!operations_.persistence_service->LoadProjectSession(session_path, &persisted_session)) {
      return false;
    }
  }

  auto& state = CurrentProjectState();
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::RestoreSessionState::ResetState");
    state.editor_groups.clear();
    state.editor_groups.emplace_back();
    state.focused_group_index = 0;
    state.editor_split.Reset();
    state.overlay.visible = false;
    state.overlay.workflow.compare_picker.matches.clear();
    state.overlay.workflow.compare_picker.items.clear();
    state.overlay.workflow.compare_picker.selected_index = 0;
  }

  // Breakpoints + launch configs persist independently of tabs; restore them
  // before any early return so they survive even an empty-tab project.
  RestoreDebugState();

  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::RestoreSessionState::RebuildTabs");
    // A file open in two panes is ONE buffer, and both of its tabs persisted the
    // same dirty snapshot. Rebuilding each from its own lines gave the restored
    // session two independent documents for one file — an edit in one pane no
    // longer showed in the other, and each pane saved its own copy. The first
    // restored view of a path is kept here (a copy shares its DocumentState) and
    // every later dirty tab on that path copies it. Clean tabs restore lazily
    // through OpenEditorViewForPath, which finds the live view the same way.
    std::unordered_map<std::string, editor::TextViewport> restored_dirty_views;
    // The buffer a record's dirty snapshot restores, as a view of it. The tabs of
    // one dirty buffer share it again: a file's by its path, an untitled buffer's
    // by its number -- it has no path, and keying it by the empty one restored each
    // pane on a buffer of its own, two texts under one Untitled-N after the first
    // edit (soak 2ae149191d79). An untitled tab from an older session has no
    // number and stays on its own.
    const auto restore_dirty_view = [&](const PersistedEditorTabState& persisted_tab,
                                        const std::filesystem::path& view_path) -> editor::TextViewport {
      const std::string view_key =
          !view_path.empty()                    ? view_path.generic_string()
          : persisted_tab.untitled_number != 0 ? "untitled:" + std::to_string(persisted_tab.untitled_number)
                                               : std::string{};
      if (const auto shared = restored_dirty_views.find(view_key);
          !view_key.empty() && shared != restored_dirty_views.end()) {
        return shared->second;  // another tab on the same dirty buffer
      }
      editor::TextViewport restored_view;
      // Load the snapshot's lines straight in (dirty=true baked in): LoadLines
      // keeps the persisted line ending, where LoadContent would detect one.
      restored_view.LoadLines(persisted_tab.buffer.ToLines(), view_path, persisted_tab.line_ending);
      restored_view.SetUtf8Bom(persisted_tab.utf8_bom);
      if (persisted_tab.disk_baseline.has_value() && !view_path.empty()) {
        restored_view.RebindToPath(view_path, RestoredDiskBaseline(*persisted_tab.disk_baseline, view_path));
      }
      // Preferences / indent detection before any view state is applied (they
      // re-run EnsureCursorVisible), so a tab's scroll survives independent of
      // the caret.
      operations_.apply_editor_preferences(restored_view);
      operations_.apply_detected_indent_on_open(restored_view);
      if (!view_key.empty()) {
        restored_dirty_views.emplace(view_key, restored_view);
      }
      return restored_view;
    };
    // Rebuild one persisted tab into a runtime TabEntry. `should_eager_hydrate`
    // controls whether an editor tab is opened immediately or left deferred;
    // compare/merge tabs are always materialized fully.
    auto rebuild_tab = [&](const PersistedEditorTabState& persisted_tab,
                           bool should_eager_hydrate) -> std::optional<TabEntry> {
    const std::filesystem::path view_path = ResolveSessionPath(state.root, persisted_tab.path);
    if (persisted_tab.kind == "compare") {
      // A diff whose editable side held unsaved text carries it, under the file's
      // path. When the diff itself cannot come back -- its repository gone, its
      // revision unreadable -- the text still does, in an editor tab on the file:
      // the record then reads as an editor tab's, below.
      if (!persisted_tab.dirty_snapshot || view_path.empty()) {
        return RestoreCompareTab(persisted_tab, state.root, nullptr);
      }
      const editor::TextViewport unsaved = restore_dirty_view(persisted_tab, view_path);
      if (std::optional<TabEntry> compare_tab = RestoreCompareTab(persisted_tab, state.root, &unsaved);
          compare_tab.has_value()) {
        return compare_tab;
      }
    } else if (persisted_tab.kind == "merge") {
      return RestoreMergeTab(persisted_tab, state.root);
    }

    // Editor tab: restores exactly one viewport from its inline single-view fields.
    TabEntry::EditorTabState editor_state;
    editor_state.restore = ReadViewState(persisted_tab, view_path);

    if (persisted_tab.dirty_snapshot) {
      editor::TextViewport restored_view = restore_dirty_view(persisted_tab, view_path);
      editor_state.restore.ApplyViewStateTo(restored_view);
      editor_state.viewport = std::move(restored_view);
      editor_state.content = EditorTabState::Content::Ready;
    } else if (std::error_code exists_ec;
               !view_path.empty() && std::filesystem::exists(view_path, exists_ec) &&
               !exists_ec) {
      editor_state.content = EditorTabState::Content::Deferred;
    } else {
      return std::nullopt;
    }

    const std::filesystem::path tab_path = operations_.editor_view_path(editor_state);

    // An untitled buffer keeps the number it had; one from an older session gets
    // the next free one (NumberUntitledTabs, below).
    const std::uint32_t untitled_number = tab_path.empty() ? persisted_tab.untitled_number : 0;
    TabEntry restored_tab{
        .kind = TabEntry::Kind::Editor,
        .path = tab_path,
        .title = TabTitleForPath(tab_path, untitled_number),
        .editor_state = std::nullopt,
        .deferred_handle = std::nullopt,
        .compare = std::nullopt,
        .merge = std::nullopt,
    };
    restored_tab.untitled_number = untitled_number;
    if (should_eager_hydrate) {
      restored_tab.editor_state = std::move(editor_state);
    } else {
      restored_tab.deferred_handle = std::move(editor_state.restore);
      restored_tab.deferred_handle->path = tab_path;
      if (!editor_state.content_pending()) {
        restored_tab.deferred_handle->selection = editor_state.viewport.selection_range();
      }
    }
    return restored_tab;
    };  // rebuild_tab

    state.editor_groups.clear();
    for (const PersistedEditorGroupState& persisted_group : persisted_session.groups) {
      if (state.editor_groups.size() >= kMaxEditorGroups) {
        break;
      }
      EditorGroup group;
      std::size_t restored_active = 0;
      // The decode cap (kMaxSessionTabsPerGroup = 4096) is a defensive parse ceiling,
      // not a UI workload size. A CRC-valid but oversized session would otherwise make
      // startup rebuild thousands of tabs — path-probing clean editors and building
      // full compare/merge models — before the first frame. Cap restored tabs at a
      // much smaller product limit; the active tab is always restored even past the
      // cap so focus never strands. TD-2026-07-17A-082.
      constexpr std::size_t kMaxRestoredTabsPerGroup = 200;
      for (std::size_t i = 0; i < persisted_group.tabs.size(); ++i) {
        const PersistedEditorTabState& persisted_tab = persisted_group.tabs[i];
        const bool is_active = i == persisted_group.active_tab_index;
        if (group.open_tabs.size() >= kMaxRestoredTabsPerGroup && !is_active) {
          continue;  // Past the product cap: skip non-active tabs (defensive).
        }
        const bool should_eager_hydrate = is_active || persisted_tab.dirty_snapshot;
        std::optional<TabEntry> tab = rebuild_tab(persisted_tab, should_eager_hydrate);
        if (!tab.has_value()) {
          continue;
        }
        if (is_active) {
          restored_active = group.open_tabs.size();
        }
        group.open_tabs.push_back(std::move(*tab));
      }
      if (!group.open_tabs.empty()) {
        group.active_tab_index = std::min(restored_active, group.open_tabs.size() - 1);
      }
      state.editor_groups.push_back(std::move(group));
    }
    if (state.editor_groups.empty()) {
      state.editor_groups.emplace_back();
    }
    // Restored untitled buffers (hot exit) take their numbers in tab order.
    NumberUntitledTabs(state.editor_groups);
    // One file, one buffer: a diff's editable side and the editor tab on its file.
    RejoinRestoredCompareSides(state.editor_groups);
    // The tree's leaves ARE the groups, so a record that does not describe exactly
    // the groups that survived the restore (missing, corrupt, or written before a
    // group was dropped for an unreadable path) is replaced by an even row rather
    // than left to disagree with them.
    if (!state.editor_split.Load(persisted_session.split_tree) ||
        state.editor_split.leaf_count() != state.editor_groups.size()) {
      state.editor_split.ResetToEvenSplit(state.editor_groups.size(),
                                          EditorSplitOrientation::Vertical);
    }
    state.focused_group_index =
        std::min(persisted_session.focused_group_index, state.editor_groups.size() - 1);
  }

  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::RestoreSessionState::RestoreLayoutState");
    // A CRC-valid but hand-edited (or NaN/Inf) persisted float would otherwise flow
    // straight into layout: std::clamp(NaN, …) returns NaN, so the render-time pane
    // clamps cannot recover it and the pane collapses/explodes. Replace any
    // non-finite or negative dimension with the schema default here.
    const auto sanitize_pixels = [](float value, float fallback) {
      // Reject non-finite and negative, AND cap an absurd finite value: a corrupt
      // session could store a huge width/height that would squeeze the rest of the
      // layout to nothing until the next resize. 100000px is far past any display.
      if (!std::isfinite(value) || value < 0.0f) {
        return fallback;
      }
      return std::min(value, 100000.0f);
    };
    state.sidebar.visible = persisted_session.sidebar_visible;
    state.sidebar.width = sanitize_pixels(persisted_session.sidebar_width, kWorkspaceDefaultSidebarWidth);
    state.panel.height = sanitize_pixels(persisted_session.bottom_panel_height, kWorkspaceDefaultBottomPanelHeight);
    state.sidebar.git.outgoing_base_choice = persisted_session.outgoing_base_choice;
    // Right-side debug pane: restore width/mode always, but only re-open it when the
    // debugger feature is currently enabled (else a pane left open in a prior session
    // would surface debug UI in the default debugger-off state).
    state.debug_pane.width = sanitize_pixels(persisted_session.right_pane_width, 0.0f);
    state.debug_pane.mode = persisted_session.right_pane_mode <=
                                    static_cast<std::uint8_t>(DebugPaneMode::Breakpoints)
                                ? static_cast<DebugPaneMode>(persisted_session.right_pane_mode)
                                : DebugPaneMode::CallStack;
    const bool debugger_enabled =
        operations_.debugger_enabled && operations_.debugger_enabled();
    state.debug_pane.visible = persisted_session.right_pane_visible && debugger_enabled;

    // File-tree expansion + sidebar view. RestoreExpansionState rebuilds the tree
    // rows; a later Refresh() (on tab activation / sidebar refresh) preserves the
    // restored expansion because it reads the same expanded_paths_ set.
    state.directory_tree.RestoreExpansionState(persisted_session.expanded_tree_paths,
                                               persisted_session.collapsed_tree_paths);
    if (!persisted_session.selected_tree_path.empty() && !state.root.empty()) {
      state.directory_tree.SelectPath(
          state.root / std::filesystem::path(persisted_session.selected_tree_path));
    }
    state.sidebar.scroll_row = std::max(0, persisted_session.sidebar_scroll_row);
    if (!persisted_session.sidebar_view_id.empty()) {
      state.sidebar.view_id = persisted_session.sidebar_view_id;
    }
    // The panel comes back maximized if it was, and project init revives the
    // terminals into it (TerminalPanelService::OpenTerminalsForProjectInit).
    state.panel.maximized = persisted_session.panel_maximized;
    state.terminal_layout_to_revive = std::move(persisted_session.terminal_layout);
    state.recently_closed_editors.clear();
    for (const PersistedEditorTabState& closed : persisted_session.closed_editors) {
      if (!closed.path.is_absolute() ||
          state.recently_closed_editors.size() >= ProjectWorkspaceState::kMaxRecentlyClosedEditors) {
        continue;
      }
      state.recently_closed_editors.push_back(ReadViewState(closed, closed.path.lexically_normal()));
    }
  }

  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::RestoreSessionState::FinalizeState");
    // Each empty group shows the welcome placeholder; per-group active index was
    // already clamped during rebuild.
    for (EditorGroup& group : state.editor_groups) {
      if (group.open_tabs.empty()) {
        group.welcome_surface.viewport.SetPlaceholderText(
            "microide\n\n"
            "Project loaded.\n"
            "Use the sidebar to open files.\n");
      }
    }
    // A restored session starts with the keyboard in the editor, as VS Code's does
    // (TD-2026-10-16-348); the sidebar used to take it whenever it was shown.
    state.surface.focus = FocusTarget::Editor;
  }
  return true;
}

void PersistenceCoordinator::SaveSessionState() {
  if (CurrentProjectState().root.empty()) {
    return;
  }

  operations_.sync_active_editor_tab();

  const std::filesystem::path session_path = SessionStatePath();
  if (session_path.empty()) {
    return;
  }

  auto& state = CurrentProjectState();

  PersistedProjectSessionState persisted_session;
  persisted_session.sidebar_visible = state.sidebar.visible;
  persisted_session.sidebar_width = state.sidebar.width;
  persisted_session.bottom_panel_height = state.panel.height;
  persisted_session.outgoing_base_choice = state.sidebar.git.outgoing_base_choice;
  persisted_session.focused_group_index = state.focused_group_index;
  persisted_session.split_tree = state.editor_split.Flatten();
  persisted_session.right_pane_visible = state.debug_pane.visible;
  persisted_session.right_pane_width = state.debug_pane.width;
  persisted_session.right_pane_mode = static_cast<std::uint8_t>(state.debug_pane.mode);

  // File-tree expansion + sidebar view state, so reopening lands on the same
  // expanded folders / selected node / scroll position (terminals excluded).
  persisted_session.expanded_tree_paths = state.directory_tree.ExpandedRelativePaths();
  persisted_session.collapsed_tree_paths = state.directory_tree.ManuallyCollapsedRelativePaths();
  if (const auto selected = state.directory_tree.SelectedPath();
      selected.has_value() && !state.root.empty()) {
    const auto relative = selected->lexically_relative(state.root);
    if (!relative.empty() && relative != std::filesystem::path(".") &&
        !(relative.begin() != relative.end() &&
          *relative.begin() == std::filesystem::path(".."))) {
      persisted_session.selected_tree_path = relative.generic_string();
    }
  }
  persisted_session.sidebar_scroll_row = state.sidebar.scroll_row;
  persisted_session.sidebar_view_id = state.sidebar.view_id;
  persisted_session.panel_maximized = state.panel.maximized;
  if (operations_.capture_terminal_layout) {
    persisted_session.terminal_layout = operations_.capture_terminal_layout();
  }

  // The diffs whose editable side holds unsaved text that fits a snapshot, and the
  // merges the session does not keep whose result does: almost always none. A
  // persistable diff's own record carries its side, a persistable merge's its
  // result. These are for the records that otherwise would not: an editor tab on
  // the same file that has not loaded (it holds no buffer, the diff holds the
  // file's), and a diff or merge the session does not keep at all -- a compare of
  // two files, a git conflict's merge -- whose text then comes back in an editor
  // tab on the file.
  struct UnkeptText {
    const editor::TextViewport* view = nullptr;
    const MergeTabState* merge = nullptr;  // a merge's result, else a diff's side
  };
  std::vector<UnkeptText> unsaved_diff_sides;
  for (const EditorGroup& group : state.editor_groups) {
    for (const TabEntry& tab : group.open_tabs) {
      const bool unkept_merge = tab.kind == TabEntry::Kind::Merge && tab.merge.has_value() && !tab.merge->persistable &&
                                !tab.merge->output_path.empty();
      if (tab.kind != TabEntry::Kind::Compare && !unkept_merge) {
        continue;
      }
      const editor::TextViewport* side = unkept_merge ? &tab.merge->result_viewport : LoadedBufferViewOf(tab);
      if (side != nullptr && side->dirty() && !side->path().empty() && FitsDirtySnapshotBudget(*side)) {
        unsaved_diff_sides.push_back(UnkeptText{.view = side, .merge = unkept_merge ? &*tab.merge : nullptr});
      }
    }
  }
  const auto stage_unkept = [](const UnkeptText& text, const std::filesystem::path& path,
                               PersistedEditorTabState& record) {
    StageDirtyBuffer(*text.view, path, record);
    if (text.merge != nullptr) {
      record.disk_baseline = MergeResultDiskBaseline(*text.merge);
    }
  };
  const auto editor_tab_is_on = [&state](const std::filesystem::path& path) {
    for (const EditorGroup& group : state.editor_groups) {
      for (const TabEntry& tab : group.open_tabs) {
        if (tab.kind == TabEntry::Kind::Editor && util::SameAsNormalizedPath(tab.path, path)) {
          return true;
        }
      }
    }
    return false;
  };
  // Whether the file's own buffer -- an editor tab's or a diff's editable side --
  // holds unsaved text. That buffer is the file's in the session: the restore
  // gives every record of one path one buffer, so a merge result staged on the
  // same path took its place and the file's text was gone (soak 02922e8eed90).
  // A result is not the file's until it is kept; it then comes back in an
  // Untitled tab of its own.
  const auto file_buffer_is_unsaved = [&state](const std::filesystem::path& path) {
    for (const EditorGroup& group : state.editor_groups) {
      for (const TabEntry& tab : group.open_tabs) {
        const editor::TextViewport* buffer = tab.kind == TabEntry::Kind::Merge ? nullptr : LoadedBufferViewOf(tab);
        if (buffer != nullptr && buffer->dirty() && util::SameAsNormalizedPath(buffer->path(), path)) {
          return true;
        }
      }
    }
    return false;
  };
  // The documents already staged in an editor record that stands in for a diff.
  std::vector<const editor::TextViewport*> stood_in;

  for (auto& group : state.editor_groups) {
    PersistedEditorGroupState persisted_group;
    for (std::size_t tab_index = 0; tab_index < group.open_tabs.size(); ++tab_index) {
      auto& tab = group.open_tabs[tab_index];
      std::optional<PersistedEditorTabState> persisted_tab;
      if (tab.kind == TabEntry::Kind::Compare || tab.kind == TabEntry::Kind::Merge) {
        persisted_tab =
            tab.kind == TabEntry::Kind::Compare ? BuildPersistedCompareTabState(tab) : BuildPersistedMergeTabState(tab);
        const editor::TextViewport* side =
            persisted_tab.has_value()                ? nullptr
            : tab.kind == TabEntry::Kind::Compare    ? LoadedBufferViewOf(tab)
            : tab.merge.has_value()                  ? &tab.merge->result_viewport
                                                     : nullptr;
        const auto unkept = std::find_if(unsaved_diff_sides.begin(), unsaved_diff_sides.end(),
                                         [side](const UnkeptText& text) { return text.view == side; });
        if (side != nullptr && unkept != unsaved_diff_sides.end() &&
            std::none_of(stood_in.begin(), stood_in.end(),
                         [side](const editor::TextViewport* other) { return other->SharesDocumentWith(*side); })) {
          const std::filesystem::path path = side->path().lexically_normal();
          if (unkept->merge != nullptr && file_buffer_is_unsaved(path)) {
            // Untitled: no path, and no number -- the restore gives it the next free one.
            persisted_tab.emplace();
            persisted_tab->kind = "editor";
            StageDirtyBuffer(*side, {}, *persisted_tab);
            stood_in.push_back(side);
          } else if (!editor_tab_is_on(path)) {
            persisted_tab.emplace();
            persisted_tab->kind = "editor";
            persisted_tab->path = path;
            stage_unkept(*unkept, path, *persisted_tab);
            stood_in.push_back(side);
          }
        }
      } else {
        persisted_tab = BuildPersistedEditorTabState(tab_index, tab);
        if (persisted_tab.has_value() && !persisted_tab->dirty_snapshot && !persisted_tab->path.empty()) {
          // A diff's side is the file's buffer and goes first; a merge's result
          // only when the file's buffer has nothing unsaved (above).
          const UnkeptText* staged = nullptr;
          for (const UnkeptText& text : unsaved_diff_sides) {
            if (util::SameAsNormalizedPath(text.view->path(), persisted_tab->path) &&
                (staged == nullptr || (staged->merge != nullptr && text.merge == nullptr))) {
              staged = &text;
            }
          }
          if (staged != nullptr && (staged->merge == nullptr || !file_buffer_is_unsaved(persisted_tab->path))) {
            stage_unkept(*staged, persisted_tab->path, *persisted_tab);
          }
        }
      }
      if (!persisted_tab.has_value()) {
        continue;
      }
      if (tab_index == group.active_tab_index) {
        persisted_group.active_tab_index = persisted_group.tabs.size();
      }
      persisted_group.tabs.push_back(std::move(*persisted_tab));
    }
    persisted_session.groups.push_back(std::move(persisted_group));
  }
  if (persisted_session.focused_group_index >= persisted_session.groups.size()) {
    persisted_session.focused_group_index =
        persisted_session.groups.empty() ? 0 : persisted_session.groups.size() - 1;
  }
  // Reopen Closed Editor reaches into an earlier session, as VS Code's does.
  for (const DeferredTabHandle& closed : state.recently_closed_editors) {
    PersistedEditorTabState persisted_closed;
    persisted_closed.path = closed.path;
    WriteViewState(closed, persisted_closed);
    persisted_session.closed_editors.push_back(std::move(persisted_closed));
  }

  if (operations_.session_saved) {
    operations_.session_saved(persisted_session);
  }
  operations_.persistence_service->SaveProjectSession(session_path, std::move(persisted_session));
  state.host_terminals_unrecorded = false;
  SaveDebugState();
}

bool PersistenceCoordinator::SaveTerminalLayout(const std::filesystem::path& root,
                                                TerminalLayoutRecord layout) {
  if (operations_.persistence_service == nullptr || root.empty()) {
    return false;
  }
  const std::filesystem::path session_path = ProjectStateDirectory(root) / "session";
  PersistedProjectSessionState persisted_session;
  // Load flushes the write queue first, so this amends what the last save wrote.
  if (!operations_.persistence_service->LoadProjectSession(session_path, &persisted_session)) {
    return false;
  }
  persisted_session.terminal_layout = std::move(layout);
  return operations_.persistence_service->SaveProjectSession(session_path, std::move(persisted_session));
}

std::optional<PersistedEditorTabState>
PersistenceCoordinator::BuildPersistedCompareTabState(
    const TabEntry& tab) const {
  if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value() ||
      !tab.compare->persistable) {
    return std::nullopt;
  }

  PersistedEditorTabState persisted_tab;
  persisted_tab.kind = "compare";
  persisted_tab.compare_path = tab.compare->path.lexically_normal();
  persisted_tab.compare_left_path = tab.compare->left_path.lexically_normal();
  persisted_tab.compare_right_path = tab.compare->right_path.lexically_normal();
  persisted_tab.compare_commit_hash = tab.compare->commit_hash;
  persisted_tab.compare_commit_short_hash = tab.compare->left_label;
  persisted_tab.compare_right_ref = tab.compare->right_ref;
  persisted_tab.compare_right_label = tab.compare->right_label;
  persisted_tab.compare_selected_row = tab.compare->selected_row;
  persisted_tab.compare_scroll_row = static_cast<std::size_t>(std::max(0, tab.compare->scroll_row));
  persisted_tab.compare_horizontal_scroll = tab.compare->horizontal_scroll;
  persisted_tab.compare_divider_fraction = tab.compare->divider_fraction;
  persisted_tab.compare_review_mode = compare::CompareReviewModeLabel(tab.compare->review_mode);
  persisted_tab.compare_staging_view =
      compare::WorkingTreeStagingViewLabel(tab.compare->staging_view);
  // The editable side is the file's buffer, and with no editor tab on the file
  // this record is the only one that can carry its unsaved text: `path` names the
  // file, as an editor record's does, and the snapshot rides beside the diff's own
  // fields. A file also open in an editor tab is staged in both records; the
  // restore makes one buffer of them (as of the two panes of a split).
  if (const editor::TextViewport* right = LoadedBufferViewOf(tab);
      right != nullptr && right->dirty() && !right->path().empty()) {
    if (FitsDirtySnapshotBudget(*right)) {
      persisted_tab.path = right->path().lexically_normal();
      StageDirtyBuffer(*right, persisted_tab.path, persisted_tab);
    } else {
      right->TraceBufferIo("session-stage-skipped-over-budget");
    }
  }
  return persisted_tab;
}

std::optional<PersistedEditorTabState>
PersistenceCoordinator::BuildPersistedMergeTabState(
    const TabEntry& tab) const {
  if (tab.kind != TabEntry::Kind::Merge || !tab.merge.has_value() ||
      !tab.merge->persistable) {
    return std::nullopt;
  }

  PersistedEditorTabState persisted_tab;
  persisted_tab.kind = "merge";
  persisted_tab.merge_base_path = tab.merge->base_path.lexically_normal();
  persisted_tab.merge_incoming_path = tab.merge->incoming_path.lexically_normal();
  persisted_tab.merge_current_path = tab.merge->current_path.lexically_normal();
  persisted_tab.merge_output_path = tab.merge->output_path.lexically_normal();
  persisted_tab.merge_selected_hunk = tab.merge->selected_hunk;
  persisted_tab.merge_scroll_row = static_cast<std::size_t>(std::max(0, tab.merge->scroll_row));
  persisted_tab.merge_horizontal_scroll = tab.merge->horizontal_scroll;
  persisted_tab.merge_left_divider_fraction = tab.merge->left_divider_fraction;
  persisted_tab.merge_right_divider_fraction = tab.merge->right_divider_fraction;
  // Cap the save-side choice vector at the reader's budget so a huge generated
  // conflict tab never builds a record set the reader (kMaxSessionMergeHunkChoices)
  // would reject. Choices stay positional and every entry is persisted verbatim —
  // the restore model rebuild does not reproduce the save-time inferred choice for
  // every hunk, so an empty/default-skip encoding would change the restored result
  // (bootstrap_choice is not the value a rebuild lands on). TD-2026-07-17A-097.
  constexpr std::size_t kMaxPersistedMergeHunkChoices = 200'000;
  const std::vector<compare::MergeHunk>& hunks = tab.merge->model.hunks;
  const std::size_t bounded =
      std::min<std::size_t>(hunks.size(), kMaxPersistedMergeHunkChoices);
  persisted_tab.merge_hunk_choices.reserve(bounded);
  for (std::size_t i = 0; i < bounded; ++i) {
    persisted_tab.merge_hunk_choices.emplace_back(compare::MergeChoiceLabel(hunks[i].choice));
  }
  StageTypedMergeResult(*tab.merge, persisted_tab);
  return persisted_tab;
}

std::optional<PersistedEditorTabState>
PersistenceCoordinator::BuildPersistedEditorTabState(std::size_t /*tab_index*/,
                                                     TabEntry& tab) {
  if (tab.kind != TabEntry::Kind::Editor) {
    return std::nullopt;
  }

  if (!tab.editor_state.has_value()) {
    if (!tab.deferred_handle.has_value() || tab.deferred_handle->path.empty()) {
      return std::nullopt;
    }
    PersistedEditorTabState persisted_tab;
    persisted_tab.kind = "editor";
    persisted_tab.path = tab.deferred_handle->path.lexically_normal();
    WriteViewState(*tab.deferred_handle, persisted_tab);
    persisted_tab.dirty_snapshot = false;
    persisted_tab.line_ending = editor::TextViewport::LineEnding::LF;
    return persisted_tab;
  }

  auto& editor_state = tab.editor_state.value();
  const editor::TextViewport* persisted_viewport = &editor_state.viewport;
  // Guarded: both sources were normalized on the way in (the tab's restored path
  // by the reader, the viewport's by the open path), and lexically_normal() is
  // ~12 allocations even when it changes nothing -- once per open tab of every
  // group, on every session save (TD-2026-08-10-174).
  const std::filesystem::path& source_path =
      editor_state.content_pending() ? editor_state.restore.path : persisted_viewport->path();
  const std::filesystem::path normalized_path =
      util::PathTextNeedsNormalizing(source_path.native()) ? source_path.lexically_normal()
                                                           : source_path;
  const bool dirty_snapshot = !editor_state.content_pending() && persisted_viewport->dirty();
  const bool persist_dirty = dirty_snapshot && FitsDirtySnapshotBudget(*persisted_viewport);
  if (normalized_path.empty() && !persist_dirty) {
    return std::nullopt;
  }
  PersistedEditorTabState persisted_tab;
  persisted_tab.kind = "editor";
  persisted_tab.path = normalized_path;
  if (const std::optional<DeferredTabHandle> view = EditorTabViewState(tab); view.has_value()) {
    WriteViewState(*view, persisted_tab);
  }
  persisted_tab.untitled_number = normalized_path.empty() ? tab.untitled_number : 0;
  persisted_tab.line_ending = persisted_viewport->line_ending();
  if (persist_dirty) {
    StageDirtyBuffer(*persisted_viewport, normalized_path, persisted_tab);
  } else if (dirty_snapshot) {
    persisted_viewport->TraceBufferIo("session-stage-skipped-over-budget");
  }
  return persisted_tab;
}

}  // namespace microide::workspace
