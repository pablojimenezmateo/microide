#include "workspace/git/MergeResultValidation.h"
#include "workspace/shell/WorkspaceShell.h"

#include <set>

#include "project/ProjectChangeNormalizer.h"
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
    // Invalidates every answer derived from the repository merely existing; see
    // the field's comment. A repository change is the only way `.git` can appear
    // or disappear under a live project without the root itself changing.
    ++context_.current_project_state.sidebar.git.repository_marker_generation;
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
  // A `.editorconfig` write changes how every buffer under it is formatted, so the
  // memoized resolution has to be dropped and preferences re-pushed. Checked with a
  // filename compare inside the loop we already run rather than a second pass.
  bool editor_config_changed = false;
  for (const project::ProjectFileChange& change : batch.file_changes) {
    const std::filesystem::path normalized_path = change.absolute_path.lexically_normal();
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
        // Suppress the watcher's echo of our own save: if every open view on this
        // path already records the current on-disk signature, nothing changed
        // underneath us and the save path already refreshed blame/compare.
        switch (editor_tabs.ClassifyExternalChange(normalized_path)) {
          case EditorTabService::ExternalChangeVerdict::OwnEcho:
            break;
          case EditorTabService::ExternalChangeVerdict::NeedsContentConfirm:
            // One digest settles it, and computing it here is a read of up to
            // 8 MiB on the shell thread — per changed path, per watcher event.
            // A `git checkout` that moves the mtime of every open file pays that
            // for each of them with the window frozen. Post it instead and
            // decide when it lands; nothing is shown to the user in between, so
            // an undecided path looks like no event at all rather than a wrong
            // one.
            BeginExternalChangeConfirm(normalized_path);
            break;
          case EditorTabService::ExternalChangeVerdict::Changed:
            ApplyExternalFileChange(normalized_path, dirty_external_paths,
                                    refresh_compare_paths);
            break;
        }
        break;
      }
    }
  }

  if (editor_config_changed) {
    context_.current_project_state.editor_config.Invalidate();
    // Re-push the cheap per-viewport setters only: `.editorconfig` drives indent,
    // line ending, and save normalization, none of which affect the language
    // contract, so the O(tabs) filetype-detect + contract rebuild is skipped.
    ApplyEditorPreferencesToAllTabs(/*refresh_language_contracts=*/false);
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
      // Reconcile compare/merge tabs across EVERY editor group, not just the focused
      // one: a compare/merge tab open only in a background split must not keep a stale
      // model/output after a pure tree-rescan. (TD-2026-07-16-57.)
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

  // Tell the language servers about the on-disk changes. This is the only path by
  // which a server learns about an edit the editor did not make — a `git switch`,
  // a `pull`, a `stash pop`, a generated header — for every file the user does not
  // have open. Without it the server keeps answering from a pre-change index:
  // go-to-definition lands on stale lines and diagnostics name deleted symbols
  // until the file happens to be opened.
  //
  // Every change in the batch is reported, including the watcher's echo of our own
  // save that the loop above skips for buffer purposes. Filtering those out would
  // save one event on a save and nothing at all on the case that matters (a branch
  // switch is hundreds of paths, none of them echoes), while costing a filtered
  // copy of the list every time. It is also safe: per LSP, once didOpen is sent the
  // client owns that document's content until didClose, so a server must not
  // re-read an open document from disk on the strength of a watched-file event.
  if (!batch.file_changes.empty()) {
    lsp_service_.NotifyWatchedFileChanges(batch.file_changes);
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
    if (!util::SameAsNormalizedPath(tab.merge->output_path, normalized_path)) {
      continue;
    }
    std::error_code exists_error;
    if (!std::filesystem::exists(normalized_path, exists_error) || exists_error) {
      continue;
    }
    // Through the shared confirmation, not a tick compare of its own: a touch or a
    // byte-identical rewrite moves the tick and changes nothing, and un-marking a
    // completed resolution over that is the worst of the three ways this mistake
    // showed up. (TD-2026-09-29-306.)
    if (MergeResultChangedOnDisk(*tab.merge)) {
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


// The per-path half of an external change, shared by the immediate verdict and by
// the deferred one that lands with a digest. Both must do the same things, and a
// second copy of "invalidate blame, restage compare/merge, then banner-or-reload"
// is how one of them ends up doing three of the four.
void WorkspaceShell::ApplyExternalFileChange(const std::filesystem::path& normalized_path,
                                             std::set<std::filesystem::path>& dirty_external_paths,
                                             std::set<std::filesystem::path>& refresh_compare_paths) {
  EditorTabService& editor_tabs = MakeEditorTabService();
  InvalidateEditorBlamePath(normalized_path);
  InvalidateMergeTabsForPath(normalized_path);
  refresh_compare_paths.insert(normalized_path);
  PromptSurfaceService& prompt_surfaces = MakePromptSurfaceService();
  if (MakePathMutationCoordinator(editor_tabs, prompt_surfaces)
          .HasDirtyEditorTabsForPath(normalized_path, nullptr)) {
    dirty_external_paths.insert(normalized_path);
    return;
  }
  // Reload silently and, only when an open clean buffer was actually refreshed,
  // surface a passive "reloaded from disk" notice.
  const bool had_open_buffer = CountOpenBufferViews(normalized_path) > 0;
  ReloadCleanEditorTabsForPath(normalized_path);
  if (had_open_buffer) {
    SetEditorBanner(context_.current_project_state, EditorBannerState::Kind::ReloadedNotice,
                    normalized_path);
    RequestEditorSurfaceRedraw();
  }
}

void WorkspaceShell::BeginExternalChangeConfirm(const std::filesystem::path& normalized_path) {
  // One in flight per path. A watcher that reports the same file twice before the
  // first digest lands would otherwise read it twice and decide twice; the second
  // event is about the same question, and the re-stat inside the confirm is what
  // makes a later change still visible.
  if (!pending_external_change_confirms_.insert(normalized_path.generic_string()).second) {
    return;
  }
  auto digest = std::make_shared<std::optional<std::uint64_t>>();
  (void)file_read_service_.Begin({
      .path = normalized_path,
      .on_worker =
          [digest](std::string& bytes) {
            *digest = util::ContentHash(bytes);
            // The bytes stop here. Only the digest crosses to the shell thread —
            // handing an 8 MiB buffer across to be hashed there would be the
            // stall this exists to remove, one hop later.
            std::string().swap(bytes);
          },
      .on_complete =
          [this, normalized_path, digest](project::FileReadService::Completion completion) {
            ApplyExternalChangeConfirm(normalized_path, completion.ok(), *digest);
          },
      .max_bytes = editor::TextViewport::kMaxConfirmBytes,
  });
}

void WorkspaceShell::ApplyExternalChangeConfirm(const std::filesystem::path& normalized_path,
                                                bool read_ok,
                                                const std::optional<std::uint64_t>& digest) {
  pending_external_change_confirms_.erase(normalized_path.generic_string());
  // A read that failed cannot excuse the stat mismatch, so the change stands.
  // Announcing a change that turns out to be an echo costs the user a dismissable
  // banner; suppressing a real one costs them their edits.
  if (read_ok && digest.has_value() &&
      MakeEditorTabService().ExternalChangeIsOwnEcho(normalized_path, *digest)) {
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
    RequestEditorSurfaceRedraw();
  }
}

}  // namespace microide::workspace
