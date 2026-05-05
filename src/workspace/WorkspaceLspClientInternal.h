#pragma once

#include "workspace/WorkspaceLspClient.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

#include "util/StartupTrace.h"

namespace microide::workspace {

namespace {

bool LspLifecycleTraceEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MICROIDE_TRACE_LSP_LIFECYCLE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}

void TraceLspLifecycle(std::string_view language_id,
                       int pid,
                       std::string_view phase,
                       std::string_view detail = {}) {
  if (!LspLifecycleTraceEnabled()) {
    return;
  }
  if (detail.empty()) {
    std::fprintf(stderr, "[lsp] %.*s pid=%d %.*s\n", static_cast<int>(language_id.size()),
                 language_id.data(), pid, static_cast<int>(phase.size()), phase.data());
    return;
  }
  std::fprintf(stderr, "[lsp] %.*s pid=%d %.*s | %.*s\n",
               static_cast<int>(language_id.size()), language_id.data(), pid,
               static_cast<int>(phase.size()), phase.data(), static_cast<int>(detail.size()),
               detail.data());
}

}  // namespace

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
  std::mutex send_mutex;
  std::condition_variable send_cv;

  // Reader thread state
  std::thread reader_thread;
  std::atomic<bool> stop_reader{false};
  std::thread writer_thread;
  std::atomic<bool> stop_writer{false};

  // Initialization thread state
  std::thread init_thread;
  std::atomic<bool> initializing{false};
  std::atomic<bool> stop_init{false};
  std::thread shutdown_thread;
  std::atomic<bool> shutdown_started{false};
  std::atomic<bool> shutdown_complete{false};
  std::atomic<bool> shutting_down{false};
  std::atomic<bool> process_shutdown_started{false};
  std::condition_variable shutdown_cv;
  bool shutdown_response_received = false;
  int shutdown_request_id = 0;

  // Per-request pending callbacks (keyed by request id), guarded by mutex.
  std::unordered_map<int, std::function<void(util::JsonValue)>> pending_requests;

  // Ready callbacks waiting to be drained on the main thread, guarded by mutex.
  std::vector<std::function<void()>> ready_callbacks;

  // Diagnostics callback — set from main thread, called on main thread via ready_callbacks.
  OnPublishDiagnostics diagnostics_callback;

  std::unordered_map<std::string, int> document_versions;
  std::vector<std::string> deferred_messages;
  std::deque<std::string> outbound_messages;
  int next_id = 1;
  std::string root_uri;
  std::string language_id;
  std::string last_error;
  std::string last_error_snapshot;
  std::atomic<bool> initialized{false};
  std::atomic<bool> supports_incremental_sync{false};
  std::atomic<Uint32> wake_event_type{0};

  void SetLastError(std::string message) {
    std::lock_guard lock(mutex);
    last_error = std::move(message);
  }

  std::string GetLastErrorCopy() {
    std::lock_guard lock(mutex);
    return last_error;
  }

  int GetNextId() {
    std::lock_guard lock(mutex);
    return next_id++;
  }

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

  std::string SerializeMessage(const util::JsonValue& msg) const {
    const std::string json = util::SerializeJson(msg);
    std::string rfc;
    rfc.reserve(32 + json.size());
    rfc += "Content-Length: ";
    rfc += std::to_string(json.size());
    rfc += "\r\n\r\n";
    rfc += json;
    return rfc;
  }

  bool SendSerializedMessageUnlocked(std::string_view message) {
    return proc.Write(message);
  }

  void StartWriterThreadLocked() {
    if (writer_thread.joinable()) {
      return;
    }
    stop_writer.store(false, std::memory_order_release);
    writer_thread = std::thread([this]() { WriterMain(); });
  }

  void StopWriterThread() {
    stop_writer.store(true, std::memory_order_release);
    send_cv.notify_all();
    if (writer_thread.joinable()) {
      writer_thread.join();
    }
  }

  void WriterMain() {
    while (true) {
      std::string message;
      {
        std::unique_lock lock(send_mutex);
        send_cv.wait(lock, [this]() {
          return stop_writer.load(std::memory_order_acquire) || !outbound_messages.empty();
        });
        if (stop_writer.load(std::memory_order_acquire) && outbound_messages.empty()) {
          return;
        }
        message = std::move(outbound_messages.front());
        outbound_messages.pop_front();
      }

      if (!SendSerializedMessageUnlocked(message)) {
        SetLastError("failed to send message to language server");
        return;
      }
    }
  }

  bool SendMessageImmediate(const util::JsonValue& msg, bool allow_during_shutdown = false) {
    if (!allow_during_shutdown && shutting_down.load(std::memory_order_acquire)) {
      return false;
    }
    const std::string serialized = SerializeMessage(msg);
    std::lock_guard lock(send_mutex);
    return SendSerializedMessageUnlocked(serialized);
  }

  bool SendMessageAfterInitialize(const util::JsonValue& msg) {
    if (shutting_down.load(std::memory_order_acquire)) {
      return false;
    }
    const std::string serialized = SerializeMessage(msg);
    std::lock_guard lock(send_mutex);
    if (shutting_down.load(std::memory_order_acquire)) {
      return false;
    }
    if (!initialized.load(std::memory_order_acquire)) {
      deferred_messages.push_back(serialized);
      return true;
    }
    if (!proc.IsRunning()) {
      return false;
    }
    outbound_messages.push_back(serialized);
    send_cv.notify_one();
    return true;
  }

  void ClearDeferredMessages() {
    std::lock_guard lock(send_mutex);
    deferred_messages.clear();
    outbound_messages.clear();
  }

  void ResetProtocolState() {
    std::lock_guard lock(mutex);
    pending_requests.clear();
    ready_callbacks.clear();
    document_versions.clear();
    shutdown_response_received = false;
    shutdown_request_id = 0;
  }

  void ShutdownProcessOnce(int timeout_ms = 3000) {
    bool expected = false;
    if (!process_shutdown_started.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      return;
    }
    proc.Shutdown(timeout_ms);
  }

  std::optional<util::JsonValue> ReadJsonRpcBlocking(ReadBuf& buf, int timeout_ms) {
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

  void PushWakeEvent() const {
    const Uint32 event_type = wake_event_type.load(std::memory_order_acquire);
    if (event_type == 0) return;
    SDL_Event ev{};
    ev.type = event_type;
    SDL_PushEvent(&ev);
  }

  void ReaderMain() {
    ReadBuf buf;
    while (!stop_reader.load(std::memory_order_relaxed)) {
      auto chunk = proc.Read(4096, 50 /*ms*/);
      if (!chunk) break;
      if (!chunk->empty()) buf.append(*chunk);

      while (true) {
        auto msg_opt = TryParseOneMessage(buf);
        if (!msg_opt) break;
        DispatchMessage(std::move(*msg_opt));
      }
    }
  }

  std::optional<util::JsonValue> TryParseOneMessage(ReadBuf& buf) {
    static constexpr std::string_view kPrefix = "Content-Length: ";

    const std::string_view v = buf.view();
    const auto nl = v.find('\n');
    if (nl == std::string_view::npos) return std::nullopt;

    std::string_view line = v.substr(0, nl);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

    if (line.substr(0, kPrefix.size()) != kPrefix) {
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

    std::size_t body_start = nl + 1;
    while (body_start < v.size()) {
      const auto nl2 = v.find('\n', body_start);
      if (nl2 == std::string_view::npos) return std::nullopt;
      std::string_view hdr = v.substr(body_start, nl2 - body_start);
      if (!hdr.empty() && hdr.back() == '\r') hdr.remove_suffix(1);
      body_start = nl2 + 1;
      if (hdr.empty()) break;
    }

    if (v.size() - body_start < static_cast<std::size_t>(content_len)) {
      return std::nullopt;
    }

    const std::string_view body = v.substr(body_start, content_len);
    auto parsed = util::ParseJson(body);
    buf.consume(body_start + content_len);
    return parsed;
  }

  void DispatchMessage(util::JsonValue msg) {
    if (shutting_down.load(std::memory_order_acquire)) {
      if (msg.HasKey("id") && msg["id"].IsInt()) {
        const int id = msg["id"].AsInt();
        std::lock_guard lock(mutex);
        if (id == shutdown_request_id) {
          shutdown_response_received = true;
          shutdown_cv.notify_all();
        }
      }
      return;
    }
    if (msg.HasKey("id") && msg["id"].IsInt()) {
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
        util::JsonValue captured = std::move(msg);
        auto ready_fn = [cb = std::move(cb), m = std::move(captured)]() mutable {
          util::StartupTrace::Scope scope("LspClient::DispatchResponse");
          cb(std::move(m));
        };
        std::lock_guard lock(mutex);
        ready_callbacks.push_back(std::move(ready_fn));
        PushWakeEvent();
      }
    } else if (msg.HasKey("method")) {
      const std::string method = msg["method"].IsString() ? msg["method"].AsString() : "";
      if (method == "textDocument/publishDiagnostics") {
        util::StartupTrace::Scope scope("LspClient::DispatchDiagnostics");
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
          auto cb = diagnostics_callback;
          ready_callbacks.push_back([cb = std::move(cb), u = std::move(uri), ds = std::move(diags)]() mutable {
            cb(std::move(u), std::move(ds));
          });
        }
        PushWakeEvent();
      }
    }
  }

  int RegisterPendingRequest(std::function<void(util::JsonValue)> cb) {
    std::lock_guard lock(mutex);
    const int id = next_id++;
    pending_requests[id] = std::move(cb);
    return id;
  }

  void RemovePendingRequest(int id) {
    std::lock_guard lock(mutex);
    pending_requests.erase(id);
  }

  void DoInitializeBlocking() {
    util::StartupTrace::Scope trace_scope("LspClient::DoInitializeBlocking");
    initializing.store(true, std::memory_order_release);

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
#if defined(__unix__) || defined(__APPLE__)
    init_params["processId"] = JsonValue(static_cast<std::int64_t>(::getpid()));
#else
    init_params["processId"] = JsonValue(static_cast<std::int64_t>(0));
#endif
    init_params["rootUri"] = JsonValue(root_uri);
    init_params["capabilities"] = JsonValue(std::move(caps));

    const int init_id = [&]() {
      std::lock_guard lock(mutex);
      return next_id++;
    }();
    const auto req = MakeRequest(init_id, "initialize", JsonValue(std::move(init_params)));
    if (!SendMessageImmediate(req)) {
      SetLastError("failed to send initialize request to language server");
      ShutdownProcessOnce();
      ClearDeferredMessages();
      initializing.store(false, std::memory_order_release);
      return;
    }

    ReadBuf buf;
    bool got_init = false;
    {
      util::StartupTrace::Scope wait_init_scope("LspClient::DoInitializeBlocking::WaitInitializeResponse");
      for (int attempts = 0; attempts < 60; ++attempts) {
        if (stop_init.load(std::memory_order_acquire)) {
          ShutdownProcessOnce();
          ClearDeferredMessages();
          initializing.store(false, std::memory_order_release);
          return;
        }
        auto resp_opt = TryParseOneMessage(buf);
        if (!resp_opt) {
          auto chunk = proc.Read(4096, 500);
          if (!chunk) break;
          if (!chunk->empty()) buf.append(*chunk);
          continue;
        }
        const auto& resp = *resp_opt;
        if (!resp.HasKey("id") || resp["id"].AsInt() != init_id) continue;

        if (resp.HasKey("result")) {
          const auto& result = resp["result"];
          if (result.HasKey("capabilities")) {
            const auto& server_caps = result["capabilities"];
            const auto& sync = server_caps["textDocumentSync"];
            int sync_kind = 1;
            if (sync.IsInt()) {
              sync_kind = sync.AsInt();
            } else if (sync.HasKey("change")) {
              sync_kind = sync["change"].AsInt(1);
            }
            supports_incremental_sync.store(sync_kind == 2, std::memory_order_release);
          }
          got_init = true;
        }
        break;
      }
    }

    if (!got_init) {
      (void)proc.IsRunning();
      const std::optional<int> exit_code = proc.exit_code();
      if (exit_code.has_value()) {
        SetLastError("language server exited before initialize response (exit code " +
                     std::to_string(*exit_code) + ")");
      } else {
        SetLastError("timed out waiting for initialize response from language server");
      }
      ShutdownProcessOnce();
      ClearDeferredMessages();
      initializing.store(false, std::memory_order_release);
      return;
    }

    {
      std::lock_guard lock(send_mutex);
      if (!SendSerializedMessageUnlocked(
              SerializeMessage(MakeNotification("initialized", JsonValue(JsonObject{}))))) {
        SetLastError("failed to send initialized notification to language server");
        deferred_messages.clear();
        ShutdownProcessOnce();
        initializing.store(false, std::memory_order_release);
        return;
      }
      initialized.store(true, std::memory_order_release);
      StartWriterThreadLocked();
      for (std::string& message : deferred_messages) {
        outbound_messages.push_back(std::move(message));
      }
      deferred_messages.clear();
      send_cv.notify_all();
    }

    stop_reader.store(false);
    reader_thread = std::thread([this]() { ReaderMain(); });

    initializing.store(false, std::memory_order_release);
  }

  void DoShutdown() {
    TraceLspLifecycle(language_id, proc.pid(), "shutdown-begin");
    shutting_down.store(true, std::memory_order_release);
    stop_init.store(true);
    if (!initialized.load(std::memory_order_acquire)) {
      TraceLspLifecycle(language_id, proc.pid(), "preinit-cancel");
      StopWriterThread();
      ShutdownProcessOnce(0);
      if (init_thread.joinable()) {
        init_thread.join();
      }
      ClearDeferredMessages();
      stop_reader.store(true, std::memory_order_release);
      ResetProtocolState();
      initialized.store(false, std::memory_order_release);
      initializing.store(false, std::memory_order_release);
      supports_incremental_sync.store(false, std::memory_order_release);
      shutting_down.store(false, std::memory_order_release);
      shutdown_complete.store(true, std::memory_order_release);
      PushWakeEvent();
      return;
    }

    if (init_thread.joinable()) {
      init_thread.join();
    }
    StopWriterThread();
    ClearDeferredMessages();

    using namespace util;
    int shutdown_id = 0;
    {
      std::lock_guard lock(mutex);
      shutdown_response_received = false;
      shutdown_request_id = next_id++;
      shutdown_id = shutdown_request_id;
    }
    const bool sent_shutdown =
        SendMessageImmediate(MakeRequest(shutdown_id, "shutdown", JsonValue(JsonObject{})), true);
    TraceLspLifecycle(language_id, proc.pid(), "shutdown-request",
                      sent_shutdown ? "sent" : "send-failed");
    if (sent_shutdown) {
      std::unique_lock lock(mutex);
      const bool got_shutdown_response =
          shutdown_cv.wait_for(lock, std::chrono::milliseconds(750), [this]() {
        return shutdown_response_received;
      });
      TraceLspLifecycle(language_id, proc.pid(), "shutdown-response",
                        got_shutdown_response ? "received" : "timeout");
    }
    if (proc.IsRunning()) {
      (void)SendMessageImmediate(MakeNotification("exit", JsonValue(JsonObject{})), true);
      TraceLspLifecycle(language_id, proc.pid(), "exit-notification", "sent");
      proc.CloseStdin();
      TraceLspLifecycle(language_id, proc.pid(), "stdin", "closed");
    }

    const auto graceful_shutdown_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (proc.IsRunning() && std::chrono::steady_clock::now() < graceful_shutdown_deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    TraceLspLifecycle(language_id, proc.pid(), "graceful-wait",
                      proc.IsRunning() ? "still-running" : "exited");

    stop_reader.store(true, std::memory_order_release);
    if (proc.IsRunning()) {
      TraceLspLifecycle(language_id, proc.pid(), "forced-shutdown");
      ShutdownProcessOnce(1000);
    }

    if (reader_thread.joinable()) {
      reader_thread.join();
    }

    ResetProtocolState();
    initialized.store(false, std::memory_order_release);
    initializing.store(false, std::memory_order_release);
    supports_incremental_sync.store(false, std::memory_order_release);
    shutting_down.store(false, std::memory_order_release);
    shutdown_complete.store(true, std::memory_order_release);
    const std::optional<int> code = proc.exit_code();
    if (code.has_value()) {
      const std::string detail = "exit-code=" + std::to_string(*code);
      TraceLspLifecycle(language_id, -1, "shutdown-complete", detail);
    } else {
      TraceLspLifecycle(language_id, -1, "shutdown-complete");
    }
    PushWakeEvent();
  }

  void BeginShutdown() {
    bool expected = false;
    if (!shutdown_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return;
    }
    shutdown_complete.store(false, std::memory_order_release);
    shutdown_thread = std::thread([this]() { DoShutdown(); });
  }

  void WaitForShutdown() {
    BeginShutdown();
    if (shutdown_thread.joinable()) {
      shutdown_thread.join();
    }
  }
};

}  // namespace microide::workspace
