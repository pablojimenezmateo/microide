#include "workspace/WorkspaceLspClient.h"

#include <charconv>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace microide::workspace {

// ---------------------------------------------------------------------------
// Internal buffer — avoids O(n) prefix-erasure on every line read.
// ---------------------------------------------------------------------------
struct ReadBuf {
  std::string data;
  std::size_t pos = 0;

  std::string_view view() const {
    return std::string_view(data).substr(pos);
  }

  void consume(std::size_t n) {
    pos += n;
    // Compact when the consumed prefix is large to bound memory usage.
    if (pos > 65536 && pos > data.size() / 2) {
      data.erase(0, pos);
      pos = 0;
    }
  }

  void append(std::string_view chunk) {
    data.append(chunk);
  }
};

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct LspClient::Impl {
  platform::AsyncSubprocess proc;
  std::mutex mutex;

  // Reader thread state
  std::thread reader_thread;
  std::atomic<bool> stop_reader{false};

  // Per-request pending callbacks (keyed by request id), guarded by mutex.
  std::unordered_map<int, std::function<void(util::JsonValue)>> pending_requests;

  // Ready callbacks waiting to be drained on the main thread, guarded by mutex.
  std::vector<std::function<void()>> ready_callbacks;

  // Diagnostics callback — set from main thread, called on main thread via ready_callbacks.
  OnPublishDiagnostics diagnostics_callback;

  std::unordered_map<std::string, int> document_versions;
  int next_id = 1;
  std::string root_uri;
  std::string language_id;
  bool initialized = false;
  bool supports_incremental_sync = false;
  Uint32 wake_event_type = 0;

  int GetNextId() {
    std::lock_guard lock(mutex);
    return next_id++;
  }

  // -------------------------------------------------------------------------
  // JSON-RPC helpers
  // -------------------------------------------------------------------------
  util::JsonValue MakeRequest(int id, const std::string& method, const util::JsonValue& params) {
    using namespace util;
    JsonObject req;
    req["jsonrpc"] = JsonValue("2.0");
    req["id"] = JsonValue(static_cast<std::int64_t>(id));
    req["method"] = JsonValue(method);
    req["params"] = params;
    return JsonValue(std::move(req));
  }

  util::JsonValue MakeNotification(const std::string& method, const util::JsonValue& params) {
    using namespace util;
    JsonObject msg;
    msg["jsonrpc"] = JsonValue("2.0");
    msg["method"] = JsonValue(method);
    msg["params"] = params;
    return JsonValue(std::move(msg));
  }

  bool SendMessage(const util::JsonValue& msg) {
    const std::string json = util::SerializeJson(msg);
    std::string rfc;
    rfc.reserve(32 + json.size());
    rfc += "Content-Length: ";
    rfc += std::to_string(json.size());
    rfc += "\r\n\r\n";
    rfc += json;
    return proc.Write(rfc);
  }

  // -------------------------------------------------------------------------
  // Blocking read of one JSON-RPC message (used only during Start() on the
  // calling thread before the reader thread is launched).
  // -------------------------------------------------------------------------
  std::optional<util::JsonValue> ReadJsonRpcBlocking(ReadBuf& buf, int timeout_ms) {
    // Read Content-Length header
    while (true) {
      const std::string_view v = buf.view();
      const auto nl = v.find('\n');
      if (nl == std::string_view::npos) {
        auto chunk = proc.Read(4096, timeout_ms);
        if (!chunk || chunk->empty()) return std::nullopt;
        buf.append(*chunk);
        continue;
      }
      std::string_view line = v.substr(0, nl);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      buf.consume(nl + 1);

      static constexpr std::string_view kPrefix = "Content-Length: ";
      if (line.substr(0, kPrefix.size()) != kPrefix) continue;

      const std::string_view len_sv = line.substr(kPrefix.size());
      int content_len = 0;
      const auto [ptr, ec] = std::from_chars(len_sv.data(), len_sv.data() + len_sv.size(), content_len);
      if (ec != std::errc{} || content_len <= 0) return std::nullopt;

      // Skip blank line
      while (true) {
        const std::string_view v2 = buf.view();
        const auto nl2 = v2.find('\n');
        if (nl2 == std::string_view::npos) {
          auto chunk = proc.Read(4096, timeout_ms);
          if (!chunk || chunk->empty()) return std::nullopt;
          buf.append(*chunk);
          continue;
        }
        buf.consume(nl2 + 1);
        break;
      }

      // Read body
      while (buf.view().size() < static_cast<std::size_t>(content_len)) {
        auto chunk = proc.Read(4096, timeout_ms);
        if (!chunk || chunk->empty()) return std::nullopt;
        buf.append(*chunk);
      }
      const std::string_view body = buf.view().substr(0, content_len);
      const auto result = util::ParseJson(body);
      buf.consume(content_len);
      return result;
    }
  }

  // -------------------------------------------------------------------------
  // Wake the main SDL event loop.
  // -------------------------------------------------------------------------
  void PushWakeEvent() const {
    if (wake_event_type == 0) return;
    SDL_Event ev{};
    ev.type = wake_event_type;
    SDL_PushEvent(&ev);
  }

  // -------------------------------------------------------------------------
  // Background reader thread — runs after initialization.
  // -------------------------------------------------------------------------
  void ReaderMain() {
    ReadBuf buf;
    while (!stop_reader.load(std::memory_order_relaxed)) {
      auto chunk = proc.Read(4096, 50 /*ms*/);
      if (!chunk) break;  // process died
      if (!chunk->empty()) buf.append(*chunk);

      // Drain all complete messages from the buffer.
      while (true) {
        auto msg_opt = TryParseOneMessage(buf);
        if (!msg_opt) break;
        DispatchMessage(std::move(*msg_opt));
      }
    }
  }

  // Try to parse one complete JSON-RPC message from buf without blocking.
  // Returns nullopt if the buffer doesn't yet contain a complete message.
  std::optional<util::JsonValue> TryParseOneMessage(ReadBuf& buf) {
    static constexpr std::string_view kPrefix = "Content-Length: ";

    const std::string_view v = buf.view();

    // Find Content-Length header
    const auto nl = v.find('\n');
    if (nl == std::string_view::npos) return std::nullopt;

    std::string_view line = v.substr(0, nl);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

    if (line.substr(0, kPrefix.size()) != kPrefix) {
      // Not a content-length line — skip it (could be a blank line between messages)
      buf.consume(nl + 1);
      return std::nullopt;
    }

    const std::string_view len_sv = line.substr(kPrefix.size());
    int content_len = 0;
    const auto [ptr, ec] = std::from_chars(len_sv.data(), len_sv.data() + len_sv.size(), content_len);
    if (ec != std::errc{} || content_len <= 0) {
      buf.consume(nl + 1);
      return std::nullopt;
    }

    // Find blank line after header
    std::size_t body_start = nl + 1;
    while (body_start < v.size()) {
      const auto nl2 = v.find('\n', body_start);
      if (nl2 == std::string_view::npos) return std::nullopt;
      std::string_view hdr = v.substr(body_start, nl2 - body_start);
      if (!hdr.empty() && hdr.back() == '\r') hdr.remove_suffix(1);
      body_start = nl2 + 1;
      if (hdr.empty()) break;  // blank line found
    }

    if (v.size() - body_start < static_cast<std::size_t>(content_len)) {
      return std::nullopt;  // body not fully buffered yet
    }

    const std::string_view body = v.substr(body_start, content_len);
    auto parsed = util::ParseJson(body);
    buf.consume(body_start + content_len);
    return parsed;
  }

  // Dispatch one parsed message from the reader thread.
  void DispatchMessage(util::JsonValue msg) {
    if (msg.HasKey("id") && msg["id"].IsInt()) {
      // Response — find the pending callback.
      const int id = msg["id"].AsInt();
      std::function<void(util::JsonValue)> cb;
      {
        std::lock_guard lock(mutex);
        auto it = pending_requests.find(id);
        if (it != pending_requests.end()) {
          cb = std::move(it->second);
          pending_requests.erase(it);
        }
      }
      if (cb) {
        // Wrap in a zero-arg lambda so it can be stored in ready_callbacks.
        util::JsonValue captured = std::move(msg);
        auto ready_fn = [cb = std::move(cb), m = std::move(captured)]() mutable {
          cb(std::move(m));
        };
        std::lock_guard lock(mutex);
        ready_callbacks.push_back(std::move(ready_fn));
        PushWakeEvent();
      }
    } else if (msg.HasKey("method")) {
      // Notification — handle on the reader thread but deliver to main thread
      // via ready_callbacks to keep thread safety.
      const std::string method = msg["method"].IsString() ? msg["method"].AsString() : "";
      if (method == "textDocument/publishDiagnostics") {
        const auto& params = msg["params"];
        const std::string uri = params["uri"].IsString() ? params["uri"].AsString() : "";
        std::vector<Diagnostic> diags;
        if (params["diagnostics"].IsArray()) {
          for (const auto& d : params["diagnostics"].AsArray()) {
            Diagnostic diag;
            diag.range.start.line = d["range"]["start"]["line"].AsInt();
            diag.range.start.character = d["range"]["start"]["character"].AsInt();
            diag.range.end.line = d["range"]["end"]["line"].AsInt();
            diag.range.end.character = d["range"]["end"]["character"].AsInt();
            diag.message = d["message"].IsString() ? d["message"].AsString() : "";
            diag.severity = d["severity"].AsInt(1);
            diag.code = d["code"].IsString() ? d["code"].AsString() : "";
            diags.push_back(std::move(diag));
          }
        }
        std::lock_guard lock(mutex);
        if (diagnostics_callback) {
          auto cb = diagnostics_callback;  // copy to capture
          ready_callbacks.push_back([cb = std::move(cb),
                                     u = std::move(uri),
                                     ds = std::move(diags)]() mutable {
            cb(std::move(u), std::move(ds));
          });
        }
        PushWakeEvent();
      }
      // Other notifications (log messages, progress, etc.) are silently ignored.
    }
  }

  // -------------------------------------------------------------------------
  // Register a pending async request and return the request id.
  // -------------------------------------------------------------------------
  int RegisterPendingRequest(std::function<void(util::JsonValue)> cb) {
    std::lock_guard lock(mutex);
    const int id = next_id++;
    pending_requests[id] = std::move(cb);
    return id;
  }
};

// ---------------------------------------------------------------------------
// LspClient public API
// ---------------------------------------------------------------------------

LspClient::LspClient() : impl_(new Impl{}) {}

LspClient::~LspClient() {
  Shutdown();
  delete impl_;
}

void LspClient::SetWakeEventType(Uint32 event_type) {
  impl_->wake_event_type = event_type;
}

bool LspClient::Start(const std::vector<std::string>& command, const std::string& root_uri,
                      const std::string& language_id) {
  if (!impl_->proc.Start(command)) return false;

  impl_->root_uri = root_uri;
  impl_->language_id = language_id;

  // Negotiate capabilities: request incremental sync and common features.
  using namespace util;
  JsonObject text_doc_sync;
  text_doc_sync["dynamicRegistration"] = JsonValue(false);
  text_doc_sync["willSave"] = JsonValue(false);
  text_doc_sync["didSave"] = JsonValue(true);

  JsonObject completion_caps;
  completion_caps["dynamicRegistration"] = JsonValue(false);

  JsonObject hover_caps;
  hover_caps["dynamicRegistration"] = JsonValue(false);

  JsonObject signature_caps;
  signature_caps["dynamicRegistration"] = JsonValue(false);

  JsonObject definition_caps;
  definition_caps["dynamicRegistration"] = JsonValue(false);

  JsonObject references_caps;
  references_caps["dynamicRegistration"] = JsonValue(false);

  JsonObject rename_caps;
  rename_caps["dynamicRegistration"] = JsonValue(false);
  rename_caps["prepareSupport"] = JsonValue(false);

  JsonObject code_action_caps;
  code_action_caps["dynamicRegistration"] = JsonValue(false);

  JsonObject formatting_caps;
  formatting_caps["dynamicRegistration"] = JsonValue(false);

  JsonObject text_document_caps;
  text_document_caps["synchronization"] = JsonValue(std::move(text_doc_sync));
  text_document_caps["completion"] = JsonValue(std::move(completion_caps));
  text_document_caps["hover"] = JsonValue(std::move(hover_caps));
  text_document_caps["signatureHelp"] = JsonValue(std::move(signature_caps));
  text_document_caps["definition"] = JsonValue(std::move(definition_caps));
  text_document_caps["references"] = JsonValue(std::move(references_caps));
  text_document_caps["rename"] = JsonValue(std::move(rename_caps));
  text_document_caps["codeAction"] = JsonValue(std::move(code_action_caps));
  text_document_caps["formatting"] = JsonValue(std::move(formatting_caps));

  JsonObject caps;
  caps["textDocument"] = JsonValue(std::move(text_document_caps));

  JsonObject init_params;
  init_params["processId"] = JsonValue(static_cast<std::int64_t>(impl_->proc.pid()));
  init_params["rootUri"] = JsonValue(root_uri);
  init_params["capabilities"] = JsonValue(std::move(caps));

  const int init_id = [&]() {
    std::lock_guard lock(impl_->mutex);
    return impl_->next_id++;
  }();
  const auto req = impl_->MakeRequest(init_id, "initialize", JsonValue(std::move(init_params)));
  if (!impl_->SendMessage(req)) {
    impl_->proc.Shutdown();
    return false;
  }

  // Wait synchronously for the initialize response (before starting the reader thread).
  ReadBuf buf;
  bool got_init = false;
  for (int attempts = 0; attempts < 60; ++attempts) {
    auto resp_opt = impl_->ReadJsonRpcBlocking(buf, 500);
    if (!resp_opt) break;
    const auto& resp = *resp_opt;
    if (!resp.HasKey("id") || resp["id"].AsInt() != init_id) continue;

    // Parse server capabilities to detect sync mode.
    if (resp.HasKey("result")) {
      const auto& result = resp["result"];
      if (result.HasKey("capabilities")) {
        const auto& server_caps = result["capabilities"];
        const auto& sync = server_caps["textDocumentSync"];
        // 1 = full, 2 = incremental
        int sync_kind = 1;
        if (sync.IsInt()) {
          sync_kind = sync.AsInt();
        } else if (sync.HasKey("change")) {
          sync_kind = sync["change"].AsInt(1);
        }
        impl_->supports_incremental_sync = (sync_kind == 2);
      }
      impl_->initialized = true;
      got_init = true;
    }
    break;
  }

  if (!got_init) {
    impl_->proc.Shutdown();
    return false;
  }

  // Send "initialized" notification.
  impl_->SendMessage(impl_->MakeNotification("initialized", JsonValue(JsonObject{})));

  // Start the background reader thread.
  impl_->stop_reader.store(false);
  impl_->reader_thread = std::thread([this]() { impl_->ReaderMain(); });
  return true;
}

bool LspClient::IsRunning() const { return impl_->proc.IsRunning(); }

bool LspClient::SupportsIncrementalSync() const {
  return impl_->supports_incremental_sync;
}

bool LspClient::HasOpenDocument(const std::string& uri) const {
  return impl_->document_versions.contains(uri);
}

void LspClient::SetDiagnosticsCallback(OnPublishDiagnostics callback) {
  std::lock_guard lock(impl_->mutex);
  impl_->diagnostics_callback = std::move(callback);
}

void LspClient::DrainCallbacks() {
  std::vector<std::function<void()>> cbs;
  {
    std::lock_guard lock(impl_->mutex);
    cbs.swap(impl_->ready_callbacks);
  }
  for (auto& cb : cbs) {
    cb();
  }
}

bool LspClient::DidOpen(const std::string& uri, const std::string& language_id,
                        const std::string& text) {
  using namespace util;
  impl_->document_versions[uri] = 1;
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);
  text_doc["languageId"] = JsonValue(language_id);
  text_doc["version"] = JsonValue(static_cast<std::int64_t>(1));
  text_doc["text"] = JsonValue(text);
  JsonObject params;
  params["textDocument"] = JsonValue(std::move(text_doc));
  return impl_->SendMessage(impl_->MakeNotification("textDocument/didOpen", JsonValue(std::move(params))));
}

bool LspClient::DidChange(const std::string& uri, const std::string& text) {
  using namespace util;
  const int version = ++impl_->document_versions[uri];
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);
  text_doc["version"] = JsonValue(static_cast<std::int64_t>(version));
  JsonObject change;
  change["text"] = JsonValue(text);
  JsonArray changes;
  changes.push_back(JsonValue(std::move(change)));
  JsonObject params;
  params["textDocument"] = JsonValue(std::move(text_doc));
  params["contentChanges"] = JsonValue(std::move(changes));
  return impl_->SendMessage(impl_->MakeNotification("textDocument/didChange", JsonValue(std::move(params))));
}

bool LspClient::DidChangeIncremental(const std::string& uri,
                                     Range changed_range,
                                     const std::string& new_text) {
  if (!impl_->supports_incremental_sync) {
    return false;
  }
  using namespace util;
  const int version = ++impl_->document_versions[uri];
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);
  text_doc["version"] = JsonValue(static_cast<std::int64_t>(version));

  JsonObject start_obj;
  start_obj["line"] = JsonValue(static_cast<std::int64_t>(changed_range.start.line));
  start_obj["character"] = JsonValue(static_cast<std::int64_t>(changed_range.start.character));
  JsonObject end_obj;
  end_obj["line"] = JsonValue(static_cast<std::int64_t>(changed_range.end.line));
  end_obj["character"] = JsonValue(static_cast<std::int64_t>(changed_range.end.character));
  JsonObject range_obj;
  range_obj["start"] = JsonValue(std::move(start_obj));
  range_obj["end"] = JsonValue(std::move(end_obj));

  JsonObject change;
  change["range"] = JsonValue(std::move(range_obj));
  change["text"] = JsonValue(new_text);

  JsonArray changes;
  changes.push_back(JsonValue(std::move(change)));

  JsonObject params;
  params["textDocument"] = JsonValue(std::move(text_doc));
  params["contentChanges"] = JsonValue(std::move(changes));
  return impl_->SendMessage(impl_->MakeNotification("textDocument/didChange", JsonValue(std::move(params))));
}

bool LspClient::DidSave(const std::string& uri) {
  using namespace util;
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);
  JsonObject params;
  params["textDocument"] = JsonValue(std::move(text_doc));
  return impl_->SendMessage(impl_->MakeNotification("textDocument/didSave", JsonValue(std::move(params))));
}

bool LspClient::DidClose(const std::string& uri) {
  using namespace util;
  impl_->document_versions.erase(uri);
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);
  JsonObject params;
  params["textDocument"] = JsonValue(std::move(text_doc));
  return impl_->SendMessage(impl_->MakeNotification("textDocument/didClose", JsonValue(std::move(params))));
}

// ---------------------------------------------------------------------------
// Async request helpers
// ---------------------------------------------------------------------------

static util::JsonValue MakeTextDocPosition(const std::string& uri,
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

static std::vector<LspClient::Location> ParseLocations(const util::JsonValue& result) {
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

void LspClient::RequestHoverAsync(std::string uri, Position pos, HoverCallback callback) {
  if (!IsRunning() || !callback) return;
  const auto params = MakeTextDocPosition(uri, pos);
  const int id = impl_->RegisterPendingRequest(
      [cb = std::move(callback)](util::JsonValue resp) {
        if (resp.HasKey("result")) {
          cb(std::optional<util::JsonValue>(resp["result"]));
        } else {
          cb(std::nullopt);
        }
      });
  impl_->SendMessage(impl_->MakeRequest(id, "textDocument/hover", params));
}

void LspClient::RequestCompletionAsync(std::string uri, Position pos, CompletionCallback callback) {
  if (!IsRunning() || !callback) return;
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
          items.push_back(std::move(ci));
        }
        cb(std::optional<std::vector<CompletionItem>>(std::move(items)));
      });
  impl_->SendMessage(impl_->MakeRequest(id, "textDocument/completion", params));
}

void LspClient::RequestCodeActionAsync(std::string uri, Range range, CodeActionCallback callback) {
  if (!IsRunning() || !callback) return;
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
  impl_->SendMessage(impl_->MakeRequest(id, "textDocument/codeAction", JsonValue(std::move(params))));
}

void LspClient::RequestFormattingAsync(std::string uri, int tab_size, bool insert_spaces,
                                        FormattingCallback callback) {
  if (!IsRunning() || !callback) return;
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
  impl_->SendMessage(impl_->MakeRequest(id, "textDocument/formatting", JsonValue(std::move(params))));
}

void LspClient::RequestGoToDefinitionAsync(std::string uri, Position pos, LocationCallback callback) {
  if (!IsRunning() || !callback) return;
  const auto params = MakeTextDocPosition(uri, pos);
  const int id = impl_->RegisterPendingRequest(
      [cb = std::move(callback)](util::JsonValue resp) {
        if (!resp.HasKey("result")) { cb(std::nullopt); return; }
        cb(std::optional<std::vector<Location>>(ParseLocations(resp["result"])));
      });
  impl_->SendMessage(impl_->MakeRequest(id, "textDocument/definition", params));
}

void LspClient::RequestFindReferencesAsync(std::string uri, Position pos,
                                            bool include_declaration, LocationCallback callback) {
  if (!IsRunning() || !callback) return;
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
  impl_->SendMessage(impl_->MakeRequest(id, "textDocument/references", JsonValue(std::move(params))));
}

void LspClient::RequestRenameAsync(std::string uri, Position pos, std::string new_name,
                                    RenameCallback callback) {
  if (!IsRunning() || !callback) return;
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
          // result.changes is { uri: TextEdit[] }
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
  impl_->SendMessage(impl_->MakeRequest(id, "textDocument/rename", JsonValue(std::move(params))));
}

void LspClient::Shutdown() {
  if (!impl_->initialized) {
    impl_->proc.Shutdown();
    return;
  }

  // Stop the reader thread first.
  impl_->stop_reader.store(true);

  // Send shutdown request (fire-and-forget; don't block waiting for response).
  using namespace util;
  const int shutdown_id = [&]() {
    std::lock_guard lock(impl_->mutex);
    return impl_->next_id++;
  }();
  impl_->SendMessage(impl_->MakeRequest(shutdown_id, "shutdown", JsonValue(JsonObject{})));
  impl_->SendMessage(impl_->MakeNotification("exit", JsonValue(JsonObject{})));

  impl_->proc.Shutdown(1000);

  if (impl_->reader_thread.joinable()) {
    impl_->reader_thread.join();
  }

  impl_->initialized = false;
}

}  // namespace microide::workspace
