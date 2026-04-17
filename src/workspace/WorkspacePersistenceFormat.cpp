#include "workspace/WorkspacePersistenceFormat.h"

#include <algorithm>
#include <sstream>
#include <utility>

#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

bool ParseSizeToken(std::string_view text, std::size_t* value) {
  if (value == nullptr) {
    return false;
  }
  try {
    *value = static_cast<std::size_t>(std::stoull(std::string(text)));
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseFloatToken(std::string_view text, float* value) {
  if (value == nullptr) {
    return false;
  }
  try {
    *value = std::stof(std::string(text));
    return true;
  } catch (...) {
    return false;
  }
}

std::string LineEndingSessionLabel(editor::TextViewport::LineEnding line_ending) {
  switch (line_ending) {
    case editor::TextViewport::LineEnding::CRLF:
      return "crlf";
    case editor::TextViewport::LineEnding::CR:
      return "cr";
    case editor::TextViewport::LineEnding::LF:
    default:
      return "lf";
  }
}

editor::TextViewport::LineEnding ParseLineEndingSessionLabel(std::string_view text) {
  if (text == "crlf") {
    return editor::TextViewport::LineEnding::CRLF;
  }
  if (text == "cr") {
    return editor::TextViewport::LineEnding::CR;
  }
  return editor::TextViewport::LineEnding::LF;
}

}  // namespace

bool ParseUserConfigText(std::string_view text, PersistedUserConfigState* state) {
  if (state == nullptr) {
    return false;
  }

  bool version_ok = false;
  std::istringstream stream{std::string(text)};
  std::string line;
  while (std::getline(stream, line)) {
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
        state->ui_scale = *scale;
      }
    }
  }

  return version_ok;
}

std::string SerializeUserConfig(const PersistedUserConfigState& state) {
  std::ostringstream stream;
  stream << "version 1\n";
  stream << "ui-scale " << state.ui_scale << '\n';
  return stream.str();
}

bool ParseProjectConfigText(std::string_view text, PersistedProjectConfigState* state) {
  if (state == nullptr) {
    return false;
  }

  bool version_ok = false;
  std::istringstream stream{std::string(text)};
  std::string line;
  while (std::getline(stream, line)) {
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
      std::size_t value = state->editor_tab_size;
      if (ParseSizeToken(tokens[1].text, &value)) {
        state->editor_tab_size = std::clamp<std::size_t>(value, 1, 16);
      }
      continue;
    }
    if (command == "editor-indent-width" && tokens.size() == 2) {
      std::size_t value = state->editor_indent_width;
      if (ParseSizeToken(tokens[1].text, &value)) {
        state->editor_indent_width = std::clamp<std::size_t>(value, 1, 16);
      }
      continue;
    }
    if (command == "editor-soft-tabs" && tokens.size() == 2) {
      state->editor_soft_tabs =
          tokens[1].text == "1" || tokens[1].text == "on" || tokens[1].text == "true";
      continue;
    }
    if (command == "colorscheme" && tokens.size() == 2) {
      state->colorscheme_name = tokens[1].text;
      continue;
    }
    if (command == "project-base-color" && tokens.size() == 2) {
      state->project_base_color = ParseProjectColor(tokens[1].text);
    }
  }

  return version_ok;
}

std::string SerializeProjectConfig(const PersistedProjectConfigState& state) {
  std::ostringstream stream;
  stream << "version 1\n";
  stream << "editor-tab-size " << state.editor_tab_size << '\n';
  stream << "editor-indent-width " << state.editor_indent_width << '\n';
  stream << "editor-soft-tabs " << (state.editor_soft_tabs ? 1 : 0) << '\n';
  stream << "colorscheme " << QuoteCommandArg(state.colorscheme_name) << '\n';
  if (state.project_base_color.has_value()) {
    stream << "project-base-color "
           << QuoteCommandArg(FormatProjectColor(*state.project_base_color)) << '\n';
  }
  return stream.str();
}

bool ParseProjectSessionText(std::string_view text, PersistedProjectSessionState* state) {
  if (state == nullptr) {
    return false;
  }

  bool version_ok = false;
  int version = 0;
  state->tabs.clear();
  std::optional<PersistedEditorTabState> current_tab;
  std::istringstream stream{std::string(text)};
  std::string line;
  while (std::getline(stream, line)) {
    const ParsedCommandLine parsed = ParseCommandLine(line);
    if (parsed.tokens.empty()) {
      continue;
    }

    const std::vector<ParsedCommandToken>& tokens = parsed.tokens;
    const std::string& command = tokens.front().text;
    if (command == "version") {
      version_ok = tokens.size() == 2 && (tokens[1].text == "1" || tokens[1].text == "2");
      version = version_ok ? std::stoi(tokens[1].text) : 0;
      continue;
    }
    if (!version_ok) {
      return false;
    }
    if (command == "sidebar-visible" && tokens.size() == 2) {
      state->sidebar_visible = tokens[1].text == "1";
      continue;
    }
    if (command == "sidebar-width" && tokens.size() == 2) {
      ParseFloatToken(tokens[1].text, &state->sidebar_width);
      continue;
    }
    if (command == "bottom-panel-height" && tokens.size() == 2) {
      ParseFloatToken(tokens[1].text, &state->bottom_panel_height);
      continue;
    }
    if (command == "active-tab" && tokens.size() == 2) {
      ParseSizeToken(tokens[1].text, &state->active_tab_index);
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
      state->tabs.push_back(*current_tab);
      current_tab.reset();
      continue;
    }
    if (command == "active-leaf" && tokens.size() == 2) {
      ParseSizeToken(tokens[1].text, &current_tab->active_leaf_id);
      continue;
    }
    if (command == "kind" && tokens.size() == 2) {
      current_tab->kind = tokens[1].text;
      continue;
    }
    if (command == "view" && tokens.size() == 7) {
      PersistedEditorViewState view_state;
      if (ParseSizeToken(tokens[1].text, &view_state.leaf_id) &&
          ParseSizeToken(tokens[3].text, &view_state.cursor_line) &&
          ParseSizeToken(tokens[4].text, &view_state.cursor_column) &&
          ParseSizeToken(tokens[5].text, &view_state.scroll_line) &&
          ParseSizeToken(tokens[6].text, &view_state.horizontal_scroll)) {
        view_state.path = std::filesystem::path(tokens[2].text);
        current_tab->views.push_back(std::move(view_state));
      }
      continue;
    }
    if (version >= 2 && command == "view-dirty" && tokens.size() == 3) {
      std::size_t leaf_id = 0;
      if (!ParseSizeToken(tokens[1].text, &leaf_id)) {
        continue;
      }
      auto it = std::find_if(current_tab->views.begin(), current_tab->views.end(),
                             [leaf_id](const auto& view) { return view.leaf_id == leaf_id; });
      if (it == current_tab->views.end()) {
        continue;
      }
      it->dirty_snapshot = true;
      it->line_ending = ParseLineEndingSessionLabel(tokens[2].text);
      continue;
    }
    if (version >= 2 && command == "view-buffer-line" && tokens.size() == 3) {
      std::size_t leaf_id = 0;
      if (!ParseSizeToken(tokens[1].text, &leaf_id)) {
        continue;
      }
      auto it = std::find_if(current_tab->views.begin(), current_tab->views.end(),
                             [leaf_id](const auto& view) { return view.leaf_id == leaf_id; });
      if (it == current_tab->views.end()) {
        continue;
      }
      it->dirty_snapshot = true;
      it->buffer_lines.push_back(tokens[2].text);
      continue;
    }
    if (command == "compare-path" && tokens.size() == 2) {
      current_tab->compare_path = std::filesystem::path(tokens[1].text);
      continue;
    }
    if (command == "compare-left-path" && tokens.size() == 2) {
      current_tab->compare_left_path = std::filesystem::path(tokens[1].text);
      continue;
    }
    if (command == "compare-right-path" && tokens.size() == 2) {
      current_tab->compare_right_path = std::filesystem::path(tokens[1].text);
      continue;
    }
    if (command == "compare-commit" && tokens.size() == 3) {
      current_tab->compare_commit_hash = tokens[1].text;
      current_tab->compare_commit_short_hash = tokens[2].text;
      continue;
    }
    if (command == "compare-right-ref" && tokens.size() == 2) {
      current_tab->compare_right_ref = tokens[1].text;
      continue;
    }
    if (command == "compare-right-label" && tokens.size() == 2) {
      current_tab->compare_right_label = tokens[1].text;
      continue;
    }
    if (command == "compare-selected-row" && tokens.size() == 2) {
      ParseSizeToken(tokens[1].text, &current_tab->compare_selected_row);
      continue;
    }
    if (command == "compare-scroll-row" && tokens.size() == 2) {
      ParseSizeToken(tokens[1].text, &current_tab->compare_scroll_row);
      continue;
    }
    if (command == "compare-horizontal-scroll" && tokens.size() == 2) {
      ParseSizeToken(tokens[1].text, &current_tab->compare_horizontal_scroll);
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
      ParseSizeToken(tokens[1].text, &current_tab->merge_selected_hunk);
      continue;
    }
    if (command == "merge-scroll-row" && tokens.size() == 2) {
      ParseSizeToken(tokens[1].text, &current_tab->merge_scroll_row);
      continue;
    }
    if (command == "merge-horizontal-scroll" && tokens.size() == 2) {
      ParseSizeToken(tokens[1].text, &current_tab->merge_horizontal_scroll);
      continue;
    }
    if (command == "merge-left-divider" && tokens.size() == 2) {
      ParseFloatToken(tokens[1].text, &current_tab->merge_left_divider_fraction);
      continue;
    }
    if (command == "merge-right-divider" && tokens.size() == 2) {
      ParseFloatToken(tokens[1].text, &current_tab->merge_right_divider_fraction);
      continue;
    }
    if (command == "merge-choice" && tokens.size() == 3) {
      std::size_t hunk_index = 0;
      if (!ParseSizeToken(tokens[1].text, &hunk_index)) {
        continue;
      }
      if (current_tab->merge_hunk_choices.size() <= hunk_index) {
        current_tab->merge_hunk_choices.resize(hunk_index + 1);
      }
      current_tab->merge_hunk_choices[hunk_index] = tokens[2].text;
      continue;
    }
    if (command == "split-node" && tokens.size() == 5) {
      const auto path = DecodeSessionNodePath(tokens[1].text);
      if (!path.has_value()) {
        continue;
      }
      PersistedSplitNodeState node_state;
      if (ParseFloatToken(tokens[3].text, &node_state.size_fraction) &&
          ParseSizeToken(tokens[4].text, &node_state.leaf_id)) {
        node_state.path = *path;
        node_state.orientation = tokens[2].text;
        current_tab->split_nodes.push_back(std::move(node_state));
      }
    }
  }

  return version_ok;
}

std::string SerializeProjectSession(const PersistedProjectSessionState& state) {
  std::ostringstream stream;
  stream << "version 2\n";
  stream << "sidebar-visible " << (state.sidebar_visible ? 1 : 0) << '\n';
  stream << "sidebar-width " << state.sidebar_width << '\n';
  stream << "bottom-panel-height " << state.bottom_panel_height << '\n';

  for (const auto& tab : state.tabs) {
    stream << "tab-begin\n";
    stream << "kind " << QuoteCommandArg(tab.kind) << '\n';
    if (tab.kind == "compare") {
      stream << "compare-path " << QuoteCommandArg(tab.compare_path.lexically_normal().string())
             << '\n';
      stream << "compare-left-path "
             << QuoteCommandArg(tab.compare_left_path.lexically_normal().string()) << '\n';
      stream << "compare-right-path "
             << QuoteCommandArg(tab.compare_right_path.lexically_normal().string()) << '\n';
      stream << "compare-commit " << QuoteCommandArg(tab.compare_commit_hash) << ' '
             << QuoteCommandArg(tab.compare_commit_short_hash) << '\n';
      stream << "compare-right-ref " << QuoteCommandArg(tab.compare_right_ref) << '\n';
      stream << "compare-right-label " << QuoteCommandArg(tab.compare_right_label) << '\n';
      stream << "compare-selected-row " << tab.compare_selected_row << '\n';
      stream << "compare-scroll-row " << tab.compare_scroll_row << '\n';
      stream << "compare-horizontal-scroll " << tab.compare_horizontal_scroll << '\n';
    } else if (tab.kind == "merge") {
      stream << "merge-base " << QuoteCommandArg(tab.merge_base_path.lexically_normal().string())
             << '\n';
      stream << "merge-incoming "
             << QuoteCommandArg(tab.merge_incoming_path.lexically_normal().string()) << '\n';
      stream << "merge-current "
             << QuoteCommandArg(tab.merge_current_path.lexically_normal().string()) << '\n';
      stream << "merge-output "
             << QuoteCommandArg(tab.merge_output_path.lexically_normal().string()) << '\n';
      stream << "merge-selected-hunk " << tab.merge_selected_hunk << '\n';
      stream << "merge-scroll-row " << tab.merge_scroll_row << '\n';
      stream << "merge-horizontal-scroll " << tab.merge_horizontal_scroll << '\n';
      stream << "merge-left-divider " << tab.merge_left_divider_fraction << '\n';
      stream << "merge-right-divider " << tab.merge_right_divider_fraction << '\n';
      for (std::size_t i = 0; i < tab.merge_hunk_choices.size(); ++i) {
        stream << "merge-choice " << i << ' ' << QuoteCommandArg(tab.merge_hunk_choices[i]) << '\n';
      }
    } else {
      stream << "active-leaf " << tab.active_leaf_id << '\n';
      for (const auto& view : tab.views) {
        stream << "view " << view.leaf_id << ' '
               << QuoteCommandArg(view.path.lexically_normal().string()) << ' ' << view.cursor_line
               << ' ' << view.cursor_column << ' ' << view.scroll_line << ' '
               << view.horizontal_scroll << '\n';
        if (view.dirty_snapshot) {
          stream << "view-dirty " << view.leaf_id << ' '
                 << QuoteCommandArg(LineEndingSessionLabel(view.line_ending)) << '\n';
          for (const auto& line : view.buffer_lines) {
            stream << "view-buffer-line " << view.leaf_id << ' ' << QuoteCommandArg(line) << '\n';
          }
        }
      }
      for (const auto& node : tab.split_nodes) {
        stream << "split-node " << EncodeSessionNodePath(node.path) << ' '
               << QuoteCommandArg(node.orientation) << ' ' << node.size_fraction << ' '
               << node.leaf_id << '\n';
      }
    }
    stream << "tab-end\n";
  }

  stream << "active-tab " << state.active_tab_index << '\n';
  return stream.str();
}

bool ParseWorkspaceSessionText(std::string_view text, PersistedWorkspaceSessionState* state) {
  if (state == nullptr) {
    return false;
  }

  bool version_ok = false;
  state->project_roots.clear();
  std::istringstream stream{std::string(text)};
  std::string line;
  while (std::getline(stream, line)) {
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
      state->project_roots.push_back(std::filesystem::path(tokens[1].text));
      continue;
    }
    if (command == "active-project" && tokens.size() == 2) {
      ParseSizeToken(tokens[1].text, &state->active_project_index);
    }
  }

  return version_ok;
}

std::string SerializeWorkspaceSession(const PersistedWorkspaceSessionState& state) {
  std::ostringstream stream;
  stream << "version 1\n";
  for (const auto& project_root : state.project_roots) {
    stream << "project " << QuoteCommandArg(project_root.lexically_normal().string()) << '\n';
  }
  stream << "active-project " << state.active_project_index << '\n';
  return stream.str();
}

std::string EncodeSessionNodePath(const std::vector<std::size_t>& path) {
  if (path.empty()) {
    return ".";
  }

  std::string encoded;
  for (std::size_t i = 0; i < path.size(); ++i) {
    if (!encoded.empty()) {
      encoded.push_back('/');
    }
    encoded += std::to_string(path[i]);
  }
  return encoded;
}

std::optional<std::vector<std::size_t>> DecodeSessionNodePath(std::string_view text) {
  if (text == ".") {
    return std::vector<std::size_t>{};
  }
  if (text.empty()) {
    return std::nullopt;
  }

  std::vector<std::size_t> path;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t slash = text.find('/', start);
    const std::string_view part = text.substr(start, slash == std::string_view::npos
                                                         ? std::string_view::npos
                                                         : slash - start);
    if (part.empty()) {
      return std::nullopt;
    }
    try {
      path.push_back(static_cast<std::size_t>(std::stoull(std::string(part))));
    } catch (...) {
      return std::nullopt;
    }
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  return path;
}

}  // namespace microide::workspace
