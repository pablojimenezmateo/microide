// External-change save operations for TabCoordinator: force-overwrite and the
// self-write signature comparison. Split out of WorkspaceTabCoordinator.cpp to
// keep that coordinator translation unit focused (and within its size budget).

#include "workspace/WorkspaceTabCoordinator.h"

namespace microide::workspace {

bool TabCoordinator::OverwriteEditorTabsForPath(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  bool saved_any = false;
  // Every group: a dirty split view of the same file must also be overwritten, not
  // just the focused group's view.
  for (EditorGroup& group : state_.editor_groups) {
    for (auto& tab : group.open_tabs) {
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
        continue;
      }
      editor::TextViewport& viewport = tab.editor_state->viewport;
      if (viewport.path().lexically_normal() != normalized_path || !viewport.dirty()) {
        continue;
      }
      if (operations_.prepare_editor_view_for_save &&
          !operations_.prepare_editor_view_for_save(viewport.path(), viewport, nullptr)) {
        continue;
      }
      // Deliberately skip DetectDiskConflict: the user chose to overwrite. Save()
      // recaptures the post-write signature so the watcher's echo is suppressed.
      if (viewport.Save()) {
        saved_any = true;
        operations_.invalidate_editor_blame_path(normalized_path);
        operations_.notify_plugin_buffer_save(normalized_path);
      }
    }
  }
  if (saved_any) {
    state_.directory_tree.Refresh();
    if (operations_.request_automatic_git_sidebar_refresh) {
      operations_.request_automatic_git_sidebar_refresh();
    }
  }
  return saved_any;
}

bool TabCoordinator::DiskSignatureMatchesOpenView(const std::filesystem::path& path,
                                                  const util::FileSignature& signature) const {
  const std::filesystem::path normalized_path = path.lexically_normal();
  bool matched_any_view = false;
  // Check views in every group: the self-write echo must only be suppressed if EVERY
  // open view of this path (including a non-focused split view) already saw our write,
  // otherwise a stale split view would be denied its reload.
  for (const EditorGroup& group : state_.editor_groups) {
    for (const auto& tab : group.open_tabs) {
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
        continue;
      }
      const editor::TextViewport& viewport = tab.editor_state->viewport;
      if (viewport.path().lexically_normal() != normalized_path) {
        continue;
      }
      matched_any_view = true;
      if (!signature.SameContentAs(viewport.disk_signature())) {
        return false;
      }
    }
  }
  return matched_any_view;
}

}  // namespace microide::workspace
