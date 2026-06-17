#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "editor/TextViewport.h"
#include "workspace/BranchReviewPersistence.h"
#include "workspace/CommitWorkflowPersistence.h"
#include "workspace/WorkspaceSidebarState.h"

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
  float compare_divider_fraction = 0.5f;
  std::string compare_review_mode;
  std::string compare_staging_view;
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
  std::vector<std::string> disabled_plugin_ids;
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
  std::optional<PersistedCommitDraftState> commit_draft;
  PersistedBranchReviewState branch_review;
};

struct PersistedMessageState {
  struct PersistedToolEventState {
    std::string call_id;
    std::string tool_id;
    std::string display_name;
    std::string arguments_summary;
    std::string status;
    std::string permission_decision;
    std::string capability_scope;
    std::string started_at;
    std::string finished_at;
    std::int64_t duration_ms = 0;
    std::string error;
    std::string output_summary;
  };

  std::string id;
  std::string role;  // "user", "assistant", "system"
  std::string content;
  std::string timestamp;
  std::string provider_id;
  std::string model;
  std::string status;  // serialized RequestStatus
  std::int64_t request_duration_ms = 0;
  std::string error;
  std::vector<PersistedToolEventState> tool_events;
};

struct PersistedConversationState {
  int schema_version = 1;
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
  float bottom_panel_height = 156.0f;
  OutgoingBaseChoice outgoing_base_choice;
  std::size_t active_tab_index = 0;
  std::vector<PersistedEditorTabState> tabs;
  // Right-side debug pane (visibility / width / active surface). Only restored
  // when `debug.enabled` is on. `right_pane_mode` is a DebugPaneMode cast to u8.
  bool right_pane_visible = false;
  float right_pane_width = 288.0f;
  std::uint8_t right_pane_mode = 0;
};

struct PersistedWorkspaceSessionState {
  std::vector<std::filesystem::path> project_roots;
  std::size_t active_project_index = 0;
};

// Per-project debug state: breakpoints (keyed by file) plus launch configs.
// `arguments_json` holds the launch config's verbatim `arguments` serialized as
// JSON text so this format carries no util::JsonValue dependency; the coordinator
// (de)serializes it. Transient adapter verification is never persisted.
struct PersistedBreakpoint {
  std::size_t line = 0;  // 0-based buffer line index
  bool enabled = true;
  std::optional<std::string> condition;
  std::optional<std::string> hit_condition;
  std::optional<std::string> log_message;
};

struct PersistedFileBreakpoints {
  std::filesystem::path path;
  std::vector<PersistedBreakpoint> breakpoints;
};

struct PersistedLaunchConfig {
  std::string name;
  std::string type;
  std::string request = "launch";
  std::string arguments_json;  // serialized launch/attach `arguments`, or empty
};

struct PersistedDebugState {
  std::vector<PersistedFileBreakpoints> files;
  std::vector<PersistedLaunchConfig> launch_configs;
  std::size_t selected_launch_config_index = 0;
  std::vector<std::string> watch_expressions;  // Phase 6 (additive; empty on old records)
  // Phase 7 (additive; empty/false on old records): the user's enabled
  // exception-breakpoint filter ids, and whether adapter defaults have been
  // seeded once (so "all filters off" persists rather than re-seeding).
  std::vector<std::string> enabled_exception_filters;
  bool exception_filters_seeded = false;
};

bool EncodeUserConfigRecord(const PersistedUserConfigState& state,
                            std::vector<std::byte>* out);
bool DecodeUserConfigRecord(std::span<const std::byte> input,
                            PersistedUserConfigState* state);
bool EncodeProjectConfigRecord(const PersistedProjectConfigState& state,
                               std::vector<std::byte>* out);
bool DecodeProjectConfigRecord(std::span<const std::byte> input,
                               PersistedProjectConfigState* state);
bool EncodeProjectSessionRecord(const PersistedProjectSessionState& state,
                                std::vector<std::byte>* out);
bool DecodeProjectSessionRecord(std::span<const std::byte> input,
                                PersistedProjectSessionState* state);
bool EncodeWorkspaceSessionRecord(const PersistedWorkspaceSessionState& state,
                                  std::vector<std::byte>* out);
bool DecodeWorkspaceSessionRecord(std::span<const std::byte> input,
                                  PersistedWorkspaceSessionState* state);
bool EncodeDebugStateRecord(const PersistedDebugState& state, std::vector<std::byte>* out);
bool DecodeDebugStateRecord(std::span<const std::byte> input, PersistedDebugState* state);

}  // namespace microide::workspace
