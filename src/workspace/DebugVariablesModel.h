#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "editor/SingleLineEditor.h"
#include "workspace/DapProtocol.h"

namespace microide::workspace {

// One flattened, render-ready row of the Variables tree. Display strings are
// prebuilt (off the render hot path) so the bottom-panel render TU only draws
// them. `node_id` is the stable identity used to match the in-edit row and to
// route expand/edit actions; it survives flat-list rebuilds caused by an
// unrelated sibling expanding.
struct DebugVariableRowView {
  std::string display_name;   // e.g. "x" or "Locals"
  std::string display_value;  // e.g. "42" (empty for scopes)
  std::string display_type;   // e.g. "int" (muted, optional)
  int depth = 0;              // 0 = scope, 1+ = nested variables
  bool has_children = false;  // structured (variablesReference > 0) → expandable
  bool expanded = false;
  bool editable = false;  // a real variable (not a scope) → can enter setVariable edit
  std::uint32_t node_id = 0;
};

// The lazy variables tree for the focused stack frame. Source of truth is a tree
// of nodes keyed by a stable monotonic id; a prebuilt flat row list is
// rematerialized whenever the tree changes so render/click are O(1) by row index
// (mirroring `debug_execution.frames`). Lives on ProjectWorkspaceState next to
// `debug_execution`; transient — rebuilt on each `stopped`/frame focus, cleared
// on resume/stop. Never persisted.
//
// The model itself performs no I/O: DebugService drives `scopes`/`variables`/
// `setVariable` requests and feeds responses back through Apply*.
class DebugVariablesModel {
 public:
  // The variable currently being edited inline: the container reference + name
  // that setVariable needs, plus the node id to apply the result back onto.
  struct EditTarget {
    std::uint32_t node_id = 0;
    int container_reference = 0;
    std::string name;
  };

  // Start a fresh tree for a focused frame. Clears everything; the service then
  // requests scopes for `frame_id`.
  void BeginFrame(int frame_id);
  int FrameId() const { return frame_id_; }

  // Install the focused frame's scopes as collapsed top-level rows.
  void ApplyScopes(const std::vector<dap_protocol::DapScope>& scopes);

  // Toggle expansion of the row at `row_index`. Returns the variablesReference
  // that must be fetched (>0) when expanding a not-yet-loaded node; otherwise 0
  // (collapse, leaf, or already-loaded — the flat list is rebuilt in place).
  int ToggleRow(std::size_t row_index);

  // Attach the children fetched for `variables_reference` to its owning node.
  void ApplyVariables(int variables_reference,
                      const std::vector<dap_protocol::DapVariable>& variables);

  // Reflect a setVariable response onto the edited node (value/type, and a
  // possibly-new structure reference).
  void ApplySetVariable(std::uint32_t node_id, const dap_protocol::DapSetVariableResult& result);

  const std::vector<DebugVariableRowView>& Rows() const { return rows_; }
  bool Empty() const { return rows_.empty(); }

  // Keyboard selection cursor (mouse click sets it directly too).
  std::size_t SelectedRow() const { return selected_row_; }
  void SetSelectedRow(std::size_t row);
  void MoveSelection(int delta);

  // Inline edit lifecycle. BeginEdit seeds the buffer with the node's current
  // value and selects all; it fails for scopes or out-of-range rows.
  bool BeginEdit(std::size_t row_index);
  void CancelEdit();
  bool IsEditing() const { return editing_node_.has_value(); }
  std::optional<std::uint32_t> EditingNodeId() const { return editing_node_; }
  std::optional<EditTarget> EditTargetForCommit() const;
  editor::SingleLineEditor& EditBuffer() { return edit_buffer_; }
  const editor::SingleLineEditor& EditBuffer() const { return edit_buffer_; }

  void Clear();

 private:
  struct Node {
    std::uint32_t id = 0;
    std::string name;
    std::string value;
    std::string type;
    int variables_reference = 0;       // this node's own container ref (children)
    int container_reference = 0;       // the ref of the container holding this node (for setVariable)
    bool is_scope = false;
    bool expanded = false;
    bool children_loaded = false;
    std::vector<std::uint32_t> children;
  };

  Node* FindNode(std::uint32_t id);
  Node* FindNodeByReference(int variables_reference);
  std::uint32_t AddNode(Node node);
  void Flatten();
  void FlattenInto(std::uint32_t node_id, int depth);

  std::unordered_map<std::uint32_t, Node> nodes_;
  std::unordered_map<int, std::uint32_t> reference_to_node_;  // variables_reference → node id
  std::vector<std::uint32_t> roots_;                          // scope node ids, in order
  std::vector<DebugVariableRowView> rows_;
  std::uint32_t next_id_ = 1;
  int frame_id_ = 0;
  std::size_t selected_row_ = 0;
  std::optional<std::uint32_t> editing_node_;
  editor::SingleLineEditor edit_buffer_;
};

}  // namespace microide::workspace
