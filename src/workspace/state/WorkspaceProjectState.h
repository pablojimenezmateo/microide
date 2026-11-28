#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <utility>

#include "editor/BreakpointStore.h"
#include "editor/FunctionBreakpointStore.h"
#include "editor/DiagnosticsStore.h"
#include "editor/EditorViewModel.h"
#include "editor/PluginDecorationStore.h"
#include "editor/PluginSurfaceStore.h"
#include "editor/SingleLineEditor.h"
#include "editor/TextViewport.h"
#include "editor/WelcomeView.h"
#include "workspace/actions/WorkspaceActionTypes.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/state/EditorNavigationHistory.h"
#include "workspace/state/TerminalLayoutRecord.h"
#include "workspace/registries/WorkspaceFileIconRegistry.h"
#include "platform/ProcessLauncher.h"
#include "project/DirectoryTree.h"
#include "project/FileFinder.h"
#include "project/EditorConfig.h"
#include "project/FileIndex.h"
#include "project/FileWriteGate.h"
#include "project/ProjectId.h"
#include "project/ProjectLocality.h"
#include "project/GitCompareService.h"
#include "compare/BranchReviewStateService.h"
#include "project/ProjectSearchService.h"
#include "workspace/debug/DebugBreakpointsModel.h"
#include "workspace/debug/DebugVariablesModel.h"
#include "workspace/debug/DebugViewModel.h"
#include "workspace/debug/DebugWatchModel.h"
#include "workspace/debug/LaunchConfig.h"
#include "workspace/debug/WorkspaceDapManager.h"
#include "workspace/lsp/WorkspaceLspManager.h"
#include "workspace/registries/WorkspaceSidebarRegistry.h"
#include "workspace/state/WorkspaceSidebarState.h"
#include "workspace/CompareInput.h"
#include "workspace/state/WorkspaceTabState.h"

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
  CommitPicker,
  QuickPick,
  CommandPalette,
  Completion,
  CodeActions,
};

inline bool OverlayIsQuickOpen(OverlayMode mode) {
  switch (mode) {
    case OverlayMode::FileFinder:
    case OverlayMode::CommitPicker:
    case OverlayMode::QuickPick:
    case OverlayMode::CommandPalette:
      return true;
    default:
      return false;
  }
}

inline bool OverlayIsModal(OverlayMode mode) {
  return mode != OverlayMode::BufferSearch && mode != OverlayMode::BufferReplace &&
         mode != OverlayMode::Completion;
}

inline float OverlayQueryRowOffset(OverlayMode mode) {
  return OverlayIsQuickOpen(mode) ? 52.0f : 0.0f;
}

inline float OverlaySummaryRowOffset(OverlayMode mode) {
  return OverlayIsQuickOpen(mode) ? 78.0f : 0.0f;
}

inline float OverlayListStartOffset(OverlayMode mode) {
  switch (mode) {
    case OverlayMode::BufferReplace:
      return 106.0f;
    case OverlayMode::Completion:
      return 8.0f;
    case OverlayMode::CodeActions:
      return 40.0f;
    case OverlayMode::BufferSearch:
      return 86.0f;
    default:
      return 108.0f;
  }
}

enum class PanelContentKind {
  None,
  Terminal,
  Output,
  PluginSurface,
};

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
  Include,
  Exclude,
};

struct ProjectSurfaceState {
  FocusTarget focus = FocusTarget::Sidebar;
  struct PendingChord {
    SDL_Keycode key = SDLK_UNKNOWN;
    SDL_Keymod modifiers = SDL_KMOD_NONE;
  };
  std::optional<PendingChord> pending_chord;
};

struct CommandFeedbackState {
  std::string text;
};

struct BufferSearchState {
  editor::SingleLineEditor query;
  editor::SingleLineEditor replace_text;
  bool regex = false;
  bool match_case = false;
  bool whole_word = false;
  struct Scope {
    editor::SelectionRange range;
    const void* document = nullptr;
    std::uint64_t content_revision = 0;
    std::size_t lines_after_end = 0;
    std::size_t columns_after_end = 0;
  };
  std::optional<Scope> scope;
  std::optional<Scope> opened_over;
  std::vector<editor::SelectionRange> matches;
  std::uint64_t matches_revision = 0;
  const void* matches_viewport = nullptr;
  const void* matches_document = nullptr;
  std::uint64_t matches_content_revision = 0;
  std::size_t selected_index = 0;
  std::vector<editor::FoldRange> temporarily_expanded_folds;
  std::filesystem::path temporarily_expanded_fold_tab_path;
  bool preserve_temporarily_expanded_folds = false;

  struct IncrementalSearchCache {
    bool valid = false;
    const void* viewport = nullptr;
    const void* document = nullptr;
    std::uint64_t content_revision = 0;
    std::string query;
    bool match_case = false;
    bool whole_word = false;
    std::optional<editor::SelectionRange> scope;
  };
  IncrementalSearchCache incremental;
};

enum class ProjectSearchRerun { Fresh, InPlace };

struct ProjectSearchState {
  TypeNavigationState type_navigation;
  editor::SingleLineEditor query;
  project::ProjectSearchOptions options;
  editor::SingleLineEditor edit_buffer;
  bool editing = false;
  ProjectSearchEditField edit_field = ProjectSearchEditField::Query;
  editor::SingleLineEditor replace_text;
  editor::SingleLineEditor include_globs;
  editor::SingleLineEditor exclude_globs;
  bool scope_expanded = false;
  static constexpr std::uint64_t kSearchOnTypeDebounceMs = 300;
  std::uint64_t search_on_type_due_ms = 0;
  std::vector<project::ProjectSearchResult> results;
  std::uint64_t results_revision = 0;
  mutable std::vector<int> cached_line_map;
  mutable std::uint64_t cached_line_map_revision = 0;
  mutable bool cached_line_map_valid = false;
  std::string searched_query;
  std::string run_query;
  project::ProjectSearchOptions run_options;
  const void* live_hits_document = nullptr;
  std::uint64_t live_hits_content_revision = 0;
  std::size_t selected_index = 0;
  std::size_t searched_files = 0;
  std::size_t total_files = 0;
  std::size_t total_matches = 0;
  bool running = false;
  bool truncated = false;
  bool index_incomplete = false;
  bool searched_local_copy = false;
  std::string error;
  bool rerun_in_place = false;
  std::vector<project::ProjectSearchResult> staged_results;
  bool staged_truncated = false;
};

inline editor::SingleLineEditor& ProjectSearchFieldEditor(ProjectSearchState& search,
                                                          ProjectSearchEditField field) {
  switch (field) {
    case ProjectSearchEditField::Replace:
      return search.replace_text;
    case ProjectSearchEditField::Include:
      return search.include_globs;
    case ProjectSearchEditField::Exclude:
      return search.exclude_globs;
    case ProjectSearchEditField::Query:
    default:
      return search.query;
  }
}
inline const editor::SingleLineEditor& ProjectSearchFieldEditor(const ProjectSearchState& search,
                                                                ProjectSearchEditField field) {
  return ProjectSearchFieldEditor(const_cast<ProjectSearchState&>(search), field);
}

struct GitPickerItem {
  enum class Kind { Commit, Branch };
  Kind kind = Kind::Commit;
  std::string ref;
  std::string apply_label;
  std::string primary_label;
  std::string secondary_label;
  std::string search_text;
  project::GitCommitEntry commit;
};

enum class ComparePickerPurpose { CompareFileHistory, OutgoingBaseRef, SwitchBranch };

struct ComparePickerState {
  ComparePickerPurpose purpose = ComparePickerPurpose::CompareFileHistory;
  std::filesystem::path path;
  std::string title;
  std::string context_label;
  std::string summary_line;
  editor::SingleLineEditor query;
  std::vector<GitPickerItem> items;
  std::vector<GitPickerItem> matches;
  std::size_t selected_index = 0;
  bool loading = false;
  std::uint64_t active_request_generation = 0;
};

enum class QuickPickKind : std::uint8_t {
  LaunchConfig,
  ColorTheme,
  DocumentSymbol,
  WorkspaceSymbol,
  LanguageMode,
  RecentEditor,
  RunCommand,
};

struct QuickPickItem {
  std::size_t index = 0;
  std::string value;
  std::size_t line = 0;
  std::size_t column = 0;
  std::string primary_label;
  std::string secondary_label;
  std::string search_text;
};

struct QuickPickState {
  QuickPickKind kind = QuickPickKind::LaunchConfig;
  std::string title;
  std::string noun = "items";
  std::string summary_line;
  editor::SingleLineEditor query;
  std::vector<QuickPickItem> items;
  std::vector<QuickPickItem> matches;
  std::size_t selected_index = 0;
  std::string revert_value;
  struct View {
    std::filesystem::path path;
    std::size_t line = 0;
    std::size_t column = 0;
    std::size_t scroll = 0;
  };
  std::optional<View> revert_view;
  bool loading = false;
  bool server_filtered = false;
};

struct CompletionAdditionalEdit {
  editor::SelectionRange range;
  std::string new_text;
};

struct CompletionSessionItem {
  std::string label;
  std::string filter_text;
  std::string detail;
  std::string documentation;
  std::string insert_text;
  bool is_snippet = false;
  std::optional<editor::SelectionRange> replacement_range;
  std::vector<CompletionAdditionalEdit> additional_edits;

  std::string_view FilterText() const { return filter_text.empty() ? label : filter_text; }
};

struct CompletionSessionState {
  std::vector<CompletionSessionItem> items;
  std::vector<std::uint32_t> visible;
  std::size_t selected_index = 0;
  editor::SelectionRange replacement_range{};
  std::size_t request_column = 0;
  bool is_incomplete = false;
  bool automatic = false;
  std::string source;
  std::string error;

  std::size_t VisibleCount() const { return visible.size(); }
  const CompletionSessionItem& VisibleItem(std::size_t row) const { return items[visible[row]]; }
};

struct CodeActionEdit {
  std::filesystem::path path;
  editor::SelectionRange range;
  std::string new_text;
};

struct WorkspaceResourceOp {
  enum class Kind : std::uint8_t { Create, Rename, Delete };
  Kind kind = Kind::Create;
  std::filesystem::path path;
  std::filesystem::path new_path;
  bool overwrite = false;
  bool ignore_if_exists = false;
  bool ignore_if_not_exists = false;
  bool recursive = false;
};

struct CodeActionSessionItem {
  std::string title;
  std::string command;
  std::vector<std::string> arguments;
  std::vector<CodeActionEdit> edits;
  std::vector<WorkspaceResourceOp> resource_ops;
  bool edits_truncated = false;
};

struct CodeActionSessionState {
  std::vector<CodeActionSessionItem> items;
  std::size_t selected_index = 0;
  std::string source;
  std::string error;
  std::filesystem::path request_path;
  std::uint64_t request_revision = 0;
};

struct CommandPaletteItem {
  std::string primary_label;
  std::string secondary_label;
  std::string search_text;
  std::string command_name;
  ActionId action = ActionId::CodeActions;
  std::string command_token;
  bool is_plugin = false;
};

struct CommandPaletteState {
  std::string title = "Commands";
  std::string summary_line;
  editor::SingleLineEditor query;
  std::vector<CommandPaletteItem> items;
  std::vector<std::size_t> matches;
  std::size_t selected_index = 0;
  std::vector<std::string> recent;
};

inline constexpr std::size_t kCommandPaletteRecentCap = 10;

inline const std::string& CommandPaletteRecentKey(const CommandPaletteItem& item) {
  if (item.is_plugin) {
    return item.command_token;
  }
  return item.command_name.empty() ? item.primary_label : item.command_name;
}

struct OverlayWorkflowState {
  std::filesystem::path file_finder_compare_left;
  std::string file_finder_compare_title;
  BufferSearchState buffer_search;
  ProjectSearchState project_search;
  ComparePickerState compare_picker;
  QuickPickState quick_pick;
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
  std::optional<SDL_FRect> caret_anchor;
  FocusTarget opened_from = FocusTarget::Editor;
};

struct OutputPanelState {
  std::string channel_id = "plugins.log";
  std::vector<std::string> open_channel_ids;
  int scroll_row = 0;
  bool follow_tail = true;
};

struct DebugPaneState {
  bool visible = false;
  float width = 288.0f;
  DebugPaneMode mode = DebugPaneMode::Variables;
  int call_stack_scroll_row = 0;
  int variables_scroll_row = 0;
  int watch_scroll_row = 0;
  int breakpoints_scroll_row = 0;
  int breakpoints_selected_row = 0;
};

struct LspUiState {
  int request_in_flight_count = 0;
  Uint64 request_started_ticks = 0;
  Uint64 request_timeout_ticks = 0;
};

struct PanelState {
  PanelContentKind content = PanelContentKind::None;
  float height = kWorkspaceDefaultBottomPanelHeight;
  bool maximized = false;
  std::optional<FocusTarget> maximized_seen_focus;
  bool maximized_seen_editor_overlay = false;
  PanelContentKind restore_content = PanelContentKind::None;
  int tab_scroll_index = 0;
  CommandFeedbackState feedback;
  OutputPanelState output;
  std::string surface_owner;
  std::string surface_id;
  int surface_scroll_y = 0;
};

struct WelcomeSurfaceState {
  editor::TextViewport viewport;
};

struct EditorBannerState {
  enum class Kind {
    ExternalChange,
    ReloadedNotice,
    OpenFailed,
    ReadOnly,
  };
  Kind kind = Kind::ExternalChange;
  std::filesystem::path path;
  std::string reason;
};

std::span<const EditorBannerButton> EditorBannerButtonsFor(EditorBannerState::Kind kind);

struct FileIconRenderCache {
  std::vector<std::optional<WorkspaceFileIconRegistry::Icon>> icons;
  std::uint64_t tree_revision = ~0ull;
  std::uint32_t icon_revision = ~0u;

  void Reset() {
    icons.clear();
    icons.shrink_to_fit();
    tree_revision = ~0ull;
    icon_revision = ~0u;
  }
};

inline constexpr std::size_t kMaxOpenTabsPerGroup = 512;

struct EditorGroup {
  WelcomeSurfaceState welcome_surface;
  std::vector<TabEntry> open_tabs;
  std::size_t active_tab_index = 0;
  int tab_scroll_index = 0;
  std::vector<std::uint64_t> recent_tab_ids;

  bool has_active_tab() const { return active_tab_index < open_tabs.size(); }
  void NoteActiveTabUse() {
    if (!has_active_tab()) {
      return;
    }
    const std::uint64_t id = EnsureTabStableId(active_tab());
    if (!recent_tab_ids.empty() && recent_tab_ids.front() == id &&
        recent_tab_ids.size() <= open_tabs.size()) {
      return;
    }
    std::erase(recent_tab_ids, id);
    recent_tab_ids.insert(recent_tab_ids.begin(), id);
    std::erase_if(recent_tab_ids, [&](std::uint64_t recent) {
      return std::none_of(open_tabs.begin(), open_tabs.end(),
                          [&](const TabEntry& tab) { return tab.stable_id == recent; });
    });
  }
  TabEntry& active_tab() { return open_tabs[active_tab_index]; }
  const TabEntry& active_tab() const { return open_tabs[active_tab_index]; }
  EditorTabState* active_editor() { return ActiveState(&TabEntry::editor_state, TabEntry::Kind::Editor); }
  const EditorTabState* active_editor() const {
    return ActiveState(&TabEntry::editor_state, TabEntry::Kind::Editor);
  }
  CompareTabState* active_compare() { return ActiveState(&TabEntry::compare, TabEntry::Kind::Compare); }
  const CompareTabState* active_compare() const {
    return ActiveState(&TabEntry::compare, TabEntry::Kind::Compare);
  }
  MergeTabState* active_merge() { return ActiveState(&TabEntry::merge, TabEntry::Kind::Merge); }
  const MergeTabState* active_merge() const { return ActiveState(&TabEntry::merge, TabEntry::Kind::Merge); }

 private:
  template <typename State>
  State* ActiveState(std::optional<State> TabEntry::*member, TabEntry::Kind kind) {
    return const_cast<State*>(std::as_const(*this).ActiveState(member, kind));
  }
  template <typename State>
  const State* ActiveState(std::optional<State> TabEntry::*member, TabEntry::Kind kind) const {
    if (!has_active_tab() || active_tab().kind != kind) {
      return nullptr;
    }
    const std::optional<State>& state = active_tab().*member;
    return state.has_value() ? &*state : nullptr;
  }
};

inline FocusTarget NextSurfaceInFocusRing(FocusTarget current, bool sidebar, bool debug_pane, bool panel,
                                          bool backward) {
  std::array<FocusTarget, 4> ring{};
  std::size_t size = 0;
  if (sidebar) {
    ring[size++] = FocusTarget::Sidebar;
  }
  ring[size++] = FocusTarget::Editor;
  if (debug_pane) {
    ring[size++] = FocusTarget::DebugPane;
  }
  if (panel) {
    ring[size++] = FocusTarget::Panel;
  }
  std::size_t at = 0;
  for (std::size_t i = 0; i < size; ++i) {
    if (ring[i] == current) {
      at = i;
      break;
    }
  }
  return ring[(at + (backward ? size - 1 : 1)) % size];
}

[[nodiscard]] inline std::uint32_t NextUntitledNumber(const std::vector<EditorGroup>& groups) {
  std::vector<bool> used;
  for (const EditorGroup& group : groups) {
    for (const TabEntry& tab : group.open_tabs) {
      if (tab.untitled_number != 0) {
        if (used.size() <= tab.untitled_number) {
          used.resize(tab.untitled_number + 1, false);
        }
        used[tab.untitled_number] = true;
      }
    }
  }
  std::uint32_t next = 1;
  while (next < used.size() && used[next]) {
    ++next;
  }
  return next;
}

inline void NumberUntitledTabs(std::vector<EditorGroup>& groups) {
  for (EditorGroup& group : groups) {
    for (TabEntry& tab : group.open_tabs) {
      if (tab.kind == TabEntry::Kind::Editor && tab.path.empty() && tab.untitled_number == 0) {
        tab.untitled_number = NextUntitledNumber(groups);
        tab.title = UntitledTitle(tab.untitled_number);
      }
    }
  }
}

inline void RenameTabsOnDocument(std::vector<EditorGroup>& groups,
                                 const editor::TextViewport& document,
                                 const std::filesystem::path& path) {
  for (EditorGroup& group : groups) {
    for (TabEntry& tab : group.open_tabs) {
      if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value() &&
          tab.editor_state->viewport.SharesDocumentWith(document)) {
        tab.path = path;
        tab.title = TabTitleForPath(path, tab.untitled_number);
      }
    }
  }
}

[[nodiscard]] inline const editor::TextViewport* LiveBufferViewOfPath(
    const std::vector<EditorGroup>& groups,
    const std::filesystem::path& normalized_path,
    const editor::TextViewport* exclude = nullptr) {
  if (normalized_path.empty()) {
    return nullptr;
  }
  for (const EditorGroup& group : groups) {
    for (const TabEntry& tab : group.open_tabs) {
      if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value() &&
          !tab.editor_state->content_pending() && &tab.editor_state->viewport != exclude &&
          EditorViewPathIs(*tab.editor_state, normalized_path)) {
        return &tab.editor_state->viewport;
      }
      if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
          tab.compare->right_editable && !tab.compare->load_pending &&
          &tab.compare->right_viewport != exclude &&
          util::SameAsNormalizedPath(tab.compare->right_viewport.path(), normalized_path)) {
        return &tab.compare->right_viewport;
      }
    }
  }
  return nullptr;
}

[[nodiscard]] inline bool AnotherBufferHoldsPath(const std::vector<EditorGroup>& groups,
                                                 const std::filesystem::path& normalized_path,
                                                 const editor::TextViewport& document) {
  for (const EditorGroup& group : groups) {
    for (const TabEntry& tab : group.open_tabs) {
      if (tab.kind == TabEntry::Kind::Editor && util::SameAsNormalizedPath(tab.path, normalized_path) &&
          !(tab.editor_state.has_value() && tab.editor_state->viewport.SharesDocumentWith(document))) {
        return true;
      }
      if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
          tab.compare->right_editable &&
          util::SameAsNormalizedPath(tab.compare->right_viewport.path(), normalized_path) &&
          !tab.compare->right_viewport.SharesDocumentWith(document)) {
        return true;
      }
    }
  }
  return false;
}

[[nodiscard]] inline const editor::TextViewport* OpenEditorViewOfPath(
    const std::vector<EditorGroup>& groups, const std::filesystem::path& normalized_path) {
  for (const EditorGroup& group : groups) {
    for (const TabEntry& tab : group.open_tabs) {
      if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value() &&
          !tab.editor_state->content_pending() &&
          EditorViewPathIs(*tab.editor_state, normalized_path)) {
        return &tab.editor_state->viewport;
      }
    }
  }
  return nullptr;
}

[[nodiscard]] inline editor::TextViewport* OpenEditorViewOfPath(
    std::vector<EditorGroup>& groups, const std::filesystem::path& normalized_path) {
  return const_cast<editor::TextViewport*>(OpenEditorViewOfPath(
      static_cast<const std::vector<EditorGroup>&>(groups), normalized_path));
}

inline bool AdoptLiveBufferForCompareRightSide(const std::vector<EditorGroup>& groups,
                                               CompareTabState& compare_tab,
                                               const editor::TextViewport* exclude = nullptr) {
  if (!compare_tab.right_editable) {
    return false;
  }
  editor::TextViewport& right = compare_tab.right_viewport;
  const editor::TextViewport* live =
      LiveBufferViewOfPath(groups, right.path().lexically_normal(), exclude);
  if (live == nullptr || live == &right || live->SharesDocumentWith(right)) {
    return false;
  }
  editor::TextViewport adopted = *live;
  adopted.SetViewportSize(right.visible_lines(), right.visible_columns());
  adopted.ApplyRestoredViewState(right.cursor_line(), right.cursor_column(), right.scroll_line(),
                                 right.horizontal_scroll());
  right = std::move(adopted);
  compare_tab.derived_fingerprint_valid = false;
  return true;
}

inline void RepointCompareRightSideToReloadedBuffer(CompareTabState& compare_tab,
                                                    const editor::TextViewport& reopened_view) {
  editor::TextViewport& right = compare_tab.right_viewport;
  editor::TextViewport restored_view = reopened_view;
  restored_view.SetViewportSize(right.visible_lines(), right.visible_columns());
  restored_view.ApplyRestoredViewState(right.cursor_line(), right.cursor_column(),
                                       right.scroll_line(), right.horizontal_scroll());
  right = std::move(restored_view);
  compare_tab.derived_fingerprint_valid = false;
}

inline void RejoinRestoredCompareSides(std::vector<EditorGroup>& groups) {
  for (EditorGroup& group : groups) {
    for (TabEntry& tab : group.open_tabs) {
      if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value() ||
          !tab.compare->right_editable || tab.compare->load_pending) {
        continue;
      }
      const editor::TextViewport& right = tab.compare->right_viewport;
      const std::filesystem::path path = right.path().lexically_normal();
      const editor::TextViewport* live = OpenEditorViewOfPath(std::as_const(groups), path);
      if (live == nullptr) {
        live = LiveBufferViewOfPath(groups, path, &right);
      }
      if (live == nullptr || live == &right || live->SharesDocumentWith(right)) {
        continue;
      }
      RepointCompareRightSideToReloadedBuffer(*tab.compare, *live);
    }
  }
}

struct GroupTabRef {
  std::size_t group_index = 0;
  std::size_t tab_index = 0;
};

struct PluginGhostText {
  std::string owner;
  std::filesystem::path path;
  std::size_t anchor_line = 0;
  std::size_t anchor_column = 0;
  std::uint64_t content_revision = 0;
  std::vector<std::string> lines;
  bool empty() const { return lines.empty(); }
};

struct ProjectWorkspaceState {
  std::filesystem::path root;
  project::ProjectId id;
  project::ProjectLocality locality;
  [[nodiscard]] const platform::ProcessLauncher& launcher() const { return *locality.launcher; }
  [[nodiscard]] project::FileWriteGate& write_gate() const { return *locality.write_gate; }
  bool initialized = false;
  bool restore_persistence_on_activate = false;
  bool root_missing = false;
  project::DirectoryTree directory_tree;
  mutable FileIconRenderCache file_icon_cache;
  project::FileIndex file_index;
  project::FileFinder file_finder;
  editor::WelcomeRemoteStatus remote_status;
  std::uint64_t remote_status_revision = 0;
  void SetPendingPaths(project::SharedPendingPaths pending) {
    file_index.SetPendingPaths(pending);
    directory_tree.SetPendingPaths(std::move(pending));
  }
  project::EditorConfigResolver editor_config;
  std::vector<EditorGroup> editor_groups = std::vector<EditorGroup>(1);
  std::size_t focused_group_index = 0;
  static constexpr std::size_t kMaxRecentlyClosedEditors = 32;
  std::vector<DeferredTabHandle> recently_closed_editors;
  EditorNavigationHistory navigation_history;
  EditorSplitTree editor_split;

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
  TerminalLayoutRecord terminal_layout_to_revive;
  bool catch_up_on_activate = false;
  bool host_terminals_unrecorded = false;

  TerminalTabState* active_terminal_tab() {
    return const_cast<TerminalTabState*>(std::as_const(*this).active_terminal_tab());
  }
  const TerminalTabState* active_terminal_tab() const {
    return active_terminal_tab_index < terminal_tabs.size()
               ? terminal_tabs[active_terminal_tab_index].get()
               : nullptr;
  }
  TerminalPaneState* active_terminal_pane() {
    TerminalTabState* tab = active_terminal_tab();
    return tab != nullptr ? tab->active() : nullptr;
  }
  const TerminalPaneState* active_terminal_pane() const {
    const TerminalTabState* tab = active_terminal_tab();
    return tab != nullptr ? tab->active() : nullptr;
  }
  editor::DiagnosticsStore diagnostics_store;

  struct SemanticOccurrenceHighlights {
    std::filesystem::path path;
    const void* document = nullptr;
    std::uint64_t content_revision = 0;
    std::vector<editor::OccurrenceRange> ranges;

    bool empty() const { return ranges.empty(); }
    void Clear() {
      path.clear();
      document = nullptr;
      content_revision = 0;
      ranges.clear();
    }
    bool CoversCaret(const std::filesystem::path& viewport_path, const void* viewport_document,
                     std::uint64_t revision, std::size_t caret_line, std::size_t caret_column) const {
      if (ranges.empty() || content_revision != revision || document != viewport_document ||
          path != viewport_path) {
        return false;
      }
      for (const editor::OccurrenceRange& range : ranges) {
        if (range.line_index == caret_line && caret_column >= range.start_column &&
            caret_column <= range.end_column) {
          return true;
        }
      }
      return false;
    }
  };
  SemanticOccurrenceHighlights semantic_occurrences;

  struct PluginEditorPresentation {
    editor::PluginDecorationStore decorations;
    editor::PluginSurfaceStore surfaces;
    using GhostText = PluginGhostText;
    std::optional<GhostText> ghost_text;
  };
  std::unique_ptr<PluginEditorPresentation> plugin_presentation;

  const PluginEditorPresentation* plugin_presentation_if_present() const {
    return plugin_presentation.get();
  }
  const PluginEditorPresentation::GhostText* ghost_text_if_present() const {
    return plugin_presentation && plugin_presentation->ghost_text
               ? &*plugin_presentation->ghost_text
               : nullptr;
  }
  PluginEditorPresentation& EnsurePluginPresentation() {
    if (!plugin_presentation) {
      plugin_presentation = std::make_unique<PluginEditorPresentation>();
    }
    return *plugin_presentation;
  }
  void MaybeReleasePluginPresentation() {
    if (plugin_presentation && plugin_presentation->decorations.empty() &&
        plugin_presentation->surfaces.empty() &&
        !plugin_presentation->ghost_text.has_value()) {
      plugin_presentation.reset();
    }
  }
  editor::BreakpointStore breakpoint_store;
  editor::FunctionBreakpointStore function_breakpoint_store;
  DebugExecutionView debug_execution;
  DebugVariablesModel debug_variables;
  DebugHoverModel debug_hover;
  DebugWatchModel debug_watch;
  DebugBreakpointsModel debug_breakpoints_panel;
  DebugPaneState debug_pane;
  std::vector<LaunchConfig> launch_configs;
  std::size_t selected_launch_config_index = 0;
  std::unique_ptr<LspManager> lsp_manager = std::make_unique<LspManager>();
  LspUiState lsp;
  std::unique_ptr<DapManager> dap_manager = std::make_unique<DapManager>();
  std::string active_colorscheme_name = "default";
  std::optional<SDL_Color> project_base_color;
  EditorPreferences editor_preferences;
  std::vector<std::pair<std::string, std::string>> settings;
  std::vector<SidebarViewPolicy> sidebar_policies;
  compare::BranchReviewStateService branch_review;
  std::optional<CompareInput> compare_selection;
  bool compare_selection_from_file = false;
  std::vector<EditorBannerState> editor_banners;
};

inline void ClearBottomPanelContent(ProjectWorkspaceState& state) {
  state.panel.content = PanelContentKind::None;
  if (state.surface.focus == FocusTarget::Panel) {
    state.surface.focus = FocusTarget::Editor;
  }
}

const EditorBannerState* ActiveEditorBannerForTab(const ProjectWorkspaceState& state);
SDL_FRect ActiveEditorBannerStripRect(const ProjectWorkspaceState& state,
                                      const EditorGroupRectsLayout& group_rects);
void SetEditorBanner(ProjectWorkspaceState& state, EditorBannerState::Kind kind,
                     const std::filesystem::path& path, std::string reason = {});
bool DismissEditorBannerForPath(ProjectWorkspaceState& state, const std::filesystem::path& path);

struct ProjectCatalogState {
  std::vector<std::unique_ptr<ProjectWorkspaceState>> entries;
  std::size_t active_index = 0;
  int tab_scroll_index = 0;
  std::string line_clipboard_text;
};

inline FocusTarget PrimarySurfaceFocus(const ProjectWorkspaceState& state) {
  return state.sidebar.visible ? FocusTarget::Sidebar : FocusTarget::Editor;
}

inline bool FocusSurfaceShown(const ProjectWorkspaceState& state, FocusTarget target) {
  switch (target) {
    case FocusTarget::Editor:
      return true;
    case FocusTarget::Sidebar:
      return state.sidebar.visible;
    case FocusTarget::Panel:
      return state.panel.content != PanelContentKind::None;
    case FocusTarget::DebugPane:
      return state.debug_pane.visible;
    case FocusTarget::Overlay:
      return state.overlay.visible;
  }
  return false;
}

inline FocusTarget FocusBackTo(const ProjectWorkspaceState& state, FocusTarget previous) {
  return FocusSurfaceShown(state, previous) ? previous : PrimarySurfaceFocus(state);
}

inline FocusTarget FocusAfterOverlay(const ProjectWorkspaceState& state) {
  return state.overlay.opened_from == FocusTarget::Overlay ? PrimarySurfaceFocus(state)
                                                           : FocusBackTo(state, state.overlay.opened_from);
}

inline bool ModalOverlayLostFocus(const ProjectWorkspaceState& state) {
  if (!state.overlay.visible || state.surface.focus == FocusTarget::Overlay) {
    return false;
  }
  switch (state.overlay.mode) {
    case OverlayMode::BufferSearch:
    case OverlayMode::BufferReplace:
      return false;
    case OverlayMode::Completion:
    case OverlayMode::CodeActions:
      return state.surface.focus != FocusTarget::Editor;
    default:
      return true;
  }
}

inline void HideOverlay(ProjectWorkspaceState& state) {
  state.overlay.visible = false;
  if (state.surface.focus == FocusTarget::Overlay) {
    state.surface.focus = FocusAfterOverlay(state);
  }
}

}
