#include "workspace/WorkspaceLspClient.h"

#include <algorithm>
#include <cstddef>

#include "util/StringUtil.h"
#include "workspace/LspProtocol.h"
#include "workspace/WorkspaceLspClientInternal.h"

namespace microide::workspace {

void LspClient::SortCompletionItemsByServerRank(std::vector<CompletionItem>& items) {
  // Mirrors VS Code's default completion comparator. Note the deliberate
  // asymmetry: sortText is only consulted when BOTH items have one, so a server
  // that ranks part of its list does not push its unranked items to an arbitrary
  // end. Ties fall through to label, then kind, so the order is total and
  // deterministic; stable_sort keeps the server's array order for full ties.
  std::stable_sort(items.begin(), items.end(),
                   [](const CompletionItem& lhs, const CompletionItem& rhs) {
                     if (!lhs.sort_text.empty() && !rhs.sort_text.empty()) {
                       const std::string left = util::ToLowerAscii(lhs.sort_text);
                       const std::string right = util::ToLowerAscii(rhs.sort_text);
                       if (left != right) {
                         return left < right;
                       }
                     }
                     if (lhs.label != rhs.label) {
                       return lhs.label < rhs.label;
                     }
                     return lhs.kind < rhs.kind;
                   });
}

void LspClient::RequestHoverAsync(std::string uri, Position pos, HoverCallback callback) {
  if (!callback) return;
  if (impl_->DispatchTestStub(impl_->test_handlers.hover, callback, std::move(uri))) {
    return;
  }
  // Hover delivers the raw `result` JSON; the consumer parses the contents shape.
  impl_->DispatchResultRequest(
      "textDocument/hover", lsp_protocol::MakeTextDocumentPositionParams(uri, pos),
      std::move(callback),
      [](const util::JsonValue& result) { return std::optional<util::JsonValue>(result); });
}

void LspClient::RequestCompletionAsync(std::string uri, Position pos, CompletionCallback callback) {
  if (!callback) return;
  if (impl_->DispatchTestStub(impl_->test_handlers.completion, callback, std::move(uri), pos)) {
    return;
  }
  impl_->DispatchResultRequest(
      "textDocument/completion", lsp_protocol::MakeTextDocumentPositionParams(uri, pos),
      std::move(callback),
      [](util::JsonValue& result) -> std::optional<std::vector<CompletionItem>> {
        std::vector<CompletionItem> items;
        // Accept either a bare CompletionItem[] or a CompletionList{items:[...]}.
        util::JsonArray* arr = result.MutableArray();
        if (arr == nullptr) {
          if (util::JsonValue* list = result.MutableAt("items")) arr = list->MutableArray();
        }
        if (arr == nullptr) {
          return items;
        }
        // Move each string field out of the owned response instead of copying: this
        // list is materialized again in the AssistService session on the main thread
        // on every keystroke that triggers completion, so per-item copies add up.
        // Cap the item count so a huge/hostile list cannot stall the UI; the overlay
        // is windowed and a human never scrolls past a few thousand candidates.
        constexpr std::size_t kMaxCompletionItems = 5000;
        const auto take = [](util::JsonValue& obj, std::string_view key) -> std::string {
          if (util::JsonValue* v = obj.MutableAt(key)) {
            if (std::string* s = v->MutableString()) return std::move(*s);
          }
          return {};
        };
        // CompletionItem.documentation is `string | MarkupContent{kind,value}` per LSP;
        // essentially every real server (clangd, rust-analyzer, gopls, pyright, tsserver)
        // sends the object form. Extract the string out of either shape (moving to avoid a
        // copy) — the bare `take` above would silently drop the object form, blanking the
        // completion popup's doc pane. Mirrors hover/signature-help's StringOrValueField.
        const auto take_markup = [](util::JsonValue& obj, std::string_view key) -> std::string {
          util::JsonValue* v = obj.MutableAt(key);
          if (v == nullptr) return {};
          if (std::string* s = v->MutableString()) return std::move(*s);
          if (util::JsonValue* value = v->MutableAt("value")) {
            if (std::string* s = value->MutableString()) return std::move(*s);
          }
          return {};
        };
        for (util::JsonValue& item : *arr) {
          if (items.size() >= kMaxCompletionItems) {
            break;
          }
          CompletionItem ci;
          ci.label = take(item, "label");
          ci.kind = static_cast<int>(item["kind"].AsInt(1));
          ci.detail = take(item, "detail");
          ci.documentation = take_markup(item, "documentation");
          ci.insert_text = take(item, "insertText");
          ci.sort_text = take(item, "sortText");
          ci.insert_text_format = static_cast<int>(item["insertTextFormat"].AsInt(1));
          // Prefer the server's textEdit: its range is authoritative (it knows the
          // token being completed, so a member/path completion extends the
          // qualifier instead of a heuristic clobbering it), and its newText wins
          // over insertText. Supports both TextEdit{range,newText} and
          // InsertReplaceEdit{insert,replace,newText}; we use the replace range so
          // accepting mid-identifier overwrites the whole token.
          if (util::JsonValue* text_edit = item.MutableAt("textEdit")) {
            if (text_edit->HasKey("range")) {
              ci.replace_range = lsp_protocol::ParseRange((*text_edit)["range"]);
            } else if (text_edit->HasKey("replace")) {
              ci.replace_range = lsp_protocol::ParseRange((*text_edit)["replace"]);
            }
            std::string new_text = take(*text_edit, "newText");
            if (!new_text.empty()) ci.insert_text = std::move(new_text);
          }
          if (ci.insert_text.empty()) ci.insert_text = ci.label;
          items.push_back(std::move(ci));
        }
        SortCompletionItemsByServerRank(items);
        return items;
      });
}

void LspClient::RequestSignatureHelpAsync(std::string uri, Position pos,
                                          SignatureHelpCallback callback) {
  if (!callback) return;
  if (impl_->DispatchTestStub(impl_->test_handlers.signature_help, callback, std::move(uri), pos)) {
    return;
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
      [](util::JsonValue& result) -> std::optional<std::vector<CodeAction>> {
        util::JsonArray* result_array = result.MutableArray();
        if (result_array == nullptr) {
          return std::nullopt;
        }
        std::vector<CodeAction> actions;
        // Cap the code-action list: a hostile server can pack a huge array into
        // one 64 MiB message; each action is re-materialized into a session item on
        // the UI thread. Mirrors the completion 5000 cap. A usable lightbulb menu
        // never approaches this. Move title/command/arguments out of the owned
        // response instead of copying.
        constexpr std::size_t kMaxCodeActions = 5000;
        const std::size_t action_count = std::min(result_array->size(), kMaxCodeActions);
        actions.reserve(action_count);
        const auto take = [](util::JsonValue& obj, std::string_view key) -> std::string {
          if (util::JsonValue* v = obj.MutableAt(key)) {
            if (std::string* s = v->MutableString()) return std::move(*s);
          }
          return {};
        };
        const auto take_array = [](util::JsonValue& obj, std::string_view key) -> util::JsonArray {
          if (util::JsonValue* v = obj.MutableAt(key)) {
            if (util::JsonArray* a = v->MutableArray()) return std::move(*a);
          }
          return {};
        };
        for (std::size_t i = 0; i < action_count; ++i) {
          util::JsonValue& action = (*result_array)[i];
          CodeAction ca;
          ca.title = take(action, "title");
          util::JsonValue* command = action.MutableAt("command");
          if (command != nullptr && command->IsString()) {
            ca.command = take(action, "command");
            ca.arguments = take_array(action, "arguments");
          } else if (command != nullptr && command->HasKey("command")) {
            ca.command = take(*command, "command");
            ca.arguments = take_array(*command, "arguments");
          }
          if (util::JsonValue* edit = action.MutableAt("edit")) {
            ca.edit = lsp_protocol::ParseWorkspaceEdit(*edit);
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
  if (impl_->DispatchTestStub(impl_->test_handlers.formatting, callback, std::move(uri))) {
    return;
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
        // Return the FULL TextEdit[]: a whole-document reformat commonly comes back
        // as many edits, and dropping all but the first silently corrupts the
        // buffer. ParseTextEdits caps the count as a hostile-server backstop.
        if (!result.IsArray()) {
          return std::nullopt;
        }
        return lsp_protocol::ParseTextEdits(result);
      });
}

void LspClient::RequestRangeFormattingAsync(std::string uri, Range range, int tab_size,
                                            bool insert_spaces, FormattingCallback callback) {
  if (!callback) return;
  // Range formatting shares the formatting stub handler (same TextEdit[] shape).
  if (impl_->DispatchTestStub(impl_->test_handlers.formatting, callback, std::move(uri))) {
    return;
  }
  using namespace util;
  JsonObject options;
  options["tabSize"] = JsonValue(static_cast<std::int64_t>(tab_size));
  options["insertSpaces"] = JsonValue(insert_spaces);
  JsonObject params;
  params["textDocument"] = lsp_protocol::MakeTextDocumentIdentifier(uri);
  params["range"] = lsp_protocol::MakeRange(range);
  params["options"] = JsonValue(std::move(options));
  impl_->DispatchResultRequest(
      "textDocument/rangeFormatting", JsonValue(std::move(params)), std::move(callback),
      [](const util::JsonValue& result) -> std::optional<std::vector<TextEdit>> {
        if (!result.IsArray()) {
          return std::nullopt;
        }
        return lsp_protocol::ParseTextEdits(result);
      });
}

namespace {
// definition / typeDefinition / implementation / declaration are identical on the
// wire: TextDocumentPositionParams in, Location|Location[]|LocationLink[] out.
// Templated on Impl so the private nested type is deduced, never named here.
template <typename Impl>
void DispatchLocationRequest(Impl* impl, const char* method, const std::string& uri,
                             LspClient::Position pos, LspClient::LocationCallback callback) {
  impl->DispatchResultRequest(
      method, lsp_protocol::MakeTextDocumentPositionParams(uri, pos), std::move(callback),
      [](const util::JsonValue& result) {
        return std::optional<std::vector<LspClient::Location>>(
            lsp_protocol::ParseLocations(result));
      });
}
}  // namespace

void LspClient::RequestGoToDefinitionAsync(std::string uri, Position pos,
                                           LocationCallback callback) {
  DispatchLocationRequest(impl_, "textDocument/definition", uri, pos, std::move(callback));
}

void LspClient::RequestGoToTypeDefinitionAsync(std::string uri, Position pos,
                                               LocationCallback callback) {
  DispatchLocationRequest(impl_, "textDocument/typeDefinition", uri, pos, std::move(callback));
}

void LspClient::RequestGoToImplementationAsync(std::string uri, Position pos,
                                               LocationCallback callback) {
  DispatchLocationRequest(impl_, "textDocument/implementation", uri, pos, std::move(callback));
}

void LspClient::RequestGoToDeclarationAsync(std::string uri, Position pos,
                                            LocationCallback callback) {
  DispatchLocationRequest(impl_, "textDocument/declaration", uri, pos, std::move(callback));
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

void LspClient::RequestPrepareRenameAsync(std::string uri, Position pos,
                                          PrepareRenameCallback callback) {
  if (!callback) return;
  if (impl_->DispatchTestStub(impl_->test_handlers.prepare_rename, callback, std::move(uri), pos)) {
    return;
  }
  // Skip the round-trip when the server has no prepareRename provider: the caller
  // then keeps its heuristic seed rather than provoking a server error per rename.
  if (!impl_->supports_prepare_rename.load(std::memory_order_acquire)) {
    callback(std::nullopt);
    return;
  }
  impl_->DispatchResultRequest(
      "textDocument/prepareRename", lsp_protocol::MakeTextDocumentPositionParams(uri, pos),
      std::move(callback), [](const util::JsonValue& result) -> std::optional<PrepareRename> {
        return lsp_protocol::ParsePrepareRename(result);
      });
}

void LspClient::RequestRenameAsync(std::string uri, Position pos, std::string new_name,
                                   RenameCallback callback) {
  if (!callback) return;
  if (impl_->DispatchTestStub(impl_->test_handlers.rename, callback, std::move(uri),
                              std::move(new_name))) {
    return;
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
  if (impl_->DispatchTestStub(impl_->test_handlers.document_symbol, callback, std::move(uri))) {
    return;
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

void LspClient::RequestWorkspaceSymbolAsync(std::string query, WorkspaceSymbolCallback callback) {
  if (!callback) return;
  if (impl_->DispatchTestStub(impl_->test_handlers.workspace_symbol, callback, std::move(query))) {
    return;
  }
  using namespace util;
  JsonObject params;
  params["query"] = JsonValue(std::move(query));
  impl_->DispatchResultRequest(
      "workspace/symbol", JsonValue(std::move(params)), std::move(callback),
      [](const util::JsonValue& result) {
        return std::optional<std::vector<WorkspaceSymbol>>(
            lsp_protocol::ParseWorkspaceSymbols(result));
      });
}

void LspClient::RequestSemanticTokensAsync(std::string uri, SemanticTokensCallback callback) {
  if (!callback) return;
  if (impl_->DispatchTestStub(impl_->test_handlers.semantic_tokens, callback, std::move(uri))) {
    return;
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

void LspClient::RequestInlayHintsAsync(std::string uri, Range range, InlayHintCallback callback) {
  if (!callback) return;
  if (impl_->DispatchTestStub(impl_->test_handlers.inlay_hint, callback, std::move(uri), range)) {
    return;
  }
  if (!impl_->supports_inlay_hints.load(std::memory_order_acquire)) {
    callback(std::nullopt);
    return;
  }
  using namespace util;
  JsonObject params;
  params["textDocument"] = lsp_protocol::MakeTextDocumentIdentifier(uri);
  params["range"] = lsp_protocol::MakeRange(range);
  impl_->DispatchResultRequest(
      "textDocument/inlayHint", JsonValue(std::move(params)), std::move(callback),
      [](const util::JsonValue& result) {
        return std::optional<std::vector<InlayHint>>(lsp_protocol::ParseInlayHints(result));
      });
}

void LspClient::RequestDocumentHighlightAsync(std::string uri, Position pos,
                                              DocumentHighlightCallback callback) {
  if (!callback) return;
  if (impl_->DispatchTestStub(impl_->test_handlers.document_highlight, callback, std::move(uri),
                              pos)) {
    return;
  }
  // Short-circuit without a round-trip when the server has no provider: this
  // request fires on caret movement, so provoking a per-move server error would be
  // the most frequent wasted message the client sends.
  if (!impl_->supports_document_highlight.load(std::memory_order_acquire)) {
    callback(std::nullopt);
    return;
  }
  impl_->DispatchResultRequest(
      "textDocument/documentHighlight", lsp_protocol::MakeTextDocumentPositionParams(uri, pos),
      std::move(callback), [](const util::JsonValue& result) {
        return std::optional<std::vector<DocumentHighlight>>(
            lsp_protocol::ParseDocumentHighlights(result));
      });
}

void LspClient::RequestCodeLensAsync(std::string uri, CodeLensCallback callback) {
  if (!callback) return;
  if (impl_->DispatchTestStub(impl_->test_handlers.code_lens, callback, std::move(uri))) {
    return;
  }
  if (!impl_->supports_code_lens.load(std::memory_order_acquire)) {
    callback(std::nullopt);
    return;
  }
  using namespace util;
  JsonObject params;
  params["textDocument"] = lsp_protocol::MakeTextDocumentIdentifier(uri);
  impl_->DispatchResultRequest(
      "textDocument/codeLens", JsonValue(std::move(params)), std::move(callback),
      [](const util::JsonValue& result) {
        return std::optional<std::vector<CodeLens>>(lsp_protocol::ParseCodeLenses(result));
      });
}

void LspClient::ResolveCodeLensAsync(util::JsonValue unresolved,
                                     ResolveCodeLensCallback callback) {
  if (!callback) return;
  if (impl_->DispatchTestStub(impl_->test_handlers.resolve_code_lens, callback,
                              std::move(unresolved))) {
    return;
  }
  if (!impl_->supports_code_lens_resolve.load(std::memory_order_acquire)) {
    callback(std::nullopt);
    return;
  }
  impl_->DispatchResultRequest("codeLens/resolve", std::move(unresolved), std::move(callback),
                               [](const util::JsonValue& result) {
                                 return std::optional<CodeLens>(lsp_protocol::ParseCodeLens(result));
                               });
}

void LspClient::ExecuteServerCommandAsync(std::string command,
                                          std::vector<util::JsonValue> arguments,
                                          ExecuteCommandCallback callback) {
  if (!callback) return;
  if (impl_->DispatchTestStub(impl_->test_handlers.execute_command, callback, std::move(command),
                              std::move(arguments))) {
    return;
  }
  using namespace util;
  JsonObject params;
  params["command"] = JsonValue(std::move(command));
  params["arguments"] = JsonValue(JsonArray(std::make_move_iterator(arguments.begin()),
                                            std::make_move_iterator(arguments.end())));
  impl_->DispatchResultRequest(
      "workspace/executeCommand", JsonValue(std::move(params)), std::move(callback),
      [](const util::JsonValue& result) { return std::optional<util::JsonValue>(result); });
}

void LspClient::RequestPrepareCallHierarchyAsync(std::string uri, Position pos,
                                                 PrepareCallHierarchyCallback callback) {
  if (!callback) return;
  if (impl_->DispatchTestStub(impl_->test_handlers.prepare_call_hierarchy, callback,
                              std::move(uri), pos)) {
    return;
  }
  if (!impl_->supports_call_hierarchy.load(std::memory_order_acquire)) {
    callback(std::nullopt);
    return;
  }
  impl_->DispatchResultRequest(
      "textDocument/prepareCallHierarchy", lsp_protocol::MakeTextDocumentPositionParams(uri, pos),
      std::move(callback), [](const util::JsonValue& result) {
        return std::optional<std::vector<CallHierarchyItem>>(
            lsp_protocol::ParseCallHierarchyItems(result));
      });
}

namespace {
// incomingCalls and outgoingCalls are the same request with a different method and
// a different key naming the far end of each edge.
template <typename Impl>
void DispatchCallHierarchyCalls(Impl* impl, bool incoming, util::JsonValue item,
                                LspClient::CallHierarchyCallsCallback callback) {
  if (impl->DispatchTestStub(impl->test_handlers.call_hierarchy_calls, callback, incoming,
                             std::move(item))) {
    return;
  }
  if (!impl->supports_call_hierarchy.load(std::memory_order_acquire)) {
    callback(std::nullopt);
    return;
  }
  util::JsonObject params;
  params["item"] = std::move(item);
  impl->DispatchResultRequest(
      incoming ? "callHierarchy/incomingCalls" : "callHierarchy/outgoingCalls",
      util::JsonValue(std::move(params)), std::move(callback),
      [incoming](const util::JsonValue& result) {
        return std::optional<std::vector<LspClient::CallHierarchyCall>>(
            lsp_protocol::ParseCallHierarchyCalls(result, incoming));
      });
}
}  // namespace

void LspClient::RequestIncomingCallsAsync(util::JsonValue item,
                                          CallHierarchyCallsCallback callback) {
  if (!callback) return;
  DispatchCallHierarchyCalls(impl_, /*incoming=*/true, std::move(item), std::move(callback));
}

void LspClient::RequestOutgoingCallsAsync(util::JsonValue item,
                                          CallHierarchyCallsCallback callback) {
  if (!callback) return;
  DispatchCallHierarchyCalls(impl_, /*incoming=*/false, std::move(item), std::move(callback));
}

}  // namespace microide::workspace
