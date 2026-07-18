#include "workspace/WorkspaceSidebarCoordinator.h"

#include <utility>

#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceGitSidebarPresentation.h"
#include "workspace/EditorTabService.h"
#include "workspace/PromptSurfaceService.h"
#include "workspace/SidebarService.h"
#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspacePathMutationCoordinator.h"
#include "workspace/WorkspaceShell.h"
#include "workspace/WorkspaceGitOutgoingBase.h"
#include "workspace/WorkspaceSidebarRegistry.h"
#include "project/GitCompareService.h"
#include "workspace/GitRepositoryService.h"

namespace microide::workspace {

SidebarCoordinator::SidebarCoordinator(ProjectWorkspaceState& state,
                                       PromptState& prompts,
                                       MenuSurfaceState& menu_state,
                                       std::filesystem::path& project_root,
                                       WorkspacePluginRuntime& plugin_runtime,
                                       Operations operations)
    : state_(state),
      prompts_(prompts),
      menu_state_(menu_state),
      project_root_(project_root),
      plugin_runtime_(plugin_runtime),
      operations_(std::move(operations)) {}

SidebarMode SidebarCoordinator::SidebarModeForViewId(std::string_view view_id) const {
  if (view_id.empty()) {
    return SidebarMode::None;
  }
  const auto view = FindSidebarView(view_id, plugin_runtime_.Host());
  return view.has_value() ? view->mode : SidebarMode::None;
}

SidebarMode SidebarCoordinator::ActiveSidebarMode() const {
  return SidebarModeForViewId(state_.sidebar.view_id);
}

void SidebarCoordinator::ShowMode(SidebarMode mode, bool temporary) {
  if (mode == SidebarMode::None) {
    Close();
    return;
  }
  if (mode != SidebarMode::Tree) {
    operations_.close_tree_context_menu();
  }

  if (ActiveSidebarMode() == SidebarMode::Search && mode != SidebarMode::Search) {
    operations_.stop_project_search();
  }

  if (temporary) {
    if (!state_.sidebar.temporary && state_.sidebar.visible) {
      state_.sidebar.prev_view_id = state_.sidebar.view_id;
    }
  } else {
    state_.sidebar.prev_view_id.clear();
  }

  if (mode != SidebarMode::Plugin) {
    if (const SidebarViewSpec* view = FindBuiltinSidebarView(mode); view != nullptr) {
      state_.sidebar.view_id = std::string(view->id);
    }
  }
  state_.sidebar.temporary = temporary;
  if (operations_.mark_layout_dirty) {
    operations_.mark_layout_dirty();
  }
  state_.sidebar.visible = true;
  state_.surface.focus = FocusTarget::Sidebar;
  state_.sidebar.scroll_row = 0;
  operations_.request_window_redraw();
}

void SidebarCoordinator::ShowTree(const std::filesystem::path& root) {
  if (!root.empty() && !operations_.open_project_tab(root, true, true)) {
    return;
  }

  ShowMode(SidebarMode::Tree, false);
}

void SidebarCoordinator::ShowSearch(std::string query, bool temporary) {
  ProjectSearchState& search = state_.overlay.workflow.project_search;
  if (!query.empty() || search.query.text().empty()) {
    search.query.SetText(std::move(query));
  }
  search.edit_buffer.SetText(search.query.text());
  search.editing = search.query.text().empty();
  search.edit_field = ProjectSearchEditField::Query;

  // Reuse cached results when returning to the search panel with a query whose
  // search already completed. Previously every activation cleared the results
  // and re-ran the search even though the query text was preserved, which
  // wasted work and discarded the user's selection. A changed query, a toggled
  // option, or a committed edit re-runs through RefreshProjectSearch (which
  // clears searched_query), so the cache stays correct.
  const bool results_cached =
      !search.query.text().empty() && search.searched_query == search.query.text();
  if (!results_cached) {
    search.selected_index = 0;
    operations_.refresh_project_search();
  }
  ShowMode(SidebarMode::Search, temporary);
}

void SidebarCoordinator::ShowProblems() {
  RefreshProblems();
  ShowMode(SidebarMode::Problems, false);
  RevealSelectedProblemsLine();
}

void SidebarCoordinator::ShowGit() {
  state_.sidebar.git.tree_git_badges_materialized = true;
  if (operations_.request_git_refresh != nullptr) {
    operations_.request_git_refresh();
  }
  RefreshGit();
  ShowMode(SidebarMode::Git, false);
  RevealSelectedGitLine();
}

void SidebarCoordinator::ShowTests() {
  RefreshTests();
  ShowMode(SidebarMode::Tests, false);
  RevealSelectedTestsLine();
}

void SidebarCoordinator::ShowOutline() {
  ShowMode(SidebarMode::Outline, false);
  RefreshOutline();
}

bool SidebarCoordinator::ShowPlugin(std::string_view id, bool temporary) {
  const auto* provider = plugin_runtime_.Host().FindSidebarProvider(id);
  if (provider == nullptr) {
    return false;
  }

  state_.sidebar.view_id = provider->id;
  ShowMode(SidebarMode::Plugin, temporary);
  return RefreshPlugin();
}

void SidebarCoordinator::Close() {
  const bool was_visible = state_.sidebar.visible;
  if (ActiveSidebarMode() == SidebarMode::Search) {
    operations_.stop_project_search();
  }
  operations_.close_tree_context_menu();

  if (state_.sidebar.temporary && !state_.sidebar.prev_view_id.empty()) {
    RestorePrevious();
    return;
  }

  state_.sidebar.visible = false;
  if (operations_.mark_layout_dirty) {
    operations_.mark_layout_dirty();
  }
  state_.sidebar.temporary = false;
  state_.sidebar.prev_view_id.clear();
  if (state_.surface.focus == FocusTarget::Sidebar) {
    state_.surface.focus = FocusTarget::Editor;
  }
  if (was_visible) {
    operations_.request_window_redraw();
  }
}

void SidebarCoordinator::Toggle() {
  const bool was_visible = state_.sidebar.visible;
  if (state_.sidebar.visible) {
    Close();
    return;
  }

  if (ActiveSidebarMode() == SidebarMode::None) {
    state_.sidebar.view_id = "tree";
  }
  state_.sidebar.visible = true;
  if (operations_.mark_layout_dirty) {
    operations_.mark_layout_dirty();
  }
  state_.sidebar.temporary = false;
  state_.sidebar.prev_view_id.clear();
  state_.surface.focus = FocusTarget::Sidebar;
  if (!was_visible) {
    operations_.request_window_redraw();
  }
}

void SidebarCoordinator::RestorePrevious() {
  if (ActiveSidebarMode() == SidebarMode::Search &&
      SidebarModeForViewId(state_.sidebar.prev_view_id) != SidebarMode::Search) {
    operations_.stop_project_search();
  }

  if (state_.sidebar.prev_view_id.empty()) {
    state_.sidebar.temporary = false;
    return;
  }

  state_.sidebar.view_id = state_.sidebar.prev_view_id;
  if (SidebarModeForViewId(state_.sidebar.view_id) == SidebarMode::None) {
    state_.sidebar.view_id = "tree";
  }
  state_.sidebar.prev_view_id.clear();
  state_.sidebar.temporary = false;
  state_.sidebar.visible = true;
  if (operations_.mark_layout_dirty) {
    operations_.mark_layout_dirty();
  }
  state_.surface.focus = FocusTarget::Sidebar;
  state_.sidebar.scroll_row = 0;
  if (ActiveSidebarMode() == SidebarMode::Plugin) {
    RefreshPlugin();
  } else if (ActiveSidebarMode() == SidebarMode::Problems) {
    RefreshProblems();
  } else if (ActiveSidebarMode() == SidebarMode::Tests) {
    RefreshTests();
  }
  operations_.request_sidebar_redraw();
}

void SidebarCoordinator::RefreshProjectFiles() {
  state_.directory_tree.Refresh();
  RevealSelectedTreeLine();
  state_.file_index.Refresh();
  state_.file_finder.InvalidateIndexCache();
  if (state_.overlay.visible && state_.overlay.mode == OverlayMode::FileFinder) {
    state_.file_finder.Refresh();
  }
  RefreshGit();
  RefreshProblems();
  RefreshTests();
  RefreshPlugin();
  if (state_.sidebar.visible) {
    operations_.request_sidebar_redraw();
  }
}

SidebarCoordinator WorkspaceShell::MakeSidebarCoordinator() {
  return SidebarCoordinator(
      context_.current_project_state, context_.prompts, context_.menu_state,
      context_.current_project_state.root, plugin_runtime_,
      SidebarCoordinator::Operations{
          .close_tree_context_menu = [this]() { MakeMenuCoordinator().CloseTreeContextMenu(); },
          .open_project_tab =
              [this](const std::filesystem::path& root, bool restore_persistence, bool log_feedback) {
                return OpenProjectTab(root, restore_persistence, log_feedback);
              },
          .stop_project_search = [this]() { StopProjectSearch(); },
          .request_window_redraw = [this]() { RequestWindowRedraw(); },
          .request_sidebar_redraw = [this]() { RequestSidebarRedraw(); },
          .mark_layout_dirty = [this]() { MarkLayoutDirty(); },
          .request_git_refresh = [this]() { RequestGitSidebarRefresh(); },
          .consume_git_refresh_snapshot =
              [this](GitSidebarState::RefreshSnapshot* snapshot) {
                const bool consumed = ConsumePendingGitSidebarRefreshSnapshot(snapshot);
                if (consumed) {
                  InvalidateStaleMergeTabs();
                }
                return consumed;
              },
          .refresh_project_search = [this]() { RefreshProjectSearch(); },
          .open_file = [this](const std::filesystem::path& path) { OpenFile(path); },
          .active_editor_viewport = [this]() { return ActiveEditorViewport(); },
          .query_lsp_document_symbols =
              [this](const editor::TextViewport& viewport, std::filesystem::path request_path,
                     std::string plugin_error) {
                QueryLspDocumentSymbolsForOutline(viewport, std::move(request_path),
                                                  std::move(plugin_error));
              },
          .apply_plugin_outline_result =
              [this](std::filesystem::path request_path,
                     std::vector<plugin::PluginHost::DocumentSymbolNode> symbols,
                     std::string plugin_error) {
                // Fresh coordinator: the transient one that issued the query is gone.
                MakeSidebarCoordinator().ApplyPluginOutlineResult(
                    std::move(request_path), std::move(symbols), std::move(plugin_error));
              },
          .apply_plugin_sidebar_result =
              [this](std::string request_view_id, bool ok,
                     std::vector<plugin::PluginHost::SidebarItem> items,
                     std::string error_message) {
                // Fresh coordinator: the transient one that issued the query is gone.
                MakeSidebarCoordinator().ApplyPluginSidebarResult(
                    request_view_id, ok, std::move(items), std::move(error_message));
              },
          .open_git_conflict_merge =
              [this](const std::filesystem::path& path) { return OpenGitConflictMerge(path); },
          .open_working_tree_comparison =
              [this](const std::filesystem::path& path,
                     const std::string& left_ref,
                     const std::string& left_label) {
                return OpenWorkingTreeComparison(path, left_ref, left_label);
              },
          .open_branch_head_comparison =
              [this](const std::filesystem::path& path,
                     const std::string& left_ref,
                     const std::string& left_label,
                     const std::string& right_ref,
                     const std::string& right_label) {
                return OpenBranchHeadComparison(path, left_ref, left_label, right_ref, right_label);
              },
          .open_prompt_surface =
              [this](PromptSurfaceState::Action action,
                     PromptSurfaceState::Kind kind,
                     const std::filesystem::path& path,
                     std::string input) {
                OpenPromptSurface(action, kind, path, std::move(input));
              },
          .has_dirty_editor_tabs_for_path =
              [this](const std::filesystem::path& path, std::string* blocking_label) {
                EditorTabService editor_tabs = MakeEditorTabService();
                PromptSurfaceService prompt_surfaces = MakePromptSurfaceService();
                return MakePathMutationCoordinator(editor_tabs, prompt_surfaces)
                    .HasDirtyEditorTabsForPath(path, blocking_label);
              },
          .invalidate_editor_blame_path =
              [this](const std::filesystem::path& path) { InvalidateEditorBlamePath(path); },
          .reload_clean_editor_tabs_for_path =
              [this](const std::filesystem::path& path) { ReloadCleanEditorTabsForPath(path); },
          .close_open_tabs_for_path =
              [this](const std::filesystem::path& path) {
                EditorTabService editor_tabs = MakeEditorTabService();
                PromptSurfaceService prompt_surfaces = MakePromptSurfaceService();
                MakePathMutationCoordinator(editor_tabs, prompt_surfaces).CloseOpenTabsForPath(path);
              },
          .current_workspace_layout = [this]() { return CurrentWorkspaceLayout(); },
          .compute_tree_sidebar_list_layout =
              [this](const SDL_FRect& rect, std::size_t count) {
                return ComputeTreeSidebarListLayout(rect, count);
              },
          .selected_git_sidebar_line_index = [this]() { return SelectedGitSidebarLineIndex(); },
          .build_git_sidebar_lines =
              [this]() -> const std::vector<GitSidebarLine>& { return BuildGitSidebarLines(); },
          .compute_git_sidebar_list_layout =
              [this](const SDL_FRect& rect, std::size_t count) {
                return ComputeGitSidebarListLayout(rect, count);
              },
          .compute_problems_sidebar_list_layout =
              [this](const SDL_FRect& rect, std::size_t count) {
                return ComputeProblemsSidebarListLayout(rect, count);
              },
          .compute_tests_sidebar_list_layout =
              [this](const SDL_FRect& rect, std::size_t count) {
                return ComputeTestsSidebarListLayout(rect, count);
              },
          .compute_plugin_sidebar_list_layout =
              [this](const SDL_FRect& rect, std::size_t count) {
                return ComputePluginSidebarListLayout(rect, count);
              },
          .refresh_tests_sidebar_state = [this]() { return RefreshTestsSidebarState(); },
          .run_tests = [this](const std::vector<std::string>& test_ids) {
            return RunTests(test_ids, nullptr);
          },
          .set_command_feedback = [this](std::string feedback) {
            context_.current_project_state.panel.feedback.text = std::move(feedback);
          },
          .execute_action =
              [this](ActionId id, const std::vector<std::string>& args, ActionSource source) {
                return ActionCoordinator(MakeActionContext()).Execute(id, args, source);
              },
          .open_commit_workflow = [this]() { return OpenCommitWorkflow(); },
      });
}

SidebarService WorkspaceShell::MakeSidebarService() {
  return SidebarService(MakeSidebarCoordinator());
}

void WorkspaceShell::ShowSidebarMode(SidebarMode mode, bool temporary) {
  MakeSidebarService().ShowMode(mode, temporary);
}

void WorkspaceShell::ShowTreeSidebar(const std::filesystem::path& root) {
  MakeSidebarService().ShowTree(root);
}

void WorkspaceShell::ShowSearchSidebar(std::string query, bool temporary) {
  MakeSidebarService().ShowSearch(std::move(query), temporary);
}

void WorkspaceShell::ShowProblemsSidebar() {
  MakeSidebarService().ShowProblems();
}

void WorkspaceShell::ShowGitSidebar() {
  MakeSidebarService().ShowGit();
}

void WorkspaceShell::ShowTestsSidebar() {
  MakeSidebarService().ShowTests();
}

void WorkspaceShell::ShowOutlineSidebar() {
  MakeSidebarService().ShowOutline();
}

bool WorkspaceShell::ShowPluginSidebar(std::string_view id, bool temporary) {
  return MakeSidebarService().ShowPlugin(id, temporary);
}

void WorkspaceShell::CloseSidebar() {
  MakeSidebarService().Close();
}

void WorkspaceShell::ToggleSidebar() {
  MakeSidebarService().Toggle();
}

void WorkspaceShell::RestorePreviousSidebar() {
  MakeSidebarService().RestorePrevious();
}

void WorkspaceShell::RefreshProjectFiles() {
  MakeSidebarService().RefreshProjectFiles();
}

void WorkspaceShell::RequestGitSidebarRefresh(GitSidebarRefreshScope scope) {
  if (context_.current_project_state.root.empty()) {
    context_.current_project_state.sidebar.git.refreshing = false;
    git_repository_service_.Reset();
    return;
  }

  if (scope == GitSidebarRefreshScope::TreeBadges) {
    context_.current_project_state.sidebar.git.tree_git_badges_materialized = true;
  } else if (scope == GitSidebarRefreshScope::Full) {
    context_.current_project_state.sidebar.git.tree_git_badges_materialized = true;
  }

  context_.current_project_state.sidebar.git.refreshing = true;
  git_repository_service_.RequestRefresh(
      context_.current_project_state.root, scope,
      context_.current_project_state.sidebar.git.outgoing_base_choice,
      context_.current_project_state.sidebar.git.tree_git_badges_materialized);
  RequestSidebarRedraw();
}

void WorkspaceShell::MaybeRequestTreeGitBadgesAfterFirstPaint() {
  if (!pending_tree_git_badge_refresh_after_paint_ ||
      context_.current_project_state.root.empty()) {
    return;
  }
  pending_tree_git_badge_refresh_after_paint_ = false;
  if (!GitRepositoryService::IsGitRepoValid(context_.current_project_state.root)) {
    return;
  }
  context_.current_project_state.sidebar.git.tree_git_badges_materialized = true;
  RequestGitSidebarRefresh(GitSidebarRefreshScope::TreeBadges);
}

void WorkspaceShell::OnFramePresented() {
  MaybeRequestTreeGitBadgesAfterFirstPaint();
  // Run any LSP hydration deferred by a tab switch now that the tab-switch frame
  // is on screen (TD-2026-07-17A-033). No-op when nothing was scheduled.
  lsp_service_.ConsumeDeferredBufferOpen();
}

void WorkspaceShell::RequestAutomaticGitSidebarRefresh() {
  if (context_.current_project_state.root.empty() ||
      context_.current_project_state.sidebar.git.refreshing) {
    return;
  }

  constexpr Uint64 kAutomaticGitRefreshThrottleMs = 750;
  const Uint64 now_ms = SDL_GetTicks();
  if (now_ms < next_automatic_git_sidebar_refresh_ms_) {
    git_repository_service_.MarkStale();
    return;
  }
  next_automatic_git_sidebar_refresh_ms_ = now_ms + kAutomaticGitRefreshThrottleMs;
  const bool git_sidebar_active =
      context_.current_project_state.sidebar.visible && ActiveSidebarMode() == SidebarMode::Git;
  RequestGitSidebarRefresh(git_sidebar_active ? GitSidebarRefreshScope::Full
                                              : GitSidebarRefreshScope::StatusOnly);
}

bool WorkspaceShell::ConsumePendingGitSidebarRefreshSnapshot(
    GitSidebarState::RefreshSnapshot* snapshot) {
  return git_repository_service_.ConsumePendingSidebarSnapshot(snapshot);
}

void WorkspaceShell::RefreshGitSidebar() {
  RequestGitSidebarRefresh();
}

void WorkspaceShell::ConsumeGitSidebarRefresh() {
  // Drain any completed async compare/ref picker query first (its wake reuses
  // this event), then publish any completed background commit, then rebuild the
  // sidebar to reflect the new state.
  compare_picker_mailbox_.Drain();
  commit_workflow_service_.DrainCompletions();
  MakeSidebarService().RefreshGit();
}

bool WorkspaceShell::RefreshProblemsSidebar() {
  return MakeSidebarService().RefreshProblems();
}

bool WorkspaceShell::RefreshTestsSidebar() {
  return MakeSidebarService().RefreshTests();
}

bool WorkspaceShell::RefreshTestsSidebarState() {
  const std::string previous_id =
      context_.current_project_state.sidebar.tests.selected_index <
              context_.current_project_state.sidebar.tests.entries.size()
          ? context_.current_project_state.sidebar.tests
                .entries[context_.current_project_state.sidebar.tests.selected_index]
                .id
          : std::string{};

  context_.current_project_state.sidebar.tests.entries.clear();
  context_.current_project_state.sidebar.tests.error.clear();
  const std::size_t item_count = test_controller_.TestItems().size();
  context_.current_project_state.sidebar.tests.entries.reserve(item_count);
  for (const TestItem& item : test_controller_.TestItems()) {
    std::string status = "queued";
    // O(1) latest-status lookup instead of allocating + scanning the full result
    // history per row (a 10k-test discovery previously did that every rebuild).
    if (const TestResult* latest = test_controller_.LatestTestResult(item.id);
        latest != nullptr) {
      switch (latest->state) {
        case TestResultState::Queued:
          status = "queued";
          break;
        case TestResultState::InProgress:
          status = "running";
          break;
        case TestResultState::Passed:
          status = "passed";
          break;
        case TestResultState::Failed:
          status = "failed";
          break;
        case TestResultState::Skipped:
          status = "skipped";
          break;
        case TestResultState::Errored:
          status = "errored";
          break;
      }
    }
    context_.current_project_state.sidebar.tests.entries.push_back(TestsSidebarEntry{
        .id = item.id,
        .label = item.label,
        .file = item.file,
        .line = item.line,
        .parent_id = item.parent_id,
        .status = status,
    });
  }

  if (!previous_id.empty()) {
    for (std::size_t i = 0; i < context_.current_project_state.sidebar.tests.entries.size(); ++i) {
      if (context_.current_project_state.sidebar.tests.entries[i].id == previous_id) {
        context_.current_project_state.sidebar.tests.selected_index = i;
        break;
      }
    }
  }
  if (!context_.current_project_state.sidebar.tests.entries.empty()) {
    context_.current_project_state.sidebar.tests.selected_index =
        std::min(context_.current_project_state.sidebar.tests.selected_index,
                 context_.current_project_state.sidebar.tests.entries.size() - 1);
  } else {
    context_.current_project_state.sidebar.tests.selected_index = 0;
  }

  RevealSelectedTestsSidebarLine();
  if (context_.current_project_state.sidebar.visible &&
      ActiveSidebarMode() == SidebarMode::Tests) {
    RequestSidebarRedraw();
  }
  return !context_.current_project_state.sidebar.tests.entries.empty();
}

bool WorkspaceShell::RefreshPluginSidebar() {
  return MakeSidebarService().RefreshPlugin();
}

void WorkspaceShell::RevealSelectedGitSidebarLine() {
  MakeSidebarService().RevealSelectedGitLine();
}

void WorkspaceShell::RevealSelectedProblemsSidebarLine() {
  MakeSidebarService().RevealSelectedProblemsLine();
}

void WorkspaceShell::RevealSelectedTestsSidebarLine() {
  MakeSidebarService().RevealSelectedTestsLine();
}

void WorkspaceShell::RevealSelectedTreeSidebarLine() {
  MakeSidebarService().RevealSelectedTreeLine();
}

void WorkspaceShell::RevealSelectedPluginSidebarLine() {
  MakeSidebarService().RevealSelectedPluginLine();
}

void WorkspaceShell::MoveGitSidebarSelection(int delta) {
  MakeSidebarService().MoveGitSelection(delta);
}

void WorkspaceShell::MoveProblemsSidebarSelection(int delta) {
  MakeSidebarService().MoveProblemsSelection(delta);
}

void WorkspaceShell::MoveTestsSidebarSelection(int delta) {
  MakeSidebarService().MoveTestsSelection(delta);
}

void WorkspaceShell::MovePluginSidebarSelection(int delta) {
  MakeSidebarService().MovePluginSelection(delta);
}

bool WorkspaceShell::OpenGitSidebarEntry(std::size_t entry_index) {
  return MakeSidebarService().OpenGitEntry(entry_index);
}

bool WorkspaceShell::OpenSelectedProblemSidebarItem() {
  return MakeSidebarService().OpenProblemItem();
}

bool WorkspaceShell::OpenSelectedTestSidebarItem() {
  return MakeSidebarService().OpenTestItem();
}

bool WorkspaceShell::OpenSelectedPluginSidebarItem() {
  return MakeSidebarService().OpenPluginItem();
}

bool WorkspaceShell::ToggleSelectedPluginSidebarItem() {
  return MakeSidebarService().TogglePluginItem();
}

bool WorkspaceShell::RunSelectedTestSidebarItem() {
  return MakeSidebarService().RunTestItem();
}

bool WorkspaceShell::CanStageAllGitSidebarEntries() const {
  return const_cast<WorkspaceShell*>(this)->MakeSidebarService().CanStageAllGitEntries();
}

bool WorkspaceShell::CanOpenGitCommitButton() const {
  const GitSidebarViewModel& view_model =
      workspace::CachedGitSidebarPresentation(context_.current_project_state.sidebar.git,
                                              context_.current_project_state.root,
                                              context_.current_project_state.branch_review)
          .view_model;
  return view_model.show_commit_button && view_model.commit_ready;
}

bool WorkspaceShell::CanDiscardAllGitSidebarEntries() const {
  return const_cast<WorkspaceShell*>(this)->MakeSidebarService().CanDiscardAllGitEntries();
}

bool WorkspaceShell::StageAllGitSidebarEntries() {
  return MakeSidebarService().StageAllGitEntries();
}

void WorkspaceShell::OpenDiscardAllGitSidebarPrompt() {
  MakeSidebarService().OpenDiscardAllGitPrompt();
}

bool WorkspaceShell::DiscardAllGitSidebarEntries() {
  return MakeSidebarService().DiscardAllGitEntries();
}

bool WorkspaceShell::StageGitSidebarEntry(std::size_t entry_index) {
  return MakeSidebarService().StageGitEntry(entry_index);
}

bool WorkspaceShell::UnstageGitSidebarEntry(std::size_t entry_index) {
  return MakeSidebarService().UnstageGitEntry(entry_index);
}

bool WorkspaceShell::DiscardGitSidebarEntry(std::size_t entry_index,
                                           const std::optional<std::filesystem::path>& expected_path) {
  return MakeSidebarService().DiscardGitEntry(entry_index, expected_path);
}

bool WorkspaceShell::DispatchGitSidebarAction(GitSidebarActionId action, std::size_t entry_index) {
  return MakeSidebarService().DispatchGitSidebarAction(action, entry_index);
}

void WorkspaceShell::ReconcileOpenTabsAfterPathDiscard(const std::filesystem::path& path) {
  MakeSidebarService().ReconcileOpenTabsAfterPathDiscard(path);
}

}  // namespace microide::workspace
