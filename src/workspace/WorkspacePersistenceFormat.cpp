#include "workspace/WorkspacePersistenceFormat.h"

#include <algorithm>
#include <sstream>
#include <utility>

#include "util/StringUtil.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/WorkspaceProjectPresentation.h"

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

bool ParseIntToken(std::string_view text, int* value) {
  if (value == nullptr) {
    return false;
  }
  try {
    *value = std::stoi(std::string(text));
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseInt64Token(std::string_view text, std::int64_t* value) {
  if (value == nullptr) {
    return false;
  }
  try {
    *value = std::stoll(std::string(text));
    return true;
  } catch (...) {
    return false;
  }
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
      continue;
    }
    if (command == "setting" && tokens.size() == 3) {
      state->settings.emplace_back(tokens[1].text, tokens[2].text);
      continue;
    }
    if (command == "keybinding-disabled" && tokens.size() == 2) {
      state->disabled_keybinding_ids.push_back(tokens[1].text);
    }
  }

  return version_ok;
}

std::string SerializeUserConfig(const PersistedUserConfigState& state) {
  std::ostringstream stream;
  stream << "version 1\n";
  stream << "ui-scale " << state.ui_scale << '\n';
  for (const auto& [id, value] : state.settings) {
    stream << "setting " << QuoteCommandArg(id) << ' ' << QuoteCommandArg(value) << '\n';
  }
  for (const auto& id : state.disabled_keybinding_ids) {
    stream << "keybinding-disabled " << QuoteCommandArg(id) << '\n';
  }
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
      continue;
    }
    if (command == "setting" && tokens.size() == 3) {
      state->settings.emplace_back(tokens[1].text, tokens[2].text);
      continue;
    }
    if (command == "sidebar-view-policy" && tokens.size() >= 2) {
      PersistedSidebarViewPolicy policy;
      policy.view_id = tokens[1].text;
      for (std::size_t i = 2; i < tokens.size(); ++i) {
        if (tokens[i].text == "hidden") {
          policy.hidden = true;
        } else if (tokens[i].text.starts_with("order=")) {
          try {
            policy.order = std::stoi(tokens[i].text.substr(6));
          } catch (...) {
          }
        }
      }
      state->sidebar_policies.push_back(std::move(policy));
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
  for (const auto& [id, value] : state.settings) {
    stream << "setting " << QuoteCommandArg(id) << ' ' << QuoteCommandArg(value) << '\n';
  }
  for (const auto& policy : state.sidebar_policies) {
    stream << "sidebar-view-policy " << QuoteCommandArg(policy.view_id);
    if (policy.hidden) {
      stream << " hidden";
    }
    if (policy.order != 0) {
      stream << " order=" << policy.order;
    }
    stream << '\n';
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
  state->chat = PersistedChatState{};
  std::optional<PersistedEditorTabState> current_tab;
  std::optional<PersistedConversationState> current_conv;
  std::optional<PersistedMessageState> current_msg;
  std::optional<PersistedMessageState::PersistedToolEventState> current_tool;
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
      version_ok = tokens.size() == 2 &&
                   (tokens[1].text == "1" || tokens[1].text == "2" || tokens[1].text == "3" ||
                    tokens[1].text == "4" || tokens[1].text == "5");
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
    if (version >= 3 && command == "chat-active-conversation" && tokens.size() == 2) {
      state->chat.active_conversation_id = tokens[1].text;
      continue;
    }
    if (version >= 3 && command == "conv-begin" && tokens.size() == 2) {
      if (current_tool.has_value() && current_msg.has_value()) {
        current_msg->tool_events.push_back(std::move(*current_tool));
        current_tool.reset();
      }
      if (current_msg.has_value() && current_conv.has_value()) {
        current_conv->messages.push_back(std::move(*current_msg));
        current_msg.reset();
      }
      if (current_conv.has_value()) {
        state->chat.conversations.push_back(std::move(*current_conv));
      }
      current_conv = PersistedConversationState{};
      current_conv->id = tokens[1].text;
      continue;
    }
    if (version >= 3 && command == "conv-end") {
      if (current_tool.has_value() && current_msg.has_value()) {
        current_msg->tool_events.push_back(std::move(*current_tool));
        current_tool.reset();
      }
      if (current_msg.has_value() && current_conv.has_value()) {
        current_conv->messages.push_back(std::move(*current_msg));
        current_msg.reset();
      }
      if (current_conv.has_value()) {
        state->chat.conversations.push_back(std::move(*current_conv));
        current_conv.reset();
      }
      continue;
    }
    if (version >= 3 && current_conv.has_value()) {
      if (command == "conv-schema" && tokens.size() == 2) {
        ParseIntToken(tokens[1].text, &current_conv->schema_version);
      } else if (command == "conv-title" && tokens.size() == 2) {
        current_conv->title = tokens[1].text;
      } else if (command == "conv-provider" && tokens.size() == 2) {
        current_conv->provider_id = tokens[1].text;
      } else if (command == "conv-model" && tokens.size() == 2) {
        current_conv->model_id = tokens[1].text;
      } else if (command == "conv-status" && tokens.size() == 2) {
        current_conv->status = tokens[1].text;
      } else if (command == "conv-tool-mode" && tokens.size() == 2) {
        current_conv->tool_mode = tokens[1].text;
      } else if (command == "conv-created" && tokens.size() == 2) {
        current_conv->created_at = tokens[1].text;
      } else if (command == "conv-updated" && tokens.size() == 2) {
        current_conv->updated_at = tokens[1].text;
      } else if (command == "conv-draft" && tokens.size() == 2) {
        current_conv->draft = tokens[1].text;
      } else if (command == "conv-system-prompt" && tokens.size() == 2) {
        current_conv->system_prompt = tokens[1].text;
      } else if (command == "conv-last-request-duration-ms" && tokens.size() == 2) {
        ParseInt64Token(tokens[1].text, &current_conv->last_request_duration_ms);
      } else if (command == "msg-begin" && tokens.size() == 2) {
        if (current_tool.has_value() && current_msg.has_value()) {
          current_msg->tool_events.push_back(std::move(*current_tool));
          current_tool.reset();
        }
        if (current_msg.has_value()) {
          current_conv->messages.push_back(std::move(*current_msg));
        }
        current_msg = PersistedMessageState{};
        current_msg->id = tokens[1].text;
      } else if (version >= 5 && command == "tool-begin" && tokens.size() == 2 &&
                 current_msg.has_value()) {
        if (current_tool.has_value()) {
          current_msg->tool_events.push_back(std::move(*current_tool));
        }
        current_tool = PersistedMessageState::PersistedToolEventState{};
        current_tool->call_id = tokens[1].text;
      } else if (version >= 5 && command == "tool-end" && current_msg.has_value()) {
        if (current_tool.has_value()) {
          current_msg->tool_events.push_back(std::move(*current_tool));
          current_tool.reset();
        }
      } else if (version >= 5 && current_tool.has_value()) {
        if (command == "tool-id" && tokens.size() == 2) {
          current_tool->tool_id = tokens[1].text;
        } else if (command == "tool-name" && tokens.size() == 2) {
          current_tool->display_name = tokens[1].text;
        } else if (command == "tool-args" && tokens.size() == 2) {
          current_tool->arguments_summary = tokens[1].text;
        } else if (command == "tool-status" && tokens.size() == 2) {
          current_tool->status = tokens[1].text;
        } else if (command == "tool-permission" && tokens.size() == 2) {
          current_tool->permission_decision = tokens[1].text;
        } else if (command == "tool-scope" && tokens.size() == 2) {
          current_tool->capability_scope = tokens[1].text;
        } else if (command == "tool-started" && tokens.size() == 2) {
          current_tool->started_at = tokens[1].text;
        } else if (command == "tool-finished" && tokens.size() == 2) {
          current_tool->finished_at = tokens[1].text;
        } else if (command == "tool-duration-ms" && tokens.size() == 2) {
          ParseInt64Token(tokens[1].text, &current_tool->duration_ms);
        } else if (command == "tool-error" && tokens.size() == 2) {
          current_tool->error = tokens[1].text;
        } else if (command == "tool-output" && tokens.size() == 2) {
          current_tool->output_summary = tokens[1].text;
        }
      } else if (current_msg.has_value()) {
        if (command == "msg-role" && tokens.size() == 2) {
          current_msg->role = tokens[1].text;
        } else if (command == "msg-status" && tokens.size() == 2) {
          current_msg->status = tokens[1].text;
        } else if (command == "msg-timestamp" && tokens.size() == 2) {
          current_msg->timestamp = tokens[1].text;
        } else if (command == "msg-provider" && tokens.size() == 2) {
          current_msg->provider_id = tokens[1].text;
        } else if (command == "msg-model" && tokens.size() == 2) {
          current_msg->model = tokens[1].text;
        } else if (command == "msg-error" && tokens.size() == 2) {
          current_msg->error = tokens[1].text;
        } else if (command == "msg-request-duration-ms" && tokens.size() == 2) {
          ParseInt64Token(tokens[1].text, &current_msg->request_duration_ms);
        } else if (command == "msg-content" && tokens.size() == 2) {
          current_msg->content = tokens[1].text;
        }
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
      it->line_ending = util::ParseLineEndingLabel(tokens[2].text);
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
  // Flush any open conversation/message at end of stream.
  if (version >= 3) {
    if (current_tool.has_value() && current_msg.has_value()) {
      current_msg->tool_events.push_back(std::move(*current_tool));
    }
    if (current_msg.has_value() && current_conv.has_value()) {
      current_conv->messages.push_back(std::move(*current_msg));
    }
    if (current_conv.has_value()) {
      state->chat.conversations.push_back(std::move(*current_conv));
    }
  }

  return version_ok;
}

std::string SerializeProjectSession(const PersistedProjectSessionState& state) {
  std::ostringstream stream;
  stream << "version 5\n";
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
                 << QuoteCommandArg(util::LineEndingLabel(view.line_ending)) << '\n';
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

  // Chat conversations.
  if (!state.chat.active_conversation_id.empty()) {
    stream << "chat-active-conversation "
           << QuoteCommandArg(state.chat.active_conversation_id) << '\n';
  }
  for (const auto& conv : state.chat.conversations) {
    stream << "conv-begin " << QuoteCommandArg(conv.id) << '\n';
    stream << "conv-schema " << conv.schema_version << '\n';
    if (!conv.title.empty()) {
      stream << "conv-title " << QuoteCommandArg(conv.title) << '\n';
    }
    if (!conv.provider_id.empty()) {
      stream << "conv-provider " << QuoteCommandArg(conv.provider_id) << '\n';
    }
    if (!conv.model_id.empty()) {
      stream << "conv-model " << QuoteCommandArg(conv.model_id) << '\n';
    }
    if (!conv.status.empty()) {
      stream << "conv-status " << QuoteCommandArg(conv.status) << '\n';
    }
    if (!conv.tool_mode.empty()) {
      stream << "conv-tool-mode " << QuoteCommandArg(conv.tool_mode) << '\n';
    }
    if (!conv.created_at.empty()) {
      stream << "conv-created " << QuoteCommandArg(conv.created_at) << '\n';
    }
    if (!conv.updated_at.empty()) {
      stream << "conv-updated " << QuoteCommandArg(conv.updated_at) << '\n';
    }
    if (!conv.draft.empty()) {
      stream << "conv-draft " << QuoteCommandArg(conv.draft) << '\n';
    }
    if (!conv.system_prompt.empty()) {
      stream << "conv-system-prompt " << QuoteCommandArg(conv.system_prompt) << '\n';
    }
    if (conv.last_request_duration_ms != 0) {
      stream << "conv-last-request-duration-ms " << conv.last_request_duration_ms << '\n';
    }
    for (const auto& msg : conv.messages) {
      stream << "msg-begin " << QuoteCommandArg(msg.id) << '\n';
      if (!msg.role.empty()) {
        stream << "msg-role " << QuoteCommandArg(msg.role) << '\n';
      }
      if (!msg.status.empty()) {
        stream << "msg-status " << QuoteCommandArg(msg.status) << '\n';
      }
      if (!msg.timestamp.empty()) {
        stream << "msg-timestamp " << QuoteCommandArg(msg.timestamp) << '\n';
      }
      if (!msg.provider_id.empty()) {
        stream << "msg-provider " << QuoteCommandArg(msg.provider_id) << '\n';
      }
      if (!msg.model.empty()) {
        stream << "msg-model " << QuoteCommandArg(msg.model) << '\n';
      }
      if (!msg.error.empty()) {
        stream << "msg-error " << QuoteCommandArg(msg.error) << '\n';
      }
      if (msg.request_duration_ms != 0) {
        stream << "msg-request-duration-ms " << msg.request_duration_ms << '\n';
      }
      if (!msg.content.empty()) {
        stream << "msg-content " << QuoteCommandArg(msg.content) << '\n';
      }
      for (const auto& tool : msg.tool_events) {
        stream << "tool-begin " << QuoteCommandArg(tool.call_id) << '\n';
        if (!tool.tool_id.empty()) {
          stream << "tool-id " << QuoteCommandArg(tool.tool_id) << '\n';
        }
        if (!tool.display_name.empty()) {
          stream << "tool-name " << QuoteCommandArg(tool.display_name) << '\n';
        }
        if (!tool.arguments_summary.empty()) {
          stream << "tool-args " << QuoteCommandArg(tool.arguments_summary) << '\n';
        }
        if (!tool.status.empty()) {
          stream << "tool-status " << QuoteCommandArg(tool.status) << '\n';
        }
        if (!tool.permission_decision.empty()) {
          stream << "tool-permission " << QuoteCommandArg(tool.permission_decision) << '\n';
        }
        if (!tool.capability_scope.empty()) {
          stream << "tool-scope " << QuoteCommandArg(tool.capability_scope) << '\n';
        }
        if (!tool.started_at.empty()) {
          stream << "tool-started " << QuoteCommandArg(tool.started_at) << '\n';
        }
        if (!tool.finished_at.empty()) {
          stream << "tool-finished " << QuoteCommandArg(tool.finished_at) << '\n';
        }
        if (tool.duration_ms != 0) {
          stream << "tool-duration-ms " << tool.duration_ms << '\n';
        }
        if (!tool.error.empty()) {
          stream << "tool-error " << QuoteCommandArg(tool.error) << '\n';
        }
        if (!tool.output_summary.empty()) {
          stream << "tool-output " << QuoteCommandArg(tool.output_summary) << '\n';
        }
        stream << "tool-end\n";
      }
    }
    stream << "conv-end\n";
  }

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
