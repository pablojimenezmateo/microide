#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>

#include "platform/AppDirectories.h"
#include "util/StartupTrace.h"
#include "util/TextFileIO.h"
#include "workspace/WorkspaceProjectCatalogCoordinator.h"
#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

void WorkspaceShell::RefreshAvailableColorschemeNames() {
  available_colorscheme_names_ = render::ListAvailableThemeNames();
}

bool WorkspaceShell::ApplyColorscheme(std::string_view name, bool persist, bool log_feedback) {
  (void) log_feedback;
  render::Theme loaded_theme;
  std::string resolved_name;
  std::string error;
  const std::string requested_name = name.empty() ? "default" : std::string(name);
  if (!render::LoadThemeByName(requested_name, loaded_theme, &resolved_name, &error)) {
    return false;
  }

  theme_ = loaded_theme;
  active_colorscheme_name_ = resolved_name.empty() ? requested_name : resolved_name;
  if (!project_base_color_.has_value() && !project_root_.empty()) {
    project_base_color_ = DefaultProjectBaseColor(project_root_);
  }
  if (project_base_color_.has_value()) {
    ApplyProjectAccent(theme_, *project_base_color_);
  }
  if (std::find(available_colorscheme_names_.begin(), available_colorscheme_names_.end(),
                active_colorscheme_name_) == available_colorscheme_names_.end()) {
    available_colorscheme_names_.push_back(active_colorscheme_name_);
    std::sort(available_colorscheme_names_.begin(), available_colorscheme_names_.end());
  }

  if (persist) {
    SaveConfigState();
  }
  return true;
}

bool WorkspaceShell::ApplyUiScale(float scale, bool persist, bool log_feedback) {
  (void) log_feedback;
  if (!std::isfinite(scale)) {
    return false;
  }

  ui_scale_ = std::clamp(scale, kMinUiScale, kMaxUiScale);
  if (persist) {
    SaveUserConfig();
  }
  return true;
}

bool WorkspaceShell::RestoreUserConfig() {
  const std::filesystem::path config_path = UserConfigPath();
  if (config_path.empty()) {
    return false;
  }

  const auto text = util::ReadTextFile(config_path);
  if (!text.has_value()) {
    return false;
  }

  PersistedUserConfigState state{.ui_scale = ui_scale_};
  if (!ParseUserConfigText(*text, &state)) {
    return false;
  }

  return ApplyUiScale(state.ui_scale, false, false);
}

void WorkspaceShell::SaveUserConfig() const {
  const std::filesystem::path config_path = UserConfigPath();
  if (config_path.empty()) {
    return;
  }
  util::WriteTextFileAtomically(config_path,
                                SerializeUserConfig(PersistedUserConfigState{.ui_scale = ui_scale_}));
}

bool WorkspaceShell::RestoreConfigState() {
  const std::filesystem::path config_path = ConfigStatePath();
  if (config_path.empty()) {
    return false;
  }
  const auto text = util::ReadTextFile(config_path);
  if (!text.has_value()) {
    return false;
  }

  PersistedProjectConfigState state{
      .editor_tab_size = editor_preferences_.tab_size,
      .editor_indent_width = editor_preferences_.indent_width,
      .editor_soft_tabs = editor_preferences_.soft_tabs,
      .colorscheme_name = active_colorscheme_name_,
      .project_base_color = project_base_color_,
  };
  if (!ParseProjectConfigText(*text, &state)) {
    return false;
  }

  editor_preferences_.tab_size = state.editor_tab_size;
  editor_preferences_.indent_width = state.editor_indent_width;
  editor_preferences_.soft_tabs = state.editor_soft_tabs;
  project_base_color_ = state.project_base_color;
  ApplyEditorPreferencesToAllTabs();
  ApplyColorscheme(state.colorscheme_name, false, false);
  return true;
}

void WorkspaceShell::SaveConfigState() const {
  if (project_root_.empty()) {
    return;
  }

  const std::filesystem::path config_path = ConfigStatePath();
  if (config_path.empty()) {
    return;
  }
  util::WriteTextFileAtomically(config_path, SerializeProjectConfig(PersistedProjectConfigState{
                                             .editor_tab_size = editor_preferences_.tab_size,
                                             .editor_indent_width = editor_preferences_.indent_width,
                                             .editor_soft_tabs = editor_preferences_.soft_tabs,
                                             .colorscheme_name = active_colorscheme_name_,
                                             .project_base_color = project_base_color_.value_or(
                                                 DefaultProjectBaseColor(project_root_)),
                                         }));
}

std::filesystem::path WorkspaceShell::SessionStatePath() const {
  return project_root_.empty() ? std::filesystem::path{} : ProjectStateDirectory() / "session";
}

void WorkspaceShell::ApplyEditorPreferences(editor::TextViewport& viewport) const {
  viewport.SetTabSize(editor_preferences_.tab_size);
  viewport.SetIndentWidth(editor_preferences_.indent_width);
  viewport.SetSoftTabs(editor_preferences_.soft_tabs);
}

void WorkspaceShell::ApplyEditorPreferencesToAllTabs() {
  ApplyEditorPreferences(text_viewport_);
  for (auto& tab : open_tabs_) {
    if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
      continue;
    }
    for (auto& view : tab.editor_state->views) {
      ApplyEditorPreferences(view.viewport);
    }
  }
}

bool WorkspaceShell::RestoreSessionState() {
  util::StartupTrace::Scope trace_scope("WorkspaceShell::RestoreSessionState");
  const std::filesystem::path session_path = SessionStatePath();
  if (session_path.empty()) {
    return false;
  }
  const auto text = util::ReadTextFile(session_path);
  if (!text.has_value()) {
    return false;
  }

  PersistedProjectSessionState persisted_session;
  persisted_session.sidebar_visible = surface_.sidebar_visible;
  persisted_session.sidebar_width = surface_.sidebar_width;
  persisted_session.bottom_panel_height = surface_.bottom_panel_height;
  persisted_session.active_tab_index = active_tab_index_;
  if (!ParseProjectSessionText(*text, &persisted_session)) {
    return false;
  }

  open_tabs_.clear();
  active_tab_index_ = 0;
  surface_.overlay_visible = false;
  surface_.command_mode = false;
  overlay_workflow_.compare_picker.matches.clear();
  overlay_workflow_.compare_picker.commits.clear();
  overlay_workflow_.compare_picker.selected_index = 0;

  for (const PersistedEditorTabState& persisted_tab : persisted_session.tabs) {
    if (persisted_tab.kind == "compare") {
      std::filesystem::path compare_path = persisted_tab.compare_path;
      if (compare_path.is_relative()) {
        compare_path = project_root_ / compare_path;
      }
      compare_path = compare_path.lexically_normal();

      if (compare_path.empty() || persisted_tab.compare_commit_hash.empty() ||
          persisted_tab.compare_commit_short_hash.empty()) {
        continue;
      }

      const project::GitCommitEntry commit{
          .hash = persisted_tab.compare_commit_hash,
          .short_hash = persisted_tab.compare_commit_short_hash,
          .subject = {},
      };
      std::optional<TabEntry> compare_tab;
      if (!persisted_tab.compare_right_ref.empty()) {
        auto resolve_path = [&](std::filesystem::path path, const std::filesystem::path& fallback) {
          if (path.empty()) {
            return fallback;
          }
          if (path.is_relative()) {
            path = project_root_ / path;
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
        compare_state.right_label = persisted_tab.compare_right_label.empty()
                                        ? (persisted_tab.compare_right_ref == "WORKTREE"
                                               ? "Working tree"
                                               : persisted_tab.compare_right_ref)
                                        : persisted_tab.compare_right_label;
        compare_state.selected_row = persisted_tab.compare_selected_row;
        compare_state.scroll_row = static_cast<int>(std::min<std::size_t>(
            persisted_tab.compare_scroll_row,
            static_cast<std::size_t>(std::numeric_limits<int>::max())));
        compare_state.horizontal_scroll = persisted_tab.compare_horizontal_scroll;
        compare_state.persistable = true;
        compare_tab = BuildCompareTabEntry(compare_path, compare_state);
      } else {
        compare_tab = BuildCompareTabEntry(compare_path, commit, persisted_tab.compare_selected_row);
      }
      if (!compare_tab.has_value()) {
        continue;
      }
      compare_tab->compare->scroll_row = static_cast<int>(std::min<std::size_t>(
          persisted_tab.compare_scroll_row, static_cast<std::size_t>(std::numeric_limits<int>::max())));
      compare_tab->compare->horizontal_scroll = persisted_tab.compare_horizontal_scroll;
      open_tabs_.push_back(std::move(*compare_tab));
      continue;
    }
    if (persisted_tab.kind == "merge") {
      auto resolve_path = [&](std::filesystem::path path) {
        if (path.is_relative()) {
          path = project_root_ / path;
        }
        return path.lexically_normal();
      };

      const std::filesystem::path merge_base = resolve_path(persisted_tab.merge_base_path);
      const std::filesystem::path merge_incoming = resolve_path(persisted_tab.merge_incoming_path);
      const std::filesystem::path merge_current = resolve_path(persisted_tab.merge_current_path);
      const std::filesystem::path merge_output = resolve_path(persisted_tab.merge_output_path);
      if (merge_base.empty() || merge_incoming.empty() || merge_current.empty() ||
          merge_output.empty()) {
        continue;
      }

      auto merge_tab = BuildMergeTabEntry(merge_base, merge_incoming, merge_current, merge_output);
      if (!merge_tab.has_value() || !merge_tab->merge.has_value()) {
        continue;
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
      RefreshMergeTabDerivedState(merge_state);
      merge_state.selected_hunk =
          merge_state.conflicts.empty()
              ? 0
              : std::min(persisted_tab.merge_selected_hunk, merge_state.conflicts.size() - 1);
      merge_state.scroll_row = static_cast<int>(std::min<std::size_t>(
          persisted_tab.merge_scroll_row, static_cast<std::size_t>(std::numeric_limits<int>::max())));
      merge_state.horizontal_scroll = persisted_tab.merge_horizontal_scroll;
      merge_state.result_viewport.SetScrollLine(
          static_cast<std::size_t>(std::max(0, merge_state.scroll_row)));
      merge_state.result_viewport.SetHorizontalScroll(merge_state.horizontal_scroll);
      merge_state.scroll_row = static_cast<int>(merge_state.result_viewport.scroll_line());
      open_tabs_.push_back(std::move(*merge_tab));
      continue;
    }

    TabEntry::EditorTabState editor_state;
    editor_state.active_leaf_id = persisted_tab.active_leaf_id;

    for (const PersistedEditorViewState& persisted_view : persisted_tab.views) {
      std::filesystem::path view_path = persisted_view.path;
      if (!view_path.empty() && view_path.is_relative()) {
        view_path = project_root_ / view_path;
      }
      view_path = view_path.lexically_normal();

      if (persisted_view.dirty_snapshot) {
        editor::TextViewport restored_view;
        restored_view.LoadContent(SerializeLines(persisted_view.buffer_lines,
                                                 persisted_view.line_ending),
                                  view_path, persisted_view.line_ending);
        restored_view.MoveCursorTo(persisted_view.cursor_line, persisted_view.cursor_column);
        restored_view.SetScrollLine(persisted_view.scroll_line);
        restored_view.SetHorizontalScroll(persisted_view.horizontal_scroll);
        restored_view.SetDirty(true);
        ApplyEditorPreferences(restored_view);
        editor_state.views.push_back(TabEntry::EditorTabState::EditorViewState{
            .leaf_id = persisted_view.leaf_id,
            .viewport = std::move(restored_view),
            .restored_path = view_path,
            .restored_cursor_line = persisted_view.cursor_line,
            .restored_cursor_column = persisted_view.cursor_column,
            .restored_scroll_line = persisted_view.scroll_line,
            .restored_horizontal_scroll = persisted_view.horizontal_scroll,
            .needs_restore = false,
        });
        continue;
      }

      if (view_path.empty() || !std::filesystem::exists(view_path)) {
        continue;
      }
      editor_state.views.push_back(TabEntry::EditorTabState::EditorViewState{
          .leaf_id = persisted_view.leaf_id,
          .viewport = editor::TextViewport{},
          .restored_path = view_path,
          .restored_cursor_line = persisted_view.cursor_line,
          .restored_cursor_column = persisted_view.cursor_column,
          .restored_scroll_line = persisted_view.scroll_line,
          .restored_horizontal_scroll = persisted_view.horizontal_scroll,
          .needs_restore = true,
      });
    }

    if (editor_state.views.empty()) {
      continue;
    }

    if (!persisted_tab.split_nodes.empty()) {
      std::vector<PersistedSplitNodeState> split_nodes = persisted_tab.split_nodes;
      std::sort(split_nodes.begin(), split_nodes.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.path.size() != rhs.path.size()) {
          return lhs.path.size() < rhs.path.size();
        }
        return lhs.path < rhs.path;
      });

      for (const PersistedSplitNodeState& node_state : split_nodes) {
        auto make_node = [&]() {
          auto node = std::make_unique<TabEntry::EditorTabState::EditorSplitNode>();
          node->leaf_id = node_state.leaf_id;
          node->size_fraction = std::max(0.0f, node_state.size_fraction);
          if (node_state.orientation == "vertical") {
            node->orientation = EditorSplitOrientation::Vertical;
          } else if (node_state.orientation == "horizontal") {
            node->orientation = EditorSplitOrientation::Horizontal;
          } else {
            node->orientation = EditorSplitOrientation::None;
          }
          return node;
        };

        if (node_state.path.empty()) {
          editor_state.split_root = make_node();
          continue;
        }

        std::vector<std::size_t> parent_path(node_state.path.begin(), node_state.path.end() - 1);
        auto* parent = FindEditorSplitNode(editor_state.split_root.get(), parent_path);
        if (parent == nullptr) {
          continue;
        }
        const std::size_t child_index = node_state.path.back();
        if (parent->children.size() <= child_index) {
          parent->children.resize(child_index + 1);
        }
        parent->children[child_index] = make_node();
      }
    }

    NormalizeEditorSplitTree(editor_state);
    const TabEntry::EditorTabState::EditorViewState* active_view =
        FindEditorViewState(editor_state, editor_state.active_leaf_id);
    if (active_view == nullptr) {
      active_view = &editor_state.views.front();
      editor_state.active_leaf_id = editor_state.views.front().leaf_id;
    }

    const std::filesystem::path tab_path = EditorViewPath(*active_view);
    open_tabs_.push_back(TabEntry{
        .kind = TabEntry::Kind::Editor,
        .path = tab_path,
        .title = tab_path.empty() ? "untitled" : tab_path.filename().string(),
        .editor_state = std::move(editor_state),
        .compare = std::nullopt,
        .merge = std::nullopt,
    });
  }

  surface_.sidebar_visible = persisted_session.sidebar_visible;
  surface_.sidebar_width = persisted_session.sidebar_width;
  surface_.bottom_panel_height = persisted_session.bottom_panel_height;

  if (open_tabs_.empty()) {
    text_viewport_.SetPlaceholderText(
        "microide\n\n"
        "Project loaded.\n"
        "Use the sidebar to open files.\n");
    surface_.focus = surface_.sidebar_visible ? FocusTarget::Sidebar : FocusTarget::Editor;
    return true;
  }

  const std::size_t active_index =
      std::min(persisted_session.active_tab_index, open_tabs_.size() - 1);
  active_tab_index_ = active_index;
  surface_.focus = surface_.sidebar_visible ? FocusTarget::Sidebar : FocusTarget::Editor;
  return true;
}

void WorkspaceShell::SaveSessionState() {
  if (project_root_.empty()) {
    return;
  }

  SyncActiveEditorTab();

  const std::filesystem::path session_path = SessionStatePath();
  if (session_path.empty()) {
    return;
  }

  PersistedProjectSessionState persisted_session;
  persisted_session.sidebar_visible = surface_.sidebar_visible;
  persisted_session.sidebar_width = surface_.sidebar_width;
  persisted_session.bottom_panel_height = surface_.bottom_panel_height;
  persisted_session.active_tab_index = 0;

  for (std::size_t tab_index = 0; tab_index < open_tabs_.size(); ++tab_index) {
    auto& tab = open_tabs_[tab_index];
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
    if (tab_index == active_tab_index_) {
      persisted_session.active_tab_index = persisted_session.tabs.size();
    }
    persisted_session.tabs.push_back(std::move(*persisted_tab));
  }

  util::WriteTextFileAtomically(session_path, SerializeProjectSession(persisted_session));
}

std::filesystem::path WorkspaceShell::WorkspaceSessionStatePath() const {
  const std::filesystem::path state_root =
      platform::ResolveAppDirectory(platform::UserDirectoryKind::State, "microide");
  return state_root.empty() ? std::filesystem::path{} : state_root / "workspace-session";
}

bool WorkspaceShell::RestoreWorkspaceSession() {
  util::StartupTrace::Scope trace_scope("WorkspaceShell::RestoreWorkspaceSession");
  const std::filesystem::path session_path = WorkspaceSessionStatePath();
  if (session_path.empty()) {
    return false;
  }
  const auto text = util::ReadTextFile(session_path);
  if (!text.has_value()) {
    return false;
  }

  PersistedWorkspaceSessionState persisted_session;
  if (!ParseWorkspaceSessionText(*text, &persisted_session)) {
    return false;
  }

  project_catalog_.entries.clear();
  project_catalog_.active_index = 0;
  project_catalog_.tab_scroll_index = 0;

  if (persisted_session.project_roots.empty()) {
    ResetProjectCatalogToWelcomeState();
    return true;
  }

  for (const auto& root : persisted_session.project_roots) {
    const std::filesystem::path normalized_root = ResolveProjectRootInput(root);
    std::error_code error;
    if (normalized_root.empty() || !std::filesystem::exists(normalized_root, error) || error ||
        !std::filesystem::is_directory(normalized_root, error)) {
      continue;
    }
    auto project_state = std::make_unique<ProjectWorkspaceState>();
    project_state->root = normalized_root;
    project_state->restore_persistence_on_activate = true;
    project_catalog_.entries.push_back(std::move(project_state));
  }

  if (project_catalog_.entries.empty()) {
    ResetProjectCatalogToWelcomeState();
    return true;
  }

  if (!ProjectCatalogCoordinator(*this).RestoreAfterRemoval(
          std::min(persisted_session.active_project_index, project_catalog_.entries.size() - 1),
          true)) {
    return true;
  }
  EnsureActiveProjectVisible();
  return true;
}

void WorkspaceShell::SaveWorkspaceSession() {
  const std::filesystem::path session_path = WorkspaceSessionStatePath();
  if (session_path.empty()) {
    return;
  }

  PersistedWorkspaceSessionState persisted_session;
  persisted_session.active_project_index =
      project_catalog_.entries.empty() ? 0 : std::min(project_catalog_.active_index, project_catalog_.entries.size() - 1);
  persisted_session.project_roots.reserve(project_catalog_.entries.size());
  for (std::size_t i = 0; i < project_catalog_.entries.size(); ++i) {
    const auto* entry = ProjectCatalogEntry(i);
    const std::filesystem::path project_root =
        entry != nullptr ? entry->root : ProjectCatalogRoot(i);
    if (project_root.empty()) {
      continue;
    }
    persisted_session.project_roots.push_back(project_root.lexically_normal());
  }
  util::WriteTextFileAtomically(session_path, SerializeWorkspaceSession(persisted_session));
}

std::optional<PersistedEditorTabState> WorkspaceShell::BuildPersistedCompareTabState(
    const TabEntry& tab) const {
  if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value() || !tab.compare->persistable) {
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
  return persisted_tab;
}

std::optional<PersistedEditorTabState> WorkspaceShell::BuildPersistedMergeTabState(
    const TabEntry& tab) const {
  if (tab.kind != TabEntry::Kind::Merge || !tab.merge.has_value() || !tab.merge->persistable) {
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

std::optional<PersistedEditorTabState> WorkspaceShell::BuildPersistedEditorTabState(
    std::size_t tab_index,
    TabEntry& tab) {
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
      tab.editor_state->views.empty()) {
    return std::nullopt;
  }

  auto& editor_state = tab.editor_state.value();
  NormalizeEditorSplitTree(editor_state);

  PersistedEditorTabState persisted_tab;
  persisted_tab.kind = "editor";
  persisted_tab.active_leaf_id = editor_state.active_leaf_id;
  for (const auto& view : editor_state.views) {
    const bool active_live_view =
        tab_index == active_tab_index_ && view.leaf_id == editor_state.active_leaf_id &&
        !view.needs_restore;
    const editor::TextViewport* persisted_viewport = active_live_view ? &text_viewport_ : &view.viewport;
    const std::filesystem::path normalized_path =
        view.needs_restore ? view.restored_path.lexically_normal()
                           : persisted_viewport->path().lexically_normal();
    const bool dirty_snapshot = !view.needs_restore && persisted_viewport->dirty();
    if (normalized_path.empty()) {
      if (!dirty_snapshot) {
        continue;
      }
    } else if (view.needs_restore && !dirty_snapshot) {
      continue;
    }
    const std::size_t cursor_line =
        view.needs_restore ? view.restored_cursor_line : persisted_viewport->cursor_line();
    const std::size_t cursor_column =
        view.needs_restore ? view.restored_cursor_column : persisted_viewport->cursor_column();
    const std::size_t scroll_line =
        view.needs_restore ? view.restored_scroll_line : persisted_viewport->scroll_line();
    const std::size_t horizontal_scroll =
        view.needs_restore ? view.restored_horizontal_scroll
                           : persisted_viewport->horizontal_scroll();
    persisted_tab.views.push_back(PersistedEditorViewState{
        .leaf_id = view.leaf_id,
        .path = normalized_path,
        .cursor_line = cursor_line,
        .cursor_column = cursor_column,
        .scroll_line = scroll_line,
        .horizontal_scroll = horizontal_scroll,
        .dirty_snapshot = dirty_snapshot,
        .line_ending = persisted_viewport->line_ending(),
        .buffer_lines = dirty_snapshot ? persisted_viewport->lines() : std::vector<std::string>{},
    });
  }

  std::vector<std::size_t> node_path;
  const auto collect_split_node =
      [&](auto&& self, const TabEntry::EditorTabState::EditorSplitNode* node) -> void {
    if (node == nullptr) {
      return;
    }

    std::string orientation = "leaf";
    if (!node->IsLeaf()) {
      orientation = node->orientation == EditorSplitOrientation::Horizontal ? "horizontal"
                                                                            : "vertical";
    }
    persisted_tab.split_nodes.push_back(PersistedSplitNodeState{
        .path = node_path,
        .orientation = orientation,
        .size_fraction = node->size_fraction,
        .leaf_id = node->leaf_id,
    });
    for (std::size_t child_index = 0; child_index < node->children.size(); ++child_index) {
      node_path.push_back(child_index);
      self(self, node->children[child_index].get());
      node_path.pop_back();
    }
  };
  collect_split_node(collect_split_node, editor_state.split_root.get());
  return persisted_tab;
}

}  // namespace microide::workspace
