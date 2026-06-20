#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "editor/BreakpointStore.h"
#include "editor/FunctionBreakpointStore.h"
#include "editor/DiagnosticsStore.h"
#include "editor/SingleLineEditor.h"
#include "editor/TextViewport.h"
#include "project/DirectoryTree.h"
#include "project/FileFinder.h"
#include "project/FileIndex.h"
#include "project/GitCompareService.h"
#include "compare/BranchReviewStateService.h"
#include "project/ProjectSearchService.h"
#include "workspace/DebugBreakpointsModel.h"
#include "workspace/DebugVariablesModel.h"
#include "workspace/DebugViewModel.h"
#include "workspace/DebugWatchModel.h"
#include "workspace/LaunchConfig.h"
#include "workspace/WorkspaceDapManager.h"
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
  DebugPane,
};

enum class OverlayMode {
  FileFinder,
  BufferSearch,
  BufferReplace,
  ProjectSearch,
  CommitPicker,
  LaunchConfigPicker,
  Completion,
  CodeActions,
};

enum class PanelContentKind {
  None,
  Terminal,
  Output,
};

// The four structured debug surfaces shown in the right-side debug pane. Selected
// by the pane's mode-row (a button switcher); only one is visible at a time. The
// backing data lives on `ProjectWorkspaceState` (`debug_execution`,
// `debug_variables`, `debug_watch`, `debug_breakpoints_panel`) and is reused as-is.
enum class DebugPaneMode {
  CallStack,
  Variables,
  Watch,
  Breakpoints,
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
  // Exact total match count from a count-all run (0 when unknown — the default
  // early-stop run cannot count past the display cap).
  std::size_t total_matches = 0;
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

// One selectable row in the launch-config picker (Phase 9). `config_index` points
// back into ProjectWorkspaceState.launch_configs; display strings are precomputed
// when the list is built so the render TU only draws them.
struct LaunchConfigPickerItem {
  std::size_t config_index = 0;
  std::string primary_label;    // launch config name (left column)
  std::string secondary_label;  // "type · request" (muted, right column)
};

struct LaunchConfigPickerState {
  std::string title = "Select Launch Configuration";
  std::string summary_line;  // "<matches> of <total>" precomputed on refresh
  editor::SingleLineEditor query;
  std::vector<LaunchConfigPickerItem> items;
  std::vector<LaunchConfigPickerItem> matches;
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
  LaunchConfigPickerState launch_config_picker;
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
  // Stick to the newest line as content streams in (e.g. debug-adapter / gdb
  // output), the same way a terminal tab follows its tail. Detaches when the
  // user scrolls up and re-attaches when they scroll back to the bottom.
  bool follow_tail = true;
};

// Right-side debug pane (mirrors the left SidebarState). The four debug surfaces
// share this pane via a mode-row button switcher; each keeps its own scroll so a
// surface switch preserves position. `visible` gates the whole pane (auto-opened
// on the first stop, toggled from the Debug menu); meaningful only when the
// `debug.enabled` setting is ON. Width is persisted per project.
struct DebugPaneState {
  bool visible = false;
  float width = 288.0f;  // mirror SidebarState default
  DebugPaneMode mode = DebugPaneMode::Variables;
  int call_stack_scroll_row = 0;
  int variables_scroll_row = 0;
  int watch_scroll_row = 0;
  int breakpoints_scroll_row = 0;
};

struct LspUiState {
  bool request_in_flight = false;
  Uint64 request_started_ticks = 0;
  Uint64 request_timeout_ticks = 0;
  // Single-slot memo for active-viewport language detection, keyed by path, so the
  // per-frame status-bar/provider/sync callers don't re-run filetype detection.
  // mutable so const provider queries can refresh it.
  mutable std::filesystem::path language_cache_path;
  mutable std::string language_cache_id;
};

struct PanelState {
  PanelContentKind content = PanelContentKind::None;
  bool command_mode = false;
  float height = 156.0f;
  // Horizontal scroll offset (first visible tab index) for the bottom-panel tab
  // strip, shared by terminal and output tabs so an overflowed strip stays
  // reachable via the chevrons or the header wheel.
  int tab_scroll_index = 0;
  CommandState command;
  OutputPanelState output;
};

struct WelcomeSurfaceState {
  editor::TextViewport viewport;
};

// A non-blocking strip shown at the top of an editor pane whose file changed on
// disk. `ExternalChange` is actionable (Reload / Overwrite / Keep) and appears
// when the in-memory buffer has unsaved edits; `ReloadedNotice` is informational
// and appears when a clean buffer was silently refreshed from disk.
struct EditorBannerState {
  enum class Kind {
    ExternalChange,
    ReloadedNotice,
  };
  Kind kind = Kind::ExternalChange;
  std::filesystem::path path;  // normalized absolute path the banner pertains to
};

enum class EditorBannerAction { Reload, Overwrite, Keep };

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
  // Per-project breakpoints keyed by file path. Adapter-agnostic; the host
  // snapshots it at launch (setBreakpoints) and reflects verification back.
  // Mirrors `diagnostics_store`: survives session restarts, persists via the
  // `debug` PersistedRecord. Only meaningful when `debug.enabled` is ON.
  editor::BreakpointStore breakpoint_store;
  // Per-project function (symbol) breakpoints. Adapter-agnostic sibling to
  // `breakpoint_store`: snapshotted at launch (setFunctionBreakpoints) with
  // verification reflected back. Persists via the `debug` PersistedRecord. Only
  // meaningful when `debug.enabled` is ON.
  editor::FunctionBreakpointStore function_breakpoint_store;
  // Transient execution state for the active debug session (current stop + call
  // stack + focused frame). Rebuilt on every `stopped`, cleared on resume/stop;
  // never persisted. Only meaningful when `debug.enabled` is ON.
  DebugExecutionView debug_execution;
  // Transient lazy Variables/Scopes tree for the focused frame (Phase 4). Rebuilt
  // on each `stopped`/frame focus, cleared on resume/stop; never persisted. Only
  // meaningful when `debug.enabled` is ON.
  DebugVariablesModel debug_variables;
  // Transient hover-to-inspect cache (Phase 5): the in-flight / most recent
  // `evaluate(context:"hover")` result for the focused frame. Cleared on
  // resume/stop and on a focused-frame switch; never persisted. Only meaningful
  // when `debug.enabled` is ON and the session is Stopped.
  DebugHoverModel debug_hover;
  // Watch expressions (Phase 6): a persisted list of expressions + their
  // transient evaluated value tree. The expression list survives session
  // restarts (persisted in the `debug` PersistedRecord); the evaluated values
  // are re-fetched on each `stopped`. Only meaningful when `debug.enabled` is ON.
  DebugWatchModel debug_watch;
  // Breakpoints panel model (Phase 7): the persisted enabled exception-filter id
  // set + the active session's advertised filters + a prebuilt row list (filter
  // toggles followed by navigable line breakpoints). The enabled set persists in
  // the `debug` PersistedRecord; advertised filters are transient. Only
  // meaningful when `debug.enabled` is ON.
  DebugBreakpointsModel debug_breakpoints_panel;
  // Right-side debug pane UI state (visibility, width, active surface, per-surface
  // scroll). The pane hosts the four structured debug surfaces; its data still
  // comes from the debug_* models above. Visibility/width/mode persist per
  // project. Only meaningful when `debug.enabled` is ON.
  DebugPaneState debug_pane;
  // Persisted + plugin-contributed launch/attach configurations and the
  // currently selected index. Start Debugging targets the selected config when
  // one exists, else falls back to the first registered adapter.
  std::vector<LaunchConfig> launch_configs;
  std::size_t selected_launch_config_index = 0;
  std::unique_ptr<LspManager> lsp_manager = std::make_unique<LspManager>();
  LspUiState lsp;
  // Per-project debug-adapter registry + active debug session (DAP). Lazily
  // populated by DebugService::EnsureProjectDapManager so a project that never
  // debugs pays nothing; mirrors `lsp_manager`.
  std::unique_ptr<DapManager> dap_manager = std::make_unique<DapManager>();
  std::string active_colorscheme_name = "default";
  std::optional<SDL_Color> project_base_color;
  EditorPreferences editor_preferences;
  std::vector<std::pair<std::string, std::string>> settings;
  std::vector<SidebarViewPolicy> sidebar_policies;
  compare::BranchReviewStateService branch_review;
  // At most one banner per path; rendered for the active editor tab whose file
  // matches. Empty in the common case (no external changes pending).
  std::vector<EditorBannerState> editor_banners;
};

// Banner state helpers (free functions; keep WorkspaceShell thin). Defined in
// WorkspaceShellEditorBanner.cpp.
const EditorBannerState* ActiveEditorBannerForTab(const ProjectWorkspaceState& state);
void SetEditorBanner(ProjectWorkspaceState& state, EditorBannerState::Kind kind,
                     const std::filesystem::path& path);
bool DismissEditorBannerForPath(ProjectWorkspaceState& state, const std::filesystem::path& path);

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
