#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "editor/TextViewport.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/BranchReviewPersistence.h"
#include "workspace/CommitWorkflowPersistence.h"
#include "workspace/WorkspaceSidebarState.h"

namespace microide::workspace {

struct PersistedEditorTabState {
  std::string kind = "editor";
  // Single editor view (the in-tab split model was removed: each tab owns exactly
  // one viewport). These fields are empty/default for compare and merge tabs.
  std::filesystem::path path;
  std::size_t cursor_line = 0;
  std::size_t cursor_column = 0;
  std::size_t scroll_line = 0;
  std::size_t horizontal_scroll = 0;
  bool dirty_snapshot = false;
  editor::TextViewport::LineEnding line_ending = editor::TextViewport::LineEnding::LF;
  std::vector<std::string> buffer_lines;
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
  float compare_divider_fraction = kWorkspaceDefaultCompareDividerFraction;
  std::string compare_review_mode;
  std::string compare_staging_view;
  std::filesystem::path merge_base_path;
  std::filesystem::path merge_incoming_path;
  std::filesystem::path merge_current_path;
  std::filesystem::path merge_output_path;
  std::size_t merge_selected_hunk = 0;
  std::size_t merge_scroll_row = 0;
  std::size_t merge_horizontal_scroll = 0;
  float merge_left_divider_fraction = kWorkspaceDefaultMergeLeftDividerFraction;
  float merge_right_divider_fraction = kWorkspaceDefaultMergeRightDividerFraction;
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
  std::string colorscheme_name = "default";
  std::optional<SDL_Color> project_base_color;
  std::vector<std::pair<std::string, std::string>> settings;  // id → serialised value
  std::vector<PersistedSidebarViewPolicy> sidebar_policies;
  std::optional<PersistedCommitDraftState> commit_draft;
  PersistedBranchReviewState branch_review;
};

// One editor group: its own tab list and the index of its active tab. The editor
// area holds 1 or 2 of these (VS Code-style splits live above tabs).
struct PersistedEditorGroupState {
  std::vector<PersistedEditorTabState> tabs;
  std::size_t active_tab_index = 0;
};

struct PersistedProjectSessionState {
  bool sidebar_visible = true;
  float sidebar_width = kWorkspaceDefaultSidebarWidth;
  float bottom_panel_height = kWorkspaceDefaultBottomPanelHeight;
  OutgoingBaseChoice outgoing_base_choice;
  // Editor groups (1 or 2; decode caps at 2). `group_split_orientation` is an
  // EditorSplitOrientation cast to u8; `group_split_fraction` is the first group's
  // share of the editor area.
  std::vector<PersistedEditorGroupState> groups;
  std::size_t focused_group_index = 0;
  std::uint8_t group_split_orientation = 0;
  float group_split_fraction = 0.5f;
  // Right-side debug pane (visibility / width / active surface). Only restored
  // when `debug.enabled` is on. `right_pane_mode` is a DebugPaneMode cast to u8.
  bool right_pane_visible = false;
  float right_pane_width = 288.0f;
  std::uint8_t right_pane_mode = 0;
  // File-tree / sidebar restore (additive; empty/default on old records). Tree
  // paths are relative to the project root. `sidebar_view_id` is the active
  // sidebar view (e.g. "tree", "search", "git"); empty means "leave default".
  std::vector<std::string> expanded_tree_paths;
  std::vector<std::string> collapsed_tree_paths;
  std::string selected_tree_path;  // relative to root; empty if none
  int sidebar_scroll_row = 0;
  std::string sidebar_view_id;
};

struct PersistedWorkspaceSessionState {
  std::vector<std::filesystem::path> project_roots;
  std::size_t active_project_index = 0;
};

// Most-recently-used projects and files, surfaced on the welcome surface and in
// the file finder. Newest-first; bounded by the RecentsService on insert. Each
// recent file carries the project root it was opened under so the finder can show
// only the active project's recents.
struct PersistedRecentFile {
  std::filesystem::path path;
  std::filesystem::path project_root;
};

struct PersistedMruState {
  std::vector<std::filesystem::path> recent_project_roots;
  std::vector<PersistedRecentFile> recent_files;
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

// Function (symbol) breakpoint (additive; empty on old records). No file/line —
// the adapter resolves `name`.
struct PersistedFunctionBreakpoint {
  std::string name;
  bool enabled = true;
  std::optional<std::string> condition;
  std::optional<std::string> hit_condition;
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
  // Function (symbol) breakpoints (additive; empty on old records).
  std::vector<PersistedFunctionBreakpoint> function_breakpoints;
  // Per-filter exception conditions (additive; empty on old records). Ordered map
  // for deterministic encoding. filterId -> condition expression.
  std::map<std::string, std::string> exception_filter_conditions;
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
bool EncodeMruRecord(const PersistedMruState& state, std::vector<std::byte>* out);
bool DecodeMruRecord(std::span<const std::byte> input, PersistedMruState* state);

}  // namespace microide::workspace
