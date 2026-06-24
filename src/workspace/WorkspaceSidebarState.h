#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "editor/DiagnosticsStore.h"
#include "plugin/PluginHost.h"
#include "project/GitStatusService.h"
#include "workspace/CommitWorkflowState.h"

namespace microide::workspace {

struct OutgoingBaseChoice {
  enum class Kind {
    Auto,
    PreviousCommit,
    SpecificRef,
  };

  Kind kind = Kind::Auto;
  std::string custom_ref;
};

enum class SidebarMode {
  None,
  Tree,
  Search,
  Chat,
  Problems,
  Git,
  Tests,
  Plugin,
  // Host-owned document outline. Reuses the plugin item-tree surface (storage,
  // render, key/mouse handlers): its rows are flattened DocumentSymbol nodes from
  // the plugin language providers, and confirming a row navigates the editor.
  Outline,
};

// One clickable mode tab in the sidebar header row. `id` points at a builtin view's static
// label/id storage, so it is allocation-free to copy.
struct SidebarModeTab {
  std::string_view id;
  SidebarMode mode = SidebarMode::None;
  SDL_FRect rect{};
};

// Geometry of the sidebar header mode-switch row: up to three primary tabs (Project / Search /
// Source Control) plus an optional overflow button for plugin-contributed views. Built without
// heap allocation so it can be recomputed cheaply on the render / hit-test / hover paths.
struct SidebarModeRowLayout {
  std::array<SidebarModeTab, 3> tabs{};
  int tab_count = 0;
  bool has_overflow = false;
  SDL_FRect overflow_rect{};
  bool icon_only = false;  // labels don't fit -> render icons only, name shown on hover
  SDL_FRect row_rect{};    // whole header band, for pointer-cursor / hover hit-testing
};

enum class GitSidebarRefreshScope {
  StatusOnly,
  TreeBadges,
  Full,
};

struct GitSidebarEntry {
  enum class Section {
    Conflicts,
    Staged,
    Changed,
    Untracked,
    Outgoing,
  };

  Section section = Section::Changed;
  std::filesystem::path path;
  std::filesystem::path relative_path;
  project::GitFileStatus status = project::GitFileStatus::Clean;
  bool conflicted = false;
  bool staged = false;
  std::string provider_id;
  std::string provider_label;
  bool supports_stage = true;
  bool supports_discard = true;
};

struct GitSidebarLine {
  enum class Kind {
    Header,
    Directory,
    Entry,
    Empty,
  };

  Kind kind = Kind::Empty;
  GitSidebarEntry::Section section = GitSidebarEntry::Section::Changed;
  std::string label;
  std::string tree_node_key;
  bool expanded = false;
  int depth = 0;
  int entry_index = -1;
};

struct GitSidebarEntryActionLayout {
  std::optional<SDL_FRect> primary_rect;
  std::optional<SDL_FRect> discard_rect;
  float content_right_edge = 0.0f;
};

struct ProblemsSidebarEntry {
  editor::PublishedDiagnostic diagnostic;
  std::string primary_label;
  std::string detail_label;

  bool operator==(const ProblemsSidebarEntry&) const = default;
};

struct GitSidebarState {
  struct RefreshSnapshotEntry {
    GitSidebarEntry::Section section = GitSidebarEntry::Section::Changed;
    std::filesystem::path relative_path;
    project::GitFileStatus status = project::GitFileStatus::Clean;
    bool conflicted = false;
    bool staged = false;
  };

  struct RefreshSnapshot {
    std::vector<RefreshSnapshotEntry> entries;
    std::unordered_map<std::string, project::GitFileStatus> tree_git_statuses;
    bool includes_tree_git_statuses = false;
    bool repo_available = false;
    std::string branch_label;
    std::string base_ref;
    std::string base_label;
    std::string upstream_label;
    int ahead = 0;
    int behind = 0;
    bool snapshot_stale = false;
    std::string refresh_error;
    std::uint64_t generation = 0;
  };

  std::vector<GitSidebarEntry> entries;
  std::string branch_label;
  std::string upstream_label;
  int ahead = 0;
  int behind = 0;
  std::string base_ref;
  std::string base_label;
  OutgoingBaseChoice outgoing_base_choice;
  bool repo_available = false;
  bool refreshing = false;
  bool snapshot_stale = false;
  std::string refresh_error;
  bool tree_git_badges_materialized = false;
  bool provider_backed = false;
  bool supports_mutations = true;
  std::string provider_id;
  std::string provider_label;
  std::string error;
  std::size_t selected_index = 0;
  std::uint64_t snapshot_generation = 0;
  std::unordered_set<std::string> collapsed_directory_keys;
  CommitWorkflowState commit_workflow;
};

struct ProblemsSidebarState {
  std::vector<ProblemsSidebarEntry> entries;
  std::size_t selected_index = 0;
};

struct TestsSidebarEntry {
  std::string id;
  std::string label;
  std::filesystem::path file;
  int line = 0;
  std::string parent_id;
  std::string status;
};

struct TestsSidebarState {
  std::vector<TestsSidebarEntry> entries;
  std::size_t selected_index = 0;
  bool running = false;
  std::string provider_id;
  std::string error;
};

struct PluginSidebarState {
  std::vector<plugin::PluginHost::SidebarItem> items;
  std::string error;
  std::size_t selected_index = 0;
  // Prebuilt empty/error text the sidebar render TU draws verbatim, recomputed
  // only when items/error change (never per frame). Empty means "draw nothing".
  // `placeholder_is_error` picks the error vs muted text color.
  std::string placeholder;
  bool placeholder_is_error = false;
};

struct SidebarState {
  bool visible = true;
  std::string view_id = "tree";
  std::string prev_view_id;
  bool temporary = false;
  float width = 288.0f;
  int scroll_row = 0;
  GitSidebarState git;
  ProblemsSidebarState problems;
  TestsSidebarState tests;
  PluginSidebarState plugin;
};

}  // namespace microide::workspace
