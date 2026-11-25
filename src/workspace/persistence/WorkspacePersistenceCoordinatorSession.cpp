#include "compare/CompareReviewTypes.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"
#include "workspace/git/CompareTabLoad.h"

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
#include "util/InvokeIfSet.h"

namespace microide::workspace {

namespace {

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
    signature.exists = true;
    signature.size = static_cast<std::uintmax_t>(-1);
  }
  return signature;
}

}

namespace {

std::size_t& MaxDirtySnapshotBytesStorage() {
  static std::size_t bytes = 64u * 1024 * 1024;
  return bytes;
}

std::size_t MaxDirtySnapshotBytes() { return MaxDirtySnapshotBytesStorage(); }

bool FitsDirtySnapshotBudget(const editor::TextViewport& view) {
  constexpr std::size_t kMaxPersistedBufferLines = 4'000'000;
  const std::size_t max_buffer_bytes = MaxDirtySnapshotBytes();
  const editor::LineSpan span = view.lines();
  if (span.size() > kMaxPersistedBufferLines) {
    return false;
  }
  std::size_t total_bytes = 0;
  for (std::size_t i = 0; i < span.size(); ++i) {
    total_bytes += span[i].size() + 1;
    if (total_bytes > max_buffer_bytes) {
      return false;
    }
  }
  return true;
}

void StageDirtyBuffer(const editor::TextViewport& view, const std::filesystem::path& normalized_path,
                      PersistedEditorTabState& out) {
  out.dirty_snapshot = true;
  out.line_ending = view.line_ending();
  out.utf8_bom = view.has_utf8_bom();
  out.buffer = view.lines().SnapshotDocument();
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

int ScrollRowFromSession(std::size_t row) {
  return static_cast<int>(
      std::min<std::size_t>(row, static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

}

void PersistenceCoordinator::SetMaxDirtySnapshotBytesForTesting(std::size_t bytes) {
  MaxDirtySnapshotBytesStorage() = bytes == 0 ? 64u * 1024 * 1024 : bytes;
}

std::filesystem::path PersistenceCoordinator::SessionStatePath() const {
  return CurrentProjectState().root.empty() ? std::filesystem::path{}
                                            : operations_.project_state_directory() / "session";
}

std::optional<TabEntry> PersistenceCoordinator::RestoreCompareTab(
    const PersistedEditorTabState& persisted_tab, const std::filesystem::path& root,
    const editor::TextViewport* unsaved_right) {
  const std::filesystem::path compare_path = ResolveSessionPath(root, persisted_tab.compare_path);
  if (compare_path.empty() || persisted_tab.compare_commit_hash.empty() ||
      persisted_tab.compare_commit_short_hash.empty()) {
    return std::nullopt;
  }

  const auto side_path = [&](const std::filesystem::path& path) {
    return path.empty() ? compare_path : ResolveSessionPath(root, path);
  };
  const std::string right_ref =
      persisted_tab.compare_right_ref.empty() ? std::string("WORKTREE") : persisted_tab.compare_right_ref;
  CompareTabState compare_state;
  compare_state.path = compare_path;
  compare_state.left_path = side_path(persisted_tab.compare_left_path);
  compare_state.right_path = side_path(persisted_tab.compare_right_path);
  compare_state.commit_hash = persisted_tab.compare_commit_hash;
  compare_state.right_ref = right_ref;
  compare_state.left_label = persisted_tab.compare_commit_short_hash;
  compare_state.right_label = !persisted_tab.compare_right_label.empty() ? persisted_tab.compare_right_label
                              : right_ref == "WORKTREE"                  ? std::string("Working tree")
                                                                          : right_ref;
  compare_state.selected_row = persisted_tab.compare_selected_row;
  compare_state.scroll_row = ScrollRowFromSession(persisted_tab.compare_scroll_row);
  compare_state.horizontal_scroll = persisted_tab.compare_horizontal_scroll;
  if (!persisted_tab.compare_review_mode.empty()) {
    compare_state.review_mode = compare::ParseCompareReviewMode(persisted_tab.compare_review_mode);
  }
  if (!persisted_tab.compare_staging_view.empty()) {
    compare_state.staging_view = compare::ParseWorkingTreeStagingView(persisted_tab.compare_staging_view);
  }
  compare_state.divider_fraction = persisted_tab.compare_divider_fraction;
  compare_state.persistable = true;
  std::optional<TabEntry> compare_tab = MakeCompareStandIn(std::move(compare_state));
  if (!compare_tab.has_value() || !compare_tab->compare.has_value()) {
    return std::nullopt;
  }
  CompareTabState& compare = *compare_tab->compare;
  compare.scroll_row = ScrollRowFromSession(persisted_tab.compare_scroll_row);
  compare.horizontal_scroll = persisted_tab.compare_horizontal_scroll;
  compare.divider_fraction = persisted_tab.compare_divider_fraction;
  if (unsaved_right != nullptr) {
    if (!compare.right_editable ||
        !util::SameAsNormalizedPath(compare.right_viewport.path(), unsaved_right->path())) {
      return std::nullopt;
    }
    RepointCompareRightSideToReloadedBuffer(compare, *unsaved_right);
  }
  return compare_tab;
}

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

  RestoreDebugState();

  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::RestoreSessionState::RebuildTabs");
    RestoreEditorGroups(persisted_session);
  }
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::RestoreSessionState::RestoreLayoutState");
    RestoreSessionLayout(persisted_session);
  }
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::RestoreSessionState::FinalizeState");
    for (EditorGroup& group : state.editor_groups) {
      if (group.open_tabs.empty()) {
        group.welcome_surface.viewport.SetPlaceholderText(
            "microide\n\n"
            "Project loaded.\n"
            "Use the sidebar to open files.\n");
      }
    }
    state.surface.focus = FocusTarget::Editor;
  }
  return true;
}

editor::TextViewport PersistenceCoordinator::RestoreDirtyView(
    const PersistedEditorTabState& persisted_tab, const std::filesystem::path& view_path,
    RestoredDirtyViews& dirty_views) {
  const std::string view_key =
      !view_path.empty()                    ? view_path.generic_string()
      : persisted_tab.untitled_number != 0 ? "untitled:" + std::to_string(persisted_tab.untitled_number)
                                           : std::string{};
  if (const auto shared = dirty_views.find(view_key);
      !view_key.empty() && shared != dirty_views.end()) {
    return shared->second;
  }
  editor::TextViewport restored_view;
  restored_view.LoadLines(persisted_tab.buffer.ToLines(), view_path, persisted_tab.line_ending);
  restored_view.SetUtf8Bom(persisted_tab.utf8_bom);
  if (persisted_tab.disk_baseline.has_value() && !view_path.empty()) {
    restored_view.RebindToPath(view_path, RestoredDiskBaseline(*persisted_tab.disk_baseline, view_path));
  }
  operations_.apply_editor_preferences(restored_view);
  operations_.apply_detected_indent_on_open(restored_view);
  if (!view_key.empty()) {
    dirty_views.emplace(view_key, restored_view);
  }
  return restored_view;
}

std::optional<TabEntry> PersistenceCoordinator::RebuildTab(
    const PersistedEditorTabState& persisted_tab, bool should_eager_hydrate,
    RestoredDirtyViews& dirty_views) {
  ProjectWorkspaceState& state = CurrentProjectState();
  const std::filesystem::path view_path = ResolveSessionPath(state.root, persisted_tab.path);
  if (persisted_tab.kind == "compare") {
    if (!persisted_tab.dirty_snapshot || view_path.empty()) {
      return RestoreCompareTab(persisted_tab, state.root, nullptr);
    }
    const editor::TextViewport unsaved = RestoreDirtyView(persisted_tab, view_path, dirty_views);
    if (std::optional<TabEntry> compare_tab = RestoreCompareTab(persisted_tab, state.root, &unsaved);
        compare_tab.has_value()) {
      return compare_tab;
    }
  } else if (persisted_tab.kind == "merge") {
    return RestoreMergeTab(persisted_tab, state.root);
  }

  TabEntry::EditorTabState editor_state;
  editor_state.restore = ReadViewState(persisted_tab, view_path);

  if (persisted_tab.dirty_snapshot) {
    editor::TextViewport restored_view = RestoreDirtyView(persisted_tab, view_path, dirty_views);
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
}

void PersistenceCoordinator::RestoreEditorGroups(
    const PersistedProjectSessionState& persisted_session) {
  ProjectWorkspaceState& state = CurrentProjectState();
  RestoredDirtyViews dirty_views;
  state.editor_groups.clear();
  for (const PersistedEditorGroupState& persisted_group : persisted_session.groups) {
    if (state.editor_groups.size() >= kMaxEditorGroups) {
      break;
    }
    EditorGroup group;
    std::size_t restored_active = 0;
    constexpr std::size_t kMaxRestoredTabsPerGroup = 200;
    for (std::size_t i = 0; i < persisted_group.tabs.size(); ++i) {
      const PersistedEditorTabState& persisted_tab = persisted_group.tabs[i];
      const bool is_active = i == persisted_group.active_tab_index;
      if (group.open_tabs.size() >= kMaxRestoredTabsPerGroup && !is_active) {
        continue;
      }
      const bool should_eager_hydrate = is_active || persisted_tab.dirty_snapshot;
      std::optional<TabEntry> tab = RebuildTab(persisted_tab, should_eager_hydrate, dirty_views);
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
  NumberUntitledTabs(state.editor_groups);
  RejoinRestoredCompareSides(state.editor_groups);
  if (!state.editor_split.Load(persisted_session.split_tree) ||
      state.editor_split.leaf_count() != state.editor_groups.size()) {
    state.editor_split.ResetToEvenSplit(state.editor_groups.size(),
                                        EditorSplitOrientation::Vertical);
  }
  state.focused_group_index =
      std::min(persisted_session.focused_group_index, state.editor_groups.size() - 1);
}

void PersistenceCoordinator::RestoreSessionLayout(
    PersistedProjectSessionState& persisted_session) {
  ProjectWorkspaceState& state = CurrentProjectState();
  const auto sanitize_pixels = [](float value, float fallback) {
    if (!std::isfinite(value) || value < 0.0f) {
      return fallback;
    }
    return std::min(value, 100000.0f);
  };
  state.sidebar.visible = persisted_session.sidebar_visible;
  state.sidebar.width = sanitize_pixels(persisted_session.sidebar_width, kWorkspaceDefaultSidebarWidth);
  state.panel.height = sanitize_pixels(persisted_session.bottom_panel_height, kWorkspaceDefaultBottomPanelHeight);
  state.sidebar.git.outgoing_base_choice = persisted_session.outgoing_base_choice;
  state.debug_pane.width = sanitize_pixels(persisted_session.right_pane_width, 0.0f);
  state.debug_pane.mode = persisted_session.right_pane_mode <=
                                  static_cast<std::uint8_t>(DebugPaneMode::Breakpoints)
                              ? static_cast<DebugPaneMode>(persisted_session.right_pane_mode)
                              : DebugPaneMode::CallStack;
  const bool debugger_enabled =
      operations_.debugger_enabled && operations_.debugger_enabled();
  state.debug_pane.visible = persisted_session.right_pane_visible && debugger_enabled;

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

  struct UnkeptText {
    const editor::TextViewport* view = nullptr;
    const MergeTabState* merge = nullptr;
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
  for (const DeferredTabHandle& closed : state.recently_closed_editors) {
    PersistedEditorTabState persisted_closed;
    persisted_closed.path = closed.path;
    WriteViewState(closed, persisted_closed);
    persisted_session.closed_editors.push_back(std::move(persisted_closed));
  }

  util::InvokeIfSet(operations_.session_saved, persisted_session);
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
PersistenceCoordinator::BuildPersistedEditorTabState(std::size_t ,
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

}
