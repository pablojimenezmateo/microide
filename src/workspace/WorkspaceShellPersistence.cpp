#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>

#include "util/StartupTrace.h"
#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

void WorkspaceShell::RefreshAvailableColorschemeNames() {
  available_colorscheme_names_ = render::ListAvailableThemeNames();
}

bool WorkspaceShell::ApplyColorscheme(std::string_view name, bool persist, bool log_feedback) {
  render::Theme loaded_theme;
  std::string resolved_name;
  std::string error;
  const std::string requested_name = name.empty() ? "default" : std::string(name);
  if (!render::LoadThemeByName(requested_name, loaded_theme, &resolved_name, &error)) {
    if (log_feedback) {
      LogMessage(error.empty() ? "Failed to load colorscheme" : error);
    }
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
  if (log_feedback) {
    LogMessage("Colorscheme set to " + active_colorscheme_name_);
  }
  return true;
}

bool WorkspaceShell::ApplyUiScale(float scale, bool persist, bool log_feedback) {
  if (!std::isfinite(scale)) {
    return false;
  }

  ui_scale_ = std::clamp(scale, kMinUiScale, kMaxUiScale);
  if (persist) {
    SaveUserConfig();
  }
  if (log_feedback) {
    LogMessage("UI scale set to " + UiScaleLabel(ui_scale_));
  }
  return true;
}

bool WorkspaceShell::RestoreUserConfig() {
  const std::filesystem::path config_path = UserConfigPath();
  if (config_path.empty()) {
    return false;
  }

  std::ifstream file(config_path);
  if (!file) {
    return false;
  }

  bool version_ok = false;
  float restored_scale = ui_scale_;
  std::string line;
  while (std::getline(file, line)) {
    const ParsedCommandLine parsed = ParseCommandLine(line);
    if (parsed.tokens.empty()) {
      continue;
    }

    const auto& tokens = parsed.tokens;
    const std::string& command = tokens.front().text;
    if (command == "version") {
      version_ok = tokens.size() == 2 && tokens[1].text == "1";
      continue;
    }
    if (!version_ok) {
      return false;
    }
    if (command == "ui-scale" && tokens.size() == 2) {
      if (const auto scale = ParseUiScaleValue(tokens[1].text); scale.has_value()) {
        restored_scale = *scale;
      }
    }
  }

  if (!version_ok) {
    return false;
  }

  return ApplyUiScale(restored_scale, false, false);
}

void WorkspaceShell::SaveUserConfig() const {
  const std::filesystem::path config_path = UserConfigPath();
  if (config_path.empty()) {
    return;
  }

  std::error_code error;
  std::filesystem::create_directories(config_path.parent_path(), error);

  std::ofstream file(config_path, std::ios::trunc);
  if (!file) {
    return;
  }

  file << "version 1\n";
  file << "ui-scale " << ui_scale_ << '\n';
}

bool WorkspaceShell::RestoreConfigState() {
  const std::filesystem::path config_path = ConfigStatePath();
  if (config_path.empty()) {
    return false;
  }
  std::ifstream file(config_path);
  if (!file) {
    return false;
  }

  bool version_ok = false;
  EditorPreferences restored = editor_preferences_;
  std::string restored_colorscheme = active_colorscheme_name_;
  std::optional<SDL_Color> restored_project_base_color = project_base_color_;
  std::string line;
  while (std::getline(file, line)) {
    const ParsedCommandLine parsed = ParseCommandLine(line);
    if (parsed.tokens.empty()) {
      continue;
    }

    const auto& tokens = parsed.tokens;
    const std::string& command = tokens.front().text;
    if (command == "version") {
      version_ok = tokens.size() == 2 && tokens[1].text == "1";
      continue;
    }
    if (!version_ok) {
      return false;
    }
    if (command == "editor-tab-size" && tokens.size() == 2) {
      try {
        restored.tab_size =
            std::clamp<std::size_t>(static_cast<std::size_t>(std::stoull(tokens[1].text)), 1, 16);
      } catch (...) {
      }
      continue;
    }
    if (command == "editor-indent-width" && tokens.size() == 2) {
      try {
        restored.indent_width =
            std::clamp<std::size_t>(static_cast<std::size_t>(std::stoull(tokens[1].text)), 1, 16);
      } catch (...) {
      }
      continue;
    }
    if (command == "editor-soft-tabs" && tokens.size() == 2) {
      restored.soft_tabs = tokens[1].text == "1" || tokens[1].text == "on" ||
                           tokens[1].text == "true";
      continue;
    }
    if (command == "colorscheme" && tokens.size() == 2) {
      restored_colorscheme = tokens[1].text;
      continue;
    }
    if (command == "project-base-color" && tokens.size() == 2) {
      restored_project_base_color = ParseProjectColor(tokens[1].text);
    }
  }

  if (!version_ok) {
    return false;
  }

  editor_preferences_ = restored;
  project_base_color_ = restored_project_base_color;
  ApplyEditorPreferencesToAllTabs();
  ApplyColorscheme(restored_colorscheme, false, false);
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
  std::error_code error;
  std::filesystem::create_directories(config_path.parent_path(), error);

  std::ofstream file(config_path, std::ios::trunc);
  if (!file) {
    return;
  }

  file << "version 1\n";
  file << "editor-tab-size " << editor_preferences_.tab_size << '\n';
  file << "editor-indent-width " << editor_preferences_.indent_width << '\n';
  file << "editor-soft-tabs " << (editor_preferences_.soft_tabs ? 1 : 0) << '\n';
  file << "colorscheme " << QuoteCommandArg(active_colorscheme_name_) << '\n';
  file << "project-base-color "
       << QuoteCommandArg(
              FormatProjectColor(project_base_color_.value_or(DefaultProjectBaseColor(project_root_))))
       << '\n';
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
  std::ifstream file(session_path);
  if (!file) {
    return false;
  }

  bool version_ok = false;
  bool restored_sidebar_visible = sidebar_visible_;
  float restored_sidebar_width = sidebar_width_;
  float restored_bottom_panel_height = bottom_panel_height_;
  std::optional<std::size_t> restored_active_tab_index;
  std::vector<PersistedEditorTabState> persisted_tabs;
  std::optional<PersistedEditorTabState> current_tab;

  std::string line;
  while (std::getline(file, line)) {
    const ParsedCommandLine parsed = ParseCommandLine(line);
    if (parsed.tokens.empty()) {
      continue;
    }

    const std::vector<ParsedCommandToken>& tokens = parsed.tokens;
    const std::string& command = tokens.front().text;
    if (command == "version") {
      version_ok = tokens.size() == 2 && tokens[1].text == "1";
      continue;
    }
    if (!version_ok) {
      return false;
    }
    if (command == "sidebar-visible" && tokens.size() == 2) {
      restored_sidebar_visible = tokens[1].text == "1";
      continue;
    }
    if (command == "sidebar-width" && tokens.size() == 2) {
      try {
        restored_sidebar_width = std::stof(tokens[1].text);
      } catch (...) {
      }
      continue;
    }
    if (command == "bottom-panel-height" && tokens.size() == 2) {
      try {
        restored_bottom_panel_height = std::stof(tokens[1].text);
      } catch (...) {
      }
      continue;
    }
    if (command == "active-tab" && tokens.size() == 2) {
      try {
        restored_active_tab_index = static_cast<std::size_t>(std::stoull(tokens[1].text));
      } catch (...) {
      }
      continue;
    }
    if (command == "tab-begin") {
      current_tab = PersistedEditorTabState{};
      continue;
    }
    if (!current_tab.has_value()) {
      continue;
    }
    if (command == "tab-end") {
      persisted_tabs.push_back(*current_tab);
      current_tab.reset();
      continue;
    }
    if (command == "active-leaf" && tokens.size() == 2) {
      try {
        current_tab->active_leaf_id = static_cast<std::size_t>(std::stoull(tokens[1].text));
      } catch (...) {
      }
      continue;
    }
    if (command == "kind" && tokens.size() == 2) {
      current_tab->kind = tokens[1].text;
      continue;
    }
    if (command == "view" && tokens.size() == 7) {
      try {
        current_tab->views.push_back(PersistedEditorViewState{
            .leaf_id = static_cast<std::size_t>(std::stoull(tokens[1].text)),
            .path = std::filesystem::path(tokens[2].text),
            .cursor_line = static_cast<std::size_t>(std::stoull(tokens[3].text)),
            .cursor_column = static_cast<std::size_t>(std::stoull(tokens[4].text)),
            .scroll_line = static_cast<std::size_t>(std::stoull(tokens[5].text)),
            .horizontal_scroll = static_cast<std::size_t>(std::stoull(tokens[6].text)),
        });
      } catch (...) {
      }
      continue;
    }
    if (command == "compare-path" && tokens.size() == 2) {
      current_tab->compare_path = std::filesystem::path(tokens[1].text);
      continue;
    }
    if (command == "compare-commit" && tokens.size() == 3) {
      current_tab->compare_commit_hash = tokens[1].text;
      current_tab->compare_commit_short_hash = tokens[2].text;
      continue;
    }
    if (command == "compare-selected-row" && tokens.size() == 2) {
      try {
        current_tab->compare_selected_row = static_cast<std::size_t>(std::stoull(tokens[1].text));
      } catch (...) {
      }
      continue;
    }
    if (command == "merge-base" && tokens.size() == 2) {
      current_tab->merge_base_path = std::filesystem::path(tokens[1].text);
      continue;
    }
    if (command == "merge-incoming" && tokens.size() == 2) {
      current_tab->merge_incoming_path = std::filesystem::path(tokens[1].text);
      continue;
    }
    if (command == "merge-current" && tokens.size() == 2) {
      current_tab->merge_current_path = std::filesystem::path(tokens[1].text);
      continue;
    }
    if (command == "merge-output" && tokens.size() == 2) {
      current_tab->merge_output_path = std::filesystem::path(tokens[1].text);
      continue;
    }
    if (command == "merge-selected-hunk" && tokens.size() == 2) {
      try {
        current_tab->merge_selected_hunk = static_cast<std::size_t>(std::stoull(tokens[1].text));
      } catch (...) {
      }
      continue;
    }
    if (command == "merge-choice" && tokens.size() == 3) {
      try {
        const std::size_t hunk_index = static_cast<std::size_t>(std::stoull(tokens[1].text));
        if (current_tab->merge_hunk_choices.size() <= hunk_index) {
          current_tab->merge_hunk_choices.resize(hunk_index + 1);
        }
        current_tab->merge_hunk_choices[hunk_index] = tokens[2].text;
      } catch (...) {
      }
      continue;
    }
    if (command == "split-node" && tokens.size() == 5) {
      const auto path = DecodeSessionNodePath(tokens[1].text);
      if (!path.has_value()) {
        continue;
      }
      try {
        current_tab->split_nodes.push_back(PersistedSplitNodeState{
            .path = *path,
            .orientation = tokens[2].text,
            .size_fraction = std::stof(tokens[3].text),
            .leaf_id = static_cast<std::size_t>(std::stoull(tokens[4].text)),
        });
      } catch (...) {
      }
      continue;
    }
  }

  if (!version_ok) {
    return false;
  }

  open_tabs_.clear();
  active_tab_index_ = 0;
  overlay_visible_ = false;
  command_mode_ = false;
  compare_picker_matches_.clear();
  compare_picker_commits_.clear();
  compare_picker_selected_index_ = 0;

  for (const PersistedEditorTabState& persisted_tab : persisted_tabs) {
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
      auto compare_tab =
          BuildCompareTabEntry(compare_path, commit, persisted_tab.compare_selected_row);
      if (!compare_tab.has_value()) {
        continue;
      }
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
        return compare::MergeChoice::Auto;
      };

      auto& merge_state = merge_tab->merge.value();
      for (std::size_t i = 0;
           i < merge_state.model.hunks.size() && i < persisted_tab.merge_hunk_choices.size(); ++i) {
        if (persisted_tab.merge_hunk_choices[i].empty()) {
          continue;
        }
        merge_state.model.hunks[i].choice = parse_choice(persisted_tab.merge_hunk_choices[i]);
      }
      merge_state.selected_hunk = merge_state.model.hunks.empty()
                                      ? 0
                                      : std::min(persisted_tab.merge_selected_hunk,
                                                 merge_state.model.hunks.size() - 1);
      RefreshMergeTabDerivedState(merge_state);
      open_tabs_.push_back(std::move(*merge_tab));
      continue;
    }

    TabEntry::EditorTabState editor_state;
    editor_state.active_leaf_id = persisted_tab.active_leaf_id;

    for (const PersistedEditorViewState& persisted_view : persisted_tab.views) {
      std::filesystem::path view_path = persisted_view.path;
      if (view_path.is_relative()) {
        view_path = project_root_ / view_path;
      }
      view_path = view_path.lexically_normal();

      if (!std::filesystem::exists(view_path)) {
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

  sidebar_visible_ = restored_sidebar_visible;
  sidebar_width_ = restored_sidebar_width;
  bottom_panel_height_ = restored_bottom_panel_height;

  if (open_tabs_.empty()) {
    text_viewport_.SetPlaceholderText(
        "microide\n\n"
        "Project loaded.\n"
        "Use the sidebar to open files.\n");
    focus_ = sidebar_visible_ ? FocusTarget::Sidebar : FocusTarget::Editor;
    return true;
  }

  const std::size_t active_index =
      std::min(restored_active_tab_index.value_or(0), open_tabs_.size() - 1);
  active_tab_index_ = active_index;
  focus_ = sidebar_visible_ ? FocusTarget::Sidebar : FocusTarget::Editor;
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
  std::error_code error;
  std::filesystem::create_directories(session_path.parent_path(), error);

  std::ofstream file(session_path, std::ios::trunc);
  if (!file) {
    return;
  }

  file << "version 1\n";
  file << "sidebar-visible " << (sidebar_visible_ ? 1 : 0) << '\n';
  file << "sidebar-width " << sidebar_width_ << '\n';
  file << "bottom-panel-height " << bottom_panel_height_ << '\n';

  std::size_t persisted_active_tab = 0;
  std::size_t persisted_tab_count = 0;
  for (std::size_t tab_index = 0; tab_index < open_tabs_.size(); ++tab_index) {
    auto& tab = open_tabs_[tab_index];
    if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
        tab.compare->persistable) {
      if (tab_index == active_tab_index_) {
        persisted_active_tab = persisted_tab_count;
      }

      file << "tab-begin\n";
      file << "kind compare\n";
      file << "compare-path " << QuoteCommandArg(tab.compare->path.lexically_normal().string())
           << '\n';
      file << "compare-commit " << QuoteCommandArg(tab.compare->commit_hash) << ' '
           << QuoteCommandArg(tab.compare->left_label) << '\n';
      file << "compare-selected-row " << tab.compare->selected_row << '\n';
      file << "tab-end\n";
      ++persisted_tab_count;
      continue;
    }
    if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value() && tab.merge->persistable) {
      if (tab_index == active_tab_index_) {
        persisted_active_tab = persisted_tab_count;
      }

      file << "tab-begin\n";
      file << "kind merge\n";
      file << "merge-base " << QuoteCommandArg(tab.merge->base_path.lexically_normal().string())
           << '\n';
      file << "merge-incoming "
           << QuoteCommandArg(tab.merge->incoming_path.lexically_normal().string()) << '\n';
      file << "merge-current "
           << QuoteCommandArg(tab.merge->current_path.lexically_normal().string()) << '\n';
      file << "merge-output "
           << QuoteCommandArg(tab.merge->output_path.lexically_normal().string()) << '\n';
      file << "merge-selected-hunk " << tab.merge->selected_hunk << '\n';
      for (std::size_t i = 0; i < tab.merge->model.hunks.size(); ++i) {
        file << "merge-choice " << i << ' '
             << QuoteCommandArg(compare::MergeChoiceLabel(tab.merge->model.hunks[i].choice))
             << '\n';
      }
      file << "tab-end\n";
      ++persisted_tab_count;
      continue;
    }

    if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
        tab.editor_state->views.empty()) {
      continue;
    }

    auto& editor_state = tab.editor_state.value();
    NormalizeEditorSplitTree(editor_state);
    if (tab_index == active_tab_index_) {
      persisted_active_tab = persisted_tab_count;
    }

    file << "tab-begin\n";
    file << "kind editor\n";
    file << "active-leaf " << editor_state.active_leaf_id << '\n';
    for (const auto& view : editor_state.views) {
      const bool active_live_view =
          tab_index == active_tab_index_ && view.leaf_id == editor_state.active_leaf_id &&
          !view.needs_restore;
      const editor::TextViewport* persisted_viewport =
          active_live_view ? &text_viewport_ : &view.viewport;
      const std::filesystem::path normalized_path =
          view.needs_restore ? view.restored_path.lexically_normal()
                             : persisted_viewport->path().lexically_normal();
      if (normalized_path.empty()) {
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
      file << "view " << view.leaf_id << ' ' << QuoteCommandArg(normalized_path.string()) << ' '
           << cursor_line << ' ' << cursor_column << ' ' << scroll_line << ' '
           << horizontal_scroll << '\n';
    }

    std::vector<std::size_t> node_path;
    const auto write_split_node =
        [&](auto&& self, const TabEntry::EditorTabState::EditorSplitNode* node) -> void {
      if (node == nullptr) {
        return;
      }

      std::string orientation = "leaf";
      if (!node->IsLeaf()) {
        orientation = node->orientation == EditorSplitOrientation::Horizontal ? "horizontal"
                                                                              : "vertical";
      }
      file << "split-node " << EncodeSessionNodePath(node_path) << ' ' << orientation << ' '
           << node->size_fraction << ' ' << node->leaf_id << '\n';
      for (std::size_t child_index = 0; child_index < node->children.size(); ++child_index) {
        node_path.push_back(child_index);
        self(self, node->children[child_index].get());
        node_path.pop_back();
      }
    };
    write_split_node(write_split_node, editor_state.split_root.get());
    file << "tab-end\n";
    ++persisted_tab_count;
  }

  file << "active-tab " << persisted_active_tab << '\n';
}

std::filesystem::path WorkspaceShell::WorkspaceSessionStatePath() const {
  if (const char* xdg_state_home = std::getenv("XDG_STATE_HOME");
      xdg_state_home != nullptr && *xdg_state_home != '\0') {
    return std::filesystem::path(xdg_state_home) / "microide" / "workspace-session";
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return std::filesystem::path(home) / ".local" / "state" / "microide" / "workspace-session";
  }
  return {};
}

bool WorkspaceShell::RestoreWorkspaceSession() {
  util::StartupTrace::Scope trace_scope("WorkspaceShell::RestoreWorkspaceSession");
  const std::filesystem::path session_path = WorkspaceSessionStatePath();
  if (session_path.empty()) {
    return false;
  }

  std::ifstream file(session_path);
  if (!file) {
    return false;
  }

  bool version_ok = false;
  std::vector<std::filesystem::path> project_roots;
  std::optional<std::size_t> restored_active_project;
  std::string line;
  while (std::getline(file, line)) {
    const ParsedCommandLine parsed = ParseCommandLine(line);
    if (parsed.tokens.empty()) {
      continue;
    }

    const auto& tokens = parsed.tokens;
    const std::string& command = tokens.front().text;
    if (command == "version") {
      version_ok = tokens.size() == 2 && tokens[1].text == "1";
      continue;
    }
    if (!version_ok) {
      return false;
    }
    if (command == "project" && tokens.size() == 2) {
      project_roots.push_back(std::filesystem::path(tokens[1].text));
      continue;
    }
    if (command == "active-project" && tokens.size() == 2) {
      try {
        restored_active_project = static_cast<std::size_t>(std::stoull(tokens[1].text));
      } catch (...) {
      }
    }
  }

  if (!version_ok) {
    return false;
  }

  projects_.clear();
  active_project_index_ = 0;
  project_tab_scroll_index_ = 0;

  if (project_roots.empty()) {
    ResetProjectScopedState(true);
    return true;
  }

  for (const auto& root : project_roots) {
    const std::filesystem::path normalized_root = ResolveProjectRootInput(root);
    std::error_code error;
    if (normalized_root.empty() || !std::filesystem::exists(normalized_root, error) || error ||
        !std::filesystem::is_directory(normalized_root, error)) {
      continue;
    }
    auto project_state = std::make_unique<ProjectWorkspaceState>();
    project_state->root = normalized_root;
    project_state->restore_persistence_on_activate = true;
    projects_.push_back(std::move(project_state));
  }

  if (projects_.empty()) {
    ResetProjectScopedState(true);
    return true;
  }

  active_project_index_ = std::min(restored_active_project.value_or(0), projects_.size() - 1);
  if (!ActivateProjectState(*projects_[active_project_index_], true)) {
    projects_.erase(projects_.begin() + static_cast<std::ptrdiff_t>(active_project_index_));
    if (projects_.empty()) {
      ResetProjectScopedState(true);
      return true;
    }
    active_project_index_ = std::min(active_project_index_, projects_.size() - 1);
    ActivateProjectState(*projects_[active_project_index_], true);
  }
  EnsureActiveProjectVisible();
  LogMessage("Restored workspace session");
  return true;
}

void WorkspaceShell::SaveWorkspaceSession() {
  const std::filesystem::path session_path = WorkspaceSessionStatePath();
  if (session_path.empty()) {
    return;
  }

  std::error_code error;
  std::filesystem::create_directories(session_path.parent_path(), error);

  std::ofstream file(session_path, std::ios::trunc);
  if (!file) {
    return;
  }

  file << "version 1\n";
  for (std::size_t i = 0; i < projects_.size(); ++i) {
    const std::filesystem::path project_root =
        projects_[i] != nullptr ? projects_[i]->root : std::filesystem::path{};
    if (project_root.empty()) {
      continue;
    }
    file << "project " << QuoteCommandArg(project_root.lexically_normal().string()) << '\n';
  }
  file << "active-project "
       << (projects_.empty() ? 0 : std::min(active_project_index_, projects_.size() - 1)) << '\n';
}

}  // namespace microide::workspace
