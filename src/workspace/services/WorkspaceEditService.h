#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "plugin/PluginHost.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

class LspService;

// Applies multi-edit requests to the open buffers: a plugin's
// `ctx.editor.apply_edits`, an LSP WorkspaceEdit (code action, formatting,
// server-initiated applyEdit), and a project-wide rename, which may need the
// user's confirmation before it writes files that are not open. Every path
// shares one discipline: resolve already-open buffers only (v1 never edits a
// file on disk behind an open tab, so undo stays coherent), apply each buffer's
// edits highest-position-first as one undo step, and refuse overlapping edits.
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

 private:
  using EditBatch = std::vector<std::pair<editor::SelectionRange, std::string>>;

  // An empty path (or one naming the active buffer) is the active editable buffer;
  // any other path must already be open in an editor tab.
  editor::TextViewport* ResolveOpenViewport(const std::filesystem::path& path) const;
  // Highest position first, with the tie rules in editor/EditBatchOrder.h (same
  // start -> the replace before the inserts, same range -> later entry first, so N
  // inserts at one position keep their order), as one undo step. Overlapping
  // ranges have no single meaning: the batch is refused and nothing is applied.
  static bool ApplyBatch(editor::TextViewport& viewport, const EditBatch& batch);
  void MarkFoldsDirty(const editor::TextViewport& edited);

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
