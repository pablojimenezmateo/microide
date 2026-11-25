#include "workspace/assist/RefactorAssist.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "util/StringUtil.h"
#include "workspace/FileUri.h"
#include "workspace/lsp/LspViewportPositions.h"
#include "workspace/lsp/LspWorkspaceEditOps.h"
#include "workspace/services/WorkspaceEditService.h"
#include "util/ErrorOut.h"

namespace microide::workspace {

namespace {

void LogLsp(const AssistHost& host, const char* message) {
  host.output().AppendLine("lsp.log", "LSP Log", message);
}

}

std::string RefactorAssist::SymbolAtCursor() const {
  editor::TextViewport* viewport = host_.ops().active_editable_viewport();
  if (viewport == nullptr) {
    return {};
  }
  const std::string_view line = LspLineView(*viewport, viewport->cursor_line());
  const std::size_t caret = std::min(viewport->cursor_column(), line.size());
  const std::size_t begin = util::Utf8IdentifierRunStart(line, caret);
  const std::size_t end = util::Utf8IdentifierRunEnd(line, caret);
  return std::string(line.substr(begin, end - begin));
}

bool RefactorAssist::RenameSymbol(const std::string& new_name, std::string* error_message) {
  util::ClearError(error_message);
  if (new_name.empty()) {
    return false;
  }
  editor::TextViewport* viewport = host_.RequireActiveEditableViewport(error_message);
  if (viewport == nullptr) {
    return false;
  }
  LspClient* client = host_.PrepareLspRequest(*viewport, error_message);
  if (client == nullptr) {
    return false;
  }
  const std::filesystem::path request_path = viewport->path();
  const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(*client);
  const LspClient::Position position =
      ByteColumnToLspPosition(*viewport, viewport->cursor_line(), viewport->cursor_column(), encoding);
  const std::uint64_t revision = viewport->content_revision();
  client->RequestRenameAsync(
      FileUriForPath(request_path), position, new_name,
      [this, request_path, new_name, client, revision](LspResult<LspClient::WorkspaceEdit> edit) {
        ApplyRenameResult(request_path, new_name, *client, revision, std::move(edit));
      });
  return true;
}

void RefactorAssist::ApplyRenameResult(const std::filesystem::path& request_path,
                                       const std::string& new_name, const LspClient& client,
                                       std::uint64_t revision,
                                       LspResult<LspClient::WorkspaceEdit> edit) {
  const AssistHost::Operations& ops = host_.ops();
  ops.finish_tracked_lsp_request();
  if (host_.IsStale(request_path)) {
    return;
  }
  if (!edit.answered()) {
    LogLsp(host_, "Rename request did not complete (server timed out or errored)");
    return;
  }
  if (!edit.has_value() || (edit->changes.empty() && edit->resource_ops.empty())) {
    LogLsp(host_, "No rename edits returned");
    return;
  }
  if (const editor::TextViewport* viewport = ops.active_editable_viewport();
      viewport == nullptr || viewport->content_revision() != revision ||
      !lsp_workspace_edit::VersionsCurrent(client, *edit)) {
    LogLsp(host_, "Rename aborted: the document changed while the server computed the rename");
    return;
  }
  std::optional<std::vector<WorkspaceResourceOp>> flattened_ops =
      lsp_workspace_edit::FlattenResourceOps(edit->resource_ops);
  if (!flattened_ops.has_value()) {
    LogLsp(host_, "Rename aborted: server returned an unusable resource-operation URI");
    return;
  }
  const std::vector<WorkspaceResourceOp> resource_ops = *std::move(flattened_ops);
  std::vector<CodeActionEdit> workspace_edits;
  for (const auto& [uri, text_edits] : edit->changes) {
    const std::optional<std::filesystem::path> path = PathFromFileUri(uri);
    if (!path.has_value()) {
      LogLsp(host_, "Rename aborted: server returned an unusable edit target URI");
      return;
    }
    for (const auto& [range, new_text] : text_edits) {
      workspace_edits.push_back(assist::ToCodeActionEdit(*path, range, new_text));
    }
  }
  host_.edits().ApplyRename(new_name, workspace_edits, resource_ops);
}

void RefactorAssist::PrepareRenameForCursor(std::function<void(bool, std::string)> callback) {
  if (!callback) {
    return;
  }
  const AssistHost::Operations& ops = host_.ops();
  editor::TextViewport* viewport = ops.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return;
  }
  LspClient* client = ops.lsp_client_for_viewport(*viewport, nullptr);
  if (client == nullptr || !client->SupportsPrepareRename()) {
    return;
  }
  const std::filesystem::path request_path = viewport->path();
  const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(*client);
  const LspClient::Position position = ByteColumnToLspPosition(
      *viewport, viewport->cursor_line(), viewport->cursor_column(), encoding);
  ops.ensure_lsp_document_open(*viewport, *client, viewport->language_id());
  client->RequestPrepareRenameAsync(
      FileUriForPath(request_path), position,
      [this, request_path, callback = std::move(callback)](
          LspResult<LspClient::PrepareRename> result) {
        if (host_.IsStale(request_path)) {
          return;
        }
        if (!result.has_value()) {
          return;
        }
        callback(result->can_rename, result->placeholder);
      });
}

bool RefactorAssist::FormatActiveDocument(std::string* error_message) {
  util::ClearError(error_message);
  editor::TextViewport* viewport = host_.RequireActiveEditableViewport(error_message);
  if (viewport == nullptr) {
    return false;
  }
  LspClient* client = host_.PrepareLspRequest(*viewport, error_message);
  if (client == nullptr) {
    return false;
  }
  const std::filesystem::path request_path = viewport->path();
  const int tab_size = static_cast<int>(viewport->indent_width() > 0 ? viewport->indent_width() : 4);
  const bool insert_spaces = viewport->soft_tabs();
  const std::uint64_t revision = viewport->content_revision();
  const auto on_result = [this, request_path, revision](
                             LspResult<std::vector<LspClient::TextEdit>> edits) {
    ApplyFormattingResult(request_path, revision, std::move(edits));
  };
  if (viewport->has_selection()) {
    if (const std::optional<editor::SelectionRange> selection = viewport->selection_range()) {
      const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(*client);
      const editor::SelectionRange normalized = editor::TextViewport::NormalizeRange(*selection);
      const LspClient::Range range{
          .start = ByteColumnToLspPosition(*viewport, normalized.start.line,
                                           normalized.start.column, encoding),
          .end = ByteColumnToLspPosition(*viewport, normalized.end.line, normalized.end.column,
                                         encoding),
      };
      client->RequestRangeFormattingAsync(FileUriForPath(request_path), range, tab_size,
                                          insert_spaces, on_result);
      return true;
    }
  }
  client->RequestFormattingAsync(FileUriForPath(request_path), tab_size, insert_spaces, on_result);
  return true;
}

void RefactorAssist::ApplyFormattingResult(const std::filesystem::path& request_path,
                                           std::uint64_t revision,
                                           LspResult<std::vector<LspClient::TextEdit>> edits) {
  const AssistHost::Operations& ops = host_.ops();
  ops.finish_tracked_lsp_request();
  if (host_.IsStale(request_path)) {
    return;
  }
  if (const editor::TextViewport* viewport = ops.active_editable_viewport();
      viewport == nullptr || viewport->content_revision() != revision) {
    LogLsp(host_, "Formatting dropped: the document changed while the server formatted it");
    return;
  }
  if (!edits.answered()) {
    LogLsp(host_, "Formatting request did not complete (server timed out or errored)");
    return;
  }
  if (!edits.has_value() || edits->empty()) {
    LogLsp(host_, "No formatting changes");
    return;
  }
  std::vector<CodeActionEdit> workspace_edits;
  workspace_edits.reserve(edits->size());
  for (const auto& [range, new_text] : *edits) {
    workspace_edits.push_back(assist::ToCodeActionEdit(request_path, range, new_text));
  }
  if (!host_.edits().ApplyLspEdit(workspace_edits)) {
    LogLsp(host_, "Could not apply formatting");
  }
}

}
