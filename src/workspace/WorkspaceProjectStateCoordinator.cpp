#include "workspace/WorkspaceShell.h"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "project/ProjectChangeNormalizer.h"
#include "util/PerformanceTrace.h"
#include "util/SdlWake.h"
#include "util/StringUtil.h"
#include "platform/AppDirectories.h"
#include "app/BackgroundTaskCounter.h"
#include "util/StartupTrace.h"
#include "workspace/SettingFlags.h"
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
  file_index_watcher_->SetExcludeGlobs(
      ParseExcludeGlobs(GetSettingValue("project.files_exclude").value_or(std::string())));
  file_index_watcher_->SetCallback([this, watcher_generation](platform::IndexUpdateBatch batch) {
    if (file_index_watcher_generation_.load(std::memory_order_acquire) != watcher_generation) {
      return;
    }
    bool applied_to_index = false;
    {
      util::PerformanceTrace::Scope scope(
          "WorkspaceShell::FileIndexWatcherCallback::ApplyBatch");
      // Abandon an in-flight initial bulk load if this watcher has since been
      // retired (StopFileIndexWatcher bumps the generation before joining), so
      // teardown/switch-away does not block ~1s on indexing a large tree.
      applied_to_index = context_.current_project_state.file_index.ApplyBatch(
          batch, [this, watcher_generation]() {
            return file_index_watcher_generation_.load(std::memory_order_acquire) !=
                   watcher_generation;
          });
    }
    LogProjectIndexBatch(context_.current_project_state.root, batch, applied_to_index);
    if (!batch.is_initial && !batch.changes.empty()) {
      project::ProjectChangeBatch normalized =
          project::NormalizeIndexUpdateBatch(context_.current_project_state.root, batch);
      project_change_coalescer_.Ingest(std::move(normalized));
    }
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
      util::PerformanceTrace::Scope scope(
          "WorkspaceShell::FileIndexWatcherCallback::PushWakeEvent");
      // TD-2026-07-17-087: route through util::PushSdlWake so a rejected push latches
      // the owed-wake bit the idle-wait poll consumes. Otherwise the flag stays set
      // with no event queued, every later batch's exchange sees `true` and skips the
      // push, and the file index is never drained/redrawn until an unrelated event
      // wakes the loop. Clear the local flag on failure so a later producer retries.
      if (!util::PushSdlWake(project_file_event_type_)) {
        project_file_event_pending_.store(false, std::memory_order_release);
      }
    }
  });

  app::IncrementBackgroundTaskCount();
  file_index_initial_build_in_flight_.store(true, std::memory_order_release);
  if (!file_index_watcher_->Watch(context_.current_project_state.root)) {
    file_index_watcher_.reset();
    // Guard the decrement with the same exchange used by the batch callback and
    // StopFileIndexWatcher: if the initial-build batch already fired and consumed
    // the in-flight flag, it already decremented, so decrementing again here would
    // underflow the background-task counter.
    if (file_index_initial_build_in_flight_.exchange(false, std::memory_order_acq_rel)) {
      app::DecrementBackgroundTaskCountAndWake();
    }
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

void WorkspaceShell::RequestFileIndexRefresh() {
  const std::filesystem::path root = context_.current_project_state.root;
  if (root.empty()) {
    return;
  }
  // Capture the scan inputs by value so the background task never dereferences
  // the FileIndex (which a project switch may destroy while the scan is in
  // flight); ScanFiles touches only its arguments.
  const bool follow = context_.current_project_state.file_index.FollowOutOfRootSymlinks();
  std::vector<std::string> excludes =
      ParseExcludeGlobs(GetSettingValue("project.files_exclude").value_or(std::string()));
  // PostLatest by a fixed key so a burst of refresh requests only runs the newest
  // queued scan (an in-flight one still finishes; its result is simply superseded).
  project_background_executor_.PostLatest(
      "file-index-refresh",
      [this, root, follow, excludes = std::move(excludes)]() {
        project::ProjectFileScanStatus status;
        std::vector<project::ProjectFile> files =
            project::FileIndex::ScanFiles(root, follow, excludes, &status);
        file_index_refresh_mailbox_.Post(
            [this, root, files = std::move(files), status]() mutable {
              ApplyForcedFileIndexRefresh(root, std::move(files), status);
            });
      });
}

void WorkspaceShell::ApplyForcedFileIndexRefresh(const std::filesystem::path& root,
                                                 std::vector<project::ProjectFile> files,
                                                 project::ProjectFileScanStatus status) {
  // Drop a scan whose project has since been switched away — the current index
  // now belongs to a different project (or was reset), so applying an old root's
  // file list would corrupt it. Mirrors the generation guard on other async paths.
  if (root != context_.current_project_state.root) {
    return;
  }
  context_.current_project_state.file_index.ReplaceScannedFiles(std::move(files), status);
  context_.current_project_state.file_finder.InvalidateIndexCache();
  // A changed file set makes cached project-search results stale; drop the cache
  // marker so re-opening Search re-runs, and refresh live if Search/Finder is open.
  context_.current_project_state.overlay.workflow.project_search.searched_query.clear();
  if (ActiveSidebarMode() == SidebarMode::Search &&
      !context_.current_project_state.overlay.workflow.project_search.query.text().empty()) {
    RefreshProjectSearch();
  }
  if (context_.current_project_state.overlay.visible &&
      context_.current_project_state.overlay.mode == OverlayMode::FileFinder) {
    context_.current_project_state.file_finder.Refresh();
  }
  RequestWindowRedraw();
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
  // The active project's settings vector may have just been moved-into; re-point
  // the store at the live vector and rebuild its resolved index.
  settings_store_.BindActiveProject(&context_.current_project_state.settings);
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
  // The reset replaced current_project_state with a fresh instance, so the old
  // settings-vector pointer dangles; re-bind the store to the new vector.
  settings_store_.BindActiveProject(&context_.current_project_state.settings);
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
  // The placeholder text itself is never rendered — an empty group draws the welcome view
  // model (DrawPlaceholderView) instead. This call only flips the viewport's is_placeholder()
  // flag, which selects that draw path; a single marker char is enough to set it.
  context_.current_project_state.focused_group().welcome_surface.viewport.SetPlaceholderText(" ");
  ApplyEditorPreferences(context_.current_project_state.focused_group().welcome_surface.viewport);
}

void WorkspaceShell::ResetProjectScopedState(bool show_welcome) {
  auto persistence = MakePersistenceCoordinator();
  StopProjectSearch();
  project_background_executor_.Cancel();
  git_repository_service_.Reset();
  context_.current_project_state.sidebar.git.refreshing = false;
  pending_tree_git_badge_refresh_after_paint_ = false;
  StopFileIndexWatcher();
  context_.current_project_state.file_index.Reset();
  project_file_monitor_.Reset();
  project_change_coalescer_.Reset();
  git_metadata_tracker_.Reset();
  last_applied_project_change_generation_ = 0;
  MakeMenuCoordinator().CloseTreeContextMenu();
  ClearEditorBlame();
  // Hand the retiring LSP clients to the host-owned pool BEFORE the project state
  // (and its LspManager) is destroyed, so ~LspManager does not block the shell thread
  // on their shutdown handshake (TD-2026-07-17-091).
  lsp_service_.RetireCurrentProjectServers();

  ResetCurrentProjectStateStorage();

  context_.current_project_state.sidebar.visible = !show_welcome;
  context_.current_project_state.surface.focus = show_welcome ? FocusTarget::Editor : FocusTarget::Sidebar;
  context_.interaction_state.tab_drag = TabDragState{};
  persistence.ApplyColorscheme(context_.current_project_state.active_colorscheme_name, false, false);
  ApplyEditorPreferences(context_.current_project_state.focused_group().welcome_surface.viewport);
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
    ApplyEditorPreferences(context_.current_project_state.focused_group().welcome_surface.viewport);
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
      OpenDefaultTerminalForProjectInit();
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
      // Open the restored buffers' LSP documents here, AFTER this reload registers
      // the plugin-contributed language servers. The earlier tab activation
      // (ActivateCurrentTabAfterStateLoad -> NotifyLspBufferOpen) runs before any
      // server is registered, so it finds no client and cannot send didOpen — a
      // session-restored file would otherwise start clangd (via the status query)
      // but never open the document, leaving it with no diagnostics/semantic tokens
      // until the user interacted. EnsureLspDocumentOpen is idempotent.
      ReloadPluginsForCurrentProject(PluginReloadRequest{
          .syntax_definitions = false,
          .replay_buffer_opens = false,
          .open_lsp_documents = true,
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
    std::error_code exists_ec;
    if (std::filesystem::exists(candidate, exists_ec) && !exists_ec &&
        startup_view.OpenFile(candidate)) {
      ApplyEditorPreferences(startup_view);
      ApplyDetectedIndentOnOpen(startup_view);
      context_.current_project_state.focused_group().welcome_surface.viewport = startup_view;
      context_.current_project_state.directory_tree.SelectPath(candidate);
      RevealSelectedTreeSidebarLine();
      context_.current_project_state.focused_group().open_tabs.push_back(TabEntry{
          .kind = TabEntry::Kind::Editor,
          .path = candidate,
          .title = candidate.filename().string(),
          .editor_state = MakeEditorTabState(startup_view),
          .deferred_handle = std::nullopt,
          .compare = std::nullopt,
          .merge = std::nullopt,
      });
      context_.current_project_state.focused_group().active_tab_index = 0;
      if (context_.current_project_state.terminal_tabs.empty()) {
        util::PerformanceTrace::Scope scope(
            "WorkspaceShell::InitializeCurrentProject::OpenDefaultTerminal");
        OpenDefaultTerminalForProjectInit();
      }
      {
        util::PerformanceTrace::Scope scope(
            "WorkspaceShell::InitializeCurrentProject::ReloadPluginsForCurrentProject");
        ReloadPluginsForCurrentProject();
      }
      return true;
    }
  }

  context_.current_project_state.focused_group().welcome_surface.viewport.SetPlaceholderText(
      "microide\n\n"
      "Project loaded.\n"
      "Use the sidebar to open files.\n");
  ApplyEditorPreferences(context_.current_project_state.focused_group().welcome_surface.viewport);
  if (context_.current_project_state.terminal_tabs.empty()) {
    util::PerformanceTrace::Scope scope(
        "WorkspaceShell::InitializeCurrentProject::OpenDefaultTerminal");
    OpenDefaultTerminalForProjectInit();
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
  const bool follow_out_of_root_symlinks =
      SettingFlagEnabled(GetSettingValue("project.follow_out_of_root_symlinks"), false);
  context_.current_project_state.directory_tree.SetFollowOutOfRootSymlinks(
      follow_out_of_root_symlinks);
  context_.current_project_state.file_index.SetFollowOutOfRootSymlinks(
      follow_out_of_root_symlinks);
  std::vector<std::string> exclude_globs =
      ParseExcludeGlobs(GetSettingValue("project.files_exclude").value_or(std::string()));
  context_.current_project_state.directory_tree.SetExcludeGlobs(exclude_globs);
  context_.current_project_state.file_index.SetExcludeGlobs(exclude_globs);
  project_file_monitor_.SetExcludeGlobs(exclude_globs);
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
    git_metadata_tracker_.SetProjectRoot(context_.current_project_state.root);
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
