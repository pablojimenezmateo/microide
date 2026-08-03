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
    // Sum in 64-bit with negatives clamped and the result capped at INT_MAX,
    // mirroring DebugValueTree::ApplyVariables: both fields are ints truncated
    // from the adapter's JSON int64, so a bare int+int here would overflow (UB)
    // and a wrapped-negative total would corrupt the paging math.
    const long long scope_total = std::max<long long>(0, scope.named_variables) +
                                  std::max<long long>(0, scope.indexed_variables);
    tree_.AddRoot(scope.name, /*value=*/{}, /*type=*/{}, scope.variables_reference,
                  /*is_scope=*/true,
                  static_cast<int>(std::min<long long>(scope_total,
                                                       std::numeric_limits<int>::max())),
                  /*total_known=*/scope.count_reported);
  }
  // Re-open whatever the user had expanded before this stop (refs are not stable
  // across stops, so expansion is path-tracked); the service issues the fetches.
  std::vector<DebugValueTree::ChildFetch> fetches = tree_.RestoreExpandedRoots();
  tree_.Rebuild();
  return fetches;
}

}  // namespace microide::workspace
