#include "workspace/WorkspacePersistenceCoordinator.h"

#include <algorithm>
#include <limits>

#include "util/StartupTrace.h"
#include "util/TextFileIO.h"
#include "workspace/WorkspacePersistenceFormat.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

std::filesystem::path WorkspaceShell::PersistenceCoordinator::SessionStatePath() const {
  return shell_.project_root_.empty() ? std::filesystem::path{}
                                      : shell_.ProjectStateDirectory() / "session";
}

bool WorkspaceShell::PersistenceCoordinator::RestoreSessionState() {
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
  persisted_session.sidebar_visible = shell_.sidebar_state_.visible;
  persisted_session.sidebar_width = shell_.sidebar_state_.width;
  persisted_session.bottom_panel_height = shell_.panel_state_.height;
  persisted_session.active_tab_index = shell_.active_tab_index_;
  if (!ParseProjectSessionText(*text, &persisted_session)) {
    return false;
  }

  shell_.open_tabs_.clear();
  shell_.active_tab_index_ = 0;
  shell_.overlay_state_.visible = false;
  shell_.panel_state_.command_mode = false;
  shell_.overlay_workflow_.compare_picker.matches.clear();
  shell_.overlay_workflow_.compare_picker.commits.clear();
  shell_.overlay_workflow_.compare_picker.selected_index = 0;

  for (const PersistedEditorTabState& persisted_tab : persisted_session.tabs) {
    if (persisted_tab.kind == "compare") {
      std::filesystem::path compare_path = persisted_tab.compare_path;
      if (compare_path.is_relative()) {
        compare_path = shell_.project_root_ / compare_path;
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
            path = shell_.project_root_ / path;
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
        compare_state.persistable = true;
        compare_tab = shell_.BuildCompareTabEntry(compare_path, compare_state);
      } else {
        compare_tab =
            shell_.BuildCompareTabEntry(compare_path, commit, persisted_tab.compare_selected_row);
      }
      if (!compare_tab.has_value()) {
        continue;
      }
      compare_tab->compare->scroll_row = static_cast<int>(std::min<std::size_t>(
          persisted_tab.compare_scroll_row,
          static_cast<std::size_t>(std::numeric_limits<int>::max())));
      compare_tab->compare->horizontal_scroll = persisted_tab.compare_horizontal_scroll;
      shell_.open_tabs_.push_back(std::move(*compare_tab));
      continue;
    }
    if (persisted_tab.kind == "merge") {
      auto resolve_path = [&](std::filesystem::path path) {
        if (path.is_relative()) {
          path = shell_.project_root_ / path;
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

      auto merge_tab =
          shell_.BuildMergeTabEntry(merge_base, merge_incoming, merge_current, merge_output);
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
      shell_.RefreshMergeTabDerivedState(merge_state);
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
      shell_.open_tabs_.push_back(std::move(*merge_tab));
      continue;
    }

    TabEntry::EditorTabState editor_state;
    editor_state.active_leaf_id = persisted_tab.active_leaf_id;

    for (const PersistedEditorViewState& persisted_view : persisted_tab.views) {
      std::filesystem::path view_path = persisted_view.path;
      if (!view_path.empty() && view_path.is_relative()) {
        view_path = shell_.project_root_ / view_path;
      }
      view_path = view_path.lexically_normal();

      if (persisted_view.dirty_snapshot) {
        editor::TextViewport restored_view;
        restored_view.LoadContent(
            SerializeLines(persisted_view.buffer_lines, persisted_view.line_ending), view_path,
            persisted_view.line_ending);
        restored_view.MoveCursorTo(persisted_view.cursor_line, persisted_view.cursor_column);
        restored_view.SetScrollLine(persisted_view.scroll_line);
        restored_view.SetHorizontalScroll(persisted_view.horizontal_scroll);
        restored_view.SetDirty(true);
        shell_.ApplyEditorPreferences(restored_view);
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
        auto* parent = shell_.FindEditorSplitNode(editor_state.split_root.get(), parent_path);
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

    shell_.NormalizeEditorSplitTree(editor_state);
    const TabEntry::EditorTabState::EditorViewState* active_view =
        shell_.FindEditorViewState(editor_state, editor_state.active_leaf_id);
    if (active_view == nullptr) {
      active_view = &editor_state.views.front();
      editor_state.active_leaf_id = editor_state.views.front().leaf_id;
    }

    const std::filesystem::path tab_path = shell_.EditorViewPath(*active_view);
    shell_.open_tabs_.push_back(TabEntry{
        .kind = TabEntry::Kind::Editor,
        .path = tab_path,
        .title = tab_path.empty() ? "untitled" : tab_path.filename().string(),
        .editor_state = std::move(editor_state),
        .compare = std::nullopt,
        .merge = std::nullopt,
    });
  }

  shell_.sidebar_state_.visible = persisted_session.sidebar_visible;
  shell_.sidebar_state_.width = persisted_session.sidebar_width;
  shell_.panel_state_.height = persisted_session.bottom_panel_height;

  if (shell_.open_tabs_.empty()) {
    shell_.text_viewport_.SetPlaceholderText(
        "microide\n\n"
        "Project loaded.\n"
        "Use the sidebar to open files.\n");
    shell_.surface_.focus =
        shell_.sidebar_state_.visible ? FocusTarget::Sidebar : FocusTarget::Editor;
    return true;
  }

  const std::size_t active_index =
      std::min(persisted_session.active_tab_index, shell_.open_tabs_.size() - 1);
  shell_.active_tab_index_ = active_index;
  shell_.surface_.focus =
      shell_.sidebar_state_.visible ? FocusTarget::Sidebar : FocusTarget::Editor;
  return true;
}

void WorkspaceShell::PersistenceCoordinator::SaveSessionState() {
  if (shell_.project_root_.empty()) {
    return;
  }

  shell_.SyncActiveEditorTab();

  const std::filesystem::path session_path = SessionStatePath();
  if (session_path.empty()) {
    return;
  }

  PersistedProjectSessionState persisted_session;
  persisted_session.sidebar_visible = shell_.sidebar_state_.visible;
  persisted_session.sidebar_width = shell_.sidebar_state_.width;
  persisted_session.bottom_panel_height = shell_.panel_state_.height;
  persisted_session.active_tab_index = 0;

  for (std::size_t tab_index = 0; tab_index < shell_.open_tabs_.size(); ++tab_index) {
    auto& tab = shell_.open_tabs_[tab_index];
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
    if (tab_index == shell_.active_tab_index_) {
      persisted_session.active_tab_index = persisted_session.tabs.size();
    }
    persisted_session.tabs.push_back(std::move(*persisted_tab));
  }

  util::WriteTextFileAtomically(session_path, SerializeProjectSession(persisted_session));
}

std::optional<PersistedEditorTabState>
WorkspaceShell::PersistenceCoordinator::BuildPersistedCompareTabState(const TabEntry& tab) const {
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

std::optional<PersistedEditorTabState>
WorkspaceShell::PersistenceCoordinator::BuildPersistedMergeTabState(const TabEntry& tab) const {
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

std::optional<PersistedEditorTabState>
WorkspaceShell::PersistenceCoordinator::BuildPersistedEditorTabState(std::size_t tab_index,
                                                                     TabEntry& tab) {
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
      tab.editor_state->views.empty()) {
    return std::nullopt;
  }

  auto& editor_state = tab.editor_state.value();
  shell_.NormalizeEditorSplitTree(editor_state);

  PersistedEditorTabState persisted_tab;
  persisted_tab.kind = "editor";
  persisted_tab.active_leaf_id = editor_state.active_leaf_id;
  for (const auto& view : editor_state.views) {
    const bool active_live_view =
        tab_index == shell_.active_tab_index_ && view.leaf_id == editor_state.active_leaf_id &&
        !view.needs_restore;
    const editor::TextViewport* persisted_viewport =
        active_live_view ? &shell_.text_viewport_ : &view.viewport;
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
