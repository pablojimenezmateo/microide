#include "workspace/services/WorkspaceEditService.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "editor/EditBatchOrder.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "project/FileWriteGate.h"
#include "util/PathMatch.h"
#include "workspace/FileUri.h"
#include "workspace/assist/AssistHost.h"
#include "workspace/lsp/LspWorkspaceEditOps.h"
#include "workspace/lsp/LspPositionEncoding.h"
#include "workspace/lsp/LspService.h"
#include "workspace/lsp/LspViewportPositions.h"
#include "util/InvokeIfSet.h"

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

bool WorkspaceEditService::ApplyBatch(editor::TextViewport& viewport, const EditBatch& batch,
                                      bool record_undo,
                                      const std::function<void(const editor::TextViewport&)>& follow) {
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
  if (record_undo) {
    viewport.BeginUndoGroup();
  }
  for (const std::size_t index : apply_order) {
    if (viewport.ReplaceRange(batch[index].first, batch[index].second, record_undo) && follow) {
      follow(viewport);
    }
  }
  if (record_undo) {
    viewport.EndUndoGroup();
  }
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

  if (request.has_staleness_guard &&
      (!util::SameAsNormalizedPath(viewport->path(), request.guard_path) ||
       viewport->content_revision() != request.captured_content_revision)) {
    return false;
  }

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
  if (!ApplyBatch(*viewport, batch, true, operations_.follow_applied_edit)) {
    return false;
  }

  if (request.has_selection) {
    const editor::TextPosition start =
        clamp_position(request.selection_start_line, request.selection_start_column);
    const editor::TextPosition end =
        clamp_position(request.selection_end_line, request.selection_end_column);
    viewport->JumpCursorTo(start.line, start.column, false);
    viewport->MoveCursorTo(end.line, end.column, true);
  } else if (request.has_cursor) {
    const editor::TextPosition cursor = clamp_position(request.cursor_line, request.cursor_column);
    viewport->JumpCursorTo(cursor.line, cursor.column, false);
  }

  operations_.reset_caret_blink();
  operations_.note_active_editable_edit();
  return true;
}

bool WorkspaceEditService::ApplyLspEdit(const std::vector<CodeActionEdit>& edits,
                                        bool* out_any_rejected) {
  return ApplyOpenBufferEdits(edits, true, out_any_rejected);
}

bool WorkspaceEditService::ApplyBufferEdits(const std::vector<CodeActionEdit>& edits) {
  return ApplyOpenBufferEdits(edits, false, nullptr);
}

bool WorkspaceEditService::ApplyOpenBufferEdits(const std::vector<CodeActionEdit>& edits,
                                                bool lsp_columns, bool* out_any_rejected) {
  const auto mark_rejected = [&]() {
    if (out_any_rejected != nullptr) {
      *out_any_rejected = true;
    }
  };
  if (edits.empty()) {
    return false;
  }

  struct Bucket {
    editor::TextViewport* viewport = nullptr;
    EditBatch edits;
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
    std::string language_id;
    LspClient* client = lsp_->LspClientForViewport(viewport, &language_id);
    const lsp_encoding::PositionEncoding encoding =
        client != nullptr ? LspEncodingForClient(*client) : lsp_encoding::PositionEncoding::Utf8;
    const std::size_t line_count = viewport.line_count();
    bool group_ok = true;
    const auto map_position = [&](editor::TextPosition pos) -> editor::TextPosition {
      if (line_count == 0) {
        group_ok = group_ok && pos.line == 0;
        return editor::TextPosition{0, 0};
      }
      if (pos.line == line_count && pos.column == 0) {
        return pos;
      }
      if (pos.line >= line_count) {
        group_ok = false;
        return pos;
      }
      if (lsp_columns) {
        pos.column = lsp_encoding::LspCharacterToByteColumn(viewport.lines().LineView(pos.line),
                                                            pos.column, encoding);
      }
      return pos;
    };
    std::size_t first_changed = std::numeric_limits<std::size_t>::max();
    std::size_t last_changed = 0;
    for (auto& [range, text] : bucket.edits) {
      range.start = map_position(range.start);
      range.end = map_position(range.end);
      const editor::SelectionRange normalized = editor::TextViewport::NormalizeRange(range);
      first_changed = std::min(first_changed, normalized.start.line);
      last_changed = std::max(last_changed, normalized.end.line);
    }
    if (!group_ok || !ApplyBatch(viewport, bucket.edits, true, operations_.follow_applied_edit)) {
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
    operations_.request_frame();
    active_synced = true;
  }
  if (!active_synced) {
    operations_.note_active_editable_edit();
  }
  operations_.request_frame();
  return true;
}

void WorkspaceEditService::ApplyRename(const std::string& new_name,
                                       const std::vector<CodeActionEdit>& edits,
                                       const std::vector<WorkspaceResourceOp>& resource_ops) {
  if (edits.empty() && resource_ops.empty()) {
    return;
  }
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
    ApplyLspEdit(edits);
    return;
  }

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

  if (!pending.resource_ops.empty() && !ApplyResourceOps(pending.resource_ops).ok) {
    operations_.notify_error("Rename aborted: file operations failed (rolled back)");
    return;
  }

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

  const DiskEditResult disk = ApplyToClosedFiles(pending.edits);
  saved += disk.files_written;

  std::string feedback = "Renamed to '" + pending.new_name + "' in " + std::to_string(saved) +
                         (saved == 1 ? " file" : " files");
  if (any_conflict || disk.any_failed) {
    feedback += " (some skipped)";
  }
  state.panel.feedback.text = std::move(feedback);
  operations_.request_frame();
}

bool WorkspaceEditService::IsOpen(const std::filesystem::path& normalized) const {
  return OpenEditorViewOfPath(context_->current_project_state.editor_groups, normalized) != nullptr;
}

std::vector<ClosedFileEditBucket> WorkspaceEditService::PrepareClosedFileBuckets(
    const std::vector<CodeActionEdit>& edits) const {
  std::vector<ClosedFileEditBucket> buckets;
  std::unordered_map<std::string, std::size_t> bucket_index;
  for (const CodeActionEdit& edit : edits) {
    if (edit.path.empty()) {
      continue;
    }
    std::filesystem::path normalized = edit.path.lexically_normal();
    if (IsOpen(normalized)) {
      continue;
    }
    const auto [slot, inserted] = bucket_index.emplace(normalized.generic_string(), buckets.size());
    if (inserted) {
      buckets.push_back(ClosedFileEditBucket{.path = std::move(normalized)});
    }
    buckets[slot->second].edits.emplace_back(edit.range, edit.new_text);
  }
  for (ClosedFileEditBucket& bucket : buckets) {
    const std::string language_id = editor::runtime_syntax::DetectFiletype(bucket.path);
    const LspClient* client = lsp_->CurrentLspManager().FindStartedServer(language_id);
    bucket.encoding =
        client != nullptr ? LspEncodingForClient(*client) : lsp_encoding::PositionEncoding::Utf8;
  }
  return buckets;
}

WorkspaceEditService::DiskEditResult WorkspaceEditService::RunClosedFileEdits(
    const std::vector<ClosedFileEditBucket>& buckets, project::FileWriteGate& gate) {
  DiskEditResult result;
  for (const ClosedFileEditBucket& bucket : buckets) {
    if (bucket.edits.empty()) {
      continue;
    }
    editor::TextViewport scratch;
    if (!scratch.OpenFile(bucket.path)) {
      result.any_failed = true;
      continue;
    }
    const std::size_t line_count = scratch.line_count();
    bool in_range = line_count > 0;
    const auto map_position = [&](editor::TextPosition pos) -> editor::TextPosition {
      if (pos.line == line_count && pos.column == 0) {
        const std::size_t last = line_count - 1;
        return editor::TextPosition{last, scratch.lines().LineLength(last)};
      }
      if (pos.line >= line_count) {
        in_range = false;
        return pos;
      }
      pos.column = lsp_encoding::LspCharacterToByteColumn(scratch.lines().LineView(pos.line),
                                                          pos.column, bucket.encoding);
      return pos;
    };
    EditBatch file_edits = bucket.edits;
    for (auto& [range, text] : file_edits) {
      if (!in_range) {
        break;
      }
      range.start = map_position(range.start);
      range.end = map_position(range.end);
    }
    if (!in_range || !ApplyBatch(scratch, file_edits, false) ||
        !scratch.Save(gate)) {
      result.any_failed = true;
      continue;
    }
    ++result.files_written;
    result.edits_applied += file_edits.size();
  }
  return result;
}

WorkspaceEditService::DiskEditResult WorkspaceEditService::ApplyToClosedFiles(
    const std::vector<CodeActionEdit>& edits) {
  return RunClosedFileEdits(PrepareClosedFileBuckets(edits),
                            context_->current_project_state.write_gate());
}

bool WorkspaceEditService::ApplyFull(const std::vector<CodeActionEdit>& edits,
                                     const std::vector<WorkspaceResourceOp>& resource_ops) {
  bool any_applied = false;
  if (!resource_ops.empty()) {
    const ResourceOpsResult ops_result = ApplyResourceOps(resource_ops);
    if (!ops_result.ok) {
      return false;
    }
    any_applied = ops_result.any_applied;
  }
  if (edits.empty()) {
    return any_applied;
  }
  const bool open_applied = ApplyLspEdit(edits);
  const DiskEditResult disk = ApplyToClosedFiles(edits);
  return any_applied || open_applied || disk.files_written > 0;
}

WorkspaceEditService::ResourceOpsResult WorkspaceEditService::ApplyResourceOps(
    const std::vector<WorkspaceResourceOp>& ops) {
  ResourceOpsResult result;
  if (ops.empty()) {
    return result;
  }
  namespace fs = std::filesystem;
  using Kind = WorkspaceResourceOp::Kind;
  ProjectWorkspaceState& state = context_->current_project_state;
  const fs::path project_root = state.root;

  std::unordered_map<std::string, bool> overlay;
  const auto target_exists = [&](const fs::path& p) {
    const auto it = overlay.find(p.generic_string());
    if (it != overlay.end()) {
      return it->second;
    }
    std::error_code ec;
    return fs::exists(p, ec);
  };
  const auto set_exists = [&](const fs::path& p, bool exists) {
    overlay[p.generic_string()] = exists;
  };
  const auto fail = [&](std::string message) {
    result.ok = false;
    result.any_applied = false;
    result.error = std::move(message);
    return result;
  };
  std::vector<char> skip(ops.size(), 0);
  for (std::size_t i = 0; i < ops.size(); ++i) {
    const WorkspaceResourceOp& op = ops[i];
    const fs::path target = op.path.lexically_normal();
    if (target.empty() || !util::PathEqualsOrWithinResolvingSymlinks(target, project_root)) {
      return fail("resource op target escapes the project root");
    }
    switch (op.kind) {
      case Kind::Create:
        if (target_exists(target) && !op.overwrite) {
          if (op.ignore_if_exists) {
            skip[i] = 1;
            break;
          }
          return fail("create target already exists: " + target.generic_string());
        }
        set_exists(target, true);
        break;
      case Kind::Rename: {
        const fs::path dest = op.new_path.lexically_normal();
        if (dest.empty() || !util::PathEqualsOrWithinResolvingSymlinks(dest, project_root)) {
          return fail("rename destination escapes the project root");
        }
        if (!target_exists(target)) {
          return fail("rename source does not exist: " + target.generic_string());
        }
        if (target_exists(dest) && !op.overwrite) {
          if (op.ignore_if_exists) {
            skip[i] = 1;
            break;
          }
          return fail("rename destination already exists: " + dest.generic_string());
        }
        set_exists(target, false);
        set_exists(dest, true);
        break;
      }
      case Kind::Delete: {
        if (!target_exists(target)) {
          if (op.ignore_if_not_exists) {
            skip[i] = 1;
            break;
          }
          return fail("delete target does not exist: " + target.generic_string());
        }
        std::error_code ec;
        if (!op.recursive && fs::is_directory(target, ec) && !fs::is_empty(target, ec)) {
          return fail("delete target is a non-empty directory (recursive not set): " +
                      target.generic_string());
        }
        set_exists(target, false);
        break;
      }
    }
  }

  using TreeOp = project::FileWriteGate::TreeOp;
  std::vector<TreeOp> tree_ops;
  tree_ops.reserve(ops.size());
  struct AppliedReconcile {
    bool is_rename = false;
    fs::path from;
    fs::path to;
  };
  std::vector<AppliedReconcile> applied_reconciles;
  fs::path last_mutated;
  for (std::size_t i = 0; i < ops.size(); ++i) {
    if (skip[i] != 0) {
      continue;
    }
    const WorkspaceResourceOp& op = ops[i];
    const fs::path target = op.path.lexically_normal();
    switch (op.kind) {
      case Kind::Create:
        tree_ops.push_back({.kind = TreeOp::Kind::CreateFile, .path = target, .overwrite = op.overwrite});
        last_mutated = target;
        break;
      case Kind::Rename: {
        const fs::path dest = op.new_path.lexically_normal();
        tree_ops.push_back(
            {.kind = TreeOp::Kind::Rename, .path = target, .new_path = dest, .overwrite = op.overwrite});
        applied_reconciles.push_back({true, target, dest});
        last_mutated = dest;
        break;
      }
      case Kind::Delete:
        tree_ops.push_back({.kind = TreeOp::Kind::Delete, .path = target, .recursive = op.recursive});
        applied_reconciles.push_back({false, target, {}});
        last_mutated = target.parent_path();
        break;
    }
  }
  if (tree_ops.empty()) {
    return result;
  }
  project::FileWriteGate::TreeResult applied = state.write_gate().ApplyTreeOps(tree_ops);
  if (!applied.ok) {
    return fail(std::move(applied.error_message));
  }
  result.any_applied = true;

  for (const AppliedReconcile& action : applied_reconciles) {
    if (action.is_rename) {
      util::InvokeIfSet(operations_.reconcile_tabs_after_resource_rename, action.from, action.to);
    } else util::InvokeIfSet(operations_.reconcile_tabs_after_resource_delete, action.from);
  }
  if (!applied.staged_directories.empty()) {
    if (operations_.dispose_staged_paths_async) {
      operations_.dispose_staged_paths_async(std::move(applied.staged_directories));
    } else {
      state.write_gate().DisposeStaged(applied.staged_directories);
    }
  }
  util::InvokeIfSet(operations_.refresh_views_after_resource_ops, last_mutated);
  return result;
}

bool WorkspaceEditService::ApplyServerEdit(const LspClient& client, LspClient::WorkspaceEdit edit) {
  if (!lsp_workspace_edit::VersionsCurrent(client, edit)) {
    return false;
  }
  bool any_resource_applied = false;
  if (!edit.resource_ops.empty()) {
    const std::optional<std::vector<WorkspaceResourceOp>> ops =
        lsp_workspace_edit::FlattenResourceOps(edit.resource_ops);
    if (!ops.has_value()) {
      return false;
    }
    const ResourceOpsResult ops_result = ApplyResourceOps(*ops);
    if (!ops_result.ok) {
      return false;
    }
    any_resource_applied = ops_result.any_applied;
  }
  std::vector<CodeActionEdit> flat;
  const std::filesystem::path project_root = context_->current_project_state.root;
  for (const auto& [uri, text_edits] : edit.changes) {
    const std::optional<std::filesystem::path> path = PathFromFileUri(uri);
    if (!path.has_value()) {
      continue;
    }
    const std::filesystem::path normalized = path->lexically_normal();
    if (!util::PathEqualsOrWithinResolvingSymlinks(normalized, project_root) && !IsOpen(normalized)) {
      continue;
    }
    for (const auto& [range, new_text] : text_edits) {
      flat.push_back(assist::ToCodeActionEdit(*path, range, new_text));
    }
  }
  if (flat.empty()) {
    return any_resource_applied;
  }
  bool open_rejected = false;
  const bool open_applied = ApplyLspEdit(flat, &open_rejected);
  std::vector<ClosedFileEditBucket> buckets = PrepareClosedFileBuckets(flat);
  const bool had_closed_edits = !buckets.empty();
  if (had_closed_edits) {
    if (operations_.run_closed_file_edits_async) {
      operations_.run_closed_file_edits_async(std::move(buckets));
    } else {
      RunClosedFileEdits(buckets, context_->current_project_state.write_gate());
    }
  }
  if (open_rejected) {
    return false;
  }
  return open_applied || had_closed_edits || any_resource_applied;
}

}
