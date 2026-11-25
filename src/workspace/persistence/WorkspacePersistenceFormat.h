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

#include "editor/DocumentSnapshot.h"

#include "editor/TextViewport.h"
#include "project/ProjectId.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/git/BranchReviewPersistence.h"
#include "workspace/git/CommitWorkflowPersistence.h"
#include "workspace/state/TerminalLayoutRecord.h"
#include "workspace/state/WorkspaceSidebarState.h"

namespace microide::workspace {

inline constexpr std::size_t kMaxPersistedClosedEditors = 64;
inline constexpr std::size_t kMaxPersistedCollapsedFolds = 100'000;
inline constexpr std::size_t kMaxPersistedMergeConflictSpans = 200'000;

struct PersistedMergeConflictSpan {
  std::size_t hunk_index = 0;
  std::size_t start_line = 0;
  std::size_t end_line = 0;
  std::string last_choice;
  bool valid = true;
  bool resolved = false;
};

struct PersistedEditorTabState {
  std::string kind = "editor";
  std::filesystem::path path;
  std::size_t cursor_line = 0;
  std::size_t cursor_column = 0;
  std::size_t scroll_line = 0;
  std::size_t horizontal_scroll = 0;
  std::vector<std::size_t> collapsed_fold_openers;
  bool dirty_snapshot = false;
  editor::TextViewport::LineEnding line_ending = editor::TextViewport::LineEnding::LF;
  bool utf8_bom = false;
  std::uint32_t untitled_number = 0;
  struct DiskBaseline {
    bool exists = false;
    std::uint64_t mtime_ticks = 0;
    std::uint64_t size = 0;
    std::optional<std::uint64_t> content_hash;
    std::uint64_t hash_probe = 0;
    friend bool operator==(const DiskBaseline&, const DiskBaseline&) = default;
  };
  std::optional<DiskBaseline> disk_baseline;
  editor::DocumentSnapshot buffer;
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
  std::vector<PersistedMergeConflictSpan> merge_conflict_spans;
};

struct PersistedUserConfigState {
  float ui_scale = 1.0f;
  std::vector<std::pair<std::string, std::string>> settings;
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
  std::vector<std::pair<std::string, std::string>> settings;
  std::vector<PersistedSidebarViewPolicy> sidebar_policies;
  std::optional<PersistedCommitDraftState> commit_draft;
  PersistedBranchReviewState branch_review;
};

struct PersistedEditorGroupState {
  std::vector<PersistedEditorTabState> tabs;
  std::size_t active_tab_index = 0;
};

struct PersistedProjectSessionState {
  bool sidebar_visible = true;
  float sidebar_width = kWorkspaceDefaultSidebarWidth;
  float bottom_panel_height = kWorkspaceDefaultBottomPanelHeight;
  OutgoingBaseChoice outgoing_base_choice;
  std::vector<PersistedEditorGroupState> groups;
  std::size_t focused_group_index = 0;
  EditorSplitTreeRecord split_tree;
  bool right_pane_visible = false;
  float right_pane_width = 288.0f;
  std::uint8_t right_pane_mode = 0;
  std::vector<std::string> expanded_tree_paths;
  std::vector<std::string> collapsed_tree_paths;
  std::string selected_tree_path;
  int sidebar_scroll_row = 0;
  std::string sidebar_view_id;
  bool panel_maximized = false;
  TerminalLayoutRecord terminal_layout;
  std::vector<PersistedEditorTabState> closed_editors;
};

struct PersistedWorkspaceSessionState {
  std::vector<project::ProjectId> projects;
  std::vector<std::filesystem::path> project_roots;
  std::size_t active_project_index = 0;
  std::vector<std::string> host_view_collapsed;
};

struct PersistedRecentFile {
  std::filesystem::path path;
  std::filesystem::path project_root;
};

struct PersistedMruState {
  std::vector<project::ProjectId> recent_projects;
  std::vector<std::filesystem::path> recent_project_roots;
  std::vector<PersistedRecentFile> recent_files;
};

struct PersistedBreakpoint {
  std::size_t line = 0;
  bool enabled = true;
  std::optional<std::string> condition;
  std::optional<std::string> hit_condition;
  std::optional<std::string> log_message;
};

struct PersistedFileBreakpoints {
  std::filesystem::path path;
  std::vector<PersistedBreakpoint> breakpoints;
};

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
  std::string arguments_json;
};

struct PersistedDebugState {
  std::vector<PersistedFileBreakpoints> files;
  std::vector<PersistedLaunchConfig> launch_configs;
  std::size_t selected_launch_config_index = 0;
  std::vector<std::string> watch_expressions;
  std::vector<std::string> enabled_exception_filters;
  bool exception_filters_seeded = false;
  std::vector<PersistedFunctionBreakpoint> function_breakpoints;
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

}
