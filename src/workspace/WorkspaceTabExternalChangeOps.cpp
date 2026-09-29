// External-change save operations for TabCoordinator: force-overwrite and the
// self-write signature comparison. Split out of WorkspaceTabCoordinator.cpp to
// keep that coordinator translation unit focused (and within its size budget).

#include "workspace/coordinators/WorkspaceTabCoordinator.h"

#include <filesystem>

#include <cstdint>
#include <optional>
#include <string>

#include "util/PathMatch.h"
#include "util/TextFileIO.h"

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
      // Blocking: "Overwrite" is a modal answer, and this loop writes several tabs
      // and then refreshes once. A deferred completion would land after that
      // refresh.
      if (operations_.prepare_editor_view_for_save &&
          !operations_.prepare_editor_view_for_save(viewport.path(), viewport, nullptr,
                                                    SaveMode::Blocking)
               .ok()) {
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

bool TabCoordinator::DiskSignatureMatchesOpenView(const std::filesystem::path& path) const {
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
  // Stat lazily, on the first view that actually names this path: the caller is a
  // watcher batch that may name thousands of paths of which a handful are open,
  // and statting each one to find out costs a syscall per path on the shell
  // thread.
  util::FileSignature signature;
  // ONE read for the whole sweep. Every view of this path asks the same question
  // of the same file, and the per-view form re-read it each time: a file open in
  // two panes cost two full reads (up to 8 MiB each) for a single watcher event,
  // and nothing shared the answer. Read lazily — the overwhelmingly common case
  // is that the first stat compare settles it and no read happens at all.
  std::optional<std::uint64_t> disk_content_hash;
  bool disk_read_failed = false;
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
      if (!matched_any_view) {
        signature = util::StatFileSignature(normalized_path);
        matched_any_view = true;
      }
      // Not `SameContentAs`: a touch or a byte-identical rewrite moves the mtime
      // and leaves the file as it was, and reading that as an external change
      // reloads every open clean view and shows a "reloaded from disk" notice for
      // a file nobody changed — or, for a dirty view, raises an external-change
      // banner unprompted. DiskContentUnchanged confirms the mismatch against the
      // content the view recorded before believing it, and re-baselines so the
      // next event on this path is one stat again.
      if (signature.SameContentAs(viewport.disk_signature())) {
        continue;  // one stat answered it for this view
      }
      if (!viewport.CouldConfirmDiskContent(signature)) {
        return false;  // a size change is a real change; no read needed
      }
      if (!disk_content_hash.has_value() && !disk_read_failed) {
        if (const std::optional<std::string> bytes = util::ReadTextFile(normalized_path);
            bytes.has_value()) {
          disk_content_hash = util::ContentHash(*bytes);
        } else {
          disk_read_failed = true;
        }
      }
      if (!disk_content_hash.has_value() ||
          !viewport.ConfirmDiskContentUnchanged(signature, *disk_content_hash)) {
        return false;
      }
    }
  }
  return matched_any_view;
}

TabCoordinator::ExternalChangeVerdict TabCoordinator::ClassifyExternalChange(
    const std::filesystem::path& path) const {
  // Same normalization discipline as DiskSignatureMatchesOpenView above: once per
  // side, only when the text says it is needed.
  std::filesystem::path normalized_storage;
  const std::filesystem::path& normalized_path =
      util::PathTextNeedsNormalizing(path.native())
          ? (normalized_storage = path.lexically_normal())
          : path;
  bool matched_any_view = false;
  bool needs_confirm = false;
  util::FileSignature signature;
  for (const EditorGroup& group : state_.editor_groups) {
    for (const auto& tab : group.open_tabs) {
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
        continue;
      }
      const editor::TextViewport& viewport = tab.editor_state->viewport;
      if (!util::SameAsNormalizedPath(viewport.path(), normalized_path)) {
        continue;
      }
      if (!matched_any_view) {
        // Stat lazily, on the first view that actually names this path: a watcher
        // batch may name thousands of paths of which a handful are open.
        signature = util::StatFileSignature(normalized_path);
        matched_any_view = true;
      }
      if (signature.SameContentAs(viewport.disk_signature())) {
        continue;  // one stat answered it for this view
      }
      if (!viewport.CouldConfirmDiskContent(signature)) {
        return ExternalChangeVerdict::Changed;  // no digest can excuse this one
      }
      needs_confirm = true;
    }
  }
  if (!matched_any_view) {
    // Nothing has this path open, so there is no echo to suppress. The caller
    // still has blame, compare and merge state keyed on it.
    return ExternalChangeVerdict::Changed;
  }
  return needs_confirm ? ExternalChangeVerdict::NeedsContentConfirm
                       : ExternalChangeVerdict::OwnEcho;
}

bool TabCoordinator::ExternalChangeIsOwnEcho(const std::filesystem::path& path,
                                             std::uint64_t disk_content_hash) const {
  std::filesystem::path normalized_storage;
  const std::filesystem::path& normalized_path =
      util::PathTextNeedsNormalizing(path.native())
          ? (normalized_storage = path.lexically_normal())
          : path;
  bool matched_any_view = false;
  util::FileSignature signature;
  for (const EditorGroup& group : state_.editor_groups) {
    for (const auto& tab : group.open_tabs) {
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
        continue;
      }
      const editor::TextViewport& viewport = tab.editor_state->viewport;
      if (!util::SameAsNormalizedPath(viewport.path(), normalized_path)) {
        continue;
      }
      if (!matched_any_view) {
        // Re-stat: the digest was computed off-thread and the file may have moved
        // again since. Confirming against a stale stat would re-baseline a view to
        // a state that no longer exists.
        signature = util::StatFileSignature(normalized_path);
        matched_any_view = true;
      }
      if (signature.SameContentAs(viewport.disk_signature())) {
        continue;
      }
      if (!viewport.ConfirmDiskContentUnchanged(signature, disk_content_hash)) {
        return false;
      }
    }
  }
  return matched_any_view;
}

}  // namespace microide::workspace
