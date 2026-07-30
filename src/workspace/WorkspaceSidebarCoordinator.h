#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceMenuState.h"
#include "workspace/WorkspacePluginRuntime.h"
#include "workspace/WorkspaceProjectState.h"
#include "workspace/WorkspacePromptState.h"
#include "workspace/GitSidebarCommandCenter.h"
#include "workspace/WorkspaceSidebarState.h"

namespace microide::workspace {

class SidebarCoordinator {
 public:
  struct Operations {
    std::function<void()> close_tree_context_menu;
    std::function<bool(const std::filesystem::path&, bool, bool)> open_project_tab;
    std::function<void()> stop_project_search;
    std::function<void()> request_window_redraw;
    std::function<void()> request_sidebar_redraw;
    std::function<void()> mark_layout_dirty;
    std::function<void()> request_git_refresh;
    std::function<bool(GitSidebarState::RefreshSnapshot*)> consume_git_refresh_snapshot;
    std::function<void()> refresh_project_search;
    // Off-thread forced full rescan (TD-2026-07-17-081/082): the whole-tree scan +
    // per-file stat runs on the shell's background executor; the finder/search
    // invalidation happens when the result applies on the main thread.
    std::function<void()> request_file_index_refresh;
    std::function<void(const std::filesystem::path&)> open_file;
    std::function<editor::TextViewport*()> active_editor_viewport;
    // LSP documentSymbol fallback for the outline: when the plugin provider yields no
    // symbols, this resolves the language server for the viewport, opens its document,
    // and requests documentSymbol. The async result is applied back through a FRESH
    // SidebarCoordinator (the shell owns it) rather than this transient one, so the
    // deferred callback never touches a destroyed coordinator. Null when LSP is off.
    std::function<void(const editor::TextViewport&, std::filesystem::path request_path,
                       std::string plugin_error)>
        query_lsp_document_symbols;
    // Applies a plugin document-symbol result to the outline through a FRESH
    // coordinator (the shell owns it). The async plugin callback routes here instead
    // of touching the transient coordinator that issued the query.
    std::function<void(std::filesystem::path request_path,
                       std::vector<plugin::PluginHost::DocumentSymbolNode> symbols,
                       std::string plugin_error)>
        apply_plugin_outline_result;
    // Applies a plugin sidebar snapshot to the active plugin view through a FRESH
    // coordinator (the shell owns it). The async snapshot callback routes here instead
    // of touching the transient coordinator that issued the query, which is destroyed
    // before the worker result lands.
    std::function<void(std::string request_view_id, bool ok,
                       std::vector<plugin::PluginHost::SidebarItem> items,
                       std::string error_message)>
        apply_plugin_sidebar_result;
    std::function<bool(const std::filesystem::path&)> open_git_conflict_merge;
    std::function<bool(const std::filesystem::path&, const std::string&, const std::string&)>
        open_working_tree_comparison;
    std::function<bool(const std::filesystem::path&,
                       const std::string&,
                       const std::string&,
                       const std::string&,
                       const std::string&)>
        open_branch_head_comparison;
    std::function<void(PromptSurfaceState::Action,
                       PromptSurfaceState::Kind,
                       const std::filesystem::path&,
                       std::string)>
        open_prompt_surface;
    std::function<bool(const std::filesystem::path&, std::string*)> has_dirty_editor_tabs_for_path;
    std::function<void(const std::filesystem::path&)> invalidate_editor_blame_path;
    std::function<void(const std::filesystem::path&)> reload_clean_editor_tabs_for_path;
    std::function<void(const std::filesystem::path&)> close_open_tabs_for_path;
    std::function<std::optional<WorkspaceLayout>()> current_workspace_layout;
    std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)> compute_tree_sidebar_list_layout;
    std::function<std::optional<std::size_t>()> selected_git_sidebar_line_index;
    // Returns a reference to the shared revision-exact cached line model (valid
    // until the next call on the same thread), so arrow-key navigation reads the
    // flattened tree without copying the whole vector on every step.
    std::function<const std::vector<GitSidebarLine>&()> build_git_sidebar_lines;
    std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)> compute_git_sidebar_list_layout;
    std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)>
        compute_problems_sidebar_list_layout;
    std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)> compute_tests_sidebar_list_layout;
    std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)> compute_plugin_sidebar_list_layout;
    std::function<bool()> refresh_tests_sidebar_state;
    std::function<bool(const std::vector<std::string>&)> run_tests;
    std::function<void(std::string)> set_command_feedback;
    std::function<bool(ActionId, const std::vector<std::string>&, ActionSource)> execute_action;
    std::function<bool()> open_commit_workflow;
  };

  SidebarCoordinator(ProjectWorkspaceState& state,
                     PromptState& prompts,
                     MenuSurfaceState& menu_state,
                     std::filesystem::path& project_root,
                     WorkspacePluginRuntime& plugin_runtime,
                     Operations operations);

  void ShowMode(SidebarMode mode, bool temporary = false);
  void ShowTree(const std::filesystem::path& root = {});
  void ShowSearch(std::string query = {}, bool temporary = false);
  void ShowProblems();
  void ShowGit();
  void ShowTests();
  void ShowOutline();
  bool ShowPlugin(std::string_view id, bool temporary = false);
  void Close();
  void Toggle();
  void RestorePrevious();
  void RefreshProjectFiles();
  void RefreshGit();
  bool RefreshProblems();
  bool RefreshTests();
  bool RefreshPlugin();
  // Rebuilds the outline rows from the active editable buffer's plugin document
  // symbols. Stored in the shared plugin item-tree state (see SidebarMode::Outline).
  bool RefreshOutline();
  // Applies an LSP documentSymbol fallback result (adapted node tree) to the outline
  // rows, dropping it if the outline/buffer changed since the request. Called on a
  // FRESH coordinator by the shell when the async LSP response lands, so it never
  // runs on the transient coordinator that issued the request.
  void ApplyLspOutlineResult(const std::filesystem::path& request_path,
                             const std::string& plugin_error,
                             const std::vector<plugin::PluginHost::DocumentSymbolNode>& lsp_symbols);
  // Applies a plugin document-symbol result to the outline (or hands off to the LSP
  // fallback when empty). Runs on a fresh coordinator, so the deferred plugin
  // callback never touches the transient one that issued the query.
  void ApplyPluginOutlineResult(const std::filesystem::path& request_path,
                                std::vector<plugin::PluginHost::DocumentSymbolNode> symbols,
                                std::string plugin_error);
  // Applies a plugin sidebar snapshot to the active plugin view, dropping it if the
  // view changed since the request. Runs on a fresh coordinator, so the deferred
  // worker callback never touches the transient one that issued the query.
  void ApplyPluginSidebarResult(const std::string& request_view_id, bool ok,
                                std::vector<plugin::PluginHost::SidebarItem> items,
                                std::string error_message);
  void RevealSelectedTreeLine();
  void RevealSelectedGitLine();
  void RevealSelectedProblemsLine();
  void RevealSelectedTestsLine();
  void RevealSelectedPluginLine();
  void MoveGitSelection(int delta);
  void MoveProblemsSelection(int delta);
  void MoveTestsSelection(int delta);
  void MovePluginSelection(int delta);
  bool OpenProblemItem();
  bool OpenTestItem();
  bool RunTestItem();
  bool OpenPluginItem();
  bool TogglePluginItem();
  bool CanStageAllGitEntries() const;
  bool CanDiscardAllGitEntries() const;
  bool StageAllGitEntries();
  void OpenDiscardAllGitPrompt();
  bool DiscardAllGitEntries();
  bool StageGitEntry(std::size_t entry_index);
  bool UnstageGitEntry(std::size_t entry_index);
  bool DiscardGitEntry(std::size_t entry_index,
                       const std::optional<std::filesystem::path>& expected_path = {});
  void OpenDiscardGitEntryPrompt(std::size_t entry_index);
  bool DispatchGitSidebarAction(GitSidebarActionId action, std::size_t entry_index);
  void ReconcileOpenTabsAfterPathDiscard(const std::filesystem::path& path);

 private:
  const GitSidebarEntry* GitEntry(std::size_t entry_index) const;
  void ReportDisabledGitAction(GitSidebarActionId action, std::size_t entry_index) const;
  void ReportGitOperationFailure(std::string_view verb, const GitSidebarEntry& entry) const;
  SidebarMode SidebarModeForViewId(std::string_view view_id) const;
  SidebarMode ActiveSidebarMode() const;
  // Rebuilds the prebuilt plugin/outline placeholder text from the current
  // items/error so the render TU never materializes it per frame. Call after any
  // mutation of `state_.sidebar.plugin.{items,error}`.
  void RecomputePluginSidebarPlaceholder();
  // Scrolls a scrollable sidebar list so `selected_index` is visible. Shared by
  // every per-mode RevealSelected*Line(): they differ only in the item count,
  // selection, and which list-layout to use.
  void RevealListSelection(
      std::size_t count, std::size_t selected_index,
      const std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)>& compute_layout);
  // Moves a flat-list sidebar selection by `delta` and reveals it. Shared by the
  // Problems / Tests / Plugin modes whose move and reveal counts are identical
  // (Git keeps its own path: its line model differs from its entry vector).
  void MoveSimpleListSelection(
      std::size_t count, std::size_t* selected_index,
      const std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)>& compute_layout,
      int delta);
  // Opens `path` in the editor (optionally moving the caret to `caret` =
  // {line, column}), restores a temporary sidebar, and hands focus to the editor.
  // Returns false without side effects when `path` is empty or no opener is wired.
  bool OpenEditorFileFromSidebar(const std::filesystem::path& path,
                                 std::optional<std::pair<std::size_t, std::size_t>> caret);

  ProjectWorkspaceState& state_;
  PromptState& prompts_;
  MenuSurfaceState& menu_state_;
  std::filesystem::path& project_root_;
  WorkspacePluginRuntime& plugin_runtime_;
  Operations operations_;
};

}  // namespace microide::workspace
