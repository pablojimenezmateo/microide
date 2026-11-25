#include "workspace/git/MergeResultValidation.h"
#include "workspace/shell/WorkspaceShell.h"

#include <set>

#include "project/ProjectChangeNormalizer.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/TextFileIO.h"
#include "workspace/services/EditorTabService.h"
#include "workspace/services/PromptSurfaceService.h"
#include "workspace/coordinators/WorkspacePathMutationCoordinator.h"

namespace microide::workspace {

void WorkspaceShell::ApplyProjectChangeBatch(const project::ProjectChangeBatch& batch) {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::ApplyProjectChangeBatch");
  if (batch.generation != 0 &&
      batch.generation <= last_applied_project_change_generation_) {
    return;
  }
  if (batch.generation != 0) {
    last_applied_project_change_generation_ = batch.generation;
  }

  const bool repository_changed = !batch.repository_changes.empty();
  if (repository_changed) {
    git_repository_service_.MarkStale();
    context_.current_project_state.sidebar.git.snapshot_stale = true;
    ++context_.current_project_state.sidebar.git.repository_marker_generation;
    bool marked_merge_tab = false;
    for (EditorGroup& group : context_.current_project_state.editor_groups) {
      for (TabEntry& tab : group.open_tabs) {
        if (!tab.merge.has_value()) {
          continue;
        }
        tab.merge->index_stale = true;
        tab.merge->marked_resolved = false;
        marked_merge_tab = true;
      }
    }
    if (marked_merge_tab) {
      RequestPreparedFrame();
    }
    RequestPreparedFrame();
  }

  if (batch.tree_rescan_requested || !batch.file_changes.empty()) {
    RefreshProjectFileViews();
  }

  std::set<std::filesystem::path> dirty_external_paths;
  std::set<std::filesystem::path> refresh_compare_paths;
  bool editor_config_changed = false;
  for (const project::ProjectFileChange& change : batch.file_changes) {
    const std::filesystem::path normalized_path = change.absolute_path.lexically_normal();
    MakeEditorTabService().RefreshRealPathsOf(normalized_path);
    if (normalized_path.filename() == ".editorconfig") {
      editor_config_changed = true;
    }
    switch (change.kind) {
      case project::ProjectFileChangeKind::Deleted:
        InvalidateEditorBlamePath(normalized_path);
        ClearDiagnosticsForPath(normalized_path);
        break;
      case project::ProjectFileChangeKind::Created:
      case project::ProjectFileChangeKind::Modified: {
        EditorTabService& editor_tabs = MakeEditorTabService();
        const auto route = [&](const std::filesystem::path& path) {
          switch (editor_tabs.ClassifyExternalChange(path)) {
            case EditorTabService::ExternalChangeVerdict::OwnEcho:
              break;
            case EditorTabService::ExternalChangeVerdict::NeedsContentConfirm:
              BeginExternalChangeConfirm(path);
              break;
            case EditorTabService::ExternalChangeVerdict::Changed:
              ApplyExternalFileChange(path, dirty_external_paths, refresh_compare_paths);
              break;
          }
        };
        route(normalized_path);
        for (const std::filesystem::path& alias : editor_tabs.OpenSymlinkAliasesOf(normalized_path)) {
          route(alias);
        }
        break;
      }
    }
  }

  if (editor_config_changed) {
    context_.current_project_state.editor_config.Invalidate();
    ApplyEditorPreferencesToAllTabs(false);
    RequestPreparedFrame();
  }

  for (const std::filesystem::path& path : refresh_compare_paths) {
    MarkCompareTabsStaleForPath(path);
  }

  for (const std::filesystem::path& path : dirty_external_paths) {
    SetEditorBanner(context_.current_project_state, EditorBannerState::Kind::ExternalChange, path);
  }
  if (!dirty_external_paths.empty()) {
    RequestPreparedFrame();
  }

  if (batch.tree_rescan_requested) {
    if (batch.file_changes.empty() || batch.file_changes_truncated) {
      RequestPreparedFrame();
      ReloadCleanOpenBuffersFromDisk();
      for (const auto& group : context_.current_project_state.editor_groups) {
        for (const TabEntry& tab : group.open_tabs) {
          if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
            refresh_compare_paths.insert(tab.compare->path);
          }
          if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value() &&
              !tab.merge->output_path.empty()) {
            InvalidateMergeTabsForPath(tab.merge->output_path);
          }
        }
      }
      for (const std::filesystem::path& path : refresh_compare_paths) {
        MarkCompareTabsStaleForPath(path);
      }
    }
  }

  if (!batch.file_changes.empty()) {
    lsp_service_.NotifyWatchedFileChanges(batch.file_changes);
  }
  if (!batch.unitemized_file_changes.empty()) {
    lsp_service_.NotifyWatchedFileChanges(batch.unitemized_file_changes);
  }

  if (repository_changed || !batch.file_changes.empty() || batch.tree_rescan_requested) {
    RequestAutomaticGitSidebarRefresh();
  }
}

void WorkspaceShell::RefreshProjectFileViews() {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::RefreshProjectFileViews");
  ProjectWorkspaceState& project = context_.current_project_state;
  project.directory_tree.Refresh();
  project.file_finder.InvalidateIndexCache();
  const bool finder_open = project.overlay.visible && project.overlay.mode == OverlayMode::FileFinder;
  if (finder_open) {
    project.file_finder.Refresh();
  }
  ProjectSearchState& search = project.overlay.workflow.project_search;
  search.searched_query.clear();
  const bool search_shown =
      (project.sidebar.visible && ActiveSidebarMode() == SidebarMode::Search) ||
      (project.overlay.visible && project.overlay.mode == OverlayMode::ProjectSearch);
  if (search_shown && !search.query.text().empty()) {
    RefreshProjectSearch(ProjectSearchRerun::InPlace);
  }
  if (project.sidebar.visible || project.overlay.visible) {
    RequestPreparedFrame();
  }
}

void WorkspaceShell::MarkCompareTabsStaleForPath(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  for (const EditorGroup& group : context_.current_project_state.editor_groups) {
    for (const TabEntry& tab : group.open_tabs) {
      if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
          tab.compare->path == normalized_path) {
        RefreshOpenCompareTabsForPath(normalized_path);
        return;
      }
    }
  }
}

void WorkspaceShell::InvalidateMergeTabsForPath(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  for (EditorGroup& group : context_.current_project_state.editor_groups) {
   for (TabEntry& tab : group.open_tabs) {
    if (!tab.merge.has_value() || tab.merge->output_path.empty()) {
      continue;
    }
    if (!util::SameAsNormalizedPath(tab.merge->output_path, normalized_path)) {
      continue;
    }
    std::error_code exists_error;
    if (!std::filesystem::exists(normalized_path, exists_error) || exists_error) {
      continue;
    }
    if (MergeResultChangedOnDisk(*tab.merge)) {
      tab.merge->external_result_stale = true;
      tab.merge->marked_resolved = false;
      RequestPreparedFrame();
    }
   }
  }
}

void WorkspaceShell::ClearDiagnosticsForPath(const std::filesystem::path& path) {
  if (context_.current_project_state.diagnostics_store.ClearPathPrefix(path)) {
    RefreshProblemsSidebar();
    QueueEditorHoverRefresh();
    RequestPreparedFrame();
  }
  auto& state = context_.current_project_state;
  if (state.plugin_presentation_if_present() != nullptr) {
    if (state.EnsurePluginPresentation().decorations.ClearPathPrefix(path)) {
      RequestPreparedFrame();
    }
    state.MaybeReleasePluginPresentation();
  }
}


void WorkspaceShell::ApplyExternalFileChange(const std::filesystem::path& normalized_path,
                                             std::set<std::filesystem::path>& dirty_external_paths,
                                             std::set<std::filesystem::path>& refresh_compare_paths) {
  EditorTabService& editor_tabs = MakeEditorTabService();
  InvalidateEditorBlamePath(normalized_path);
  InvalidateMergeTabsForPath(normalized_path);
  refresh_compare_paths.insert(normalized_path);
  PromptSurfaceService& prompt_surfaces = MakePromptSurfaceService();
  const bool under_edits = MakePathMutationCoordinator(editor_tabs, prompt_surfaces)
                               .HasDirtyEditorTabsForPath(normalized_path, nullptr);
  if (under_edits) {
    editor_tabs.NoteDiskChangedUnderEditsForPath(normalized_path);
    dirty_external_paths.insert(normalized_path);
  }
  std::vector<std::filesystem::path> reloaded_paths;
  if (editor_tabs.ReloadCleanEditorTabsForPath(normalized_path, EditorReloadEchoGuard::AlreadyResolved,
                                               &reloaded_paths)) {
    if (!under_edits) {
      for (const std::filesystem::path& reloaded : reloaded_paths) {
        SetEditorBanner(context_.current_project_state, EditorBannerState::Kind::ReloadedNotice, reloaded);
      }
    }
    RequestPreparedFrame();
  }
}

void WorkspaceShell::BeginExternalChangeConfirm(const std::filesystem::path& normalized_path) {
  if (!pending_external_change_confirms_.insert(normalized_path.generic_string()).second) {
    return;
  }
  util::AddPerformanceCounter(util::PerfCounterId::ExternalChangeConfirmPostedReads);
  auto digest = std::make_shared<std::optional<std::uint64_t>>();
  const util::FileSignature read_signature = util::StatFileSignature(normalized_path);
  (void)file_read_service_.Begin({
      .path = normalized_path,
      .on_worker =
          [digest, normalized_path, read_signature](std::string& bytes) {
            if (util::StatFileSignature(normalized_path).SameStatAs(read_signature)) {
              *digest = util::QuickContentHash(bytes);
            }
            std::string().swap(bytes);
          },
      .on_complete =
          [this, normalized_path, digest,
           read_signature](project::FileReadService::Completion completion) {
            if (completion.status == project::FileReadService::Status::Cancelled) {
              pending_external_change_confirms_.erase(normalized_path.generic_string());
              return;
            }
            ApplyExternalChangeConfirm(normalized_path, completion.ok(), *digest, read_signature);
          },
      .max_bytes = editor::TextViewport::kMaxConfirmBytes,
  });
}

void WorkspaceShell::ApplyExternalChangeConfirm(const std::filesystem::path& normalized_path,
                                                bool read_ok,
                                                const std::optional<std::uint64_t>& digest,
                                                const util::FileSignature& read_signature) {
  if (pending_external_change_confirms_.erase(normalized_path.generic_string()) == 0) {
    return;
  }
  if (read_ok && digest.has_value() &&
      MakeEditorTabService().ExternalChangeIsOwnEcho(normalized_path, *digest, read_signature)) {
    return;
  }
  std::set<std::filesystem::path> dirty_external_paths;
  std::set<std::filesystem::path> refresh_compare_paths;
  ApplyExternalFileChange(normalized_path, dirty_external_paths, refresh_compare_paths);
  for (const std::filesystem::path& path : refresh_compare_paths) {
    MarkCompareTabsStaleForPath(path);
  }
  for (const std::filesystem::path& path : dirty_external_paths) {
    SetEditorBanner(context_.current_project_state, EditorBannerState::Kind::ExternalChange, path);
  }
  if (!dirty_external_paths.empty()) {
    RequestPreparedFrame();
  }
}

}
