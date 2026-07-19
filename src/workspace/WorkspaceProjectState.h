#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "editor/BreakpointStore.h"
#include "editor/FunctionBreakpointStore.h"
#include "editor/DiagnosticsStore.h"
#include "editor/PluginDecorationStore.h"
#include "editor/PluginSurfaceStore.h"
#include "editor/SingleLineEditor.h"
#include "editor/TextViewport.h"
#include "workspace/WorkspaceActionTypes.h"
#include "workspace/WorkspaceFileIconRegistry.h"
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
#include "workspace/CompareInput.h"
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
  CommandPalette,
  Completion,
  CodeActions,
};

enum class PanelContentKind {
  None,
  Terminal,
  Output,
  PluginSurface,
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

// Backing state for the command-line executor. The command palette is now the sole entry
// surface (its own `query` editor drives input); this carries only the executor feedback
// string that the control channel and command dispatch read back. No on-screen prompt, no
// history (the palette's Up/Down navigate its result list).
struct CommandFeedbackState {
  std::string text;
};

struct BufferSearchState {
  editor::SingleLineEditor query;
  editor::SingleLineEditor replace_text;
  std::vector<editor::SelectionRange> matches;
  // Bumped whenever `matches` is reassigned (see WorkspaceShell::RefreshBufferSearch) so
  // the editor overview ruler can cheaply detect when its cached markers are stale.
  std::uint64_t matches_revision = 0;
  std::size_t selected_index = 0;
  std::vector<std::size_t> temporarily_expanded_fold_openers;
  std::filesystem::path temporarily_expanded_fold_tab_path;
  bool preserve_temporarily_expanded_folds = false;

  // Find-as-you-type incremental cache (see WorkspaceShell::RefreshBufferSearch).
  // Identifies the query + buffer state that produced the current `matches`. When
  // the next query merely extends `query` over the same unchanged buffer, the new
  // match set is a subset and is refined from `matches` instead of rescanning the
  // whole document. `viewport` is an opaque identity token (never dereferenced).
  struct IncrementalSearchCache {
    bool valid = false;
    const void* viewport = nullptr;
    std::uint64_t content_revision = 0;
    std::string query;
  };
  IncrementalSearchCache incremental;
};

struct ProjectSearchState {
  editor::SingleLineEditor query;
  project::ProjectSearchOptions options;
  editor::SingleLineEditor edit_buffer;
  bool editing = false;
  ProjectSearchEditField edit_field = ProjectSearchEditField::Query;
  editor::SingleLineEditor replace_text;
  std::vector<project::ProjectSearchResult> results;
  // Monotonic revision of `results`, bumped by every mutation (clear on a new run /
  // overlay close, append+sort in ConsumeProjectSearchUpdates). The grouped sidebar
  // line map (file-header rows interleaved with result indices) is a pure function
  // of `results`, so caching it against this revision lets render, hit-testing, and
  // keyboard navigation share one build instead of each walking every result,
  // copying filesystem paths for the group boundaries, and allocating a fresh
  // vector per frame / keystroke. See BuildProjectSearchLineMap.
  std::uint64_t results_revision = 0;
  mutable std::vector<int> cached_line_map;
  mutable std::uint64_t cached_line_map_revision = 0;
  mutable bool cached_line_map_valid = false;
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
  std::string search_text;      // lowercased "primary secondary ref", built once for filtering
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
  // True while the git history/branch query for this picker runs on the
  // background executor. The overlay opens immediately in this loading state and
  // the item list stays empty until the async result lands.
  bool loading = false;
  // Process-monotonic id of the in-flight async request, stamped by the shell on
  // open and compared on completion so stale results (overlay closed, project
  // switched, a newer picker opened) are dropped instead of populating the wrong
  // list. Main-thread-only.
  std::uint64_t active_request_generation = 0;
};

// One selectable row in the launch-config picker (Phase 9). `config_index` points
// back into ProjectWorkspaceState.launch_configs; display strings are precomputed
// when the list is built so the render TU only draws them.
struct LaunchConfigPickerItem {
  std::size_t config_index = 0;
  std::string primary_label;    // launch config name (left column)
  std::string secondary_label;  // "type · request" (muted, right column)
  std::string search_text;      // lowercased "primary secondary", built once for filtering
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
  // Per-item replacement range from an LSP textEdit (0-based, already converted to
  // editor byte columns). When set it overrides the session's heuristic
  // `replacement_range` so member/path completions extend the qualifier instead of
  // overwriting it.
  std::optional<editor::SelectionRange> replacement_range;
};

struct CompletionSessionState {
  std::vector<CompletionSessionItem> items;
  std::size_t selected_index = 0;
  editor::SelectionRange replacement_range{};
  std::string source;
  std::string error;
};

// One resolved text edit from a code action's inline WorkspaceEdit. Coordinates
// are 0-based (LSP-native). An empty `path` targets the active editable buffer.
struct CodeActionEdit {
  std::filesystem::path path;
  editor::SelectionRange range;
  std::string new_text;
};

struct CodeActionSessionItem {
  std::string title;
  std::string command;
  std::vector<std::string> arguments;
  // When non-empty the action is applied directly as buffer edits (no command
  // dispatch). This is how clangd delivers quickfixes like "remove #include X".
  std::vector<CodeActionEdit> edits;
  // True when this action's inline WorkspaceEdit was NOT materialized because the
  // overlay's shared aggregate edit/byte budget was exhausted — the action is shown
  // but its inline fix is disabled (no edits to apply). TD-2026-07-17A-057.
  bool edits_truncated = false;
};

struct CodeActionSessionState {
  std::vector<CodeActionSessionItem> items;
  std::size_t selected_index = 0;
  std::string source;
  std::string error;
};

// One selectable row in the command palette. `primary_label` is the human-facing
// command name and `secondary_label` its key chord (may be empty). Built-in commands
// carry an ActionId; plugin-contributed commands set is_plugin and dispatch by their
// command token instead. Labels are precomputed when the list is built so the render
// TU only draws them.
struct CommandPaletteItem {
  std::string primary_label;
  std::string secondary_label;
  std::string search_text;  // lowercased "primary secondary", built once for filtering
  ActionId action = ActionId::CodeActions;  // meaningful only when !is_plugin
  std::string command_token;                // plugin command name when is_plugin
  bool is_plugin = false;
};

struct CommandPaletteState {
  std::string title = "Commands";
  std::string summary_line;  // "<matches> of <total>" precomputed on refresh
  editor::SingleLineEditor query;
  std::vector<CommandPaletteItem> items;
  // Indices into `items` for the rows surviving the current query. Storing indices (not
  // full CommandPaletteItem copies) keeps palette typing from copying every matched row's
  // strings on each keystroke (TD-2026-07-17A-032); `items` stays the owner.
  std::vector<std::size_t> matches;
  std::size_t selected_index = 0;
};

struct OverlayWorkflowState {
  BufferSearchState buffer_search;
  ProjectSearchState project_search;
  ComparePickerState compare_picker;
  LaunchConfigPickerState launch_config_picker;
  CommandPaletteState command_palette;
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
  // Number of interactive requests currently in flight. A single bool could not
  // represent two overlapping requests (e.g. outline + hover), so the first to
  // return would flicker the busy indicator off while the other was still live.
  int request_in_flight_count = 0;
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
  float height = 156.0f;
  // Horizontal scroll offset (first visible tab index) for the bottom-panel tab
  // strip, shared by terminal and output tabs so an overflowed strip stays
  // reachable via the chevrons or the header wheel.
  int tab_scroll_index = 0;
  CommandFeedbackState feedback;
  OutputPanelState output;
  // Active plugin surface shown when `content == PluginSurface` (Phase E0). Keyed
  // by owner+id into `surface_store`; scroll is host-owned and panel-local.
  std::string surface_owner;
  std::string surface_id;
  int surface_scroll_y = 0;
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

// Render-side resolved file-tree icons, parallel to DirectoryTree::entries().
// Rebuilt lazily only when the tree entries change or plugin icon themes reload,
// so the per-frame sidebar loop reads cached icons instead of re-resolving (and
// re-lowercasing) every visible row. icons[i] aligns 1:1 with entries()[i];
// directory rows hold nullopt. Session-scoped, never persisted.
struct FileIconRenderCache {
  std::vector<std::optional<WorkspaceFileIconRegistry::Icon>> icons;
  std::uint64_t tree_revision = ~0ull;
  std::uint32_t icon_revision = ~0u;

  // Drop the resolved-icon cache back to its pristine "never built" state. Called
  // when file icons are disabled so the feature leaves no per-project footprint
  // and a later re-enable rebuilds cleanly (sentinel revisions force a refresh).
  void Reset() {
    icons.clear();
    icons.shrink_to_fit();
    tree_revision = ~0ull;
    icon_revision = ~0u;
  }
};

// Upper bound on tabs in a single editor group. Guards the interactive /
// control-driven open paths (the `tab` command, file/compare/merge opens) against
// a flood that would OOM the heap and drag down every O(open_tabs) layout, dirty
// scan, and tab-strip render pass. Set well beyond any human workflow; a hostile
// control client issuing `tab` in a tight loop hits this instead of growing
// without bound. Session restore is exempt (it replays trusted persisted state).
inline constexpr std::size_t kMaxOpenTabsPerGroup = 512;

// A single editor group: its own tab strip (open_tabs + active index + scroll)
// and its own home/placeholder surface. The editor area holds 1 or 2 groups
// arranged side-by-side or stacked (see `ProjectWorkspaceState::editor_groups`).
struct EditorGroup {
  WelcomeSurfaceState welcome_surface;
  std::vector<TabEntry> open_tabs;
  std::size_t active_tab_index = 0;
  int tab_scroll_index = 0;

  bool has_active_tab() const { return active_tab_index < open_tabs.size(); }
  TabEntry& active_tab() { return open_tabs[active_tab_index]; }
  const TabEntry& active_tab() const { return open_tabs[active_tab_index]; }
};

// A tab addressed by (group, tab) across ALL editor groups of one project. Used
// by the all-groups dirty enumerators and the group-aware save primitive so
// autosave / save-on-quit flush a buffer dirtied in the non-focused split group
// too (VSCode "Save All" semantics). Focused-group-scoped operations (close-tab /
// close-tabs UI) keep using a bare tab index into the focused group.
struct GroupTabRef {
  std::size_t group_index = 0;
  std::size_t tab_index = 0;
};

struct ProjectWorkspaceState {
  std::filesystem::path root;
  bool initialized = false;
  bool restore_persistence_on_activate = false;
  project::DirectoryTree directory_tree;
  // Resolved icons for `directory_tree.entries()`, rebuilt lazily by the sidebar
  // renderer. `mutable` so the const render path can refresh the cache.
  mutable FileIconRenderCache file_icon_cache;
  project::FileIndex file_index;
  project::FileFinder file_finder;
  // Editor groups: always 1 or 2. Group 0 is the primary. `focused_group_index`
  // selects which group owns keyboard focus / receives newly opened files.
  std::vector<EditorGroup> editor_groups = std::vector<EditorGroup>(1);
  std::size_t focused_group_index = 0;
  // Monotonic source of TabEntry::stable_id (never reused within a project session).
  // Only advanced when a dirty prompt first stamps a tab (TD-2026-07-17-024).
  std::uint64_t next_tab_stable_id = 1;
  EditorSplitOrientation group_split_orientation = EditorSplitOrientation::None;
  float group_split_fraction = 0.5f;

  // Side-effect-free accessors. `editor_groups` is invariantly non-empty (the
  // mutation sites that erase/clear a group always restore at least one), and
  // `focused_group_index` is kept valid by those same sites; a stale index here
  // is clamped on read rather than silently mutated, so const and non-const
  // resolve to the same group.
  std::size_t clamped_focused_group_index() const {
    return focused_group_index < editor_groups.size() ? focused_group_index : 0;
  }
  EditorGroup& focused_group() { return editor_groups[clamped_focused_group_index()]; }
  const EditorGroup& focused_group() const {
    return editor_groups[clamped_focused_group_index()];
  }

  ProjectSurfaceState surface;
  SidebarState sidebar;
  OverlayState overlay;
  PanelState panel;
  std::vector<std::unique_ptr<TerminalTabState>> terminal_tabs;
  std::size_t active_terminal_tab_index = 0;

  // Bounds-checked active terminal tab, or nullptr when there are none / the
  // stored index is stale. Mirrors clamped_focused_group_index()'s clamp-on-read
  // discipline so callers never index terminal_tabs by a possibly-stale index
  // after only an emptiness check.
  TerminalTabState* active_terminal_tab() {
    return active_terminal_tab_index < terminal_tabs.size()
               ? terminal_tabs[active_terminal_tab_index].get()
               : nullptr;
  }
  const TerminalTabState* active_terminal_tab() const {
    return active_terminal_tab_index < terminal_tabs.size()
               ? terminal_tabs[active_terminal_tab_index].get()
               : nullptr;
  }
  editor::DiagnosticsStore diagnostics_store;
  // Plugin/LSP-published editor presentation: decorations (inline text styles,
  // gutter marks, inline/virtual text, code lenses) keyed by owner+path, and
  // content surfaces (display lists / rasters) keyed by owner+surface_id. Both
  // mirror `diagnostics_store`: a producer replaces its contribution atomically
  // and the renderer reads the merged view. Session-scoped (never persisted).
  //
  // Bundled behind a lazily-allocated pointer so an unloaded session pays zero
  // bytes and the per-frame render path gates on a single null check (mirroring
  // the debugger's emplace-on-enable view model). `unique_ptr` — not `optional`
  // — because the stores hand out pointers/spans valid "until the next mutation"
  // and the bundle's address must stay stable across publish/clear cycles. Null
  // until the first publish from any producer (Lua plugin or LSP semantic
  // tokens); released back to null when both stores drain empty.
  struct PluginEditorPresentation {
    editor::PluginDecorationStore decorations;
    editor::PluginSurfaceStore surfaces;
    // Ghost text (Copilot-style inline suggestion). At most one is live at a
    // time (single owner, last writer wins). The host validates the anchor
    // against the live caret before storing and clears it synchronously on any
    // edit/caret/focus/buffer change, so it never renders stale. `lines[0]` is
    // the caret-line tail inserted at `anchor_column`; `lines[1..]` are the
    // dimmed rows shown below. Split once at publish; the renderer hands out
    // views into `lines`, never copies per frame.
    struct GhostText {
      std::string owner;                   // publishing plugin id
      std::filesystem::path path;          // normalized target buffer
      std::size_t anchor_line = 0;         // 0-based caret line it was computed for
      std::size_t anchor_column = 0;       // 0-based caret column (byte) = insert point
      std::uint64_t content_revision = 0;  // viewport rev at store time (staleness)
      std::vector<std::string> lines;
      bool empty() const { return lines.empty(); }
    };
    std::optional<GhostText> ghost_text;
  };
  std::unique_ptr<PluginEditorPresentation> plugin_presentation;

  // Reader gate: returns nullptr when nothing is published (the common case).
  const PluginEditorPresentation* plugin_presentation_if_present() const {
    return plugin_presentation.get();
  }
  // Ghost-text reader gate: nullptr unless a suggestion is currently live.
  const PluginEditorPresentation::GhostText* ghost_text_if_present() const {
    return plugin_presentation && plugin_presentation->ghost_text
               ? &*plugin_presentation->ghost_text
               : nullptr;
  }
  // Writer entry: lazily allocates the bundle on the first contribution.
  PluginEditorPresentation& EnsurePluginPresentation() {
    if (!plugin_presentation) {
      plugin_presentation = std::make_unique<PluginEditorPresentation>();
    }
    return *plugin_presentation;
  }
  // Restore the zero-footprint state once both stores are empty. Call as the
  // last statement of a clear handler (after redraw is requested, never
  // mid-render) so no reader holds a store pointer across the reset.
  void MaybeReleasePluginPresentation() {
    if (plugin_presentation && plugin_presentation->decorations.empty() &&
        plugin_presentation->surfaces.empty() &&
        !plugin_presentation->ghost_text.has_value()) {
      plugin_presentation.reset();
    }
  }
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
  // "Select for Compare" stash: the reference side awaiting "Compare with
  // Selected". Runtime-only (never persisted). `compare_selection_from_file`
  // marks a file source so the compare re-reads fresh content at compare time
  // (buffer/clipboard snapshots are used as captured).
  std::optional<CompareInput> compare_selection;
  bool compare_selection_from_file = false;
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
