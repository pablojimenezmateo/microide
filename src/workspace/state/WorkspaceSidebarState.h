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
#include "workspace/TypeNavigation.h"
#include "plugin/PluginHost.h"
#include "project/GitStatusService.h"
#include "workspace/git/CommitWorkflowState.h"
#include "workspace/WorkspaceLayout.h"

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
  Problems,
  Git,
  Tests,
  Plugin,
  Outline,
  HostChanges,
  Host,
  Conflicts,
};

constexpr bool UsesPluginItemSurface(SidebarMode mode) {
  return mode == SidebarMode::Plugin || mode == SidebarMode::Outline ||
         mode == SidebarMode::HostChanges || mode == SidebarMode::Host ||
         mode == SidebarMode::Conflicts;
}

constexpr bool IsHostOwnedItemView(SidebarMode mode) {
  return mode == SidebarMode::HostChanges || mode == SidebarMode::Host ||
         mode == SidebarMode::Conflicts;
}

struct SidebarModeTab {
  std::string_view id;
  SidebarMode mode = SidebarMode::None;
  SDL_FRect rect{};
};

struct SidebarModeRowLayout {
  std::array<SidebarModeTab, 3> tabs{};
  int tab_count = 0;
  bool has_overflow = false;
  SDL_FRect overflow_rect{};
  bool icon_only = false;
  SDL_FRect row_rect{};
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
  std::string path;
  std::string relative_path;
  project::GitFileStatus status = project::GitFileStatus::Clean;
  bool conflicted = false;
  bool staged = false;
  bool is_staged_rename = false;
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
  std::string display_label;
  std::string tree_node_key;
  bool expanded = false;
  int depth = 0;
  int entry_index = -1;
};

struct ProblemsSidebarEntry {
  editor::PublishedDiagnostic diagnostic;
  std::string primary_label;
  std::string detail_label;

  bool operator==(const ProblemsSidebarEntry&) const = default;
};

struct GitSidebarState {
  TypeNavigationState type_navigation;
  struct RefreshSnapshotEntry {
    GitSidebarEntry::Section section = GitSidebarEntry::Section::Changed;
    std::string relative_path;
    project::GitFileStatus status = project::GitFileStatus::Clean;
    bool conflicted = false;
    bool staged = false;
    bool is_staged_rename = false;
  };

  struct RefreshSnapshot {
    std::vector<RefreshSnapshotEntry> entries;
    project::SharedGitTreeStatusMap tree_git_statuses;
    bool includes_tree_git_statuses = false;
    bool repo_available = false;
    std::string branch_label;
    std::string base_ref;
    std::string base_label;
    std::string upstream_label;
    int ahead = 0;
    int behind = 0;
    std::size_t worktree_count = 0;
    bool snapshot_stale = false;
    std::string refresh_error;
    std::uint64_t generation = 0;
  };

  std::vector<GitSidebarEntry> entries;
  std::string branch_label;
  std::string upstream_label;
  int ahead = 0;
  int behind = 0;
  std::size_t worktree_count = 0;
  std::string base_ref;
  std::string base_label;
  OutgoingBaseChoice outgoing_base_choice;
  bool repo_available = false;
  bool refreshing = false;
  bool snapshot_stale = false;
  std::string refresh_error;
  bool tree_git_badges_materialized = false;
  bool supports_mutations = true;
  std::string error;
  std::size_t selected_index = 0;
  std::uint64_t snapshot_generation = 0;
  std::uint64_t repository_marker_generation = 0;
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
  std::string detail_label;
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
  std::string placeholder;
  bool placeholder_is_error = false;
};

struct SidebarState;
bool HostViewShown(const SidebarState& sidebar);

struct SidebarState {
  bool visible = true;
  std::string view_id = "tree";
  std::string prev_view_id;
  bool temporary = false;
  float width = kWorkspaceDefaultSidebarWidth;
  int scroll_row = 0;
  GitSidebarState git;
  ProblemsSidebarState problems;
  TestsSidebarState tests;
  PluginSidebarState plugin;
  TypeNavigationState tree_type_navigation;
  struct TreeClipboard {
    std::filesystem::path path;
    bool cut = false;
  };
  std::optional<TreeClipboard> tree_clipboard;
};

inline bool HostViewShown(const SidebarState& sidebar) {
  return sidebar.visible && sidebar.view_id == "host";
}

}
