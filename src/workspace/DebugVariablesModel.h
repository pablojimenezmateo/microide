#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "editor/SingleLineEditor.h"
#include "workspace/DapProtocol.h"
#include "workspace/DebugValueTree.h"

namespace microide::workspace {

// The lazy variables tree for the focused stack frame. A thin wrapper over the
// shared DebugValueTree (which owns the node store, flatten, lazy-expand and
// inline-edit machinery shared with the Watch panel); this model adds only the
// scope/frame semantics. Lives on ProjectWorkspaceState next to
// `debug_execution`; transient — rebuilt on each `stopped`/frame focus, cleared
// on resume/stop. Never persisted.
//
// The model performs no I/O: DebugService drives `scopes`/`variables`/
// `setVariable` requests and feeds responses back through Apply*.
class DebugVariablesModel {
 public:
  using EditTarget = DebugValueTree::EditTarget;

  // Start a fresh tree for a focused frame. Clears everything; the service then
  // requests scopes for `frame_id`.
  void BeginFrame(int frame_id) {
    tree_.Clear();
    frame_id_ = frame_id;
  }
  int FrameId() const { return frame_id_; }

  // Install the focused frame's scopes as collapsed top-level rows, re-expanding
  // any the user had open before this stop. Returns the bounded fetches the
  // service must issue to repopulate those restored containers.
  std::vector<DebugValueTree::ChildFetch> ApplyScopes(
      const std::vector<dap_protocol::DapScope>& scopes);

  // Toggle expansion / load-more for the row at `row_index` (see
  // DebugValueTree::ToggleRow).
  DebugValueTree::ChildFetch ToggleRow(std::size_t row_index) { return tree_.ToggleRow(row_index); }

  // Attach fetched children; returns the bounded fetches for any restored
  // descendants the user had expanded (empty in the common case).
  std::vector<DebugValueTree::ChildFetch> ApplyVariables(
      int variables_reference, const std::vector<dap_protocol::DapVariable>& variables, int start) {
    return tree_.ApplyVariables(variables_reference, variables, start);
  }

  void MarkChildrenError(int variables_reference) { tree_.MarkChildrenError(variables_reference); }

  void ApplySetVariable(std::uint32_t node_id, const dap_protocol::DapSetVariableResult& result) {
    tree_.ApplySetVariable(node_id, result);
  }

  const std::vector<DebugVariableRowView>& Rows() const { return tree_.Rows(); }
  bool Empty() const { return tree_.Empty(); }

  std::size_t SelectedRow() const { return tree_.SelectedRow(); }
  void SetSelectedRow(std::size_t row) { tree_.SetSelectedRow(row); }
  void MoveSelection(int delta) { tree_.MoveSelection(delta); }

  bool BeginEdit(std::size_t row_index) { return tree_.BeginEdit(row_index); }
  void CancelEdit() { tree_.CancelEdit(); }
  bool IsEditing() const { return tree_.IsEditing(); }
  std::optional<std::uint32_t> EditingNodeId() const { return tree_.EditingNodeId(); }
  std::optional<EditTarget> EditTargetForCommit() const { return tree_.EditTargetForCommit(); }
  editor::SingleLineEditor& EditBuffer() { return tree_.EditBuffer(); }
  const editor::SingleLineEditor& EditBuffer() const { return tree_.EditBuffer(); }

  void Clear() {
    tree_.Clear();
    frame_id_ = 0;
  }

 private:
  DebugValueTree tree_;
  int frame_id_ = 0;
};

}  // namespace microide::workspace
