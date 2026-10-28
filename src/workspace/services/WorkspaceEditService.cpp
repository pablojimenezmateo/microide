#include "workspace/services/WorkspaceEditService.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "editor/EditBatchOrder.h"
#include "util/PathMatch.h"
#include "workspace/lsp/LspPositionEncoding.h"
#include "workspace/lsp/LspService.h"
#include "workspace/lsp/LspViewportPositions.h"

namespace microide::workspace {

void WorkspaceEditService::Configure(WorkspaceContext& context, LspService& lsp,
                                     Operations operations) {
  context_ = &context;
  lsp_ = &lsp;
  operations_ = std::move(operations);
}

editor::TextViewport* WorkspaceEditService::ResolveOpenViewport(
    const std::filesystem::path& path) const {
  editor::TextViewport* active = operations_.active_editable_viewport();
  if (path.empty()) {
    return active;
  }
  const std::filesystem::path normalized = util::NormalizedPath(path);
  if (active != nullptr && util::SameAsNormalizedPath(active->path(), normalized)) {
    return active;
  }
  return OpenEditorViewOfPath(context_->current_project_state.editor_groups, normalized);
}

bool WorkspaceEditService::ApplyBatch(editor::TextViewport& viewport, const EditBatch& batch) {
  std::vector<editor::SelectionRange> ranges;
  ranges.reserve(batch.size());
  for (const auto& [range, text] : batch) {
    ranges.push_back(range);
  }
  std::vector<std::size_t> apply_order;
  editor::OrderEditsForApplication(ranges, apply_order);
  if (editor::EditsOverlap(ranges, apply_order)) {
    return false;
  }
  if (batch.empty()) {
    return true;
  }
  viewport.BeginUndoGroup();
  for (const std::size_t index : apply_order) {
    viewport.ReplaceRange(batch[index].first, batch[index].second, /*record_undo=*/true);
  }
  viewport.EndUndoGroup();
  return true;
}

void WorkspaceEditService::MarkFoldsDirty(const editor::TextViewport& edited) {
  for (auto& group : context_->current_project_state.editor_groups) {
    for (auto& tab : group.open_tabs) {
      if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value() &&
          &tab.editor_state->viewport == &edited && tab.editor_state->folding_model) {
        tab.editor_state->folding_model->MarkDirty();
        return;
      }
    }
  }
}

bool WorkspaceEditService::ApplyPluginEdit(const plugin::PluginHost::WorkspaceEditRequest& request) {
  editor::TextViewport* viewport = ResolveOpenViewport(request.path);
  if (viewport == nullptr) {
    return false;
  }

  // Staleness guard for edits deferred from the plugin worker: the coordinates
  // were computed against `guard_path` at `captured_content_revision`. If the
  // resolved buffer is a different file (the user switched tabs) or has advanced
  // (the user typed) during the async hop, the edit is stale — drop it rather than
  // apply now-invalid coordinates over newer input.
  if (request.has_staleness_guard &&
      (!util::SameAsNormalizedPath(viewport->path(), request.guard_path) ||
       viewport->content_revision() != request.captured_content_revision)) {
    return false;
  }

  // 1-based plugin coordinates → clamped 0-based document positions, evaluated
  // live so caret clamping sees post-edit line lengths.
  const auto clamp_position = [&](std::size_t one_based_line,
                                  std::size_t one_based_column) -> editor::TextPosition {
    const std::size_t line_count = viewport->line_count();
    if (line_count == 0) {
      return editor::TextPosition{0, 0};
    }
    const std::size_t line = std::min(one_based_line >= 1 ? one_based_line - 1 : 0, line_count - 1);
    const std::size_t column =
        std::min(one_based_column >= 1 ? one_based_column - 1 : 0, viewport->lines().LineLength(line));
    return editor::TextPosition{line, column};
  };

  EditBatch batch;
  batch.reserve(request.edits.size());
  for (const auto& edit : request.edits) {
    const editor::TextPosition start = clamp_position(edit.start_line, edit.start_column);
    const editor::TextPosition end =
        edit.end_line >= 1 ? clamp_position(edit.end_line, edit.end_column) : start;
    batch.emplace_back(editor::SelectionRange{start, end}, edit.text);
  }
  if (!ApplyBatch(*viewport, batch)) {
    return false;
  }

  if (request.has_selection) {
    const editor::TextPosition start =
        clamp_position(request.selection_start_line, request.selection_start_column);
    const editor::TextPosition end =
        clamp_position(request.selection_end_line, request.selection_end_column);
    viewport->JumpCursorTo(start.line, start.column, /*extend_selection=*/false);
    viewport->MoveCursorTo(end.line, end.column, /*extend_selection=*/true);
  } else if (request.has_cursor) {
    const editor::TextPosition cursor = clamp_position(request.cursor_line, request.cursor_column);
    viewport->JumpCursorTo(cursor.line, cursor.column, /*extend_selection=*/false);
  }

  operations_.reset_caret_blink();
  operations_.request_active_editable_last_change_redraw();
  operations_.request_chrome_redraw();
  return true;
}

bool WorkspaceEditService::ApplyLspEdit(const std::vector<CodeActionEdit>& edits,
                                        bool* out_any_rejected) {
  const auto mark_rejected = [&]() {
    if (out_any_rejected != nullptr) {
      *out_any_rejected = true;
    }
  };
  if (edits.empty()) {
    return false;
  }

  // Group edits by the buffer they resolve to. A large rename / code action
  // repeatedly targets the same handful of files, so the (normalized) path ->
  // viewport resolution is memoized and `bucket_index` maps a viewport to its slot,
  // keeping grouping O(1) per edit instead of O(edits * touched_files).
  struct Bucket {
    editor::TextViewport* viewport = nullptr;
    EditBatch edits;
    // The compact delta the LSP re-sync and the redraw need, instead of
    // snapshotting every whole buffer before and after (TD-2026-07-17A-016):
    // pre-edit line count plus the affected line span (before-coords).
    std::size_t before_line_count = 0;
    std::size_t first_changed_line = 0;
    std::size_t last_changed_line = 0;
    bool applied = false;
  };
  std::vector<Bucket> buckets;
  std::unordered_map<std::string, editor::TextViewport*> resolve_cache;
  std::unordered_map<const editor::TextViewport*, std::size_t> bucket_index;
  for (const CodeActionEdit& edit : edits) {
    std::string key = util::NormalizedPath(edit.path).generic_string();
    auto cached = resolve_cache.find(key);
    if (cached == resolve_cache.end()) {
      cached = resolve_cache.emplace(std::move(key), ResolveOpenViewport(edit.path)).first;
    }
    editor::TextViewport* viewport = cached->second;
    if (viewport == nullptr) {
      continue;
    }
    const auto [slot, inserted] = bucket_index.emplace(viewport, buckets.size());
    if (inserted) {
      buckets.push_back(Bucket{.viewport = viewport, .before_line_count = viewport->line_count()});
    }
    buckets[slot->second].edits.emplace_back(edit.range, edit.new_text);
  }

  bool applied_any = false;
  for (Bucket& bucket : buckets) {
    editor::TextViewport& viewport = *bucket.viewport;
    // Resolve the server's position encoding for this buffer so the edit's LSP
    // `character` offsets (utf-16 code units by spec default) map to the right
    // UTF-8 byte column — mis-mapping on a non-ASCII line splits a multibyte
    // sequence and corrupts the buffer.
    std::string language_id;
    LspClient* client = lsp_->LspClientForViewport(viewport, &language_id);
    const lsp_encoding::PositionEncoding encoding =
        client != nullptr ? LspEncodingForClient(*client) : lsp_encoding::PositionEncoding::Utf8;
    // Convert 0-based LSP coordinates to editor byte columns. A line beyond EOF is a
    // stale/confused/hostile server target: unlike the old forgiving clamp (which
    // silently rewrote it onto the LAST real line and mutated it — visible data loss
    // in dirty state, undo, diagnostics, folds), the whole buffer's edit group is
    // REJECTED, matching the closed-file applier's range policy. The sole allowed
    // beyond-EOF position is the end-of-document sentinel {line == line_count,
    // character == 0} (an append at EOF). LspCharacterToByteColumn still soft-clamps
    // the column within a valid line.
    const std::size_t line_count = viewport.line_count();
    bool group_ok = true;
    const auto map_position = [&](editor::TextPosition pos) -> editor::TextPosition {
      if (line_count == 0) {
        group_ok = group_ok && pos.line == 0;
        return editor::TextPosition{0, 0};
      }
      if (pos.line == line_count && pos.column == 0) {
        return pos;  // the end-of-document append sentinel
      }
      if (pos.line >= line_count) {
        group_ok = false;
        return pos;
      }
      pos.column = lsp_encoding::LspCharacterToByteColumn(viewport.lines().LineView(pos.line),
                                                          pos.column, encoding);
      return pos;
    };
    std::size_t first_changed = std::numeric_limits<std::size_t>::max();
    std::size_t last_changed = 0;
    for (auto& [range, text] : bucket.edits) {
      range.start = map_position(range.start);
      range.end = map_position(range.end);
      // Every edit changes its target line, so the lowest start line is the first
      // differing line, and the highest end line bounds the redraw range for a
      // same-line-count edit.
      const editor::SelectionRange normalized = editor::TextViewport::NormalizeRange(range);
      first_changed = std::min(first_changed, normalized.start.line);
      last_changed = std::max(last_changed, normalized.end.line);
    }
    // A beyond-EOF target or an overlapping group invalidates this buffer's whole
    // group; record the rejection so a server-initiated applyEdit reports partial
    // failure.
    if (!group_ok || !ApplyBatch(viewport, bucket.edits)) {
      mark_rejected();
      continue;
    }
    bucket.first_changed_line = first_changed;
    bucket.last_changed_line = last_changed;
    bucket.applied = true;
    applied_any = true;
  }
  if (!applied_any) {
    return false;
  }

  operations_.reset_caret_blink();
  // Full-document re-sync of EVERY edited buffer (the incremental last-change path
  // only carries the last of N edits, which would desync the server): a
  // WorkspaceEdit can touch several open buffers (header + source rename,
  // multi-file fixit), and each one's server mirror and stored diagnostics must be
  // reconciled, not just the active tab's. Every edited buffer's fold model is
  // recomputed too: its content_revision fingerprint alone does not force a
  // rescan once a file is fully resolved (see the Undo/Redo guard in
  // WorkspaceActionContext), so a same-line-count edit — a Format-Document
  // re-indent, a bracket-renesting code action / rename — would otherwise leave
  // phantom fold markers that hide arbitrary line ranges.
  const editor::TextViewport* active_viewport = operations_.active_editable_viewport();
  bool active_synced = false;
  for (const Bucket& bucket : buckets) {
    if (!bucket.applied) {
      continue;
    }
    MarkFoldsDirty(*bucket.viewport);
    lsp_->SyncLspForBufferChange(
        *bucket.viewport,
        LspService::BufferChangeDelta{bucket.before_line_count, bucket.first_changed_line});
    if (bucket.viewport != active_viewport) {
      continue;
    }
    // Repaint only the touched line span of the visible buffer. A net line-count
    // change reflows everything below, so it repaints the whole editor.
    if (bucket.before_line_count != bucket.viewport->line_count()) {
      operations_.request_focused_editor_redraw();
    } else {
      operations_.request_active_editable_line_range_redraw(
          bucket.first_changed_line,
          std::max(bucket.last_changed_line + 1, bucket.first_changed_line + 1));
    }
    active_synced = true;
  }
  if (!active_synced) {
    // The edit did not touch the active buffer; still refresh its last-change
    // derived state so the caret/redraw bookkeeping stays consistent.
    operations_.request_active_editable_last_change_redraw();
  }
  // Repaint the tab strip so the dirty indicator appears immediately; the edit
  // just flipped the buffer to dirty and a chrome redraw only damages the
  // menu-bar region, leaving the tab dot stale until the next full repaint.
  operations_.request_active_tab_redraw(/*include_tree_sidebar=*/false);
  return true;
}

void WorkspaceEditService::ApplyRename(const std::string& new_name,
                                       const std::vector<CodeActionEdit>& edits,
                                       const std::vector<WorkspaceResourceOp>& resource_ops) {
  if (edits.empty() && resource_ops.empty()) {
    return;
  }
  // Distinct affected files (an empty path targets the active buffer, always open),
  // deduped through a hash set so a large legal rename stays O(edits).
  std::vector<std::filesystem::path> affected;
  std::unordered_set<std::string> affected_seen;
  std::size_t closed_count = 0;
  for (const CodeActionEdit& edit : edits) {
    if (edit.path.empty()) {
      continue;
    }
    std::filesystem::path normalized = util::NormalizedPath(edit.path);
    if (!affected_seen.insert(normalized.generic_string()).second) {
      continue;
    }
    if (OpenEditorViewOfPath(context_->current_project_state.editor_groups, normalized) == nullptr) {
      ++closed_count;
    }
    affected.push_back(std::move(normalized));
  }

  if (closed_count == 0 && resource_ops.empty()) {
    // Everything is already open: apply in place, leaving the buffers dirty like any
    // other edit (the user saves as usual).
    ApplyLspEdit(edits);
    return;
  }

  // Some files are not open, or the rename carries file operations (e.g. a
  // rust-analyzer module rename renames the file itself). Confirm before applying —
  // writing files the user hasn't opened and mutating paths are outward,
  // hard-to-undo actions. The closed files are applied SILENTLY on disk
  // (VSCode-style); no tabs are opened for them.
  std::string detail = "Renaming to '" + new_name + "' changes " +
                       std::to_string(affected.size()) + (affected.size() == 1 ? " file" : " files") +
                       " (" + std::to_string(closed_count) + " not open)";
  if (!resource_ops.empty()) {
    detail += " and performs " + std::to_string(resource_ops.size()) +
              (resource_ops.size() == 1 ? " file operation" : " file operations");
  }
  detail += ". Apply and save?";
  pending_rename_ = PendingRename{new_name, edits, std::move(affected), resource_ops};
  operations_.confirm_rename_save(std::move(detail));
}

void WorkspaceEditService::CommitPendingRename() {
  if (!pending_rename_.has_value()) {
    return;
  }
  const PendingRename pending = std::move(*pending_rename_);
  pending_rename_.reset();

  // Resource ops run FIRST (the text edits are keyed by the post-rename paths;
  // open tabs are retargeted by the reconcile hooks as each op lands). A failed
  // op batch rolls itself back and aborts the rename — applying the text edits
  // against paths the ops never produced would corrupt the workspace.
  if (!pending.resource_ops.empty() && !lsp_->ApplyWorkspaceResourceOps(pending.resource_ops).ok) {
    operations_.notify_error("Rename aborted: file operations failed (rolled back)");
    return;
  }

  // Open files keep their edits applied in place (ApplyLspEdit resolves only
  // already-open buffers; closed files are skipped there), then are saved.
  ApplyLspEdit(pending.edits);

  std::unordered_set<std::string> affected;
  affected.reserve(pending.affected_paths.size());
  for (const std::filesystem::path& path : pending.affected_paths) {
    affected.insert(path.generic_string());
  }
  ProjectWorkspaceState& state = context_->current_project_state;
  std::size_t saved = 0;
  bool any_conflict = false;
  for (auto& group : state.editor_groups) {
    for (auto& tab : group.open_tabs) {
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
          tab.editor_state->content_pending()) {
        continue;
      }
      editor::TextViewport& viewport = tab.editor_state->viewport;
      if (!viewport.dirty() || affected.count(util::NormalizedPath(viewport.path()).generic_string()) == 0) {
        continue;
      }
      if (viewport.SaveWouldOverwriteUnseenDisk()) {
        any_conflict = true;
        continue;
      }
      if (viewport.Save(state.write_gate())) {
        operations_.notify_buffer_saved(viewport.path());
        ++saved;
      }
    }
  }

  // Apply the remaining edits to the CLOSED files directly on disk — no tab spam.
  const auto is_open = [&state](const std::filesystem::path& normalized) {
    return OpenEditorViewOfPath(state.editor_groups, normalized) != nullptr;
  };
  const LspService::DiskEditResult disk = lsp_->ApplyLspEditsToClosedFilesOnDisk(pending.edits, is_open);
  saved += disk.files_written;

  std::string feedback = "Renamed to '" + pending.new_name + "' in " + std::to_string(saved) +
                         (saved == 1 ? " file" : " files");
  if (any_conflict || disk.any_failed) {
    feedback += " (some skipped)";
  }
  state.panel.feedback.text = std::move(feedback);
  operations_.request_active_tab_redraw(/*include_tree_sidebar=*/true);
}

}  // namespace microide::workspace
