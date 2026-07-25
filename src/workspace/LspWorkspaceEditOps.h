#pragma once

#include <optional>
#include <vector>

#include "workspace/WorkspaceLspClient.h"
#include "workspace/WorkspaceProjectState.h"

namespace microide::workspace {

// Single home for turning a parsed WorkspaceEdit's protocol-level pieces into the
// host-level types the appliers consume. Three call sites need this — the
// server-initiated applyEdit handler, inline code-action edits, and rename
// results — and each one used to carry its own copy of the URI decode + kind
// mapping, so a fix to one silently left the others behind.
namespace lsp_workspace_edit {

// Resolve every resource op's URI(s) into filesystem paths, preserving array
// order. Returns nullopt when ANY op carries an undecodable URI (a rename needs
// both): the caller must then refuse the WHOLE edit — applying a partial op list
// would leave the workspace in a state no later edit is keyed to.
std::optional<std::vector<WorkspaceResourceOp>> FlattenResourceOps(
    const std::vector<LspClient::WorkspaceEdit::ResourceOp>& ops);

// True when every versioned TextDocumentEdit in `edit` matches the version
// `client` currently tracks for that document (documents the client does not have
// open pass — there is no local version to conflict with). False means the server
// computed the edit against text the user has since changed, and LSP requires the
// client to fail the whole request.
bool VersionsCurrent(const LspClient& client, const LspClient::WorkspaceEdit& edit);

}  // namespace lsp_workspace_edit

}  // namespace microide::workspace
