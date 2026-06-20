#include "workspace/DebugVariablesModel.h"

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
    tree_.AddRoot(scope.name, /*value=*/{}, /*type=*/{}, scope.variables_reference,
                  /*is_scope=*/true, scope.named_variables + scope.indexed_variables,
                  /*total_known=*/scope.count_reported);
  }
  // Re-open whatever the user had expanded before this stop (refs are not stable
  // across stops, so expansion is path-tracked); the service issues the fetches.
  std::vector<DebugValueTree::ChildFetch> fetches = tree_.RestoreExpandedRoots();
  tree_.Rebuild();
  return fetches;
}

}  // namespace microide::workspace
