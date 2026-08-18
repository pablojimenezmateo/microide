#include "workspace/coordinators/SelectionAutoscroll.h"
#include "workspace/shell/WorkspaceShell.h"

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
#include "util/SdlWake.h"
#include "util/StringUtil.h"
#include "platform/AppDirectories.h"
#include "app/BackgroundTaskCounter.h"
#include "util/StartupTrace.h"
#include "workspace/SettingFlags.h"
#include "workspace/coordinators/WorkspaceMenuCoordinator.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"
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
  util::PerformanceTrace::ScopeLabel perf_label(
      "WorkspaceShell::StartFileIndexWatcherForCurrentProject");
  if (!context_.current_project_state.root.empty()) {
    perf_label.Field("root", context_.current_project_state.root);
  }
  util::PerformanceTrace::Scope perf_scope(perf_label.View());
  if (context_.current_project_state.root.empty()) {
    return false;
  }
  StopFileIndexWatcher();
  util::AddPerformanceCounter(util::PerfCounterId::FileIndexWatcherStarts);
  const std::uint64_t watcher_generation =
      file_index_watcher_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;

  project_index_truncated_notice_pending_.store(false, std::memory_order_relaxed);
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
    // A truncated initial scan means the index holds only a prefix of the project:
    // the finder and search already say so inline, but it is worth a toast. This is
    // the successor to the project file monitor's "too large to live-watch" notice;
    // live watching itself survives truncation here (the merged walk keeps
    // registering watches past the file budget). The toast service coalesces
    // duplicates, so a resync repeating it cannot flood the UI.
    if (batch.is_initial && batch.truncated) {
      project_index_truncated_notice_pending_.store(true, std::memory_order_release);
    }
    if (batch.is_initial &&
        file_index_initial_build_in_flight_.exchange(false, std::memory_order_acq_rel)) {
      app::DecrementBackgroundTaskCountAndWake();
    }
    if (project_file_event_type_ != 0 &&
        (batch.is_initial || applied_to_index || batch.tree_structure_changed ||
         project_index_truncated_notice_pending_.load(std::memory_order_acquire)) &&
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
  project_index_truncated_notice_pending_.store(false, std::memory_order_relaxed);
  const bool had_initial_build = file_index_initial_build_in_flight_.exchange(
      false, std::memory_order_acq_rel);
  if (had_initial_build) {
    app::DecrementBackgroundTaskCountAndWake();
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
    return;  // identical to what the index already holds: nothing to report
  }
  project::ProjectChangeBatch normalized = project::NormalizeIndexUpdateBatch(root, rescanned);
  batch.file_changes.insert(batch.file_changes.end(),
                            std::make_move_iterator(normalized.file_changes.begin()),
                            std::make_move_iterator(normalized.file_changes.end()));
  // The coarse bit too. Consumers with no per-file handling (the sidebar tree, the
  // finder, project search) key off it, and it is the only signal for a change the
  // per-file diff cannot see — a directory added or removed with no file in it.
  batch.tree_rescan_requested = true;
}

void WorkspaceShell::RequestFileIndexRefresh() {
  const std::filesystem::path root = context_.current_project_state.root;
  if (root.empty()) {
    return;
  }
  util::AddPerformanceCounter(util::PerfCounterId::FileIndexRefreshRequests);
  // Capture the scan inputs by value so the background task never dereferences
  // the FileIndex (which a project switch may destroy while the scan is in
  // flight); ScanFiles touches only its arguments.
  const bool follow = context_.current_project_state.file_index.FollowOutOfRootSymlinks();
  std::vector<std::string> excludes =
      ParseExcludeGlobs(GetSettingValue("project.files_exclude").value_or(std::string()));
  // The index's version NOW. A scan describes the filesystem as of this moment,
  // so if a watcher batch lands while it runs, the result that comes back is
  // already stale for whatever that batch reported. See ApplyForcedFileIndexRefresh.
  const std::uint64_t version_at_dispatch = context_.current_project_state.file_index.version();
  // PostLatest by a fixed key so a burst of refresh requests only runs the newest
  // queued scan (an in-flight one still finishes; its result is simply superseded).
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
  // Drop a scan whose project has since been switched away — the current index
  // now belongs to a different project (or was reset), so applying an old root's
  // file list would corrupt it. Mirrors the generation guard on other async paths.
  if (root != context_.current_project_state.root) {
    return;
  }
  // And drop a scan the index has moved on from. `ReplaceScannedFiles` REPLACES
  // the whole file list, so a scan that started before a watcher batch landed
  // silently deletes everything that batch reported: a file created while a
  // project is still opening vanished from the finder and from project search
  // until the next full rescan, which nothing schedules. It reproduced as an
  // intermittent failure of
  // `WorkspaceShell/InjectedFileIndexBatchUpdatesFinderAndSearch` — roughly one
  // run in five, and only when the initial scan happened to land between the
  // batch and the query (TD-2026-08-15-247).
  //
  // The version only moves on a real mutation (SetRoot, Reset, a scan apply, a
  // batch that changed something) and never on a read, so a quiet tree re-scans
  // zero times and a busy one re-scans exactly as often as it is changing —
  // which is what a watcher-driven index is for. `PostLatest` coalesces the
  // requests, so a burst still runs one scan.
  if (context_.current_project_state.file_index.version() != index_version_at_dispatch) {
    util::AddPerformanceCounter(util::PerfCounterId::FileIndexScanSupersededByBatch);
    RequestFileIndexRefresh();
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

void WorkspaceShell::RebindProjectState(ProjectWorkspaceState& state) {
  WorkspaceContext::RebindProjectState(state);
  // The active project's settings vector may have just been moved-into; re-point
  // the store at the live vector and rebuild its resolved index.
  settings_store_.BindActiveProject(&context_.current_project_state.settings);
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
  // A text drag and a box selection are gestures too, and neither is tracked by
  // `mouse_selecting`. Both carry line/column coordinates captured at press
  // against a document this reset is throwing away, so a survivor would let the
  // next button-up apply the old document's offsets to the new one — for the text
  // drag, that is an edit: it would move text it never selected. Focus loss
  // already ends all three of these together; a project reset has to as well
  // (TD-2026-08-14-216).
  context_.interaction_state.text_drag = InteractionState::TextDragState::None;
  context_.interaction_state.text_drag_has_drop = false;
  context_.interaction_state.editor_box_selecting = false;
  // The tab drag and its slide animation are transient interaction state too. The
  // slide was surviving a project reset with offsets indexed by the old project's
  // tabs, and kept the animation tick awake asking to finish an animation whose
  // strip no longer existed.
  context_.interaction_state.tab_drag = TabDragState{};
  context_.interaction_state.tab_slide = TabSlideState{};
  selection_autoscroll::Disarm(context_.interaction_state);
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
  // Bounds the `.editorconfig` upward walk and drops any cache from the previous
  // project (no-op when the root is unchanged).
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
  // Record what opening the project just applied. `ApplyLiveSettings` compares
  // the resolved settings against these memos to decide whether a live EDIT
  // happened, and they started empty — so for any user who has either setting
  // set, the FIRST prepared frame read the restored configuration as an edit and
  // paid for it: a whole-tree file-index rescan, a directory-tree refresh, and a
  // full re-arm of the native watcher (a second tree walk plus one
  // inotify_add_watch per directory), on every single launch. The values are
  // already applied above; the memos have to say so.
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

}  // namespace microide::workspace
