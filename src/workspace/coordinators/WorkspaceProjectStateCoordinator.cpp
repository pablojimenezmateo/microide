#include "workspace/coordinators/SelectionAutoscroll.h"
#include "workspace/shell/WorkspaceShell.h"

#include "workspace/services/TerminalPanelService.h"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "project/ProjectChangeNormalizer.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/Waker.h"
#include "util/StringUtil.h"
#include "platform/AppDirectories.h"
#include "util/BackgroundTaskCounter.h"
#include "util/StartupTrace.h"
#include "workspace/SettingFlags.h"
#include "workspace/coordinators/WorkspaceMenuCoordinator.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"
#include "workspace/WorkspaceProjectPresentation.h"
#include "workspace/services/SidebarService.h"

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

}

bool WorkspaceShell::StartFileIndexWatcherForCurrentProject() {
  util::PerformanceTrace::ScopeLabel perf_label(
      "WorkspaceShell::StartFileIndexWatcherForCurrentProject");
  if (!context_.current_project_state.root.empty()) {
    perf_label.Field("root", context_.current_project_state.root);
  }
  util::PerformanceTrace::Scope perf_scope(perf_label.View());
  const bool has_tree = !context_.current_project_state.root.empty() && !context_.current_project_state.root_missing;
  StopFileIndexWatcher();
  util::AddPerformanceCounter(util::PerfCounterId::FileIndexWatcherStarts);
  const std::uint64_t watcher_generation =
      file_index_watcher_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;

  project_index_truncated_notice_pending_.store(false, std::memory_order_relaxed);
  notification_service_.DismissKey(kProjectIndexTruncatedNotificationKey);
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
      applied_to_index = context_.current_project_state.file_index.ApplyBatch(
          batch, [this, watcher_generation]() {
            return file_index_watcher_generation_.load(std::memory_order_acquire) !=
                   watcher_generation;
          });
    }
    LogProjectIndexBatch(context_.current_project_state.root, batch, applied_to_index);
    if (batch.tree_structure_changed) {
      util::AddPerformanceCounter(util::PerfCounterId::FileWatcherTreeShapeBatches);
    }
    if (batch.tree_structure_changed || (!batch.is_initial && !batch.changes.empty())) {
      project::ProjectChangeBatch normalized =
          project::NormalizeIndexUpdateBatch(context_.current_project_state.root, batch);
      project_change_coalescer_.Ingest(std::move(normalized));
    }
    if (!batch.is_initial && applied_to_index) {
      file_index_has_pending_changes_.store(true, std::memory_order_release);
    }
    if (batch.is_initial && batch.truncated) {
      project_index_truncated_notice_pending_.store(true, std::memory_order_release);
    }
    if (batch.is_initial &&
        file_index_initial_build_in_flight_.exchange(false, std::memory_order_acq_rel)) {
      util::DecrementBackgroundTaskCountAndWake();
    }
    if (project_file_event_type_ != 0 &&
        (batch.is_initial || applied_to_index || batch.tree_structure_changed ||
         project_index_truncated_notice_pending_.load(std::memory_order_acquire)) &&
        !project_file_event_pending_.exchange(true, std::memory_order_acq_rel)) {
      util::PerformanceTrace::Scope scope(
          "WorkspaceShell::FileIndexWatcherCallback::PushWakeEvent");
      if (!util::PushWake(project_file_event_type_)) {
        project_file_event_pending_.store(false, std::memory_order_release);
      }
    }
  });

  bool watching_tree = false;
  if (has_tree) {
    util::IncrementBackgroundTaskCount();
    file_index_initial_build_in_flight_.store(true, std::memory_order_release);
    watching_tree = file_index_watcher_->Watch(context_.current_project_state.root);
  }
  if (!watching_tree) {
    if (file_index_initial_build_in_flight_.exchange(false, std::memory_order_acq_rel)) {
      util::DecrementBackgroundTaskCountAndWake();
    }
  }
  file_index_watcher_->WatchOpenFiles(
      [this, watcher_generation, root = context_.current_project_state.root](
          std::vector<std::filesystem::path> paths) {
        if (file_index_watcher_generation_.load(std::memory_order_acquire) != watcher_generation) {
          return;
        }
        project_change_coalescer_.Ingest(project::NormalizeOpenFileChanges(root, paths));
        if (project_file_event_type_ != 0 &&
            !project_file_event_pending_.exchange(true, std::memory_order_acq_rel) &&
            !util::PushWake(project_file_event_type_)) {
          project_file_event_pending_.store(false, std::memory_order_release);
        }
      });
  return watching_tree;
}

void WorkspaceShell::StopFileIndexWatcher() {
  file_index_watcher_generation_.fetch_add(1, std::memory_order_acq_rel);
  if (file_index_watcher_ != nullptr) {
    file_index_watcher_->Unwatch();
    file_index_watcher_.reset();
  }
  file_index_has_pending_changes_.store(false, std::memory_order_relaxed);
  project_file_event_pending_.store(false, std::memory_order_relaxed);
  project_index_truncated_notice_pending_.store(false, std::memory_order_relaxed);
  notification_service_.DismissKey(kProjectIndexTruncatedNotificationKey);
  const bool had_initial_build = file_index_initial_build_in_flight_.exchange(
      false, std::memory_order_acq_rel);
  if (had_initial_build) {
    util::DecrementBackgroundTaskCountAndWake();
  }
}

void WorkspaceShell::AppendForcedProjectRescanChanges(project::ProjectChangeBatch& batch) {
  const std::filesystem::path& root = context_.current_project_state.root;
  if (root.empty()) {
    return;
  }
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::AppendForcedProjectRescanChanges");
  util::AddPerformanceCounter(util::PerfCounterId::FileIndexForcedRescans);
  project::ProjectFileScanStatus status;
  std::vector<project::ProjectFile> files = project::FileIndex::ScanFiles(
      root, context_.current_project_state.file_index.FollowOutOfRootSymlinks(),
      ParseExcludeGlobs(GetSettingValue("project.files_exclude").value_or(std::string())),
      &status);
  platform::IndexUpdateBatch rescanned;
  if (!context_.current_project_state.file_index.ReplaceScannedFilesReportingChanges(
          std::move(files), status, &rescanned.changes)) {
    return;
  }
  project::ProjectChangeBatch normalized = project::NormalizeIndexUpdateBatch(root, rescanned);
  batch.file_changes.insert(batch.file_changes.end(),
                            std::make_move_iterator(normalized.file_changes.begin()),
                            std::make_move_iterator(normalized.file_changes.end()));
  batch.tree_rescan_requested = true;
}

void WorkspaceShell::RequestFileIndexRefresh() {
  const std::filesystem::path root = context_.current_project_state.root;
  if (root.empty()) {
    return;
  }
  util::AddPerformanceCounter(util::PerfCounterId::FileIndexRefreshRequests);
  const bool follow = context_.current_project_state.file_index.FollowOutOfRootSymlinks();
  std::vector<std::string> excludes =
      ParseExcludeGlobs(GetSettingValue("project.files_exclude").value_or(std::string()));
  const std::uint64_t version_at_dispatch = context_.current_project_state.file_index.version();
  project_background_executor_.PostLatest(
      "file-index-refresh",
      [this, root, follow, version_at_dispatch, excludes = std::move(excludes)]() {
        project::ProjectFileScanStatus status;
        std::vector<project::ProjectFile> files =
            project::FileIndex::ScanFiles(root, follow, excludes, &status);
        file_index_refresh_mailbox_.Post(
            [this, root, files = std::move(files), status, version_at_dispatch]() mutable {
              ApplyForcedFileIndexRefresh(root, std::move(files), status, version_at_dispatch);
            });
      });
}

void WorkspaceShell::ApplyForcedFileIndexRefresh(const std::filesystem::path& root,
                                                 std::vector<project::ProjectFile> files,
                                                 project::ProjectFileScanStatus status,
                                                 std::uint64_t index_version_at_dispatch) {
  if (root != context_.current_project_state.root) {
    return;
  }
  if (context_.current_project_state.file_index.version() != index_version_at_dispatch) {
    util::AddPerformanceCounter(util::PerfCounterId::FileIndexScanSupersededByBatch);
    RequestFileIndexRefresh();
    return;
  }
  context_.current_project_state.file_index.ReplaceScannedFiles(std::move(files), status);
  RefreshProjectFileViews();
}

void WorkspaceShell::RebindProjectState(ProjectWorkspaceState& state) {
  WorkspaceContext::RebindProjectState(state);
  settings_store_.BindActiveProject(&context_.current_project_state.settings);
  MakePersistenceCoordinator().MaterializeCanonicalPreferences();
}

void WorkspaceShell::ClearDragState() {
  context_.interaction_state.drag_target = DragTarget::None;
  context_.interaction_state.drag_scrollbar_offset = 0.0f;
  context_.interaction_state.drag_editor_split_node = 0;
  context_.interaction_state.drag_editor_split_boundary = 0;
}

void WorkspaceShell::ResetTransientInteractionState() {
  ClearDragState();
  context_.interaction_state.mouse_selecting = false;
  context_.interaction_state.text_drag = InteractionState::TextDragState::None;
  context_.interaction_state.text_drag_has_drop = false;
  context_.interaction_state.editor_box_selecting = false;
  context_.interaction_state.tab_drag = TabDragState{};
  context_.interaction_state.tab_slide = TabSlideState{};
  selection_autoscroll::Disarm(context_.interaction_state);
}

void WorkspaceShell::ResetCurrentProjectStateStorage() {
  context_.ResetCurrentProjectStateStorage();
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
  context_.current_project_state.focused_group().welcome_surface.viewport.SetPlaceholderText(" ");
  ApplyEditorPreferences(context_.current_project_state.focused_group().welcome_surface.viewport);
}

void WorkspaceShell::ResetProjectScopedState(bool show_welcome) {
  auto& persistence = MakePersistenceCoordinator();
  StopProjectSearch();
  project_background_executor_.Cancel();
  git_repository_service_.Reset();
  context_.current_project_state.sidebar.git.refreshing = false;
  pending_tree_git_badge_refresh_after_paint_ = false;
  StopFileIndexWatcher();
  context_.current_project_state.file_index.Reset();
  project_change_coalescer_.Reset();
  git_metadata_tracker_.Reset();
  last_applied_project_change_generation_ = 0;
  pending_external_change_confirms_.clear();
  file_read_service_.CancelAllAndFlush();
  MakeMenuCoordinator().CloseTreeContextMenu();
  ClearEditorBlame();
  lsp_service_.RetireCurrentProjectServers();

  ResetCurrentProjectStateStorage();

  context_.current_project_state.sidebar.visible = !show_welcome;
  context_.current_project_state.surface.focus = show_welcome ? FocusTarget::Editor : FocusTarget::Sidebar;
  persistence.ApplyColorscheme(context_.current_project_state.active_colorscheme_name, false, false);
  ApplyEditorPreferences(context_.current_project_state.focused_group().welcome_surface.viewport);
  if (show_welcome) {
    SetWelcomePlaceholder();
  }
}

bool WorkspaceShell::InitializeCurrentProject(const project::ProjectId& id,
                                              const std::filesystem::path& project_root,
                                              bool restore_persistence,
                                              bool log_feedback,
                                              bool activate_restored_tab,
                                              bool root_missing,
                                              project::ProjectLocality locality) {
  (void) log_feedback;
  auto& persistence = MakePersistenceCoordinator();
  util::StartupTrace::Scope trace_scope("WorkspaceShell::InitializeCurrentProject");
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::InitializeCurrentProject");
  {
    util::PerformanceTrace::Scope scope(
        "WorkspaceShell::InitializeCurrentProject::ResetProjectScopedState");
    ResetProjectScopedState(false);
  }
  context_.current_project_state.id = id;
  context_.current_project_state.locality = locality;
  context_.current_project_state.root_missing = root_missing;
  if (root_missing) {
    context_.current_project_state.root = project_root.lexically_normal();
    context_.current_project_state.editor_config.SetProjectRoot(context_.current_project_state.root);
  } else {
    util::StartupTrace::Scope set_root_scope("WorkspaceShell::SetProjectRoot");
    util::PerformanceTrace::Scope perf_set_root_scope(
        "WorkspaceShell::InitializeCurrentProject::SetProjectRoot");
    if (!SetProjectRoot(project_root)) {
      return false;
    }
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
  if (root_missing) {
    Notify(NotificationService::Request{
        .tone = NotificationService::Tone::Warning,
        .key = "project-root-missing:" + context_.current_project_state.root.generic_string(),
        .message = "The folder " + context_.current_project_state.root.generic_string() +
                   " is gone. Its unsaved buffers are open here: Save As each one to keep it, "
                   "or close the project. It opens as usual once the folder is back.",
        .sticky = true,
    });
  }
  if (restore_persistence && restored_session) {
    if (context_.current_project_state.terminal_tabs.empty() && !root_missing) {
      util::PerformanceTrace::Scope scope(
          "WorkspaceShell::InitializeCurrentProject::OpenDefaultTerminal");
      MakeTerminalPanelService().OpenTerminalsForProjectInit();
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
      MakeSidebarService().RevealSelectedTreeLine();
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
      context_.current_project_state.surface.focus = FocusTarget::Editor;
      if (context_.current_project_state.terminal_tabs.empty()) {
        util::PerformanceTrace::Scope scope(
            "WorkspaceShell::InitializeCurrentProject::OpenDefaultTerminal");
        MakeTerminalPanelService().OpenTerminalsForProjectInit();
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
  context_.current_project_state.surface.focus = FocusTarget::Editor;
  if (context_.current_project_state.terminal_tabs.empty() && !root_missing) {
    util::PerformanceTrace::Scope scope(
        "WorkspaceShell::InitializeCurrentProject::OpenDefaultTerminal");
    MakeTerminalPanelService().OpenTerminalsForProjectInit();
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
  context_.current_project_state.editor_config.SetProjectRoot(
      context_.current_project_state.root);
  const bool follow_out_of_root_symlinks =
      SettingFlagEnabled(GetSettingValue("project.follow_out_of_root_symlinks"), false);
  context_.current_project_state.directory_tree.SetFollowOutOfRootSymlinks(
      follow_out_of_root_symlinks);
  context_.current_project_state.file_index.SetFollowOutOfRootSymlinks(
      follow_out_of_root_symlinks);
  std::string files_exclude = GetSettingValue("project.files_exclude").value_or(std::string());
  std::vector<std::string> exclude_globs = ParseExcludeGlobs(files_exclude);
  context_.current_project_state.directory_tree.SetExcludeGlobs(exclude_globs);
  context_.current_project_state.file_index.SetExcludeGlobs(exclude_globs);
  last_applied_follow_out_of_root_symlinks_ = follow_out_of_root_symlinks;
  last_applied_files_exclude_ = std::move(files_exclude);
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
    util::StartupTrace::Scope git_metadata_scope("GitMetadataTracker::SetProjectRoot");
    git_metadata_tracker_.SetProjectRoot(context_.current_project_state.root);
  }

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

}
