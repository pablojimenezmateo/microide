#include "workspace/WorkspaceLspClient.h"

#include "workspace/LspProtocol.h"
#include "workspace/WorkspaceLspClientInternal.h"

namespace microide::workspace {

void LspClient::RequestHoverAsync(std::string uri, Position pos, HoverCallback callback) {
  if (!callback) return;
  HoverCallback failure = callback;
  impl_->DispatchRequest(
      "textDocument/hover", lsp_protocol::MakeTextDocumentPositionParams(uri, pos),
      [cb = std::move(callback)](util::JsonValue resp) {
        cb(resp.HasKey("result") ? std::optional<util::JsonValue>(resp["result"]) : std::nullopt);
      },
      [failure = std::move(failure)]() { failure(std::nullopt); });
}

void LspClient::RequestCompletionAsync(std::string uri, Position pos, CompletionCallback callback) {
  if (!callback) return;
  CompletionCallback failure = callback;
  impl_->DispatchRequest(
      "textDocument/completion", lsp_protocol::MakeTextDocumentPositionParams(uri, pos),
      [cb = std::move(callback)](util::JsonValue resp) {
        if (!resp.HasKey("result")) { cb(std::nullopt); return; }
        std::vector<CompletionItem> items;
        const auto& result = resp["result"];
        const auto& arr = result.IsArray() ? result.AsArray() : result["items"].AsArray();
        for (const auto& item : arr) {
          CompletionItem ci;
          ci.label = item["label"].AsString();
          ci.kind = item["kind"].AsInt(1);
          ci.detail = item["detail"].AsString();
          ci.documentation = item["documentation"].AsString();
          ci.insert_text = item["insertText"].AsString();
          if (ci.insert_text.empty()) ci.insert_text = ci.label;
          ci.insert_text_format = item["insertTextFormat"].AsInt(1);
          items.push_back(std::move(ci));
        }
        cb(std::optional<std::vector<CompletionItem>>(std::move(items)));
      },
      [failure = std::move(failure)]() { failure(std::nullopt); });
}

void LspClient::RequestCodeActionAsync(std::string uri, Range range, CodeActionCallback callback) {
  if (!callback) return;
  CodeActionCallback failure = callback;
  using namespace util;
  JsonObject params;
  params["textDocument"] = lsp_protocol::MakeTextDocumentIdentifier(uri);
  params["range"] = lsp_protocol::MakeRange(range);
  params["context"] = JsonValue(JsonObject{});
  impl_->DispatchRequest(
      "textDocument/codeAction", JsonValue(std::move(params)),
      [cb = std::move(callback)](util::JsonValue resp) {
        if (!resp.HasKey("result") || !resp["result"].IsArray()) { cb(std::nullopt); return; }
        std::vector<CodeAction> actions;
        for (const auto& action : resp["result"].AsArray()) {
          CodeAction ca;
          ca.title = action["title"].AsString();
          if (action["command"].IsString()) {
            ca.command = action["command"].AsString();
            ca.arguments = action["arguments"].AsArray();
          } else if (action["command"].HasKey("command")) {
            const auto& command = action["command"];
            ca.command = command["command"].AsString();
            ca.arguments = command["arguments"].AsArray();
          }
          actions.push_back(std::move(ca));
        }
        cb(std::optional<std::vector<CodeAction>>(std::move(actions)));
      },
      [failure = std::move(failure)]() { failure(std::nullopt); });
}

void LspClient::RequestFormattingAsync(std::string uri, int tab_size, bool insert_spaces,
                                       FormattingCallback callback) {
  if (!callback) return;
  FormattingCallback failure = callback;
  using namespace util;
  JsonObject options;
  options["tabSize"] = JsonValue(static_cast<std::int64_t>(tab_size));
  options["insertSpaces"] = JsonValue(insert_spaces);
  JsonObject params;
  params["textDocument"] = lsp_protocol::MakeTextDocumentIdentifier(uri);
  params["options"] = JsonValue(std::move(options));
  impl_->DispatchRequest(
      "textDocument/formatting", JsonValue(std::move(params)),
      [cb = std::move(callback)](util::JsonValue resp) {
        if (!resp.HasKey("result") || !resp["result"].IsArray()) {
          cb(std::optional<std::string>(std::string{}));
          return;
        }
        const auto& edits = resp["result"].AsArray();
        if (edits.empty()) { cb(std::optional<std::string>(std::string{})); return; }
        cb(std::optional<std::string>(edits.front()["newText"].AsString()));
      },
      [failure = std::move(failure)]() { failure(std::nullopt); });
}

void LspClient::RequestGoToDefinitionAsync(std::string uri, Position pos,
                                           LocationCallback callback) {
  if (!callback) return;
  LocationCallback failure = callback;
  impl_->DispatchRequest(
      "textDocument/definition", lsp_protocol::MakeTextDocumentPositionParams(uri, pos),
      [cb = std::move(callback)](util::JsonValue resp) {
        if (!resp.HasKey("result")) { cb(std::nullopt); return; }
        cb(std::optional<std::vector<Location>>(lsp_protocol::ParseLocations(resp["result"])));
      },
      [failure = std::move(failure)]() { failure(std::nullopt); });
}

void LspClient::RequestFindReferencesAsync(std::string uri, Position pos,
                                           bool include_declaration, LocationCallback callback) {
  if (!callback) return;
  LocationCallback failure = callback;
  using namespace util;
  JsonObject context_obj;
  context_obj["includeDeclaration"] = JsonValue(include_declaration);
  JsonObject params;
  params["textDocument"] = lsp_protocol::MakeTextDocumentIdentifier(uri);
  params["position"] = lsp_protocol::MakePosition(pos);
  params["context"] = JsonValue(std::move(context_obj));
  impl_->DispatchRequest(
      "textDocument/references", JsonValue(std::move(params)),
      [cb = std::move(callback)](util::JsonValue resp) {
        if (!resp.HasKey("result")) { cb(std::nullopt); return; }
        cb(std::optional<std::vector<Location>>(lsp_protocol::ParseLocations(resp["result"])));
      },
      [failure = std::move(failure)]() { failure(std::nullopt); });
}

void LspClient::RequestRenameAsync(std::string uri, Position pos, std::string new_name,
                                   RenameCallback callback) {
  if (!callback) return;
  RenameCallback failure = callback;
  using namespace util;
  JsonObject params;
  params["textDocument"] = lsp_protocol::MakeTextDocumentIdentifier(uri);
  params["position"] = lsp_protocol::MakePosition(pos);
  params["newName"] = JsonValue(std::move(new_name));
  impl_->DispatchRequest(
      "textDocument/rename", JsonValue(std::move(params)),
      [cb = std::move(callback)](util::JsonValue resp) {
        if (!resp.HasKey("result")) { cb(std::nullopt); return; }
        WorkspaceEdit edit;
        const auto& result = resp["result"];
        if (result.HasKey("changes")) {
          for (const auto& [file_uri, edits_val] : result["changes"].AsObject()) {
            auto& file_edits = edit.changes[file_uri];
            for (const auto& e : edits_val.AsArray()) {
              file_edits.emplace_back(lsp_protocol::ParseRange(e["range"]), e["newText"].AsString());
            }
          }
        }
        cb(std::optional<WorkspaceEdit>(std::move(edit)));
      },
      [failure = std::move(failure)]() { failure(std::nullopt); });
}

void LspClient::RequestDocumentSymbolAsync(std::string uri, DocumentSymbolCallback callback) {
  if (!callback) return;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->test_stub_mode.load(std::memory_order_acquire)) {
      auto handler = impl_->test_document_symbol_handler;
      impl_->ready_callbacks.push_back(
          [handler, uri = std::move(uri), cb = std::move(callback)]() mutable {
            if (handler) {
              handler(std::move(uri), std::move(cb));
            } else {
              cb(std::nullopt);
            }
          });
      return;
    }
  }
  DocumentSymbolCallback failure = callback;
  using namespace util;
  JsonObject params;
  params["textDocument"] = lsp_protocol::MakeTextDocumentIdentifier(uri);
  impl_->DispatchRequest(
      "textDocument/documentSymbol", JsonValue(std::move(params)),
      [cb = std::move(callback)](util::JsonValue resp) {
        if (!resp.HasKey("result")) { cb(std::nullopt); return; }
        cb(std::optional<std::vector<DocumentSymbol>>(
            lsp_protocol::ParseDocumentSymbols(resp["result"])));
      },
      [failure = std::move(failure)]() { failure(std::nullopt); });
}

}  // namespace microide::workspace
