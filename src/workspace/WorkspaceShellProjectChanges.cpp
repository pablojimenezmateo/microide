#include "workspace/WorkspaceShell.h"

#include <set>

#include "project/ProjectChangeNormalizer.h"
#include "util/PerformanceTrace.h"
#include "util/TextFileIO.h"
#include "workspace/EditorTabService.h"
#include "workspace/PromptSurfaceService.h"
#include "workspace/WorkspacePathMutationCoordinator.h"

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
    // Mark merge tabs stale in EVERY editor group, not just the focused split: a
    // merge tab in the other split must not keep rendering a pre-change index.
    for (EditorGroup& group : context_.current_project_state.editor_groups) {
      for (TabEntry& tab : group.open_tabs) {
        if (!tab.merge.has_value()) {
          continue;
        }
        tab.merge->index_stale = true;
        tab.merge->marked_resolved = false;
      }
    }
  }

  if (batch.tree_rescan_requested || !batch.file_changes.empty()) {
    context_.current_project_state.directory_tree.Refresh();
    context_.current_project_state.file_finder.InvalidateIndexCache();
    if (context_.current_project_state.overlay.visible &&
        context_.current_project_state.overlay.mode == OverlayMode::FileFinder) {
      context_.current_project_state.file_finder.Refresh();
    }
    if (!context_.current_project_state.overlay.workflow.project_search.query.text().empty()) {
      RefreshProjectSearch();
    }
  }

  std::set<std::filesystem::path> dirty_external_paths;
  std::set<std::filesystem::path> refresh_compare_paths;
  for (const project::ProjectFileChange& change : batch.file_changes) {
    const std::filesystem::path normalized_path = change.absolute_path.lexically_normal();
    switch (change.kind) {
      case project::ProjectFileChangeKind::Deleted:
        InvalidateEditorBlamePath(normalized_path);
        ClearDiagnosticsForPath(normalized_path);
        break;
      case project::ProjectFileChangeKind::Created:
      case project::ProjectFileChangeKind::Modified: {
        EditorTabService editor_tabs = MakeEditorTabService();
        // Suppress the watcher's echo of our own save: if every open view on this
        // path already records the current on-disk signature, nothing changed
        // underneath us and the save path already refreshed blame/compare.
        const util::FileSignature signature = util::StatFileSignature(normalized_path);
        if (editor_tabs.DiskSignatureMatchesOpenView(normalized_path, signature)) {
          break;
        }
        InvalidateEditorBlamePath(normalized_path);
        InvalidateMergeTabsForPath(normalized_path);
        refresh_compare_paths.insert(normalized_path);
        PromptSurfaceService prompt_surfaces = MakePromptSurfaceService();
        if (MakePathMutationCoordinator(editor_tabs, prompt_surfaces)
                .HasDirtyEditorTabsForPath(normalized_path, nullptr)) {
          dirty_external_paths.insert(normalized_path);
        } else {
          // Reload silently and, only when an open clean buffer was actually
          // refreshed, surface a passive "reloaded from disk" notice.
          const bool had_open_buffer = CountOpenBufferViews(normalized_path) > 0;
          ReloadCleanEditorTabsForPath(normalized_path);
          if (had_open_buffer) {
            SetEditorBanner(context_.current_project_state,
                            EditorBannerState::Kind::ReloadedNotice, normalized_path);
            RequestEditorSurfaceRedraw();
          }
        }
        break;
      }
    }
  }

  for (const std::filesystem::path& path : refresh_compare_paths) {
    MarkCompareTabsStaleForPath(path);
  }

  for (const std::filesystem::path& path : dirty_external_paths) {
    SetEditorBanner(context_.current_project_state, EditorBannerState::Kind::ExternalChange, path);
  }
  if (!dirty_external_paths.empty()) {
    RequestEditorSurfaceRedraw();
  }

  if (batch.tree_rescan_requested) {
    if (batch.file_changes.empty()) {
      ReloadCleanOpenBuffersFromDisk();
      for (const TabEntry& tab : context_.current_project_state.focused_group().open_tabs) {
        if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
          refresh_compare_paths.insert(tab.compare->path);
        }
        if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value() &&
            !tab.merge->output_path.empty()) {
          InvalidateMergeTabsForPath(tab.merge->output_path);
        }
      }
      for (const std::filesystem::path& path : refresh_compare_paths) {
        MarkCompareTabsStaleForPath(path);
      }
    }
  }

  if (repository_changed || !batch.file_changes.empty() || batch.tree_rescan_requested) {
    RequestAutomaticGitSidebarRefresh();
  }
}

void WorkspaceShell::MarkCompareTabsStaleForPath(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  bool refreshed_any = false;
  // Scan every editor group: a compare tab in the non-focused split must also be
  // invalidated when its file changes externally, otherwise it keeps rendering a
  // pre-change diff.
  for (EditorGroup& group : context_.current_project_state.editor_groups) {
    for (TabEntry& tab : group.open_tabs) {
      if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value() ||
          tab.compare->path != normalized_path) {
        continue;
      }
      tab.compare->model_stale = true;
      tab.compare->model_refreshing = true;
      refreshed_any = true;
    }
  }
  if (refreshed_any) {
    RefreshOpenCompareTabsForPath(normalized_path);
    for (EditorGroup& group : context_.current_project_state.editor_groups) {
      for (TabEntry& tab : group.open_tabs) {
        if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
            tab.compare->path == normalized_path) {
          tab.compare->model_stale = false;
          tab.compare->model_refreshing = false;
        }
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
    if (tab.merge->output_path.lexically_normal() != normalized_path) {
      continue;
    }
    std::error_code exists_error;
    if (!std::filesystem::exists(normalized_path, exists_error) || exists_error) {
      continue;
    }
    std::error_code error;
    const auto tick = std::filesystem::last_write_time(normalized_path, error);
    if (!error && tab.merge->disk_result_tick.has_value() &&
        static_cast<std::uint64_t>(tick.time_since_epoch().count()) != *tab.merge->disk_result_tick) {
      tab.merge->external_result_stale = true;
      tab.merge->marked_resolved = false;
    }
   }
  }
}

void WorkspaceShell::ClearDiagnosticsForPath(const std::filesystem::path& path) {
  if (context_.current_project_state.diagnostics_store.ClearPathPrefix(path)) {
    RefreshProblemsSidebar();
    QueueEditorHoverRefresh();
    RequestEditorSurfaceRedraw();
  }
  // Plugin/LSP decorations are path-keyed just like diagnostics, so an external delete
  // (filesystem watcher) must clear them too — otherwise they linger stale and the
  // plugin-presentation bundle can never drain empty and release. Gated on presence so
  // a delete of an undecorated file never allocates the bundle.
  auto& state = context_.current_project_state;
  if (state.plugin_presentation_if_present() != nullptr) {
    if (state.EnsurePluginPresentation().decorations.ClearPathPrefix(path)) {
      RequestEditorSurfaceRedraw();
    }
    state.MaybeReleasePluginPresentation();
  }
}

}  // namespace microide::workspace
