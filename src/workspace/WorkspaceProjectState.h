#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "editor/DiagnosticsStore.h"
#include "project/DirectoryTree.h"
#include "project/FileFinder.h"
#include "project/FileIndex.h"
#include "project/GitCompareService.h"
#include "project/ProjectSearchService.h"
#include "workspace/WorkspaceSidebarState.h"
#include "workspace/WorkspaceTabState.h"

namespace microide::workspace {

enum class FocusTarget {
  Sidebar,
  Editor,
  Panel,
  Overlay,
};

enum class OverlayMode {
  FileFinder,
  BufferSearch,
  BufferReplace,
  ProjectSearch,
  CommitPicker,
};

enum class BufferSearchField {
  Search,
  Replace,
};

enum class ProjectSearchEditField {
  Query,
  Replace,
};

struct ProjectSurfaceState {
  FocusTarget focus = FocusTarget::Sidebar;
};

struct CommandState {
  std::string input;
  std::vector<std::string> history;
  std::optional<std::size_t> history_index;
  std::string history_pending_input;
  std::string feedback_text;
};

struct BufferSearchState {
  std::string query;
  std::string replace_text;
  std::vector<editor::SelectionRange> matches;
  std::size_t selected_index = 0;
};

struct ProjectSearchState {
  std::string query;
  project::ProjectSearchOptions options;
  std::string edit_buffer;
  bool editing = false;
  ProjectSearchEditField edit_field = ProjectSearchEditField::Query;
  std::string replace_text;
  std::vector<project::ProjectSearchResult> results;
  std::size_t selected_index = 0;
  bool running = false;
  bool truncated = false;
  std::string error;
};

struct ComparePickerState {
  std::filesystem::path path;
  std::string query;
  std::vector<project::GitCommitEntry> commits;
  std::vector<project::GitCommitEntry> matches;
  std::size_t selected_index = 0;
};

struct OverlayWorkflowState {
  BufferSearchState buffer_search;
  ProjectSearchState project_search;
  ComparePickerState compare_picker;
};

struct OverlayState {
  bool visible = false;
  OverlayMode mode = OverlayMode::FileFinder;
  BufferSearchField buffer_search_field = BufferSearchField::Search;
  int scroll_row = 0;
  OverlayWorkflowState workflow;
};

struct PanelState {
  bool command_mode = false;
  float height = 184.0f;
  CommandState command;
};

struct ProjectWorkspaceState {
  std::filesystem::path root;
  bool initialized = false;
  bool restore_persistence_on_activate = false;
  project::DirectoryTree directory_tree;
  project::FileIndex file_index;
  project::FileFinder file_finder;
  editor::TextViewport text_viewport;
  std::vector<TabEntry> open_tabs;
  std::size_t active_tab_index = 0;
  int tab_scroll_index = 0;
  ProjectSurfaceState surface;
  SidebarState sidebar;
  OverlayState overlay;
  PanelState panel;
  std::vector<std::unique_ptr<TerminalTabState>> terminal_tabs;
  std::size_t active_terminal_tab_index = 0;
  editor::DiagnosticsStore diagnostics_store;
  std::string active_colorscheme_name = "default";
  std::optional<SDL_Color> project_base_color;
  EditorPreferences editor_preferences;
};

struct ProjectCatalogState {
  std::vector<std::unique_ptr<ProjectWorkspaceState>> entries;
  std::size_t active_index = 0;
  int tab_scroll_index = 0;
};

}  // namespace microide::workspace
