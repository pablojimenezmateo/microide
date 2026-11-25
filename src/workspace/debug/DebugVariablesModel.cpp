#include "workspace/debug/DebugVariablesModel.h"

#include <algorithm>
#include <limits>

#include "util/DebugTrace.h"

namespace microide::workspace {

std::vector<DebugValueTree::ChildFetch> DebugVariablesModel::ApplyScopes(
    const std::vector<dap_protocol::DapScope>& scopes) {
  util::DebugTrace::Note("locals", "apply-scopes count",
                         static_cast<long long>(scopes.size()));
  tree_.ClearRoots();
  for (const dap_protocol::DapScope& scope : scopes) {
    util::DebugTrace::Note("locals", "scope", scope.name,
                           static_cast<long long>(scope.variables_reference));
    const long long scope_total = std::max<long long>(0, scope.named_variables) +
                                  std::max<long long>(0, scope.indexed_variables);
    tree_.AddRoot(scope.name, {}, {}, scope.variables_reference,
                  true,
                  static_cast<int>(std::min<long long>(scope_total,
                                                       std::numeric_limits<int>::max())),
                  scope.count_reported);
  }
  std::vector<DebugValueTree::ChildFetch> fetches = tree_.RestoreExpandedRoots();
  tree_.Rebuild();
  return fetches;
}

}
