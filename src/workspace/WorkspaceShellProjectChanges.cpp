#include "workspace/WorkspaceShell.h"

#include <set>

#include "project/ProjectChangeNormalizer.h"
#include "util/PerformanceTrace.h"
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
    for (TabEntry& tab : context_.current_project_state.open_tabs) {
      if (!tab.merge.has_value()) {
        continue;
      }
      tab.merge->index_stale = true;
      tab.merge->marked_resolved = false;
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
      case project::ProjectFileChangeKind::Modified:
        InvalidateEditorBlamePath(normalized_path);
        InvalidateMergeTabsForPath(normalized_path);
        refresh_compare_paths.insert(normalized_path);
        {
          EditorTabService editor_tabs = MakeEditorTabService();
          PromptSurfaceService prompt_surfaces = MakePromptSurfaceService();
          if (MakePathMutationCoordinator(editor_tabs, prompt_surfaces)
                  .HasDirtyEditorTabsForPath(normalized_path, nullptr)) {
            dirty_external_paths.insert(normalized_path);
          } else {
            ReloadCleanEditorTabsForPath(normalized_path);
          }
        }
        break;
    }
  }

  for (const std::filesystem::path& path : refresh_compare_paths) {
    MarkCompareTabsStaleForPath(path);
  }

  if (!dirty_external_paths.empty()) {
    PromptExternalFileChanges(dirty_external_paths);
  }

  if (batch.tree_rescan_requested) {
    if (batch.file_changes.empty()) {
      ReloadCleanOpenBuffersFromDisk();
      for (const TabEntry& tab : context_.current_project_state.open_tabs) {
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
  for (std::size_t index = 0; index < context_.current_project_state.open_tabs.size(); ++index) {
    const auto& tab = context_.current_project_state.open_tabs[index];
    if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value() ||
        tab.compare->path != normalized_path) {
      continue;
    }
    context_.current_project_state.open_tabs[index].compare->model_stale = true;
    context_.current_project_state.open_tabs[index].compare->model_refreshing = true;
    refreshed_any = true;
  }
  if (refreshed_any) {
    RefreshOpenCompareTabsForPath(normalized_path);
    for (TabEntry& tab : context_.current_project_state.open_tabs) {
      if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
          tab.compare->path == normalized_path) {
        tab.compare->model_stale = false;
        tab.compare->model_refreshing = false;
      }
    }
  }
}

void WorkspaceShell::PromptExternalFileChanges(
    const std::set<std::filesystem::path>& paths) {
  if (paths.empty() || context_.prompts.dirty_visible) {
    return;
  }

  const std::filesystem::path path = *paths.begin();
  const std::filesystem::path normalized_path = path.lexically_normal();
  std::vector<std::size_t> dirty_tabs;
  for (std::size_t i = 0; i < context_.current_project_state.open_tabs.size(); ++i) {
    if (!TabIsDirty(i)) {
      continue;
    }
    const TabEntry& tab = context_.current_project_state.open_tabs[i];
    if (tab.kind == TabEntry::Kind::Editor && tab.path.lexically_normal() == normalized_path) {
      dirty_tabs.push_back(i);
      continue;
    }
    if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
        tab.compare->right_path.lexically_normal() == normalized_path) {
      dirty_tabs.push_back(i);
      continue;
    }
    if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value() &&
        tab.merge->output_path.lexically_normal() == normalized_path) {
      dirty_tabs.push_back(i);
    }
  }
  if (dirty_tabs.empty()) {
    return;
  }

  MakePromptSurfaceService().ShowDirtyPathPrompt(DirtyPromptState::Kind::ExternalFileChange,
                                                 std::move(dirty_tabs), dirty_tabs.size(), path);
}

void WorkspaceShell::InvalidateMergeTabsForPath(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  for (TabEntry& tab : context_.current_project_state.open_tabs) {
    if (!tab.merge.has_value() || tab.merge->output_path.empty()) {
      continue;
    }
    if (tab.merge->output_path.lexically_normal() != normalized_path) {
      continue;
    }
    if (!std::filesystem::exists(normalized_path)) {
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

void WorkspaceShell::ClearDiagnosticsForPath(const std::filesystem::path& path) {
  if (context_.current_project_state.diagnostics_store.ClearPathPrefix(path)) {
    RefreshProblemsSidebar();
    QueueEditorHoverRefresh();
    RequestEditorSurfaceRedraw();
  }
}

}  // namespace microide::workspace
