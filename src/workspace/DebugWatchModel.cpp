#include "workspace/DebugWatchModel.h"

#include <utility>

namespace microide::workspace {

void DebugWatchModel::SetExpressions(std::vector<std::string> expressions) {
  expressions_ = std::move(expressions);
  BeginEvaluation();
}

std::size_t DebugWatchModel::AddExpression(std::string expression) {
  if (expression.empty()) {
    return expressions_.size();
  }
  expressions_.push_back(std::move(expression));
  const std::size_t index = expressions_.size() - 1;
  BeginEvaluation();
  return index;
}

void DebugWatchModel::EditExpression(std::size_t index, std::string expression) {
  if (index >= expressions_.size()) {
    return;
  }
  if (expression.empty()) {
    RemoveExpression(index);
    return;
  }
  expressions_[index] = std::move(expression);
  BeginEvaluation();
}

void DebugWatchModel::RemoveExpression(std::size_t index) {
  if (index >= expressions_.size()) {
    return;
  }
  expressions_.erase(expressions_.begin() + static_cast<std::ptrdiff_t>(index));
  BeginEvaluation();
}

void DebugWatchModel::BeginEvaluation() {
  tree_.ClearRoots();
  expression_root_ids_.clear();
  expression_root_ids_.reserve(expressions_.size());
  for (const std::string& expression : expressions_) {
    expression_root_ids_.push_back(
        tree_.AddRoot(expression, /*value=*/{}, /*type=*/{}, /*variables_reference=*/0,
                      /*is_scope=*/false));
  }
  tree_.Rebuild();
}

void DebugWatchModel::ApplyEvaluate(std::size_t index,
                                    const dap_protocol::DapEvaluateResult& result) {
  if (index >= expression_root_ids_.size()) {
    return;
  }
  tree_.SetNodeValue(expression_root_ids_[index], result.result, result.type,
                     result.variables_reference);
}

std::optional<std::size_t> DebugWatchModel::ExpressionIndexForRow(std::size_t row_index) const {
  const std::vector<DebugVariableRowView>& rows = tree_.Rows();
  if (row_index >= rows.size() || rows[row_index].depth != 0) {
    return std::nullopt;
  }
  const std::uint32_t node_id = rows[row_index].node_id;
  for (std::size_t i = 0; i < expression_root_ids_.size(); ++i) {
    if (expression_root_ids_[i] == node_id) {
      return i;
    }
  }
  return std::nullopt;
}

void DebugWatchModel::ClearResults() {
  // Keep the expressions visible (with blank values) while running; only the
  // evaluated values are transient.
  BeginEvaluation();
}

}  // namespace microide::workspace
