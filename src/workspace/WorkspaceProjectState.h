#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "editor/DiagnosticsStore.h"
#include "editor/SingleLineEditor.h"
#include "editor/TextViewport.h"
#include "project/DirectoryTree.h"
#include "project/FileFinder.h"
#include "project/FileIndex.h"
#include "project/GitCompareService.h"
#include "project/ProjectSearchService.h"
#include "workspace/WorkspaceConversation.h"
#include "workspace/WorkspaceAiContext.h"
#include "workspace/WorkspaceLspManager.h"
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
  editor::SingleLineEditor input;
  std::vector<std::string> history;
  std::optional<std::size_t> history_index;
  std::string history_pending_input;
  std::string feedback_text;
};

struct BufferSearchState {
  editor::SingleLineEditor query;
  editor::SingleLineEditor replace_text;
  std::vector<editor::SelectionRange> matches;
  std::size_t selected_index = 0;
};

struct ProjectSearchState {
  editor::SingleLineEditor query;
  project::ProjectSearchOptions options;
  editor::SingleLineEditor edit_buffer;
  bool editing = false;
  ProjectSearchEditField edit_field = ProjectSearchEditField::Query;
  editor::SingleLineEditor replace_text;
  std::vector<project::ProjectSearchResult> results;
  std::size_t selected_index = 0;
  bool running = false;
  bool truncated = false;
  std::string error;
};

struct ComparePickerState {
  std::filesystem::path path;
  editor::SingleLineEditor query;
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

enum class ChatPaneFocusRegion {
  Rail,
  Header,
  Transcript,
  Composer,
};

struct ChatPanelState {
  struct RequestSnapshot {
    std::string provider_id;
    std::string model_id;
    ToolMode tool_mode = ToolMode::Ask;
    ContextPolicy context_policy;
    std::vector<ContextItem> context_items;
  };

  struct PendingToolApproval {
    std::string conversation_id;
    std::string assistant_message_id;
    std::string provider_id;
    std::string request_id;
    std::string tool_call_id;
    std::string tool_id;
    std::string display_name;
    std::string arguments_json;
    std::string arguments_summary;
    std::string capability_scope;
    Uint64 requested_ticks = 0;
    Uint64 expires_at_ticks = 0;
  };

  struct RememberedToolApproval {
    std::string capability_scope;
    std::string tool_id;
    std::string display_name;
    Uint64 granted_at_ticks = 0;
  };

  std::string conversation_id;
  std::string request_conversation_id;
  std::string pending_assistant_message_id;
  editor::TextViewport composer;
  ChatPaneFocusRegion focus_region = ChatPaneFocusRegion::Composer;
  std::size_t header_focus_index = 0;
  int scroll_row = 0;
  bool request_in_flight = false;
  Uint64 request_started_ticks = 0;
  std::string status_text;
  std::string pending_provider_id;
  std::string pending_request_id;
  RequestSnapshot active_request;
  std::optional<PendingToolApproval> pending_tool_approval;
  std::vector<RememberedToolApproval> remembered_tool_approvals;
  // Restore warning displayed after session restore detected interrupted requests.
  bool has_restore_warning = false;
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
  std::string pending_provider_id;
  std::string pending_request_id;
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

struct WelcomeSurfaceState {
  editor::TextViewport viewport;
};

struct ProjectWorkspaceState {
  std::filesystem::path root;
  bool initialized = false;
  bool restore_persistence_on_activate = false;
  project::DirectoryTree directory_tree;
  project::FileIndex file_index;
  project::FileFinder file_finder;
  WelcomeSurfaceState welcome_surface;
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
  std::unique_ptr<LspManager> lsp_manager = std::make_unique<LspManager>();
  std::string active_colorscheme_name = "default";
  std::optional<SDL_Color> project_base_color;
  EditorPreferences editor_preferences;
  std::vector<std::pair<std::string, std::string>> settings;
  std::vector<SidebarViewPolicy> sidebar_policies;
  // Chat conversations persisted per-project.
  ConversationRegistry conversations;
};

struct ProjectCatalogState {
  std::vector<std::unique_ptr<ProjectWorkspaceState>> entries;
  std::size_t active_index = 0;
  int tab_scroll_index = 0;
};

}  // namespace microide::workspace
