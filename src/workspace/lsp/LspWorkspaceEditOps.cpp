#include "workspace/lsp/LspWorkspaceEditOps.h"

#include <filesystem>
#include <utility>

#include "workspace/FileUri.h"

namespace microide::workspace::lsp_workspace_edit {

namespace {

using ProtocolOp = LspClient::WorkspaceEdit::ResourceOp;

WorkspaceResourceOp::Kind ToHostKind(ProtocolOp::Kind kind) {
  switch (kind) {
    case ProtocolOp::Kind::Create:
      return WorkspaceResourceOp::Kind::Create;
    case ProtocolOp::Kind::Rename:
      return WorkspaceResourceOp::Kind::Rename;
    case ProtocolOp::Kind::Delete:
      return WorkspaceResourceOp::Kind::Delete;
  }
  return WorkspaceResourceOp::Kind::Create;
}

}  // namespace

std::optional<std::vector<WorkspaceResourceOp>> FlattenResourceOps(
    const std::vector<ProtocolOp>& ops) {
  std::vector<WorkspaceResourceOp> resolved;
  resolved.reserve(ops.size());
  for (const ProtocolOp& op : ops) {
    const std::optional<std::filesystem::path> path = PathFromFileUri(op.uri);
    if (!path.has_value()) {
      return std::nullopt;
    }
    std::optional<std::filesystem::path> new_path;
    if (op.kind == ProtocolOp::Kind::Rename) {
      new_path = PathFromFileUri(op.new_uri);
      if (!new_path.has_value()) {
        return std::nullopt;
      }
    }
    resolved.push_back(WorkspaceResourceOp{
        .kind = ToHostKind(op.kind),
        .path = std::move(*path),
        .new_path = std::move(new_path).value_or(std::filesystem::path{}),
        .overwrite = op.overwrite,
        .ignore_if_exists = op.ignore_if_exists,
        .ignore_if_not_exists = op.ignore_if_not_exists,
        .recursive = op.recursive,
    });
  }
  return resolved;
}

bool VersionsCurrent(const LspClient& client, const LspClient::WorkspaceEdit& edit) {
  for (const auto& [uri, expected] : edit.expected_versions) {
    const std::optional<int> tracked = client.TrackedDocumentVersion(uri);
    if (tracked.has_value() && *tracked != expected) {
      return false;
    }
  }
  return true;
}

}  // namespace microide::workspace::lsp_workspace_edit
