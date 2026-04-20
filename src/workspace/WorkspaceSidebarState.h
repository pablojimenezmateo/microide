#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "editor/DiagnosticsStore.h"
#include "plugin/PluginHost.h"
#include "project/GitStatusService.h"

namespace microide::workspace {

enum class SidebarMode {
  None,
  Tree,
  Search,
  Problems,
  Git,
  Plugin,
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
  std::vector<GitSidebarEntry> entries;
  std::string base_ref;
  std::string base_label;
  bool repo_available = false;
  std::size_t selected_index = 0;
};

struct ProblemsSidebarState {
  std::vector<ProblemsSidebarEntry> entries;
  std::size_t selected_index = 0;
};

struct PluginSidebarState {
  std::vector<plugin::PluginHost::SidebarItem> items;
  std::string error;
  std::size_t selected_index = 0;
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
  PluginSidebarState plugin;
};

}  // namespace microide::workspace
