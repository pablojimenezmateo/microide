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
#include "compare/BranchReviewStateService.h"
#include "project/ProjectSearchService.h"
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
  // After Ctrl+K in the editor with no overriding Ctrl+K binding; cleared on the
  // next key (fold-all / unfold-all follow-ups, or when focus leaves the editor).
  bool editor_ctrl_k_leader_armed = false;
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
  std::vector<std::size_t> temporarily_expanded_fold_openers;
  std::filesystem::path temporarily_expanded_fold_tab_path;
  bool preserve_temporarily_expanded_folds = false;
};

struct ProjectSearchState {
  editor::SingleLineEditor query;
  project::ProjectSearchOptions options;
  editor::SingleLineEditor edit_buffer;
  bool editing = false;
  ProjectSearchEditField edit_field = ProjectSearchEditField::Query;
  editor::SingleLineEditor replace_text;
  std::vector<project::ProjectSearchResult> results;
  // Query text whose search has fully completed and whose `results` are cached.
  // Empty until a search finishes. Lets the search sidebar reuse results when
  // the user leaves and returns to the panel instead of re-running every time.
  // RefreshProjectSearch clears it (a new run invalidates the cache) and
  // ConsumeProjectSearchUpdates sets it when the worker reports `finished`.
  std::string searched_query;
  std::size_t selected_index = 0;
  // Latest progress counters from ProjectSearchService — "X of Y files" UX.
  // total_files is the candidate-set size pinned at search start; searched_files
  // advances as the worker visits each file.
  std::size_t searched_files = 0;
  std::size_t total_files = 0;
  bool running = false;
  bool truncated = false;
  std::string error;
};

// One selectable row in the ref/commit picker. Display strings are precomputed
// when the list is built (in the coordinator) so the render TU only draws them.
struct GitPickerItem {
  enum class Kind { Commit, Branch };
  Kind kind = Kind::Commit;
  std::string ref;              // commit hash, or short branch name
  std::string apply_label;      // user-facing label applied on selection (short_hash / branch)
  std::string primary_label;    // left column text
  std::string secondary_label;  // right column text (muted)
  project::GitCommitEntry commit;  // populated when kind == Commit (for open_comparison)
};

// What selecting an item does. CompareFileHistory opens a diff of the picked
// commit against the working tree; OutgoingBaseRef sets the sidebar's outgoing
// comparison base to the picked branch/commit.
enum class ComparePickerPurpose { CompareFileHistory, OutgoingBaseRef };

struct ComparePickerState {
  ComparePickerPurpose purpose = ComparePickerPurpose::CompareFileHistory;
  std::filesystem::path path;   // file being compared (CompareFileHistory only)
  std::string title;            // header title, e.g. "Compare against"
  std::string context_label;    // subtitle: filename or branch context
  std::string summary_line;     // "<matches> of <total>" precomputed in RefreshPicker
  editor::SingleLineEditor query;
  std::vector<GitPickerItem> items;
  std::vector<GitPickerItem> matches;
  std::size_t selected_index = 0;
};

struct CompletionSessionItem {
  std::string label;
  std::string detail;
  std::string documentation;
  std::string insert_text;
  bool is_snippet = false;
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

struct OverlayWorkflowState {
  BufferSearchState buffer_search;
  ProjectSearchState project_search;
  ComparePickerState compare_picker;
  CompletionSessionState completion;
  CodeActionSessionState code_actions;
};

struct OverlayState {
  bool visible = false;
  OverlayMode mode = OverlayMode::FileFinder;
  BufferSearchField buffer_search_field = BufferSearchField::Search;
  int scroll_row = 0;
  OverlayWorkflowState workflow;
  // Caret rect (editor coordinates) captured when a caret-anchored overlay (completion /
  // code actions) opens, so the popup can render as a compact list next to the caret
  // instead of a large centered modal that hides the code being completed.
  std::optional<SDL_FRect> caret_anchor;
};

struct OutputPanelState {
  std::string channel_id = "plugins.log";
  std::vector<std::string> open_channel_ids;
  int scroll_row = 0;
};

struct LspUiState {
  bool request_in_flight = false;
  Uint64 request_started_ticks = 0;
  Uint64 request_timeout_ticks = 0;
};

struct PanelState {
  PanelContentKind content = PanelContentKind::None;
  bool command_mode = false;
  float height = 156.0f;
  CommandState command;
  OutputPanelState output;
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
  editor::DiagnosticsStore diagnostics_store;
  std::unique_ptr<LspManager> lsp_manager = std::make_unique<LspManager>();
  LspUiState lsp;
  std::string active_colorscheme_name = "default";
  std::optional<SDL_Color> project_base_color;
  EditorPreferences editor_preferences;
  std::vector<std::pair<std::string, std::string>> settings;
  std::vector<SidebarViewPolicy> sidebar_policies;
  compare::BranchReviewStateService branch_review;
};

struct ProjectCatalogState {
  std::vector<std::unique_ptr<ProjectWorkspaceState>> entries;
  std::size_t active_index = 0;
  int tab_scroll_index = 0;
};

// The surface keyboard focus lands on when no overlay/prompt owns input — the
// sidebar when it is visible, otherwise the editor. Single source of truth so
// dismissal paths cannot drift apart.
inline FocusTarget PrimarySurfaceFocus(const ProjectWorkspaceState& state) {
  return state.sidebar.visible ? FocusTarget::Sidebar : FocusTarget::Editor;
}

// Hide the overlay and ensure keyboard focus never strands on the now-hidden
// surface (the root cause of several "dead input" bugs). Shell-level dismissal
// (fold-reveal cleanup, redraw requests, cursor-kind invalidation) lives in
// WorkspaceShell::DismissOverlay; coordinators that cannot reach the shell use
// this minimal, focus-safe transition instead of a bare `overlay.visible = false`.
inline void HideOverlay(ProjectWorkspaceState& state) {
  state.overlay.visible = false;
  if (state.surface.focus == FocusTarget::Overlay) {
    state.surface.focus = PrimarySurfaceFocus(state);
  }
}

}  // namespace microide::workspace
