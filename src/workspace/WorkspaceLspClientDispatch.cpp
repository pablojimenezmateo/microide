// LspClient::Impl message dispatch: routing inbound frames to response callbacks,
// diagnostics, and progress; answering server-initiated requests; and the
// timeout sweep that fails stranded requests. Split out of
// WorkspaceLspClientInternal.h to keep the transport header declaration-only.
#include "workspace/WorkspaceLspClientInternal.h"

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "util/StartupTrace.h"
#include "workspace/LspProtocol.h"

namespace microide::workspace {

// Reply to a server-initiated request. `id` is echoed verbatim (it may be an
// int or a string per JSON-RPC). Server requests only arrive post-initialize,
// so these flow through the normal outbound queue.
void LspClient::Impl::SendResponseResult(const util::JsonValue& id, util::JsonValue result) {
  using namespace util;
  JsonValue msg = MakeResponse(id);
  std::get<JsonObject>(msg.v)["result"] = std::move(result);
  SendMessageAfterInitialize(std::move(msg));
}

void LspClient::Impl::SendResponseError(const util::JsonValue& id, int code, std::string message) {
  using namespace util;
  JsonObject error;
  error["code"] = JsonValue(static_cast<std::int64_t>(code));
  error["message"] = JsonValue(std::move(message));
  JsonValue msg = MakeResponse(id);
  std::get<JsonObject>(msg.v)["error"] = JsonValue(std::move(error));
  SendMessageAfterInitialize(std::move(msg));
}

// Server -> client requests must always get a reply, or chatty servers
// (clangd, Roslyn/OmniSharp) log errors or stall.
void LspClient::Impl::HandleServerRequest(const util::JsonValue& id, const std::string& method,
                                          const util::JsonValue& params) {
  using namespace util;
  if (method == "workspace/configuration") {
    JsonArray result;
    for (const auto& item : params["items"].AsArray()) {
      const std::string& section = item["section"].AsString();
      JsonValue value;  // Null when we have nothing configured.
      if (settings.IsObject()) {
        if (section.empty()) {
          value = settings;
        } else if (settings.HasKey(section)) {
          value = settings[section];
        }
      }
      result.push_back(std::move(value));
    }
    SendResponseResult(id, JsonValue(std::move(result)));
    return;
  }
  // Capabilities are treated as static; accept dynamic (un)registration and
  // progress-token creation with an empty result.
  if (method == "client/registerCapability" || method == "client/unregisterCapability" ||
      method == "window/workDoneProgress/create" || method == "window/showMessageRequest" ||
      method == "window/showDocument") {
    SendResponseResult(id, JsonValue(nullptr));
    return;
  }
  if (method == "workspace/applyEdit") {
    std::function<bool(WorkspaceEdit)> handler;
    {
      std::lock_guard lock(mutex);
      handler = apply_edit_handler;
    }
    if (!handler) {
      // No host applier bound: report not-applied so the server recovers rather
      // than wait forever.
      JsonObject obj;
      obj["applied"] = JsonValue(false);
      SendResponseResult(id, JsonValue(std::move(obj)));
      return;
    }
    // Apply on the MAIN thread (it mutates open buffers / writes files), then reply
    // with the real applied flag. The mailbox task is dropped unrun if the client
    // is destroyed first, so capturing `this` cannot dangle.
    WorkspaceEdit edit = lsp_protocol::ParseWorkspaceEdit(params["edit"]);
    util::JsonValue captured_id = id;
    main_mailbox.Post([this, captured_id = std::move(captured_id), edit = std::move(edit),
                       handler = std::move(handler)]() mutable {
      const bool applied = handler(std::move(edit));
      JsonObject obj;
      obj["applied"] = JsonValue(applied);
      SendResponseResult(captured_id, JsonValue(std::move(obj)));
    });
    return;
  }
  SendResponseError(id, -32601, "method not found: " + method);
}

void LspClient::Impl::DispatchMessage(util::JsonValue msg) {
  const bool has_method = msg.HasKey("method") && msg["method"].IsString();
  const bool has_id = msg.HasKey("id") && !msg["id"].IsNull();
  if (shutting_down.load(std::memory_order_acquire)) {
    if (has_id && !has_method && (msg["id"].IsInt() || msg["id"].IsDouble())) {
      const int id = static_cast<int>(msg["id"].AsInt());
      std::lock_guard lock(mutex);
      if (id == shutdown_request_id) {
        shutdown_response_received = true;
        shutdown_cv.notify_all();
      }
    }
    return;
  }
  if (has_method && has_id) {
    HandleServerRequest(msg["id"], msg["method"].AsString(), msg["params"]);
    return;
  }
  // Accept the id echoed as a JSON float (e.g. "id": 5.0) as well as an integer:
  // some servers round-trip our integer ids through a float. AsInt() truncates
  // 5.0 -> 5. Mirrors the DAP client's request_seq gate.
  if (has_id && !has_method && (msg["id"].IsInt() || msg["id"].IsDouble())) {
    const int id = static_cast<int>(msg["id"].AsInt());
    std::function<void(util::JsonValue)> cb;
    {
      std::lock_guard lock(mutex);
      auto it = pending_requests.find(id);
      if (it != pending_requests.end()) {
        cb = std::move(it->second.callback);
        pending_requests.erase(it);
      }
    }
    if (cb) {
      util::JsonValue captured = std::move(msg);
      main_mailbox.Post([cb = std::move(cb), m = std::move(captured)]() mutable {
        util::StartupTrace::Scope scope("LspClient::DispatchResponse");
        cb(std::move(m));
      });
    }
  } else if (msg.HasKey("method")) {
    const std::string& method = msg["method"].AsString();
    if (method == "textDocument/publishDiagnostics") {
      util::StartupTrace::Scope scope("LspClient::DispatchDiagnostics");
      const auto& params = msg["params"];
      std::string uri = params["uri"].AsString();
      const util::JsonValue& version_val = params["version"];
      std::vector<Diagnostic> diags = lsp_protocol::ParseDiagnostics(params["diagnostics"]);
      TraceLspLifecycle(language_id, proc.pid(), "publishDiagnostics", uri);
      OnPublishDiagnostics cb;
      {
        std::lock_guard lock(mutex);
        // Drop diagnostics computed against a document version we have already
        // superseded with a newer edit. The server may still be reporting on
        // v_old while the user has typed on to v_new; applying v_old ranges to
        // the v_new buffer paints squiggles on the wrong spans until the v_new
        // diagnostics arrive. Only gate when the server sent a version and the
        // document is still open (tracked) at a strictly newer version. Accept a
        // float-echoed version ("version": 3.0) as well as an integer, mirroring
        // the response-id gate above — a server that round-trips our integer ids
        // through a float does the same to the didChange version we sent.
        if (version_val.IsInt() || version_val.IsDouble()) {
          const auto it = document_versions.find(uri);
          if (it != document_versions.end() && version_val.AsInt() < it->second) {
            return;
          }
        }
        cb = diagnostics_callback;
      }
      if (cb) {
        main_mailbox.Post(
            [cb = std::move(cb), u = std::move(uri), ds = std::move(diags)]() mutable {
              cb(std::move(u), std::move(ds));
            });
      }
    } else if (method == "$/progress") {
      SetProgressReadiness(msg["params"]["value"]);
    }
  }
}

// Synthesize empty responses for pending requests so a non-responding server never
// strands a request (and its UI loading state). `only_expired` fails just those
// past their deadline (the periodic sweep); otherwise fails all (the server exited
// and no response can ever arrive). Runs on the I/O thread: handlers are posted to
// the main-thread mailbox. The empty `{}` envelope has no "result" key, so every
// response handler degrades to its no-result path.
void LspClient::Impl::FailPendingRequests(bool only_expired) {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(mutex);
  bool any = false;
  for (auto it = pending_requests.begin(); it != pending_requests.end();) {
    if (only_expired && now < it->second.deadline) {
      ++it;
      continue;
    }
    auto cb = std::move(it->second.callback);
    it = pending_requests.erase(it);
    main_mailbox.PostWithoutWake([cb = std::move(cb)]() mutable {
      cb(util::JsonValue(util::JsonObject{}));
    });
    any = true;
  }
  if (any) {
    main_mailbox.PushWake();
  }
}

}  // namespace microide::workspace
