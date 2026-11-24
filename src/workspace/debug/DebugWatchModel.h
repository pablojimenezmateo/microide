#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "editor/SingleLineEditor.h"
#include "workspace/debug/DapProtocol.h"
#include "workspace/debug/DebugValueTreePane.h"

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
class DebugWatchModel : public DebugValueTreePane {
 public:
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

  // Row depth 0 is an expression root (edited as a string via a prompt); deeper
  // rows are evaluated children (edited inline via setVariable). Returns the
  // expression index for a root row, else nullopt.
  std::optional<std::size_t> ExpressionIndexForRow(std::size_t row_index) const;

  // Clear only the evaluated tree (keeps expressions); used on resume/stop.
  void ClearResults();

 private:
  std::vector<std::string> expressions_;
  std::vector<std::uint32_t> expression_root_ids_;  // parallel to expressions_
  // Set by ApplyEvaluate (a value was folded in), cleared by BeginEvaluation (the
  // tree is pristine placeholders again). See NeedsPlaceholderRebuild().
  bool needs_placeholder_rebuild_ = false;
};

}  // namespace microide::workspace
