#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "editor/DiagnosticsStore.h"
#include "plugin/PluginHost.h"
#include "project/GitStatusService.h"

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
  Outline,
};

struct GitSidebarEntry {
  enum class Section {
    Modified,
    Outgoing,
  };

  Section section = Section::Modified;
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
    Entry,
    Empty,
  };

  Kind kind = Kind::Empty;
  GitSidebarEntry::Section section = GitSidebarEntry::Section::Modified;
  std::string label;
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
    GitSidebarEntry::Section section = GitSidebarEntry::Section::Modified;
    std::filesystem::path relative_path;
    project::GitFileStatus status = project::GitFileStatus::Clean;
    bool conflicted = false;
    bool staged = false;
  };

  struct RefreshSnapshot {
    std::vector<RefreshSnapshotEntry> entries;
    bool repo_available = false;
    std::string branch_label;
    std::string base_ref;
    std::string base_label;
    std::uint64_t generation = 0;
  };

  std::vector<GitSidebarEntry> entries;
  std::string branch_label;
  std::string base_ref;
  std::string base_label;
  OutgoingBaseChoice outgoing_base_choice;
  bool repo_available = false;
  bool refreshing = false;
  bool provider_backed = false;
  bool supports_mutations = true;
  std::string provider_id;
  std::string provider_label;
  std::string error;
  std::size_t selected_index = 0;
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
};

struct OutlineSymbolNode {
  std::string name;
  int kind = 1;
  std::size_t selection_line = 0;
  std::size_t selection_column = 0;
  std::vector<OutlineSymbolNode> children;
};

struct OutlineSidebarState {
  bool from_fallback = false;
  bool indexing = false;
  std::vector<OutlineSymbolNode> roots;
  std::unordered_set<std::string> collapsed_paths;
  std::size_t selected_flat_index = 0;

  void Clear() {
    from_fallback = false;
    indexing = false;
    roots.clear();
    collapsed_paths.clear();
    selected_flat_index = 0;
  }
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
  OutlineSidebarState outline;
};

}  // namespace microide::workspace
