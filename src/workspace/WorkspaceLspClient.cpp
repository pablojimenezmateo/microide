#include "workspace/WorkspaceLspClient.h"

#include <cassert>
#include <chrono>
#include <memory>
#include <sstream>
#include <thread>

namespace microide::workspace {

struct LspClient::Impl {
  platform::AsyncSubprocess proc;
  std::unordered_map<int, util::JsonValue> pending_responses;
  int next_id = 1;
  std::string root_uri;
  std::string language_id;
  bool initialized = false;

  int GetNextId() { return next_id++; }

  util::JsonValue MakeRequest(const std::string& method, const util::JsonValue& params) {
    using namespace util;
    JsonObject req;
    req["jsonrpc"] = JsonValue("2.0");
    req["id"] = JsonValue(static_cast<std::int64_t>(GetNextId()));
    req["method"] = JsonValue(method);
    req["params"] = params;
    return JsonValue(req);
  }

  util::JsonValue MakeNotification(const std::string& method, const util::JsonValue& params) {
    using namespace util;
    JsonObject msg;
    msg["jsonrpc"] = JsonValue("2.0");
    msg["method"] = JsonValue(method);
    msg["params"] = params;
    return JsonValue(msg);
  }

  bool SendMessage(const util::JsonValue& msg) {
    const std::string json = util::SerializeJson(msg);
    const std::string rfc7230 = "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json;
    return proc.Write(rfc7230);
  }

  std::optional<util::JsonValue> ReadJsonRpc(int timeout_ms = 5000) {
    std::string buffer;
    while (true) {
      const auto line_opt = ReadLine(buffer, timeout_ms);
      if (!line_opt) return std::nullopt;
      const auto& line = *line_opt;

      // Look for Content-Length header
      if (line.substr(0, 16) == "Content-Length: ") {
        int content_len = 0;
        if (sscanf(line.c_str(), "Content-Length: %d", &content_len) != 1) {
          return std::nullopt;
        }

        // Read blank line
        const auto blank_opt = ReadLine(buffer, timeout_ms);
        if (!blank_opt || !blank_opt->empty()) {
          return std::nullopt;
        }

        // Read exactly content_len bytes
        const auto body_opt = ReadExact(buffer, content_len, timeout_ms);
        if (!body_opt) {
          return std::nullopt;
        }
        const auto& body = *body_opt;
        return util::ParseJson(body);
      }
    }
  }

  std::optional<std::string> ReadLine(std::string& buffer, int timeout_ms) {
    while (true) {
      const auto pos = buffer.find('\n');
      if (pos != std::string::npos) {
        std::string line = buffer.substr(0, pos);
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        buffer = buffer.substr(pos + 1);
        return line;
      }

      const auto chunk_opt = proc.Read(4096, timeout_ms);
      if (!chunk_opt) return std::nullopt;
      if (chunk_opt->empty()) return std::nullopt;  // timeout
      buffer += *chunk_opt;
    }
  }

  std::optional<std::string> ReadExact(std::string& buffer, std::size_t n, int timeout_ms) {
    while (buffer.size() < n) {
      const auto chunk_opt = proc.Read(4096, timeout_ms);
      if (!chunk_opt) return std::nullopt;
      if (chunk_opt->empty()) return std::nullopt;
      buffer += *chunk_opt;
    }
    const std::string result = buffer.substr(0, n);
    buffer = buffer.substr(n);
    return result;
  }
};

LspClient::LspClient() : impl_(new Impl{}) {}

LspClient::~LspClient() {
  Shutdown();
  delete impl_;
}

bool LspClient::Start(const std::vector<std::string>& command, const std::string& root_uri,
                      const std::string& language_id) {
  if (!impl_->proc.Start(command)) {
    return false;
  }

  impl_->root_uri = root_uri;
  impl_->language_id = language_id;

  using namespace util;
  JsonObject init_params;
  init_params["processId"] = JsonValue(static_cast<std::int64_t>(impl_->proc.pid()));
  init_params["rootUri"] = JsonValue(root_uri);
  init_params["capabilities"] = JsonValue(JsonObject{});

  const auto req = impl_->MakeRequest("initialize", JsonValue(init_params));
  if (!impl_->SendMessage(req)) {
    impl_->proc.Shutdown();
    return false;
  }

  // Wait for initialize response
  std::string buffer;
  for (int attempts = 0; attempts < 30; ++attempts) {
    const auto resp_opt = impl_->ReadJsonRpc(500);
    if (!resp_opt) {
      impl_->proc.Shutdown();
      return false;
    }

    const auto& resp = *resp_opt;
    if (resp.HasKey("result")) {
      impl_->initialized = true;
      // Send initialized notification
      const auto notif = impl_->MakeNotification("initialized", JsonValue(JsonObject{}));
      impl_->SendMessage(notif);
      return true;
    }
  }

  impl_->proc.Shutdown();
  return false;
}

bool LspClient::IsRunning() const { return impl_->proc.IsRunning(); }

void LspClient::PollNotifications() {
  while (impl_->proc.IsRunning()) {
    const auto msg_opt = impl_->ReadJsonRpc(100);
    if (!msg_opt) break;
    const auto& msg = *msg_opt;

    // Check if this is a notification (no id)
    if (!msg.HasKey("id")) {
      const auto& method_val = msg["method"];
      if (method_val.IsString() && method_val.AsString() == "textDocument/publishDiagnostics") {
        if (on_diagnostics_) {
          const auto& params = msg["params"];
          const auto& uri_val = params["uri"];
          std::vector<Diagnostic> diags;

          const auto& diag_array = params["diagnostics"].AsArray();
          for (const auto& d : diag_array) {
            Diagnostic diag;
            diag.range.start.line = d["range"]["start"]["line"].AsInt();
            diag.range.start.character = d["range"]["start"]["character"].AsInt();
            diag.range.end.line = d["range"]["end"]["line"].AsInt();
            diag.range.end.character = d["range"]["end"]["character"].AsInt();
            diag.message = d["message"].AsString();
            diag.severity = d["severity"].AsInt(1);
            diag.code = d["code"].AsString();
            diags.push_back(diag);
          }

          on_diagnostics_(uri_val.AsString(), std::move(diags));
        }
      }
    }
  }
}

bool LspClient::DidOpen(const std::string& uri, const std::string& language_id,
                        const std::string& text) {
  using namespace util;
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);
  text_doc["languageId"] = JsonValue(language_id);
  text_doc["version"] = JsonValue(static_cast<std::int64_t>(1));
  text_doc["text"] = JsonValue(text);

  JsonObject params;
  params["textDocument"] = JsonValue(text_doc);

  const auto req = impl_->MakeNotification("textDocument/didOpen", JsonValue(params));
  return impl_->SendMessage(req);
}

bool LspClient::DidChange(const std::string& uri, const std::string& text) {
  using namespace util;
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);
  text_doc["version"] = JsonValue(static_cast<std::int64_t>(2));

  JsonObject change;
  change["text"] = JsonValue(text);

  JsonArray changes;
  changes.push_back(JsonValue(change));

  JsonObject params;
  params["textDocument"] = JsonValue(text_doc);
  params["contentChanges"] = JsonValue(changes);

  const auto req = impl_->MakeNotification("textDocument/didChange", JsonValue(params));
  return impl_->SendMessage(req);
}

bool LspClient::DidSave(const std::string& uri) {
  using namespace util;
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);

  JsonObject params;
  params["textDocument"] = JsonValue(text_doc);

  const auto req = impl_->MakeNotification("textDocument/didSave", JsonValue(params));
  return impl_->SendMessage(req);
}

bool LspClient::DidClose(const std::string& uri) {
  using namespace util;
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);

  JsonObject params;
  params["textDocument"] = JsonValue(text_doc);

  const auto req = impl_->MakeNotification("textDocument/didClose", JsonValue(params));
  return impl_->SendMessage(req);
}

std::optional<util::JsonValue> LspClient::RequestHover(const std::string& uri, Position pos) {
  using namespace util;
  JsonObject position_obj;
  position_obj["line"] = JsonValue(static_cast<std::int64_t>(pos.line));
  position_obj["character"] = JsonValue(static_cast<std::int64_t>(pos.character));

  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);

  JsonObject params;
  params["textDocument"] = JsonValue(text_doc);
  params["position"] = JsonValue(position_obj);

  const auto req = impl_->MakeRequest("textDocument/hover", JsonValue(params));
  if (!impl_->SendMessage(req)) {
    return std::nullopt;
  }

  const int req_id = impl_->next_id - 1;
  for (int attempts = 0; attempts < 30; ++attempts) {
    const auto resp_opt = impl_->ReadJsonRpc(500);
    if (!resp_opt) return std::nullopt;
    const auto& resp = *resp_opt;

    if (resp.HasKey("id") && resp["id"].AsInt() == req_id) {
      return resp["result"];
    }
  }
  return std::nullopt;
}

std::optional<std::vector<LspClient::CompletionItem>> LspClient::RequestCompletion(
    const std::string& uri, Position pos) {
  using namespace util;
  JsonObject position_obj;
  position_obj["line"] = JsonValue(static_cast<std::int64_t>(pos.line));
  position_obj["character"] = JsonValue(static_cast<std::int64_t>(pos.character));

  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);

  JsonObject params;
  params["textDocument"] = JsonValue(text_doc);
  params["position"] = JsonValue(position_obj);

  const auto req = impl_->MakeRequest("textDocument/completion", JsonValue(params));
  if (!impl_->SendMessage(req)) {
    return std::nullopt;
  }

  const int req_id = impl_->next_id - 1;
  for (int attempts = 0; attempts < 30; ++attempts) {
    const auto resp_opt = impl_->ReadJsonRpc(500);
    if (!resp_opt) return std::nullopt;
    const auto& resp = *resp_opt;

    if (resp.HasKey("id") && resp["id"].AsInt() == req_id) {
      std::vector<CompletionItem> items;
      const auto& result = resp["result"];
      const auto& item_array = result.IsArray() ? result.AsArray() : result["items"].AsArray();
      for (const auto& item : item_array) {
        CompletionItem ci;
        ci.label = item["label"].AsString();
        ci.kind = item["kind"].AsInt(1);
        ci.detail = item["detail"].AsString();
        ci.documentation = item["documentation"].AsString();
        ci.insert_text = item["insertText"].AsString();
        if (ci.insert_text.empty()) {
          ci.insert_text = ci.label;
        }
        items.push_back(ci);
      }
      return items;
    }
  }
  return std::nullopt;
}

std::optional<std::vector<LspClient::CodeAction>> LspClient::RequestCodeAction(
    const std::string& uri, Range range) {
  using namespace util;
  JsonObject start_obj;
  start_obj["line"] = JsonValue(static_cast<std::int64_t>(range.start.line));
  start_obj["character"] = JsonValue(static_cast<std::int64_t>(range.start.character));

  JsonObject end_obj;
  end_obj["line"] = JsonValue(static_cast<std::int64_t>(range.end.line));
  end_obj["character"] = JsonValue(static_cast<std::int64_t>(range.end.character));

  JsonObject range_obj;
  range_obj["start"] = JsonValue(start_obj);
  range_obj["end"] = JsonValue(end_obj);

  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);

  JsonObject params;
  params["textDocument"] = JsonValue(text_doc);
  params["range"] = JsonValue(range_obj);
  params["context"] = JsonValue(JsonObject{});

  const auto req = impl_->MakeRequest("textDocument/codeAction", JsonValue(params));
  if (!impl_->SendMessage(req)) {
    return std::nullopt;
  }

  const int req_id = impl_->next_id - 1;
  for (int attempts = 0; attempts < 30; ++attempts) {
    const auto resp_opt = impl_->ReadJsonRpc(500);
    if (!resp_opt) return std::nullopt;
    const auto& resp = *resp_opt;

    if (resp.HasKey("id") && resp["id"].AsInt() == req_id) {
      std::vector<CodeAction> actions;
      const auto& result = resp["result"];
      const auto& action_array = result.IsArray() ? result.AsArray() : result.AsArray();
      for (const auto& action : action_array) {
        CodeAction ca;
        ca.title = action["title"].AsString();
        ca.command = action["command"].AsString();
        const auto& args = action["arguments"];
        if (args.IsArray()) {
          ca.arguments = args.AsArray();
        }
        actions.push_back(ca);
      }
      return actions;
    }
  }
  return std::nullopt;
}

std::optional<std::string> LspClient::RequestFormatting(const std::string& uri,
                                                        int tab_size,
                                                        bool insert_spaces) {
  using namespace util;
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);

  JsonObject options;
  options["tabSize"] = JsonValue(static_cast<std::int64_t>(tab_size));
  options["insertSpaces"] = JsonValue(insert_spaces);

  JsonObject params;
  params["textDocument"] = JsonValue(text_doc);
  params["options"] = JsonValue(options);

  const auto req = impl_->MakeRequest("textDocument/formatting", JsonValue(params));
  if (!impl_->SendMessage(req)) {
    return std::nullopt;
  }

  const int req_id = impl_->next_id - 1;
  for (int attempts = 0; attempts < 30; ++attempts) {
    const auto resp_opt = impl_->ReadJsonRpc(500);
    if (!resp_opt) {
      return std::nullopt;
    }
    const auto& resp = *resp_opt;
    if (!resp.HasKey("id") || resp["id"].AsInt() != req_id) {
      continue;
    }

    const auto& edits = resp["result"];
    if (!edits.IsArray()) {
      return std::string{};
    }
    const auto& edit_array = edits.AsArray();
    if (edit_array.empty()) {
      return std::string{};
    }
    return edit_array.front()["newText"].AsString();
  }
  return std::nullopt;
}

void LspClient::Shutdown() {
  if (!impl_->initialized) {
    impl_->proc.Shutdown();
    return;
  }

  using namespace util;
  const auto req = impl_->MakeRequest("shutdown", JsonValue(JsonObject{}));
  impl_->SendMessage(req);

  // Wait briefly for response
  for (int attempts = 0; attempts < 10; ++attempts) {
    const auto resp_opt = impl_->ReadJsonRpc(200);
    if (!resp_opt) break;
  }

  impl_->MakeNotification("exit", JsonValue(JsonObject{}));
  impl_->proc.Shutdown();
  impl_->initialized = false;
}

}  // namespace microide::workspace
