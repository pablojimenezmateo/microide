#include "workspace/DebugVariablesModel.h"

namespace microide::workspace {

void DebugVariablesModel::ApplyScopes(const std::vector<dap_protocol::DapScope>& scopes) {
  tree_.ClearRoots();
  for (const dap_protocol::DapScope& scope : scopes) {
    tree_.AddRoot(scope.name, /*value=*/{}, /*type=*/{}, scope.variables_reference,
                  /*is_scope=*/true);
  }
  tree_.Rebuild();
}

}  // namespace microide::workspace
