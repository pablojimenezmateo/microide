#include "workspace/WorkspaceLspClient.h"

#include <algorithm>
#include <cstddef>

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
        // Cap the item count: this list is materialized twice (here and in the
        // AssistService session) on the main thread on every keystroke that
        // triggers completion, so a server returning a huge list would stall the
        // UI. The overlay is windowed and a human never scrolls past a few
        // thousand candidates.
        constexpr std::size_t kMaxCompletionItems = 5000;
        for (const auto& item : arr) {
          if (items.size() >= kMaxCompletionItems) {
            break;
          }
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
        // Cap the code-action list: a hostile server can pack a huge array into
        // one 64 MiB message; each action (with a copied arguments array) is
        // re-materialized into a session item on the UI thread. Mirrors the
        // completion 5000 cap. A usable lightbulb menu never approaches this.
        constexpr std::size_t kMaxCodeActions = 5000;
        const auto& result_array = resp["result"].AsArray();
        const std::size_t action_count = std::min(result_array.size(), kMaxCodeActions);
        actions.reserve(action_count);
        for (std::size_t i = 0; i < action_count; ++i) {
          const auto& action = result_array[i];
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
          // Cap the total files and edits materialized on the main thread. A
          // hostile/buggy server can pack a sub-64 MiB rename result with
          // thousands of files each carrying a huge edit array (or one file with
          // millions of edits), each edit becoming a Range + newText string here.
          // These ceilings are far beyond any real rename's footprint.
          constexpr std::size_t kMaxRenameFiles = 10000;
          constexpr std::size_t kMaxRenameEditsTotal = 200000;
          std::size_t total_edits = 0;
          for (const auto& [file_uri, edits_val] : result["changes"].AsObject()) {
            if (edit.changes.size() >= kMaxRenameFiles || total_edits >= kMaxRenameEditsTotal) {
              break;
            }
            auto& file_edits = edit.changes[file_uri];
            for (const auto& e : edits_val.AsArray()) {
              if (total_edits >= kMaxRenameEditsTotal) {
                break;
              }
              file_edits.emplace_back(lsp_protocol::ParseRange(e["range"]), e["newText"].AsString());
              ++total_edits;
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
      impl_->main_mailbox.PostWithoutWake(
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

void LspClient::RequestSemanticTokensAsync(std::string uri, SemanticTokensCallback callback) {
  if (!callback) return;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->test_stub_mode.load(std::memory_order_acquire)) {
      auto handler = impl_->test_semantic_tokens_handler;
      impl_->main_mailbox.PostWithoutWake(
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
  if (!impl_->supports_semantic_tokens.load(std::memory_order_acquire)) {
    callback(std::nullopt);
    return;
  }
  SemanticTokensCallback failure = callback;
  using namespace util;
  JsonObject params;
  params["textDocument"] = lsp_protocol::MakeTextDocumentIdentifier(uri);
  impl_->DispatchRequest(
      "textDocument/semanticTokens/full", JsonValue(std::move(params)),
      [cb = std::move(callback)](util::JsonValue resp) {
        if (!resp.HasKey("result")) {
          cb(std::nullopt);
          return;
        }
        cb(std::optional<std::vector<SemanticToken>>(
            lsp_protocol::ParseSemanticTokensData(resp["result"])));
      },
      [failure = std::move(failure)]() { failure(std::nullopt); });
}

}  // namespace microide::workspace
