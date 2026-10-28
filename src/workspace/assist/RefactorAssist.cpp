#include "workspace/assist/RefactorAssist.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "util/StringUtil.h"
#include "workspace/FileUri.h"
#include "workspace/lsp/LspViewportPositions.h"
#include "workspace/lsp/LspWorkspaceEditOps.h"
#include "workspace/services/WorkspaceEditService.h"

namespace microide::workspace {

namespace {

void LogLsp(const AssistHost& host, const char* message) {
  host.output().AppendLine("lsp.log", "LSP Log", message);
}

}  // namespace

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
  if (error_message != nullptr) {
    error_message->clear();
  }
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
  client->RequestRenameAsync(
      FileUriForPath(request_path), position, new_name,
      // `client` cannot dangle: the callback is stored on the client and invoked
      // from its own DrainCallbacks (live or retiring), so the client outlives it.
      [this, request_path, new_name, client](LspResult<LspClient::WorkspaceEdit> edit) {
        ApplyRenameResult(request_path, new_name, *client, std::move(edit));
      });
  return true;
}

void RefactorAssist::ApplyRenameResult(const std::filesystem::path& request_path,
                                       const std::string& new_name, const LspClient& client,
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
  // Version gate: a versioned TextDocumentEdit pinned to a document version
  // we have since advanced past is stale — applying it would corrupt the
  // buffer the user kept typing in. LSP requires failing the whole edit.
  if (!lsp_workspace_edit::VersionsCurrent(client, *edit)) {
    LogLsp(host_, "Rename aborted: the document changed while the server computed the rename");
    return;
  }
  // Flatten resource ops (file create/rename/delete — e.g. rust-analyzer
  // renames the file when a module is renamed). An undecodable target URI
  // refuses the whole rename, mirroring the text-edit rule below.
  std::optional<std::vector<WorkspaceResourceOp>> flattened_ops =
      lsp_workspace_edit::FlattenResourceOps(edit->resource_ops);
  if (!flattened_ops.has_value()) {
    LogLsp(host_, "Rename aborted: server returned an unusable resource-operation URI");
    return;
  }
  const std::vector<WorkspaceResourceOp> resource_ops = *std::move(flattened_ops);
  // Flatten the URI-keyed WorkspaceEdit into the shared edit records. The host
  // decides whether to apply in place or open + save the files that are not
  // currently open.
  std::vector<CodeActionEdit> workspace_edits;
  for (const auto& [uri, text_edits] : edit->changes) {
    const std::optional<std::filesystem::path> path = PathFromFileUri(uri);
    if (!path.has_value()) {
      // A rename that touches a target whose URI cannot be decoded (non-local
      // authority, malformed percent escape, unsupported scheme) must not be
      // silently applied as a partial rename — and must never fall back to the
      // empty "active buffer" path. Refuse the whole rename so the user is not
      // left with a half-renamed symbol.
      LogLsp(host_, "Rename aborted: server returned an unusable edit target URI");
      return;
    }
    for (const auto& [range, new_text] : text_edits) {
      workspace_edits.push_back(assist::ToCodeActionEdit(*path, range, new_text));
    }
  }
  // Applies in place when every file is open, or asks before writing closed ones.
  host_.edits().ApplyRename(new_name, workspace_edits, resource_ops);
}

void RefactorAssist::PrepareRenameForCursor(std::function<void(bool, std::string)> callback) {
  if (!callback) {
    return;
  }
  const AssistHost::Operations& ops = host_.ops();
  editor::TextViewport* viewport = ops.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return;  // no callback: the caller keeps its heuristic seed
  }
  LspClient* client = ops.lsp_client_for_viewport(*viewport, nullptr);
  if (client == nullptr || !client->SupportsPrepareRename()) {
    return;  // no server / no prepareRename provider — keep the heuristic seed
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
        // Only meaningful while the same buffer is active; a switch abandons refine.
        if (host_.IsStale(request_path)) {
          return;
        }
        if (!result.has_value()) {
          return;  // no provider / timeout / error — the caller keeps its heuristic seed
        }
        callback(result->can_rename, result->placeholder);
      });
}

bool RefactorAssist::FormatActiveDocument(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = host_.RequireActiveEditableViewport(error_message);
  if (viewport == nullptr) {
    return false;
  }
  // Formatting is LSP-only (no plugin provider path). PrepareLspRequest resolves the
  // client, opens the document, begins the tracked request, and records the
  // unavailable-server message when there is none.
  LspClient* client = host_.PrepareLspRequest(*viewport, error_message);
  if (client == nullptr) {
    return false;
  }
  const std::filesystem::path request_path = viewport->path();
  const int tab_size = static_cast<int>(viewport->indent_width() > 0 ? viewport->indent_width() : 4);
  const auto on_result = [this, request_path](
                             LspResult<std::vector<LspClient::TextEdit>> edits) {
    ApplyFormattingResult(request_path, std::move(edits));
  };
  // Format the SELECTION only when there is one (VSCode "Format Selection"), else the
  // whole document. Both return the same TextEdit[] shape and apply identically.
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
                                          /*insert_spaces=*/true, on_result);
      return true;
    }
  }
  client->RequestFormattingAsync(FileUriForPath(request_path), tab_size, /*insert_spaces=*/true,
                                 on_result);
  return true;
}

void RefactorAssist::ApplyFormattingResult(const std::filesystem::path& request_path,
                                           LspResult<std::vector<LspClient::TextEdit>> edits) {
  const AssistHost::Operations& ops = host_.ops();
  ops.finish_tracked_lsp_request();
  // Drop superseded results (buffer switched/closed) before mutating.
  if (host_.IsStale(request_path)) {
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
  // Apply the whole set together, highest-position-first.
  std::vector<CodeActionEdit> workspace_edits;
  workspace_edits.reserve(edits->size());
  for (const auto& [range, new_text] : *edits) {
    workspace_edits.push_back(assist::ToCodeActionEdit(request_path, range, new_text));
  }
  if (!host_.edits().ApplyLspEdit(workspace_edits)) {
    LogLsp(host_, "Could not apply formatting");
  }
}

}  // namespace microide::workspace
