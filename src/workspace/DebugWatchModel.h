#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "editor/SingleLineEditor.h"
#include "workspace/DapProtocol.h"
#include "workspace/DebugValueTree.h"

namespace microide::workspace {

// The Watch panel model. A persistent ordered list of watch *expressions* plus a
// transient evaluated value tree (the shared DebugValueTree). On each `stopped`
// (and call-stack frame switch) DebugService re-evaluates every expression with
// `evaluate(context:"watch")` and folds the result onto that expression's
// pre-created root, so rows stay stable and ordered while results stream in. A
// structured result (variablesReference > 0) is expandable via the shared tree.
//
// Unlike DebugVariablesModel, the expression list is NOT frame-scoped: it
// survives steps, frame switches and session restarts (persisted in the project
// `debug` record). Only the evaluated tree is cleared on resume/stop.
class DebugWatchModel {
 public:
  using EditTarget = DebugValueTree::EditTarget;

  const std::vector<std::string>& Expressions() const { return expressions_; }
  bool HasExpressions() const { return !expressions_.empty(); }

  // Replace the whole list (persistence restore). Drops any evaluated tree.
  void SetExpressions(std::vector<std::string> expressions);
  // Append a (non-empty) expression; returns its index. Pre-creates its root so
  // it shows immediately even before the next evaluate.
  std::size_t AddExpression(std::string expression);
  // Edit / remove by index (no-ops when out of range). Both clear the evaluated
  // tree so a stale value never shows against a changed/removed expression.
  void EditExpression(std::size_t index, std::string expression);
  void RemoveExpression(std::size_t index);

  // Begin a fresh evaluation pass: rebuild one placeholder root per expression
  // (clearing prior results) so DebugService can fold values in by index.
  void BeginEvaluation();
  // Fold an evaluate result onto the expression at `index`.
  void ApplyEvaluate(std::size_t index, const dap_protocol::DapEvaluateResult& result);

  // True once a value has been folded in since the last BeginEvaluation, i.e. the
  // placeholder tree is no longer pristine. DebugService::EvaluateWatches uses this
  // to skip a redundant BeginEvaluation right after a model mutation already
  // rebuilt the tree (mutations self-rebuild for standalone/persistence use), while
  // still rebuilding on the stop/frame-switch path where prior values are present.
  bool NeedsPlaceholderRebuild() const { return needs_placeholder_rebuild_; }

  // Tree pass-throughs (lazy expand + child setVariable edit reuse the shared
  // DebugValueTree verbatim).
  DebugValueTree::ChildFetch ToggleRow(std::size_t row_index) { return tree_.ToggleRow(row_index); }
  // Returns the cascade fetches DebugValueTree::ApplyVariables produced (auto-expand of
  // a freshly-attached child whose path is still remembered in expanded_paths_). The
  // caller MUST issue them, exactly like the Variables pane does — dropping them leaves
  // an auto-expanded nested child stuck on the "loading…" placeholder forever.
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

  // Row depth 0 is an expression root (edited as a string via a prompt); deeper
  // rows are evaluated children (edited inline via setVariable). Returns the
  // expression index for a root row, else nullopt.
  std::optional<std::size_t> ExpressionIndexForRow(std::size_t row_index) const;

  // Clear only the evaluated tree (keeps expressions); used on resume/stop.
  void ClearResults();

 private:
  DebugValueTree tree_;
  std::vector<std::string> expressions_;
  std::vector<std::uint32_t> expression_root_ids_;  // parallel to expressions_
  // Set by ApplyEvaluate (a value was folded in), cleared by BeginEvaluation (the
  // tree is pristine placeholders again). See NeedsPlaceholderRebuild().
  bool needs_placeholder_rebuild_ = false;
};

}  // namespace microide::workspace
