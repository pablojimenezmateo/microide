#include "workspace/DebugWatchModel.h"

#include <utility>

namespace microide::workspace {

namespace {

// Watch expressions are re-evaluated (one DAP `evaluate` request each) on every
// stop and frame switch, so an unbounded list makes each stop arbitrarily
// expensive. A paste or control command could otherwise add thousands. These
// caps bound the per-stop cost and per-expression memory.
constexpr std::size_t kMaxWatchExpressions = 512;
constexpr std::size_t kMaxWatchExpressionLength = 4096;

std::string ClampExpression(std::string expression) {
  if (expression.size() > kMaxWatchExpressionLength) {
    expression.resize(kMaxWatchExpressionLength);
  }
  return expression;
}

}  // namespace

void DebugWatchModel::SetExpressions(std::vector<std::string> expressions) {
  if (expressions.size() > kMaxWatchExpressions) {
    expressions.resize(kMaxWatchExpressions);
  }
  for (std::string& expression : expressions) {
    expression = ClampExpression(std::move(expression));
  }
  expressions_ = std::move(expressions);
  BeginEvaluation();
}

std::size_t DebugWatchModel::AddExpression(std::string expression) {
  if (expression.empty() || expressions_.size() >= kMaxWatchExpressions) {
    return expressions_.size();
  }
  expressions_.push_back(ClampExpression(std::move(expression)));
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
  expressions_[index] = ClampExpression(std::move(expression));
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
  needs_placeholder_rebuild_ = false;
}

void DebugWatchModel::ApplyEvaluate(std::size_t index,
                                    const dap_protocol::DapEvaluateResult& result) {
  if (index >= expression_root_ids_.size()) {
    return;
  }
  tree_.SetNodeValue(expression_root_ids_[index], result.result, result.type,
                     result.variables_reference);
  needs_placeholder_rebuild_ = true;
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
