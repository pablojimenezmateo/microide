// External-change save operations for TabCoordinator: force-overwrite and the
// self-write signature comparison. Split out of WorkspaceTabCoordinator.cpp to
// keep that coordinator translation unit focused (and within its size budget).

#include "workspace/coordinators/WorkspaceTabCoordinator.h"

#include <filesystem>

#include "util/PathMatch.h"

namespace microide::workspace {

bool TabCoordinator::OverwriteEditorTabsForPath(const std::filesystem::path& path) {
  // Normalize the query ONCE (and only when its text says it is needed); the scan
  // below then rejects a mismatching tab with a string compare instead of a fresh
  // path per tab -- the same shape DiskSignatureMatchesOpenView below uses.
  std::filesystem::path normalized_storage;
  const std::filesystem::path& normalized_path =
      util::PathTextNeedsNormalizing(path.native()) ? (normalized_storage = path.lexically_normal())
                                                    : path;
  bool saved_any = false;
  // Every group: a dirty split view of the same file must also be overwritten, not
  // just the focused group's view.
  for (EditorGroup& group : state_.editor_groups) {
    for (auto& tab : group.open_tabs) {
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
        continue;
      }
      editor::TextViewport& viewport = tab.editor_state->viewport;
      if (!util::SameAsNormalizedPath(viewport.path(), normalized_path) || !viewport.dirty()) {
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
  // `lexically_normal()` is ~12 allocations and purely lexical, and every path
  // reaching here has been normalized on ingress — so the normalization was
  // spending a fresh path per open tab to confirm the tab's path was already the
  // shape it is stored in. Once per side, only when the text says it is needed
  // (TD-2026-08-06-159). The dominant caller is opening a file that is already
  // open, which walks EVERY tab, so this was quadratic in tab count.
  std::filesystem::path normalized_storage;
  const std::filesystem::path& normalized_path =
      util::PathTextNeedsNormalizing(path.native())
          ? (normalized_storage = path.lexically_normal())
          : path;
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
      // The mismatching tabs are the majority and were the cost: normalizing each
      // of them to reject it is ~12 allocations spent to learn nothing, and a path
      // whose own text is already normal cannot become `normalized_path` by
      // normalizing, so the string compare has already answered for it.
      if (!util::SameAsNormalizedPath(viewport.path(), normalized_path)) {
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
