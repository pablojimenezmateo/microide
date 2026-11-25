#pragma once

#include <optional>
#include <vector>

#include "workspace/lsp/WorkspaceLspClient.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

namespace lsp_workspace_edit {

std::optional<std::vector<WorkspaceResourceOp>> FlattenResourceOps(
    const std::vector<LspClient::WorkspaceEdit::ResourceOp>& ops);

bool VersionsCurrent(const LspClient& client, const LspClient::WorkspaceEdit& edit);

}

}
