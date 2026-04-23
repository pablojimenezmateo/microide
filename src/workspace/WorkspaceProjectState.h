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
#include "util/SingleLineText.h"
#include "workspace/WorkspaceSidebarRegistry.h"
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
  Completion,
  CodeActions,
  TaskPicker,
};

enum class PanelContentKind {
  None,
  Terminal,
  Output,
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
  util::SingleLineTextState input;
  std::vector<std::string> history;
  std::optional<std::size_t> history_index;
  std::string history_pending_input;
  std::string feedback_text;
};

struct BufferSearchState {
  util::SingleLineTextState query;
  util::SingleLineTextState replace_text;
  std::vector<editor::SelectionRange> matches;
  std::size_t selected_index = 0;
};

struct ProjectSearchState {
  util::SingleLineTextState query;
  project::ProjectSearchOptions options;
  util::SingleLineTextState edit_buffer;
  bool editing = false;
  ProjectSearchEditField edit_field = ProjectSearchEditField::Query;
  util::SingleLineTextState replace_text;
  std::vector<project::ProjectSearchResult> results;
  std::size_t selected_index = 0;
  bool running = false;
  bool truncated = false;
  std::string error;
};

struct ComparePickerState {
  std::filesystem::path path;
  util::SingleLineTextState query;
  std::vector<project::GitCommitEntry> commits;
  std::vector<project::GitCommitEntry> matches;
  std::size_t selected_index = 0;
};

struct CompletionSessionItem {
  std::string label;
  std::string detail;
  std::string documentation;
  std::string insert_text;
};

struct CompletionSessionState {
  std::vector<CompletionSessionItem> items;
  std::size_t selected_index = 0;
  editor::SelectionRange replacement_range{};
  std::string source;
  std::string error;
};

struct CodeActionSessionItem {
  std::string title;
  std::string command;
  std::vector<std::string> arguments;
};

struct CodeActionSessionState {
  std::vector<CodeActionSessionItem> items;
  std::size_t selected_index = 0;
  std::string source;
  std::string error;
};

struct TaskPickerEntry {
  std::string id;
  std::string label;
  std::string group;
};

struct TaskPickerState {
  std::vector<TaskPickerEntry> entries;
  std::size_t selected_index = 0;
  std::string error;
};

struct OverlayWorkflowState {
  BufferSearchState buffer_search;
  ProjectSearchState project_search;
  ComparePickerState compare_picker;
  CompletionSessionState completion;
  CodeActionSessionState code_actions;
  TaskPickerState task_picker;
};

struct OverlayState {
  bool visible = false;
  OverlayMode mode = OverlayMode::FileFinder;
  BufferSearchField buffer_search_field = BufferSearchField::Search;
  int scroll_row = 0;
  OverlayWorkflowState workflow;
};

struct OutputPanelState {
  std::string channel_id = "plugins.log";
  std::vector<std::string> open_channel_ids;
  int scroll_row = 0;
};

struct ChatPanelState {
  std::string conversation_id;
  std::string pending_assistant_message_id;
  util::SingleLineTextState composer;
  int scroll_row = 0;
  bool request_in_flight = false;
  std::string status_text;
};

struct InlineCompletionState {
  bool visible = false;
  bool request_in_flight = false;
  std::string text;
  std::size_t start_line = 0;
  std::size_t start_column = 0;
  std::string provider_id;
  std::string model_id;
  std::string error;
};

struct DebugSessionState {
  bool running = false;
  std::string type;
  std::string channel_id;
  std::string status_text;
};

struct PanelState {
  PanelContentKind content = PanelContentKind::None;
  bool command_mode = false;
  float height = 184.0f;
  CommandState command;
  OutputPanelState output;
  ChatPanelState chat;
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
  InlineCompletionState inline_completion;
  DebugSessionState debug_session;
  editor::DiagnosticsStore diagnostics_store;
  std::string active_colorscheme_name = "default";
  std::optional<SDL_Color> project_base_color;
  EditorPreferences editor_preferences;
  std::vector<std::pair<std::string, std::string>> settings;
  std::vector<SidebarViewPolicy> sidebar_policies;
};

struct ProjectCatalogState {
  std::vector<std::unique_ptr<ProjectWorkspaceState>> entries;
  std::size_t active_index = 0;
  int tab_scroll_index = 0;
};

}  // namespace microide::workspace
