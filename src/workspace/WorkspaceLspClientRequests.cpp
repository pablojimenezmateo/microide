#include "workspace/WorkspaceLspClient.h"

#include <algorithm>
#include <cstddef>

#include "workspace/LspProtocol.h"
#include "workspace/WorkspaceLspClientInternal.h"

namespace microide::workspace {

void LspClient::RequestHoverAsync(std::string uri, Position pos, HoverCallback callback) {
  if (!callback) return;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->test_stub_mode.load(std::memory_order_acquire)) {
      auto handler = impl_->test_hover_handler;
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
  // Hover delivers the raw `result` JSON; the consumer parses the contents shape.
  impl_->DispatchResultRequest(
      "textDocument/hover", lsp_protocol::MakeTextDocumentPositionParams(uri, pos),
      std::move(callback),
      [](const util::JsonValue& result) { return std::optional<util::JsonValue>(result); });
}

void LspClient::RequestCompletionAsync(std::string uri, Position pos, CompletionCallback callback) {
  if (!callback) return;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->test_stub_mode.load(std::memory_order_acquire)) {
      auto handler = impl_->test_completion_handler;
      impl_->main_mailbox.PostWithoutWake(
          [handler, uri = std::move(uri), pos, cb = std::move(callback)]() mutable {
            if (handler) {
              handler(std::move(uri), pos, std::move(cb));
            } else {
              cb(std::nullopt);
            }
          });
      return;
    }
  }
  impl_->DispatchResultRequest(
      "textDocument/completion", lsp_protocol::MakeTextDocumentPositionParams(uri, pos),
      std::move(callback),
      [](const util::JsonValue& result) -> std::optional<std::vector<CompletionItem>> {
        std::vector<CompletionItem> items;
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
          ci.insert_text_format = item["insertTextFormat"].AsInt(1);
          // Prefer the server's textEdit: its range is authoritative (it knows the
          // token being completed, so a member/path completion extends the
          // qualifier instead of a heuristic clobbering it), and its newText wins
          // over insertText. Supports both TextEdit{range,newText} and
          // InsertReplaceEdit{insert,replace,newText}; we use the replace range so
          // accepting mid-identifier overwrites the whole token.
          if (item.HasKey("textEdit")) {
            const auto& text_edit = item["textEdit"];
            if (text_edit.HasKey("range")) {
              ci.replace_range = lsp_protocol::ParseRange(text_edit["range"]);
            } else if (text_edit.HasKey("replace")) {
              ci.replace_range = lsp_protocol::ParseRange(text_edit["replace"]);
            }
            std::string new_text = text_edit["newText"].AsString();
            if (!new_text.empty()) ci.insert_text = std::move(new_text);
          }
          if (ci.insert_text.empty()) ci.insert_text = ci.label;
          items.push_back(std::move(ci));
        }
        return items;
      });
}

void LspClient::RequestSignatureHelpAsync(std::string uri, Position pos,
                                          SignatureHelpCallback callback) {
  if (!callback) return;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->test_stub_mode.load(std::memory_order_acquire)) {
      auto handler = impl_->test_signature_help_handler;
      impl_->main_mailbox.PostWithoutWake(
          [handler, uri = std::move(uri), pos, cb = std::move(callback)]() mutable {
            if (handler) {
              handler(std::move(uri), pos, std::move(cb));
            } else {
              cb(std::nullopt);
            }
          });
      return;
    }
  }
  impl_->DispatchResultRequest(
      "textDocument/signatureHelp", lsp_protocol::MakeTextDocumentPositionParams(uri, pos),
      std::move(callback), [](const util::JsonValue& result) -> std::optional<SignatureHelp> {
        return lsp_protocol::ParseSignatureHelp(result);
      });
}

void LspClient::RequestCodeActionAsync(std::string uri, Range range,
                                       std::vector<Diagnostic> context_diagnostics,
                                       CodeActionCallback callback) {
  using namespace util;
  JsonObject params;
  params["textDocument"] = lsp_protocol::MakeTextDocumentIdentifier(uri);
  params["range"] = lsp_protocol::MakeRange(range);
  JsonObject context;
  JsonArray diagnostics_json;
  diagnostics_json.reserve(context_diagnostics.size());
  for (const auto& diagnostic : context_diagnostics) {
    diagnostics_json.push_back(lsp_protocol::MakeDiagnostic(diagnostic));
  }
  context["diagnostics"] = JsonValue(std::move(diagnostics_json));
  params["context"] = JsonValue(std::move(context));
  impl_->DispatchResultRequest(
      "textDocument/codeAction", JsonValue(std::move(params)), std::move(callback),
      [](const util::JsonValue& result) -> std::optional<std::vector<CodeAction>> {
        if (!result.IsArray()) {
          return std::nullopt;
        }
        std::vector<CodeAction> actions;
        // Cap the code-action list: a hostile server can pack a huge array into
        // one 64 MiB message; each action (with a copied arguments array) is
        // re-materialized into a session item on the UI thread. Mirrors the
        // completion 5000 cap. A usable lightbulb menu never approaches this.
        constexpr std::size_t kMaxCodeActions = 5000;
        const auto& result_array = result.AsArray();
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
          if (action.HasKey("edit")) {
            ca.edit = lsp_protocol::ParseWorkspaceEdit(action["edit"]);
            ca.has_edit = !ca.edit.changes.empty();
          }
          actions.push_back(std::move(ca));
        }
        return actions;
      });
}

void LspClient::RequestFormattingAsync(std::string uri, int tab_size, bool insert_spaces,
                                       FormattingCallback callback) {
  if (!callback) return;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->test_stub_mode.load(std::memory_order_acquire)) {
      auto handler = impl_->test_formatting_handler;
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
  using namespace util;
  JsonObject options;
  options["tabSize"] = JsonValue(static_cast<std::int64_t>(tab_size));
  options["insertSpaces"] = JsonValue(insert_spaces);
  JsonObject params;
  params["textDocument"] = lsp_protocol::MakeTextDocumentIdentifier(uri);
  params["options"] = JsonValue(std::move(options));
  impl_->DispatchResultRequest(
      "textDocument/formatting", JsonValue(std::move(params)), std::move(callback),
      [](const util::JsonValue& result) -> std::optional<std::vector<TextEdit>> {
        if (!result.IsArray()) {
          return std::nullopt;
        }
        // Return the FULL TextEdit[]: a whole-document reformat commonly comes back
        // as many edits, and dropping all but the first silently corrupts the
        // buffer. Cap the count as a hostile-server backstop (mirrors the other
        // request caps); a real formatter never approaches this.
        constexpr std::size_t kMaxFormattingEdits = 200000;
        std::vector<TextEdit> edits;
        const auto& array = result.AsArray();
        const std::size_t count = std::min(array.size(), kMaxFormattingEdits);
        edits.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
          edits.emplace_back(lsp_protocol::ParseRange(array[i]["range"]),
                             array[i]["newText"].AsString());
        }
        return edits;
      });
}

void LspClient::RequestGoToDefinitionAsync(std::string uri, Position pos,
                                           LocationCallback callback) {
  impl_->DispatchResultRequest(
      "textDocument/definition", lsp_protocol::MakeTextDocumentPositionParams(uri, pos),
      std::move(callback), [](const util::JsonValue& result) {
        return std::optional<std::vector<Location>>(lsp_protocol::ParseLocations(result));
      });
}

void LspClient::RequestFindReferencesAsync(std::string uri, Position pos,
                                           bool include_declaration, LocationCallback callback) {
  using namespace util;
  JsonObject context_obj;
  context_obj["includeDeclaration"] = JsonValue(include_declaration);
  JsonObject params;
  params["textDocument"] = lsp_protocol::MakeTextDocumentIdentifier(uri);
  params["position"] = lsp_protocol::MakePosition(pos);
  params["context"] = JsonValue(std::move(context_obj));
  impl_->DispatchResultRequest(
      "textDocument/references", JsonValue(std::move(params)), std::move(callback),
      [](const util::JsonValue& result) {
        return std::optional<std::vector<Location>>(lsp_protocol::ParseLocations(result));
      });
}

void LspClient::RequestRenameAsync(std::string uri, Position pos, std::string new_name,
                                   RenameCallback callback) {
  if (!callback) return;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->test_stub_mode.load(std::memory_order_acquire)) {
      auto handler = impl_->test_rename_handler;
      impl_->main_mailbox.PostWithoutWake([handler, uri = std::move(uri),
                                           new_name = std::move(new_name),
                                           cb = std::move(callback)]() mutable {
        if (handler) {
          handler(std::move(uri), std::move(new_name), std::move(cb));
        } else {
          cb(std::nullopt);
        }
      });
      return;
    }
  }
  using namespace util;
  JsonObject params;
  params["textDocument"] = lsp_protocol::MakeTextDocumentIdentifier(uri);
  params["position"] = lsp_protocol::MakePosition(pos);
  params["newName"] = JsonValue(std::move(new_name));
  impl_->DispatchResultRequest(
      "textDocument/rename", JsonValue(std::move(params)), std::move(callback),
      [](const util::JsonValue& result) {
        // ParseWorkspaceEdit bounds the total files/edits materialized on the main
        // thread (a hostile server could otherwise pack a sub-64 MiB result with
        // thousands of files each carrying a huge edit array). A rename result also
        // supports the `documentChanges` shape, which the helper handles too.
        return std::optional<WorkspaceEdit>(lsp_protocol::ParseWorkspaceEdit(result));
      });
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
  using namespace util;
  JsonObject params;
  params["textDocument"] = lsp_protocol::MakeTextDocumentIdentifier(uri);
  impl_->DispatchResultRequest(
      "textDocument/documentSymbol", JsonValue(std::move(params)), std::move(callback),
      [](const util::JsonValue& result) {
        return std::optional<std::vector<DocumentSymbol>>(
            lsp_protocol::ParseDocumentSymbols(result));
      });
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
  using namespace util;
  JsonObject params;
  params["textDocument"] = lsp_protocol::MakeTextDocumentIdentifier(uri);
  impl_->DispatchResultRequest(
      "textDocument/semanticTokens/full", JsonValue(std::move(params)), std::move(callback),
      [](const util::JsonValue& result) {
        return std::optional<std::vector<SemanticToken>>(
            lsp_protocol::ParseSemanticTokensData(result));
      });
}

}  // namespace microide::workspace
