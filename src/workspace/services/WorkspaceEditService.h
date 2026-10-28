#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "plugin/PluginHost.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/lsp/LspPositionEncoding.h"
#include "workspace/lsp/WorkspaceLspClient.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::project {
class FileWriteGate;
}

namespace microide::workspace {

class LspService;

// A closed-file target of a WorkspaceEdit: the file's ranged edits plus the
// server's negotiated position encoding for mapping them. Prepared on the main
// thread (path/encoding resolution needs the shared syntax registry + LSP manager);
// WorkspaceEditService::RunClosedFileEdits then loads/applies/saves it, off the
// shell thread for a server-initiated edit (TD-2026-07-17-011 / TD-2026-07-16-18).
struct ClosedFileEditBucket {
  std::filesystem::path path;
  std::vector<std::pair<editor::SelectionRange, std::string>> edits;
  lsp_encoding::PositionEncoding encoding = lsp_encoding::PositionEncoding::Utf8;
};

// Applies every multi-edit request: a plugin's `ctx.editor.apply_edits`, an LSP
// WorkspaceEdit (code action, formatting, the server-initiated workspace/applyEdit),
// its file resource ops, and a project-wide rename, which may need the user's
// confirmation before it writes files that are not open. One discipline throughout:
// an open file is edited in its buffer (never on disk behind an open tab, so undo
// stays coherent) and a closed one through a scratch viewport saved in place; each
// file's edits apply highest-position-first as one step, and overlapping edits or a
// target beyond EOF reject that file's whole group.
class WorkspaceEditService {
 public:
  struct Operations {
    std::function<editor::TextViewport*()> active_editable_viewport;
    std::function<void()> reset_caret_blink;
    std::function<void()> request_active_editable_last_change_redraw;
    std::function<void()> request_chrome_redraw;
    std::function<void()> request_focused_editor_redraw;
    // Damage [start_line, end_line) of the active editable surface (an editor,
    // a compare tab's right side, a merge result).
    std::function<void(std::size_t, std::size_t)> request_active_editable_line_range_redraw;
    std::function<void(bool include_tree_sidebar)> request_active_tab_redraw;
    // Ask the user to confirm the pending rename; `detail` says what it changes.
    std::function<void(std::string detail)> confirm_rename_save;
    std::function<void(std::string message)> notify_error;
    std::function<void(const std::filesystem::path&)> notify_buffer_saved;
    // Run a server-initiated edit's closed-file buckets off the shell thread (a
    // server rename can touch thousands of closed files) through RunClosedFileEdits;
    // the file-index watcher picks the changes up. Null in headless setups, where
    // they apply synchronously.
    std::function<void(std::vector<ClosedFileEditBucket>)> run_closed_file_edits_async;
    // Reconcile shell state after a resource op renamed / deleted a path on disk:
    // retarget or close open tabs, diagnostics, and plugin decorations. Null in
    // headless setups (disk-only apply).
    std::function<void(const std::filesystem::path&, const std::filesystem::path&)>
        reconcile_tabs_after_resource_rename;
    std::function<void(const std::filesystem::path&)> reconcile_tabs_after_resource_delete;
    // Refresh the file tree / project search once after a resource-op batch.
    std::function<void(const std::filesystem::path&)> refresh_views_after_resource_ops;
    // Dispose the staged backups of applied delete/overwrite ops off the shell
    // thread (removing a staged directory can be slow). Null: synchronously.
    std::function<void(std::vector<std::filesystem::path>)> dispose_staged_paths_async;
  };

  void Configure(WorkspaceContext& context, LspService& lsp, Operations operations);

  // Applies a plugin `ctx.editor.apply_edits` request: resolves the target
  // viewport (active editable buffer, or a named open editor buffer), validates
  // and clamps every edit, applies them as one grouped-undo step, then optionally
  // moves the caret / sets the selection. Returns true when a buffer was edited.
  bool ApplyPluginEdit(const plugin::PluginHost::WorkspaceEditRequest& request);
  // Apply an inline WorkspaceEdit (0-based, LSP-native coordinates) to the open
  // buffers. `out_any_rejected` (optional) is set true when a resolved buffer's edit
  // group was dropped without applying (a line beyond EOF, or an overlapping group),
  // so the server-initiated workspace/applyEdit response can report partial failure.
  bool ApplyLspEdit(const std::vector<CodeActionEdit>& edits, bool* out_any_rejected = nullptr);
  // Apply an LSP rename result. If every affected file is already open (and no
  // resource ops), applies in place (dirty). If some are closed — or the rename
  // carries file create/rename/delete ops — stashes the edit and confirms before
  // applying ops, edits, and saves — a project-wide rename that stays undo-coherent.
  void ApplyRename(const std::string& new_name, const std::vector<CodeActionEdit>& edits,
                   const std::vector<WorkspaceResourceOp>& resource_ops);
  // Apply and save the pending rename after the user confirms (or discard it on
  // cancel). No-op when nothing is pending.
  void CommitPendingRename();
  void DiscardPendingRename() { pending_rename_.reset(); }

  // Outcome of a silent on-disk WorkspaceEdit application (VSCode-style rename
  // across unopened files), for a feedback summary.
  struct DiskEditResult {
    std::size_t files_written = 0;
    std::size_t edits_applied = 0;
    bool any_failed = false;
  };
  // Apply the edits of `edits` whose files are NOT open, directly on disk: each
  // file loads into a scratch viewport (line-ending / BOM / encoding detection and
  // the atomic, permission-preserving save are reused), its edits map through the
  // server's position encoding, and it is saved — no tab opens. Synchronous.
  DiskEditResult ApplyToClosedFiles(const std::vector<CodeActionEdit>& edits);
  // Load/apply/save each prepared bucket. Touches only its arguments and the
  // filesystem, so it is safe on a background thread; `gate` is the project's
  // write gate, captured on the shell thread by whoever dispatches this.
  static DiskEditResult RunClosedFileEdits(const std::vector<ClosedFileEditBucket>& buckets,
                                           project::FileWriteGate& gate);

  // Outcome of applying a WorkspaceEdit's file resource ops.
  struct ResourceOpsResult {
    bool ok = true;            // every op applied (or was skipped per its ignore option)
    bool any_applied = false;  // at least one op mutated the filesystem
    std::string error;         // first failure, for the log / notification
  };
  // Validate + apply `ops` in order as one all-or-nothing batch (TD-2026-07-17-011):
  // every op's preconditions are checked against a simulated overlay BEFORE anything
  // mutates, and the write gate journals and unwinds the batch on an I/O failure.
  // Every target must live inside the project root — an op that escapes it fails
  // the whole batch. On success, open tabs/diagnostics are reconciled per op and
  // the project views refresh once. Main thread only.
  ResourceOpsResult ApplyResourceOps(const std::vector<WorkspaceResourceOp>& ops);
  // A client-initiated WorkspaceEdit that carries resource ops: the ops first
  // (nothing applies if they fail), then the text edits — open buffers in place,
  // closed files on disk (an ops-carrying edit typically fills the file it just
  // created, which no buffer has open).
  bool ApplyFull(const std::vector<CodeActionEdit>& edits,
                 const std::vector<WorkspaceResourceOp>& resource_ops);
  // A server-initiated workspace/applyEdit: versioned edits are gated against
  // `client`'s tracked document versions (stale → the whole edit fails), resource
  // ops run first, then open buffers edit in place and closed files are written in
  // the background. Only targets inside the project (or open in it) are touched.
  // Returns whether the edit was accepted.
  bool ApplyServerEdit(const LspClient& client, LspClient::WorkspaceEdit edit);

 private:
  using EditBatch = std::vector<std::pair<editor::SelectionRange, std::string>>;

  // An empty path (or one naming the active buffer) is the active editable buffer;
  // any other path must already be open in an editor tab.
  editor::TextViewport* ResolveOpenViewport(const std::filesystem::path& path) const;
  // Highest position first, with the tie rules in editor/EditBatchOrder.h (same
  // start -> the replace before the inserts, same range -> later entry first, so N
  // inserts at one position keep their order), as one undo step. Overlapping
  // ranges have no single meaning: the batch is refused and nothing is applied.
  // `record_undo` is false for a scratch viewport that is saved and discarded.
  static bool ApplyBatch(editor::TextViewport& viewport, const EditBatch& batch,
                         bool record_undo = true);
  void MarkFoldsDirty(const editor::TextViewport& edited);
  bool IsOpen(const std::filesystem::path& normalized) const;
  // Group `edits` by closed-file path (dropping open paths and the active buffer)
  // and resolve each file's server position encoding — main-thread work; the heavy
  // load/apply/save is RunClosedFileEdits.
  std::vector<ClosedFileEditBucket> PrepareClosedFileBuckets(const std::vector<CodeActionEdit>& edits) const;

  struct PendingRename {
    std::string new_name;
    std::vector<CodeActionEdit> edits;
    std::vector<std::filesystem::path> affected_paths;
    std::vector<WorkspaceResourceOp> resource_ops;
  };

  WorkspaceContext* context_ = nullptr;
  LspService* lsp_ = nullptr;
  Operations operations_{};
  std::optional<PendingRename> pending_rename_;
};

}  // namespace microide::workspace
