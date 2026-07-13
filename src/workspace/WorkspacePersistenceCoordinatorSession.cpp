#include "compare/CompareReviewTypes.h"
#include "workspace/WorkspacePersistenceCoordinator.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>

#include "util/JsonValue.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "util/StartupTrace.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "workspace/WorkspacePersistenceFormat.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {
}  // namespace

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
  for (const auto& file : state.breakpoint_store.SnapshotAll()) {
    PersistedFileBreakpoints persisted_file;
    persisted_file.path = file.path;
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
    state.group_split_orientation = EditorSplitOrientation::None;
    state.group_split_fraction = 0.5f;
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
    // Rebuild one persisted tab into a runtime TabEntry. `should_eager_hydrate`
    // controls whether an editor tab is opened immediately or left deferred;
    // compare/merge tabs are always materialized fully.
    auto rebuild_tab = [&](const PersistedEditorTabState& persisted_tab,
                           bool should_eager_hydrate) -> std::optional<TabEntry> {
    if (persisted_tab.kind == "compare") {
      std::filesystem::path compare_path = persisted_tab.compare_path;
      if (compare_path.is_relative()) {
        compare_path = state.root / compare_path;
      }
      compare_path = compare_path.lexically_normal();

      if (compare_path.empty() || persisted_tab.compare_commit_hash.empty() ||
          persisted_tab.compare_commit_short_hash.empty()) {
        return std::nullopt;
      }

      const project::GitCommitEntry commit{
          .hash = persisted_tab.compare_commit_hash,
          .short_hash = persisted_tab.compare_commit_short_hash,
          .subject = {},
          .author = {},
          .relative_date = {},
      };
      std::optional<TabEntry> compare_tab;
      if (!persisted_tab.compare_right_ref.empty()) {
        auto resolve_path = [&](std::filesystem::path path, const std::filesystem::path& fallback) {
          if (path.empty()) {
            return fallback;
          }
          if (path.is_relative()) {
            path = state.root / path;
          }
          return path.lexically_normal();
        };
        CompareTabState compare_state;
        compare_state.path = compare_path;
        compare_state.left_path = resolve_path(persisted_tab.compare_left_path, compare_path);
        compare_state.right_path = resolve_path(persisted_tab.compare_right_path, compare_path);
        compare_state.commit_hash = persisted_tab.compare_commit_hash;
        compare_state.right_ref = persisted_tab.compare_right_ref;
        compare_state.left_label = persisted_tab.compare_commit_short_hash;
        compare_state.right_label =
            persisted_tab.compare_right_label.empty()
                ? (persisted_tab.compare_right_ref == "WORKTREE" ? "Working tree"
                                                                 : persisted_tab.compare_right_ref)
                : persisted_tab.compare_right_label;
        compare_state.selected_row = persisted_tab.compare_selected_row;
        compare_state.scroll_row = static_cast<int>(std::min<std::size_t>(
            persisted_tab.compare_scroll_row,
            static_cast<std::size_t>(std::numeric_limits<int>::max())));
        compare_state.horizontal_scroll = persisted_tab.compare_horizontal_scroll;
        if (!persisted_tab.compare_review_mode.empty()) {
          if (persisted_tab.compare_review_mode == "commit") {
            compare_state.review_mode = compare::CompareReviewMode::Commit;
          } else if (persisted_tab.compare_review_mode == "branch") {
            compare_state.review_mode = compare::CompareReviewMode::Branch;
          } else if (persisted_tab.compare_review_mode == "conflict") {
            compare_state.review_mode = compare::CompareReviewMode::Conflict;
          } else {
            compare_state.review_mode = compare::CompareReviewMode::WorkingTree;
          }
        }
        if (!persisted_tab.compare_staging_view.empty()) {
          if (persisted_tab.compare_staging_view == "staged") {
            compare_state.staging_view = compare::WorkingTreeStagingView::Staged;
          } else if (persisted_tab.compare_staging_view == "unstaged") {
            compare_state.staging_view = compare::WorkingTreeStagingView::Unstaged;
          } else {
            compare_state.staging_view = compare::WorkingTreeStagingView::Combined;
          }
        }
        compare_state.divider_fraction = persisted_tab.compare_divider_fraction;
        compare_state.persistable = true;
        compare_tab = operations_.build_compare_tab_from_state(compare_path, compare_state);
      } else {
        compare_tab = operations_.build_compare_tab_from_commit(compare_path, commit,
                                                                persisted_tab.compare_selected_row);
      }
      if (!compare_tab.has_value()) {
        return std::nullopt;
      }
      compare_tab->compare->scroll_row = static_cast<int>(std::min<std::size_t>(
          persisted_tab.compare_scroll_row,
          static_cast<std::size_t>(std::numeric_limits<int>::max())));
      compare_tab->compare->horizontal_scroll = persisted_tab.compare_horizontal_scroll;
      compare_tab->compare->divider_fraction = persisted_tab.compare_divider_fraction;
      return compare_tab;
    }
    if (persisted_tab.kind == "merge") {
      auto resolve_path = [&](std::filesystem::path path) {
        if (path.is_relative()) {
          path = state.root / path;
        }
        return path.lexically_normal();
      };

      const std::filesystem::path merge_base = resolve_path(persisted_tab.merge_base_path);
      const std::filesystem::path merge_incoming = resolve_path(persisted_tab.merge_incoming_path);
      const std::filesystem::path merge_current = resolve_path(persisted_tab.merge_current_path);
      const std::filesystem::path merge_output = resolve_path(persisted_tab.merge_output_path);
      if (merge_base.empty() || merge_incoming.empty() || merge_current.empty() ||
          merge_output.empty()) {
        return std::nullopt;
      }

      auto merge_tab =
          operations_.build_merge_tab_entry(merge_base, merge_incoming, merge_current, merge_output);
      if (!merge_tab.has_value() || !merge_tab->merge.has_value()) {
        return std::nullopt;
      }

      const auto parse_choice = [](std::string_view text) {
        if (text == "base") {
          return compare::MergeChoice::Base;
        }
        if (text == "incoming") {
          return compare::MergeChoice::Incoming;
        }
        if (text == "current") {
          return compare::MergeChoice::Current;
        }
        if (text == "both") {
          return compare::MergeChoice::Both;
        }
        if (text == "both-current-first") {
          return compare::MergeChoice::BothCurrentFirst;
        }
        if (text == "both-incoming-first") {
          return compare::MergeChoice::BothIncomingFirst;
        }
        return compare::MergeChoice::Base;
      };

      auto& merge_state = merge_tab->merge.value();
      for (std::size_t i = 0;
           i < merge_state.model.hunks.size() && i < persisted_tab.merge_hunk_choices.size(); ++i) {
        if (persisted_tab.merge_hunk_choices[i].empty()) {
          continue;
        }
        merge_state.model.hunks[i].choice = parse_choice(persisted_tab.merge_hunk_choices[i]);
      }
      merge_state.left_divider_fraction = persisted_tab.merge_left_divider_fraction;
      merge_state.right_divider_fraction = persisted_tab.merge_right_divider_fraction;
      operations_.refresh_merge_tab_derived_state(merge_state);
      merge_state.selected_hunk =
          merge_state.conflicts.empty()
              ? 0
              : std::min(persisted_tab.merge_selected_hunk, merge_state.conflicts.size() - 1);
      merge_state.scroll_row = static_cast<int>(std::min<std::size_t>(
          persisted_tab.merge_scroll_row,
          static_cast<std::size_t>(std::numeric_limits<int>::max())));
      merge_state.horizontal_scroll = persisted_tab.merge_horizontal_scroll;
      merge_state.result_viewport.SetScrollLine(
          static_cast<std::size_t>(std::max(0, merge_state.scroll_row)));
      merge_state.result_viewport.SetHorizontalScroll(merge_state.horizontal_scroll);
      merge_state.scroll_row = static_cast<int>(merge_state.result_viewport.scroll_line());
      return merge_tab;
    }

    // Editor tab: restores exactly one viewport from its inline single-view fields.
    std::filesystem::path view_path = persisted_tab.path;
    if (!view_path.empty() && view_path.is_relative()) {
      view_path = state.root / view_path;
    }
    view_path = view_path.lexically_normal();

    TabEntry::EditorTabState editor_state;
    editor_state.restored_path = view_path;
    editor_state.restored_cursor_line = persisted_tab.cursor_line;
    editor_state.restored_cursor_column = persisted_tab.cursor_column;
    editor_state.restored_scroll_line = persisted_tab.scroll_line;
    editor_state.restored_horizontal_scroll = persisted_tab.horizontal_scroll;

    if (persisted_tab.dirty_snapshot) {
      editor::TextViewport restored_view;
      // Load the already line-split snapshot straight in (dirty=true baked in).
      // LoadContent would SerializeLines(...) then re-split -- two extra full passes
      // over the buffer plus a throwaway joined string.
      restored_view.LoadLines(persisted_tab.buffer_lines, view_path, persisted_tab.line_ending);
      // Apply preferences / indent detection first (they re-run EnsureCursorVisible),
      // then restore view state last so scroll survives independent of the caret.
      operations_.apply_editor_preferences(restored_view);
      operations_.apply_detected_indent_on_open(restored_view);
      restored_view.ApplyRestoredViewState(persisted_tab.cursor_line,
                                           persisted_tab.cursor_column,
                                           persisted_tab.scroll_line,
                                           persisted_tab.horizontal_scroll);
      editor_state.viewport = std::move(restored_view);
      editor_state.needs_restore = false;
    } else if (!view_path.empty() && std::filesystem::exists(view_path)) {
      editor_state.needs_restore = true;
    } else {
      return std::nullopt;
    }

    const std::filesystem::path tab_path = operations_.editor_view_path(editor_state);

    TabEntry restored_tab{
        .kind = TabEntry::Kind::Editor,
        .path = tab_path,
        .title = tab_path.empty() ? "untitled" : tab_path.filename().string(),
        .editor_state = std::nullopt,
        .deferred_handle = std::nullopt,
        .compare = std::nullopt,
        .merge = std::nullopt,
    };
    if (should_eager_hydrate) {
      restored_tab.editor_state = std::move(editor_state);
    } else {
      std::optional<editor::SelectionRange> selection;
      if (!editor_state.needs_restore) {
        selection = editor_state.viewport.selection_range();
      }
      restored_tab.deferred_handle = TabEntry::DeferredTabHandle{
          .path = tab_path,
          .language_hint = editor::runtime_syntax::DetectFiletype(tab_path),
          .cursor_line = editor_state.restored_cursor_line,
          .cursor_column = editor_state.restored_cursor_column,
          .scroll_line = editor_state.restored_scroll_line,
          .horizontal_scroll = editor_state.restored_horizontal_scroll,
          .selection = selection,
      };
    }
    return restored_tab;
    };  // rebuild_tab

    state.editor_groups.clear();
    for (const PersistedEditorGroupState& persisted_group : persisted_session.groups) {
      if (state.editor_groups.size() >= 2) {
        break;  // Editor groups are capped at 2.
      }
      EditorGroup group;
      std::size_t restored_active = 0;
      for (std::size_t i = 0; i < persisted_group.tabs.size(); ++i) {
        const PersistedEditorTabState& persisted_tab = persisted_group.tabs[i];
        const bool is_active = i == persisted_group.active_tab_index;
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
    if (state.editor_groups.size() >= 2) {
      EditorSplitOrientation orientation =
          persisted_session.group_split_orientation <=
                  static_cast<std::uint8_t>(EditorSplitOrientation::Horizontal)
              ? static_cast<EditorSplitOrientation>(persisted_session.group_split_orientation)
              : EditorSplitOrientation::Vertical;
      // Two groups always imply a visible divider; never leave the orientation None.
      state.group_split_orientation =
          orientation == EditorSplitOrientation::None ? EditorSplitOrientation::Vertical : orientation;
    } else {
      state.group_split_orientation = EditorSplitOrientation::None;
    }
    state.group_split_fraction = std::clamp(persisted_session.group_split_fraction, 0.1f, 0.9f);
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
    state.sidebar.width = sanitize_pixels(persisted_session.sidebar_width, 288.0f);
    state.panel.height = sanitize_pixels(persisted_session.bottom_panel_height, 156.0f);
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
    state.surface.focus = state.sidebar.visible ? FocusTarget::Sidebar : FocusTarget::Editor;
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
  persisted_session.group_split_orientation =
      static_cast<std::uint8_t>(state.group_split_orientation);
  persisted_session.group_split_fraction = state.group_split_fraction;
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

  for (auto& group : state.editor_groups) {
    PersistedEditorGroupState persisted_group;
    for (std::size_t tab_index = 0; tab_index < group.open_tabs.size(); ++tab_index) {
      auto& tab = group.open_tabs[tab_index];
      std::optional<PersistedEditorTabState> persisted_tab;
      if (tab.kind == TabEntry::Kind::Compare) {
        persisted_tab = BuildPersistedCompareTabState(tab);
      } else if (tab.kind == TabEntry::Kind::Merge) {
        persisted_tab = BuildPersistedMergeTabState(tab);
      } else {
        persisted_tab = BuildPersistedEditorTabState(tab_index, tab);
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

  operations_.persistence_service->SaveProjectSession(session_path, persisted_session);
  SaveDebugState();
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
  persisted_tab.merge_hunk_choices.reserve(tab.merge->model.hunks.size());
  for (const auto& hunk : tab.merge->model.hunks) {
    persisted_tab.merge_hunk_choices.push_back(compare::MergeChoiceLabel(hunk.choice));
  }
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
    persisted_tab.cursor_line = tab.deferred_handle->cursor_line;
    persisted_tab.cursor_column = tab.deferred_handle->cursor_column;
    persisted_tab.scroll_line = tab.deferred_handle->scroll_line;
    persisted_tab.horizontal_scroll = tab.deferred_handle->horizontal_scroll;
    persisted_tab.dirty_snapshot = false;
    persisted_tab.line_ending = editor::TextViewport::LineEnding::LF;
    return persisted_tab;
  }

  auto& editor_state = tab.editor_state.value();
  const editor::TextViewport* persisted_viewport = &editor_state.viewport;
  const std::filesystem::path normalized_path =
      editor_state.needs_restore ? editor_state.restored_path.lexically_normal()
                                 : persisted_viewport->path().lexically_normal();
  const bool dirty_snapshot = !editor_state.needs_restore && persisted_viewport->dirty();
  if ((normalized_path.empty() && !dirty_snapshot) ||
      (editor_state.needs_restore && !dirty_snapshot && normalized_path.empty())) {
    return std::nullopt;
  }
  const std::size_t cursor_line =
      editor_state.needs_restore ? editor_state.restored_cursor_line
                                 : persisted_viewport->cursor_line();
  const std::size_t cursor_column =
      editor_state.needs_restore ? editor_state.restored_cursor_column
                                 : persisted_viewport->cursor_column();
  const std::size_t scroll_line =
      editor_state.needs_restore ? editor_state.restored_scroll_line
                                 : persisted_viewport->scroll_line();
  const std::size_t horizontal_scroll =
      editor_state.needs_restore ? editor_state.restored_horizontal_scroll
                                 : persisted_viewport->horizontal_scroll();

  PersistedEditorTabState persisted_tab;
  persisted_tab.kind = "editor";
  persisted_tab.path = normalized_path;
  persisted_tab.cursor_line = cursor_line;
  persisted_tab.cursor_column = cursor_column;
  persisted_tab.scroll_line = scroll_line;
  persisted_tab.horizontal_scroll = horizontal_scroll;
  persisted_tab.dirty_snapshot = dirty_snapshot;
  persisted_tab.line_ending = persisted_viewport->line_ending();
  persisted_tab.buffer_lines = dirty_snapshot ? persisted_viewport->lines().Snapshot() : std::vector<std::string>{};
  return persisted_tab;
}

}  // namespace microide::workspace
