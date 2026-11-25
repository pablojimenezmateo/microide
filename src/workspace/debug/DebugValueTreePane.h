#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "editor/SingleLineEditor.h"
#include "workspace/debug/DapProtocol.h"
#include "workspace/debug/DebugValueTree.h"

namespace microide::workspace {

class DebugValueTreePane {
 public:
  using EditTarget = DebugValueTree::EditTarget;

  DebugValueTree::ChildFetch ToggleRow(std::size_t row_index) { return tree_.ToggleRow(row_index); }
  [[nodiscard]] std::vector<DebugValueTree::ChildFetch> ApplyVariables(
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

 protected:
  DebugValueTreePane() = default;
  ~DebugValueTreePane() = default;
  DebugValueTreePane(const DebugValueTreePane&) = default;
  DebugValueTreePane& operator=(const DebugValueTreePane&) = default;
  DebugValueTreePane(DebugValueTreePane&&) = default;
  DebugValueTreePane& operator=(DebugValueTreePane&&) = default;

  DebugValueTree tree_;
};

}
