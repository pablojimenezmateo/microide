#include "workspace/WorkspaceLspClient.h"

#include "workspace/WorkspaceLspClientInternal.h"

namespace microide::workspace {

namespace {

util::JsonValue MakeTextDocPosition(const std::string& uri,
                                    const LspClient::Position& pos) {
  using namespace util;
  JsonObject position_obj;
  position_obj["line"] = JsonValue(static_cast<std::int64_t>(pos.line));
  position_obj["character"] = JsonValue(static_cast<std::int64_t>(pos.character));
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);
  JsonObject params;
  params["textDocument"] = JsonValue(std::move(text_doc));
  params["position"] = JsonValue(std::move(position_obj));
  return util::JsonValue(std::move(params));
}

std::vector<LspClient::Location> ParseLocations(const util::JsonValue& result) {
  std::vector<LspClient::Location> locs;
  const auto parse_one = [&](const util::JsonValue& v) {
    LspClient::Location loc;
    loc.uri = v["uri"].IsString() ? v["uri"].AsString() : "";
    loc.range.start.line = v["range"]["start"]["line"].AsInt();
    loc.range.start.character = v["range"]["start"]["character"].AsInt();
    loc.range.end.line = v["range"]["end"]["line"].AsInt();
    loc.range.end.character = v["range"]["end"]["character"].AsInt();
    locs.push_back(std::move(loc));
  };
  if (result.IsArray()) {
    for (const auto& item : result.AsArray()) parse_one(item);
  } else if (result.HasKey("uri")) {
    parse_one(result);
  }
  return locs;
}

LspClient::Range ParseLspRange(const util::JsonValue& r) {
  LspClient::Range rng;
  if (!r.HasKey("start") || !r.HasKey("end")) {
    return rng;
  }
  rng.start.line = r["start"]["line"].AsInt();
  rng.start.character = r["start"]["character"].AsInt();
  rng.end.line = r["end"]["line"].AsInt();
  rng.end.character = r["end"]["character"].AsInt();
  return rng;
}

LspClient::DocumentSymbol ParseDocumentSymbolValue(const util::JsonValue& v) {
  LspClient::DocumentSymbol s;
  s.name = v["name"].IsString() ? v["name"].AsString() : "";
  s.detail = v["detail"].IsString() ? v["detail"].AsString() : "";
  s.kind = v["kind"].AsInt(1);
  if (v.HasKey("location")) {
    const auto& loc = v["location"];
    s.range = ParseLspRange(loc["range"]);
    s.selection_range = s.range;
    (void)loc["uri"];
  } else {
    s.range = ParseLspRange(v["range"]);
    if (v.HasKey("selectionRange")) {
      s.selection_range = ParseLspRange(v["selectionRange"]);
    } else {
      s.selection_range = s.range;
    }
  }
  if (v["children"].IsArray()) {
    for (const auto& ch : v["children"].AsArray()) {
      s.children.push_back(ParseDocumentSymbolValue(ch));
    }
  }
  return s;
}

std::vector<LspClient::DocumentSymbol> ParseDocumentSymbolResult(
    const util::JsonValue& result) {
  std::vector<LspClient::DocumentSymbol> out;
  if (!result.IsArray()) {
    return out;
  }
  for (const auto& item : result.AsArray()) {
    out.push_back(ParseDocumentSymbolValue(item));
  }
  return out;
}

}  // namespace

void LspClient::RequestHoverAsync(std::string uri, Position pos, HoverCallback callback) {
  if (!callback) return;
  const HoverCallback failure_callback = callback;
  const auto params = MakeTextDocPosition(uri, pos);
  const int id = impl_->RegisterPendingRequest(
      [cb = std::move(callback)](util::JsonValue resp) {
        if (resp.HasKey("result")) {
          cb(std::optional<util::JsonValue>(resp["result"]));
        } else {
          cb(std::nullopt);
        }
      });
  if (!impl_->SendMessageAfterInitialize(impl_->MakeRequest(id, "textDocument/hover", params))) {
    impl_->RemovePendingRequest(id);
    failure_callback(std::nullopt);
  }
}

void LspClient::RequestCompletionAsync(std::string uri, Position pos, CompletionCallback callback) {
  if (!callback) return;
  const CompletionCallback failure_callback = callback;
  const auto params = MakeTextDocPosition(uri, pos);
  const int id = impl_->RegisterPendingRequest(
      [cb = std::move(callback)](util::JsonValue resp) {
        if (!resp.HasKey("result")) { cb(std::nullopt); return; }
        std::vector<CompletionItem> items;
        const auto& result = resp["result"];
        const auto& arr = result.IsArray() ? result.AsArray() : result["items"].AsArray();
        for (const auto& item : arr) {
          CompletionItem ci;
          ci.label = item["label"].IsString() ? item["label"].AsString() : "";
          ci.kind = item["kind"].AsInt(1);
          ci.detail = item["detail"].IsString() ? item["detail"].AsString() : "";
          ci.documentation = item["documentation"].IsString() ? item["documentation"].AsString() : "";
          ci.insert_text = item["insertText"].IsString() ? item["insertText"].AsString() : "";
          if (ci.insert_text.empty()) ci.insert_text = ci.label;
          ci.insert_text_format = item["insertTextFormat"].AsInt(1);
          items.push_back(std::move(ci));
        }
        cb(std::optional<std::vector<CompletionItem>>(std::move(items)));
      });
  if (!impl_->SendMessageAfterInitialize(
          impl_->MakeRequest(id, "textDocument/completion", params))) {
    impl_->RemovePendingRequest(id);
    failure_callback(std::nullopt);
  }
}

void LspClient::RequestCodeActionAsync(std::string uri, Range range, CodeActionCallback callback) {
  if (!callback) return;
  const CodeActionCallback failure_callback = callback;
  using namespace util;
  JsonObject start_obj;
  start_obj["line"] = JsonValue(static_cast<std::int64_t>(range.start.line));
  start_obj["character"] = JsonValue(static_cast<std::int64_t>(range.start.character));
  JsonObject end_obj;
  end_obj["line"] = JsonValue(static_cast<std::int64_t>(range.end.line));
  end_obj["character"] = JsonValue(static_cast<std::int64_t>(range.end.character));
  JsonObject range_obj;
  range_obj["start"] = JsonValue(std::move(start_obj));
  range_obj["end"] = JsonValue(std::move(end_obj));
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);
  JsonObject params;
  params["textDocument"] = JsonValue(std::move(text_doc));
  params["range"] = JsonValue(std::move(range_obj));
  params["context"] = JsonValue(JsonObject{});

  const int id = impl_->RegisterPendingRequest(
      [cb = std::move(callback)](util::JsonValue resp) {
        if (!resp.HasKey("result") || !resp["result"].IsArray()) { cb(std::nullopt); return; }
        std::vector<CodeAction> actions;
        for (const auto& action : resp["result"].AsArray()) {
          CodeAction ca;
          ca.title = action["title"].IsString() ? action["title"].AsString() : "";
          if (action["command"].IsString()) {
            ca.command = action["command"].AsString();
            if (action["arguments"].IsArray()) {
              ca.arguments = action["arguments"].AsArray();
            }
          } else if (action["command"].HasKey("command")) {
            ca.command = action["command"]["command"].IsString()
                             ? action["command"]["command"].AsString()
                             : "";
            if (action["command"]["arguments"].IsArray()) {
              ca.arguments = action["command"]["arguments"].AsArray();
            }
          }
          actions.push_back(std::move(ca));
        }
        cb(std::optional<std::vector<CodeAction>>(std::move(actions)));
      });
  if (!impl_->SendMessageAfterInitialize(
          impl_->MakeRequest(id, "textDocument/codeAction", JsonValue(std::move(params))))) {
    impl_->RemovePendingRequest(id);
    failure_callback(std::nullopt);
  }
}

void LspClient::RequestFormattingAsync(std::string uri, int tab_size, bool insert_spaces,
                                       FormattingCallback callback) {
  if (!callback) return;
  const FormattingCallback failure_callback = callback;
  using namespace util;
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);
  JsonObject options;
  options["tabSize"] = JsonValue(static_cast<std::int64_t>(tab_size));
  options["insertSpaces"] = JsonValue(insert_spaces);
  JsonObject params;
  params["textDocument"] = JsonValue(std::move(text_doc));
  params["options"] = JsonValue(std::move(options));

  const int id = impl_->RegisterPendingRequest(
      [cb = std::move(callback)](util::JsonValue resp) {
        if (!resp.HasKey("result") || !resp["result"].IsArray()) {
          cb(std::optional<std::string>(std::string{}));
          return;
        }
        const auto& edits = resp["result"].AsArray();
        if (edits.empty()) { cb(std::optional<std::string>(std::string{})); return; }
        cb(std::optional<std::string>(
            edits.front()["newText"].IsString() ? edits.front()["newText"].AsString() : ""));
      });
  if (!impl_->SendMessageAfterInitialize(
          impl_->MakeRequest(id, "textDocument/formatting", JsonValue(std::move(params))))) {
    impl_->RemovePendingRequest(id);
    failure_callback(std::nullopt);
  }
}

void LspClient::RequestGoToDefinitionAsync(std::string uri, Position pos, LocationCallback callback) {
  if (!callback) return;
  const LocationCallback failure_callback = callback;
  const auto params = MakeTextDocPosition(uri, pos);
  const int id = impl_->RegisterPendingRequest(
      [cb = std::move(callback)](util::JsonValue resp) {
        if (!resp.HasKey("result")) { cb(std::nullopt); return; }
        cb(std::optional<std::vector<Location>>(ParseLocations(resp["result"])));
      });
  if (!impl_->SendMessageAfterInitialize(
          impl_->MakeRequest(id, "textDocument/definition", params))) {
    impl_->RemovePendingRequest(id);
    failure_callback(std::nullopt);
  }
}

void LspClient::RequestFindReferencesAsync(std::string uri, Position pos,
                                           bool include_declaration, LocationCallback callback) {
  if (!callback) return;
  const LocationCallback failure_callback = callback;
  using namespace util;
  JsonObject position_obj;
  position_obj["line"] = JsonValue(static_cast<std::int64_t>(pos.line));
  position_obj["character"] = JsonValue(static_cast<std::int64_t>(pos.character));
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);
  JsonObject context_obj;
  context_obj["includeDeclaration"] = JsonValue(include_declaration);
  JsonObject params;
  params["textDocument"] = JsonValue(std::move(text_doc));
  params["position"] = JsonValue(std::move(position_obj));
  params["context"] = JsonValue(std::move(context_obj));

  const int id = impl_->RegisterPendingRequest(
      [cb = std::move(callback)](util::JsonValue resp) {
        if (!resp.HasKey("result")) { cb(std::nullopt); return; }
        cb(std::optional<std::vector<Location>>(ParseLocations(resp["result"])));
      });
  if (!impl_->SendMessageAfterInitialize(
          impl_->MakeRequest(id, "textDocument/references", JsonValue(std::move(params))))) {
    impl_->RemovePendingRequest(id);
    failure_callback(std::nullopt);
  }
}

void LspClient::RequestRenameAsync(std::string uri, Position pos, std::string new_name,
                                   RenameCallback callback) {
  if (!callback) return;
  const RenameCallback failure_callback = callback;
  using namespace util;
  JsonObject position_obj;
  position_obj["line"] = JsonValue(static_cast<std::int64_t>(pos.line));
  position_obj["character"] = JsonValue(static_cast<std::int64_t>(pos.character));
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);
  JsonObject params;
  params["textDocument"] = JsonValue(std::move(text_doc));
  params["position"] = JsonValue(std::move(position_obj));
  params["newName"] = JsonValue(std::move(new_name));

  const int id = impl_->RegisterPendingRequest(
      [cb = std::move(callback)](util::JsonValue resp) {
        if (!resp.HasKey("result")) { cb(std::nullopt); return; }
        WorkspaceEdit edit;
        const auto& result = resp["result"];
        if (result.HasKey("changes")) {
          for (const auto& [file_uri, edits_val] : result["changes"].AsObject()) {
            auto& file_edits = edit.changes[file_uri];
            for (const auto& e : edits_val.AsArray()) {
              Range r;
              r.start.line = e["range"]["start"]["line"].AsInt();
              r.start.character = e["range"]["start"]["character"].AsInt();
              r.end.line = e["range"]["end"]["line"].AsInt();
              r.end.character = e["range"]["end"]["character"].AsInt();
              std::string text = e["newText"].IsString() ? e["newText"].AsString() : "";
              file_edits.emplace_back(r, std::move(text));
            }
          }
        }
        cb(std::optional<WorkspaceEdit>(std::move(edit)));
      });
  if (!impl_->SendMessageAfterInitialize(
          impl_->MakeRequest(id, "textDocument/rename", JsonValue(std::move(params))))) {
    impl_->RemovePendingRequest(id);
    failure_callback(std::nullopt);
  }
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
  const DocumentSymbolCallback failure_callback = callback;
  using namespace util;
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(std::move(uri));
  JsonObject params;
  params["textDocument"] = JsonValue(std::move(text_doc));

  const int id = impl_->RegisterPendingRequest(
      [cb = std::move(callback)](util::JsonValue resp) {
        if (!resp.HasKey("result")) {
          cb(std::nullopt);
          return;
        }
        cb(std::optional<std::vector<DocumentSymbol>>(
            ParseDocumentSymbolResult(resp["result"])));
      });
  if (!impl_->SendMessageAfterInitialize(
          impl_->MakeRequest(id, "textDocument/documentSymbol", JsonValue(std::move(params))))) {
    impl_->RemovePendingRequest(id);
    failure_callback(std::nullopt);
  }
}

}  // namespace microide::workspace
