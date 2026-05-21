#include "workspace/WorkspaceShell.h"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "util/PerformanceTrace.h"
#include "platform/AppDirectories.h"
#include "app/BackgroundTaskCounter.h"
#include "util/StartupTrace.h"
#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspacePersistenceCoordinator.h"
#include "workspace/WorkspaceProjectPresentation.h"

namespace microide::workspace {

namespace {

bool TraceProjectEventsEnabled() {
  static const bool enabled =
      util::PerformanceTrace::FlagEnabled("MICROIDE_TRACE_PROJECT_EVENTS");
  return enabled;
}

void LogProjectIndexBatch(const std::filesystem::path& root,
                          const platform::IndexUpdateBatch& batch,
                          bool applied_to_index) {
  if (!TraceProjectEventsEnabled()) {
    return;
  }

  const std::string root_text = root.string();
  const std::string first_path =
      batch.changes.empty() ? std::string{} : batch.changes.front().entry.relative_path.string();
  SDL_Log("microide project: file-index-batch root=%s initial=%d changes=%zu applied=%d first=%s",
          root_text.c_str(), batch.is_initial ? 1 : 0, batch.changes.size(),
          applied_to_index ? 1 : 0,
          first_path.empty() ? "-" : first_path.c_str());
}

}  // namespace

bool WorkspaceShell::StartFileIndexWatcherForCurrentProject() {
  std::string perf_label = "WorkspaceShell::StartFileIndexWatcherForCurrentProject";
  if (util::PerformanceTrace::Enabled() && !context_.current_project_state.root.empty()) {
    perf_label += "(root=" + context_.current_project_state.root.string() + ")";
  }
  util::PerformanceTrace::Scope perf_scope(perf_label);
  if (context_.current_project_state.root.empty()) {
    return false;
  }
  StopFileIndexWatcher();
  const std::uint64_t watcher_generation =
      file_index_watcher_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;

  file_index_watcher_ = std::make_unique<platform::FileIndexWatcher>();
  file_index_watcher_->SetCallback([this, watcher_generation](platform::IndexUpdateBatch batch) {
    if (file_index_watcher_generation_.load(std::memory_order_acquire) != watcher_generation) {
      return;
    }
    bool applied_to_index = false;
    {
      util::PerformanceTrace::Scope scope(
          "WorkspaceShell::FileIndexWatcherCallback::ApplyBatch");
      applied_to_index = context_.current_project_state.file_index.ApplyBatch(batch);
    }
    LogProjectIndexBatch(context_.current_project_state.root, batch, applied_to_index);
    if (batch.is_initial) {
      project_background_executor_.PostLatest(
          "project-file-monitor-arm",
          [this]() { project_file_monitor_.ArmPendingWatch(); });
    }
    if (!batch.is_initial && applied_to_index) {
      file_index_has_pending_changes_.store(true, std::memory_order_release);
    }
    if (batch.is_initial &&
        file_index_initial_build_in_flight_.exchange(false, std::memory_order_acq_rel)) {
      app::DecrementBackgroundTaskCountAndWake();
    }
    if (project_file_event_type_ != 0 && (batch.is_initial || applied_to_index) &&
        !project_file_event_pending_.exchange(true, std::memory_order_acq_rel)) {
      SDL_Event event{};
      event.type = project_file_event_type_;
      {
        util::PerformanceTrace::Scope scope(
            "WorkspaceShell::FileIndexWatcherCallback::PushWakeEvent");
        SDL_PushEvent(&event);
      }
    }
  });

  app::IncrementBackgroundTaskCount();
  file_index_initial_build_in_flight_.store(true, std::memory_order_release);
  if (!file_index_watcher_->Watch(context_.current_project_state.root)) {
    file_index_watcher_.reset();
    file_index_initial_build_in_flight_.store(false, std::memory_order_release);
    app::DecrementBackgroundTaskCountAndWake();
    project_background_executor_.PostLatest(
        "project-file-monitor-arm",
        [this]() { project_file_monitor_.ArmPendingWatch(); });
    return false;
  }
  return true;
}

void WorkspaceShell::StopFileIndexWatcher() {
  file_index_watcher_generation_.fetch_add(1, std::memory_order_acq_rel);
  if (file_index_watcher_ != nullptr) {
    file_index_watcher_->Unwatch();
    file_index_watcher_.reset();
  }
  file_index_has_pending_changes_.store(false, std::memory_order_relaxed);
  project_file_event_pending_.store(false, std::memory_order_relaxed);
  const bool had_initial_build = file_index_initial_build_in_flight_.exchange(
      false, std::memory_order_acq_rel);
  if (had_initial_build) {
    app::DecrementBackgroundTaskCountAndWake();
  }
}

bool WorkspaceShell::ConfigureProjectState(ProjectWorkspaceState& state,
                                           const std::filesystem::path& project_root) {
  std::error_code error;
  const auto absolute_root = std::filesystem::absolute(project_root, error);
  if (error || absolute_root.empty()) {
    return false;
  }

  state.root = absolute_root.lexically_normal();
  if (!state.directory_tree.SetRoot(state.root)) {
    return false;
  }
    if (!state.file_index.SetRoot(state.root,
                                  project::FileIndex::RootPopulationMode::Deferred)) {
    return false;
  }
  state.file_finder.SetIndex(&state.file_index);
  return true;
}

void WorkspaceShell::RebindProjectState(ProjectWorkspaceState& state) {
  WorkspaceContext::RebindProjectState(state);
}

void WorkspaceShell::ClearDragState() {
  context_.interaction_state.drag_target = DragTarget::None;
  context_.interaction_state.drag_scrollbar_offset = 0.0f;
  context_.interaction_state.drag_editor_split_path.clear();
  context_.interaction_state.drag_editor_split_divider_index = 0;
}

void WorkspaceShell::ResetTransientInteractionState() {
  ClearDragState();
  context_.interaction_state.mouse_selecting = false;
}

void WorkspaceShell::ResetCurrentProjectStateStorage() {
  context_.ResetCurrentProjectStateStorage();
  ResetTransientInteractionState();
}

std::filesystem::path WorkspaceShell::ResolveProjectRootInput(
    const std::filesystem::path& project_root) const {
  if (project_root.empty()) {
    return {};
  }

  std::error_code error;
  if (project_root.is_absolute()) {
    const auto absolute_root = std::filesystem::absolute(project_root, error);
    return error ? std::filesystem::path{} : absolute_root.lexically_normal();
  }

  const std::filesystem::path base_root =
      context_.current_project_state.root.empty() ? std::filesystem::current_path(error) : context_.current_project_state.root;
  if (error || base_root.empty()) {
    return {};
  }

  const auto absolute_root = std::filesystem::absolute(base_root / project_root, error);
  return error ? std::filesystem::path{} : absolute_root.lexically_normal();
}

void WorkspaceShell::SetWelcomePlaceholder() {
  context_.current_project_state.welcome_surface.viewport.SetPlaceholderText(
      "microide\n\n"
      "Welcome.\n"
      "Use File > New Project Tab... or run project-open.\n");
  ApplyEditorPreferences(context_.current_project_state.welcome_surface.viewport);
}

void WorkspaceShell::ResetProjectScopedState(bool show_welcome) {
  auto persistence = MakePersistenceCoordinator();
  StopProjectSearch();
  project_background_executor_.Cancel();
  {
    std::lock_guard lock(git_sidebar_refresh_mutex_);
    ++git_sidebar_refresh_generation_;
    git_sidebar_refresh_snapshot_.reset();
  }
  context_.current_project_state.sidebar.git.refreshing = false;
  pending_tree_git_badge_refresh_after_paint_ = false;
  StopFileIndexWatcher();
  context_.current_project_state.file_index.Reset();
  project_file_monitor_.Reset();
  MakeMenuCoordinator().CloseTreeContextMenu();
  ClearEditorBlame();
  CurrentLspManager().BeginShutdownAll();

  ResetCurrentProjectStateStorage();

  context_.current_project_state.sidebar.visible = !show_welcome;
  context_.current_project_state.surface.focus = show_welcome ? FocusTarget::Editor : FocusTarget::Sidebar;
  context_.interaction_state.tab_drag = TabDragState{};
  persistence.ApplyColorscheme(context_.current_project_state.active_colorscheme_name, false, false);
  ApplyEditorPreferences(context_.current_project_state.welcome_surface.viewport);
  if (show_welcome) {
    SetWelcomePlaceholder();
  }
}

bool WorkspaceShell::InitializeCurrentProject(const std::filesystem::path& project_root,
                                              bool restore_persistence,
                                              bool log_feedback,
                                              bool activate_restored_tab) {
  (void) log_feedback;
  auto persistence = MakePersistenceCoordinator();
  util::StartupTrace::Scope trace_scope("WorkspaceShell::InitializeCurrentProject");
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::InitializeCurrentProject");
  {
    util::PerformanceTrace::Scope scope(
        "WorkspaceShell::InitializeCurrentProject::ResetProjectScopedState");
    ResetProjectScopedState(false);
  }
  {
    util::StartupTrace::Scope set_root_scope("WorkspaceShell::SetProjectRoot");
    util::PerformanceTrace::Scope perf_set_root_scope(
        "WorkspaceShell::InitializeCurrentProject::SetProjectRoot");
    project_file_monitor_.SetDeferredArming(true);
    if (!SetProjectRoot(project_root)) {
      project_file_monitor_.SetDeferredArming(false);
      return false;
    }
    project_file_monitor_.SetDeferredArming(false);
  }

  context_.current_project_state.project_base_color = DefaultProjectBaseColor(context_.current_project_state.root);

  {
    util::PerformanceTrace::Scope scope(
        "WorkspaceShell::InitializeCurrentProject::ApplyColorscheme");
    persistence.ApplyColorscheme(context_.current_project_state.active_colorscheme_name, false, false);
  }
  {
    util::PerformanceTrace::Scope scope(
        "WorkspaceShell::InitializeCurrentProject::ApplyEditorPreferences");
    ApplyEditorPreferences(context_.current_project_state.welcome_surface.viewport);
  }
  if (restore_persistence) {
    util::PerformanceTrace::Scope scope(
        "WorkspaceShell::InitializeCurrentProject::RestoreConfigState");
    persistence.RestoreConfigState();
  }

  bool restored_session = false;
  if (restore_persistence) {
    util::PerformanceTrace::Scope scope(
        "WorkspaceShell::InitializeCurrentProject::RestoreSessionState");
    restored_session = persistence.RestoreSessionState();
  }
  if (restore_persistence && restored_session) {
    if (context_.current_project_state.terminal_tabs.empty()) {
      util::PerformanceTrace::Scope scope(
          "WorkspaceShell::InitializeCurrentProject::OpenDefaultTerminal");
#ifdef MICROIDE_TESTING
      context_.current_project_state.terminal_tabs.push_back(
          std::make_unique<TerminalTabState>());
      context_.current_project_state.active_terminal_tab_index =
          context_.current_project_state.terminal_tabs.size() - 1;
      context_.current_project_state.panel.content = PanelContentKind::Terminal;
      context_.current_project_state.surface.focus = FocusTarget::Panel;
#else
      OpenTerminal({}, true, false);
#endif
    }
    {
      util::PerformanceTrace::Scope scope(
          "WorkspaceShell::InitializeCurrentProject::ApplyEditorPreferencesToAllTabs");
      ApplyEditorPreferencesToAllTabs();
    }
    if (activate_restored_tab) {
      util::PerformanceTrace::Scope scope(
          "WorkspaceShell::InitializeCurrentProject::ActivateCurrentTabAfterStateLoad");
      ActivateCurrentTabAfterStateLoad();
    }
    {
      util::PerformanceTrace::Scope scope(
          "WorkspaceShell::InitializeCurrentProject::ReloadPluginsForCurrentProject");
      ReloadPluginsForCurrentProject(PluginReloadRequest{
          .syntax_definitions = false,
          .replay_buffer_opens = false,
          .open_lsp_documents = false,
      });
    }
    return true;
  }

  const std::vector<std::filesystem::path> preferred_files = {
      context_.current_project_state.root / "docs" / "implementation-guide.md",
      context_.current_project_state.root / "README.md",
  };

  for (const auto& candidate : preferred_files) {
    editor::TextViewport startup_view;
    if (std::filesystem::exists(candidate) && startup_view.OpenFile(candidate)) {
      ApplyEditorPreferences(startup_view);
      ApplyDetectedIndentOnOpen(startup_view);
      context_.current_project_state.welcome_surface.viewport = startup_view;
      context_.current_project_state.directory_tree.SelectPath(candidate);
      RevealSelectedTreeSidebarLine();
      context_.current_project_state.open_tabs.push_back(TabEntry{
          .kind = TabEntry::Kind::Editor,
          .path = candidate,
          .title = candidate.filename().string(),
          .editor_state = MakeEditorTabState(startup_view),
          .deferred_handle = std::nullopt,
          .compare = std::nullopt,
          .merge = std::nullopt,
      });
      context_.current_project_state.active_tab_index = 0;
      if (context_.current_project_state.terminal_tabs.empty()) {
        util::PerformanceTrace::Scope scope(
            "WorkspaceShell::InitializeCurrentProject::OpenDefaultTerminal");
#ifdef MICROIDE_TESTING
        context_.current_project_state.terminal_tabs.push_back(
            std::make_unique<TerminalTabState>());
        context_.current_project_state.active_terminal_tab_index =
            context_.current_project_state.terminal_tabs.size() - 1;
        context_.current_project_state.panel.content = PanelContentKind::Terminal;
        context_.current_project_state.surface.focus = FocusTarget::Panel;
#else
        OpenTerminal({}, true, false);
#endif
      }
      {
        util::PerformanceTrace::Scope scope(
            "WorkspaceShell::InitializeCurrentProject::ReloadPluginsForCurrentProject");
        ReloadPluginsForCurrentProject();
      }
      return true;
    }
  }

  context_.current_project_state.welcome_surface.viewport.SetPlaceholderText(
      "microide\n\n"
      "Project loaded.\n"
      "Use the sidebar to open files.\n");
  ApplyEditorPreferences(context_.current_project_state.welcome_surface.viewport);
  if (context_.current_project_state.terminal_tabs.empty()) {
    util::PerformanceTrace::Scope scope(
        "WorkspaceShell::InitializeCurrentProject::OpenDefaultTerminal");
#ifdef MICROIDE_TESTING
    context_.current_project_state.terminal_tabs.push_back(
        std::make_unique<TerminalTabState>());
    context_.current_project_state.active_terminal_tab_index =
        context_.current_project_state.terminal_tabs.size() - 1;
    context_.current_project_state.panel.content = PanelContentKind::Terminal;
    context_.current_project_state.surface.focus = FocusTarget::Panel;
#else
    OpenTerminal({}, true, false);
#endif
  }
  {
    util::PerformanceTrace::Scope scope(
        "WorkspaceShell::InitializeCurrentProject::ReloadPluginsForCurrentProject");
    ReloadPluginsForCurrentProject(PluginReloadRequest{
        .syntax_definitions = true,
        .replay_buffer_opens = false,
        .open_lsp_documents = false,
    });
  }
  return true;
}

bool WorkspaceShell::SetProjectRoot(const std::filesystem::path& project_root) {
  util::StartupTrace::Scope trace_scope("WorkspaceShell::SetProjectRoot");
  const std::filesystem::path absolute_root = ResolveProjectRootInput(project_root);
  if (absolute_root.empty()) {
    return false;
  }

  StopProjectSearch();
  context_.current_project_state.root = absolute_root.lexically_normal();
  {
    util::StartupTrace::Scope tree_scope("DirectoryTree::SetRoot");
    if (!context_.current_project_state.directory_tree.SetRoot(context_.current_project_state.root)) {
      return false;
    }
  }
  {
    util::StartupTrace::Scope index_scope("FileIndex::SetRoot");
    StopFileIndexWatcher();
    if (!context_.current_project_state.file_index.SetRoot(
            context_.current_project_state.root,
            project::FileIndex::RootPopulationMode::Deferred)) {
      return false;
    }
    if (!StartFileIndexWatcherForCurrentProject()) {
      return false;
    }
  }
  context_.current_project_state.file_finder.SetIndex(&context_.current_project_state.file_index);
  context_.current_project_state.sidebar.scroll_row = 0;
  if (ActiveSidebarMode() == SidebarMode::Git) {
    RefreshGitSidebar();
  } else {
    context_.current_project_state.sidebar.git = GitSidebarState{};
  }
  RefreshProblemsSidebar();

  {
    util::StartupTrace::Scope monitor_scope("WorkspaceProjectFileMonitor::SetProjectRoot");
    project_file_monitor_.SetProjectRoot(context_.current_project_state.root);
  }
  project_file_monitor_.SetPollInterval(std::chrono::milliseconds(2000));

  if (ActiveSidebarMode() == SidebarMode::Search &&
      !context_.current_project_state.overlay.workflow.project_search.query.text().empty()) {
    RefreshProjectSearch();
  }
  pending_tree_git_badge_refresh_after_paint_ = true;
  return true;
}

std::filesystem::path WorkspaceShell::ConfigStatePath() const {
  return context_.current_project_state.root.empty() ? std::filesystem::path{} : ProjectStateDirectory() / "config";
}

std::filesystem::path WorkspaceShell::UserConfigPath() const {
  const std::filesystem::path config_root =
      platform::ResolveAppDirectory(platform::UserDirectoryKind::Config, "microide");
  return config_root.empty() ? std::filesystem::path{} : config_root / "config";
}

std::filesystem::path WorkspaceShell::ProjectStateDirectory() const {
  return ::microide::workspace::ProjectStateDirectory(context_.current_project_state.root);
}

}  // namespace microide::workspace
