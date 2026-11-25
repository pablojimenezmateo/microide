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

class DebugWatchModel : public DebugValueTreePane {
 public:
  const std::vector<std::string>& Expressions() const { return expressions_; }
  bool HasExpressions() const { return !expressions_.empty(); }

  void SetExpressions(std::vector<std::string> expressions);
  std::size_t AddExpression(std::string expression);
  void EditExpression(std::size_t index, std::string expression);
  void RemoveExpression(std::size_t index);

  void BeginEvaluation();
  void ApplyEvaluate(std::size_t index, const dap_protocol::DapEvaluateResult& result);

  bool NeedsPlaceholderRebuild() const { return needs_placeholder_rebuild_; }

  std::optional<std::size_t> ExpressionIndexForRow(std::size_t row_index) const;

  void ClearResults();

 private:
  std::vector<std::string> expressions_;
  std::vector<std::uint32_t> expression_root_ids_;
  bool needs_placeholder_rebuild_ = false;
};

}
