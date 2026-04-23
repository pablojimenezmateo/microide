#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/TextViewport.h"

namespace microide::workspace {

struct PersistedEditorViewState {
  std::size_t leaf_id = 0;
  std::filesystem::path path;
  std::size_t cursor_line = 0;
  std::size_t cursor_column = 0;
  std::size_t scroll_line = 0;
  std::size_t horizontal_scroll = 0;
  bool dirty_snapshot = false;
  editor::TextViewport::LineEnding line_ending = editor::TextViewport::LineEnding::LF;
  std::vector<std::string> buffer_lines;
};

struct PersistedSplitNodeState {
  std::vector<std::size_t> path;
  std::string orientation;
  float size_fraction = 1.0f;
  std::size_t leaf_id = 0;
};

struct PersistedEditorTabState {
  std::string kind = "editor";
  std::size_t active_leaf_id = 0;
  std::vector<PersistedEditorViewState> views;
  std::vector<PersistedSplitNodeState> split_nodes;
  std::filesystem::path compare_path;
  std::filesystem::path compare_left_path;
  std::filesystem::path compare_right_path;
  std::string compare_commit_hash;
  std::string compare_commit_short_hash;
  std::string compare_right_ref;
  std::string compare_right_label;
  std::size_t compare_selected_row = 0;
  std::size_t compare_scroll_row = 0;
  std::size_t compare_horizontal_scroll = 0;
  std::filesystem::path merge_base_path;
  std::filesystem::path merge_incoming_path;
  std::filesystem::path merge_current_path;
  std::filesystem::path merge_output_path;
  std::size_t merge_selected_hunk = 0;
  std::size_t merge_scroll_row = 0;
  std::size_t merge_horizontal_scroll = 0;
  float merge_left_divider_fraction = 1.0f / 3.0f;
  float merge_right_divider_fraction = 2.0f / 3.0f;
  std::vector<std::string> merge_hunk_choices;
};

struct PersistedUserConfigState {
  float ui_scale = 1.0f;
  std::vector<std::pair<std::string, std::string>> settings;  // id → serialised value
  std::vector<std::string> disabled_keybinding_ids;
};

struct PersistedSidebarViewPolicy {
  std::string view_id;
  bool hidden = false;
  int order = 0;
};

struct PersistedProjectConfigState {
  std::size_t editor_tab_size = 4;
  std::size_t editor_indent_width = 4;
  bool editor_soft_tabs = false;
  std::string colorscheme_name = "default";
  std::optional<SDL_Color> project_base_color;
  std::vector<std::pair<std::string, std::string>> settings;  // id → serialised value
  std::vector<PersistedSidebarViewPolicy> sidebar_policies;
};

struct PersistedMessageState {
  std::string id;
  std::string role;  // "user", "assistant", "system"
  std::string content;
  std::string timestamp;
  std::string provider_id;
  std::string model;
  std::string status;  // serialized RequestStatus
  std::int64_t request_duration_ms = 0;
  std::string error;
};

struct PersistedConversationState {
  std::string id;
  std::string title;
  std::string provider_id;
  std::string model_id;
  std::string status;  // serialized RequestStatus
  std::string tool_mode;  // "no_tools", "ask", "auto"
  std::string draft;
  std::string system_prompt;
  std::string created_at;
  std::string updated_at;
  std::int64_t last_request_duration_ms = 0;
  std::vector<PersistedMessageState> messages;
};

struct PersistedChatState {
  std::string active_conversation_id;
  std::vector<PersistedConversationState> conversations;
};

struct PersistedProjectSessionState {
  bool sidebar_visible = true;
  float sidebar_width = 288.0f;
  float bottom_panel_height = 184.0f;
  std::size_t active_tab_index = 0;
  std::vector<PersistedEditorTabState> tabs;
  PersistedChatState chat;
};

struct PersistedWorkspaceSessionState {
  std::vector<std::filesystem::path> project_roots;
  std::size_t active_project_index = 0;
};

std::string EncodeSessionNodePath(const std::vector<std::size_t>& path);
std::optional<std::vector<std::size_t>> DecodeSessionNodePath(std::string_view text);

bool ParseUserConfigText(std::string_view text, PersistedUserConfigState* state);
std::string SerializeUserConfig(const PersistedUserConfigState& state);
bool ParseProjectConfigText(std::string_view text, PersistedProjectConfigState* state);
std::string SerializeProjectConfig(const PersistedProjectConfigState& state);
bool ParseProjectSessionText(std::string_view text, PersistedProjectSessionState* state);
std::string SerializeProjectSession(const PersistedProjectSessionState& state);
bool ParseWorkspaceSessionText(std::string_view text, PersistedWorkspaceSessionState* state);
std::string SerializeWorkspaceSession(const PersistedWorkspaceSessionState& state);

}  // namespace microide::workspace
