#pragma once

#include "workspace/WorkspaceLspClient.h"
#include "workspace/LspProtocol.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <poll.h>
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
  struct QueuedMessage {
    std::string serialized;
    std::function<std::string()> build_serialized;

    std::string TakeSerialized() && {
      if (!serialized.empty()) {
        return std::move(serialized);
      }
      return build_serialized ? build_serialized() : std::string{};
    }
  };

  platform::AsyncSubprocess proc;
  std::mutex mutex;
  std::mutex send_mutex;   // guards the outbound/deferred queues
  std::mutex write_mutex;  // serializes every proc.Write so stdin is never written by two threads

  // Single I/O thread state. One thread per server reads stdout and writes stdin;
  // it blocks in poll() over stdout + a self-pipe wakeup, so it makes no
  // fixed-cadence idle wakeups and reacts immediately to data or new outbound.
  // io_buf is filled first by the initialize handshake and then handed to the I/O
  // thread; keeping it a member preserves any bytes the server pushed right after
  // the initialize response (e.g. clangd's early registerCapability / progress /
  // configuration requests) across that handoff.
  ReadBuf io_buf;
  std::thread io_thread;
  std::atomic<bool> stop_io{false};
  std::mutex wake_mutex;          // guards wake_pipe_ open/close/Wake (brief, non-blocking)
  int wake_pipe_[2] = {-1, -1};  // [0]=read (polled by I/O thread), [1]=write (Wake())
  int cached_stdout_fd_ = -1;

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

  // When true, the client behaves as a connected server for unit tests (no subprocess).
  std::atomic<bool> test_stub_mode{false};
  std::function<void(std::string, DocumentSymbolCallback)> test_document_symbol_handler;

  // Diagnostics callback — set from main thread, called on main thread via ready_callbacks.
  OnPublishDiagnostics diagnostics_callback;

  std::unordered_map<std::string, int> document_versions;
  std::deque<QueuedMessage> deferred_messages;
  std::deque<QueuedMessage> outbound_messages;
  int next_id = 1;
  std::string root_uri;
  std::string language_id;
  util::JsonValue initialization_options;  // LSP initializationOptions (object or Null)
  util::JsonValue settings;                // answers workspace/configuration (object or Null)
  std::string last_error;
  std::string last_error_snapshot;
  LspClient::ReadinessSnapshot readiness_snapshot;
  std::atomic<bool> initialized{false};
  std::atomic<bool> supports_incremental_sync{false};
  std::atomic<Uint32> wake_event_type{0};

  void SetLastError(std::string message) {
    std::lock_guard lock(mutex);
    last_error = std::move(message);
    readiness_snapshot.state = LspClient::ReadinessSnapshot::State::Failed;
    readiness_snapshot.message = last_error;
    readiness_snapshot.indexed_count = 0;
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

  // Self-pipe wakeup: the I/O thread blocks in poll() over stdout + wake_pipe_[0];
  // enqueuing outbound work writes a byte here to break the poll immediately.
  void OpenWakePipe() {
#if defined(__unix__) || defined(__APPLE__)
    std::lock_guard lock(wake_mutex);
    if (wake_pipe_[0] >= 0) {
      return;
    }
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
      return;
    }
    ::fcntl(fds[0], F_SETFL, O_NONBLOCK);
    ::fcntl(fds[1], F_SETFL, O_NONBLOCK);
    wake_pipe_[0] = fds[0];
    wake_pipe_[1] = fds[1];
#endif
  }

  void CloseWakePipe() {
#if defined(__unix__) || defined(__APPLE__)
    std::lock_guard lock(wake_mutex);
    if (wake_pipe_[1] >= 0) {
      ::close(wake_pipe_[1]);
      wake_pipe_[1] = -1;
    }
    if (wake_pipe_[0] >= 0) {
      ::close(wake_pipe_[0]);
      wake_pipe_[0] = -1;
    }
#endif
  }

  void Wake() {
#if defined(__unix__) || defined(__APPLE__)
    std::lock_guard lock(wake_mutex);
    if (wake_pipe_[1] < 0) {
      return;
    }
    const char byte = 1;
    ssize_t written = ::write(wake_pipe_[1], &byte, 1);
    (void)written;  // a full pipe already means a wake is pending
#endif
  }

  void DrainWakePipe() {
#if defined(__unix__) || defined(__APPLE__)
    if (wake_pipe_[0] < 0) {
      return;
    }
    char scratch[64];
    while (::read(wake_pipe_[0], scratch, sizeof(scratch)) > 0) {
    }
#endif
  }

  // Flush every queued outbound message. Runs only on the I/O thread; holds
  // write_mutex across the proc.Write calls so it never races a shutdown-time
  // SendMessageImmediate on stdin. The queue lock is released before writing so a
  // main-thread enqueue never blocks behind a slow write.
  void DrainOutbound() {
    std::deque<QueuedMessage> batch;
    {
      std::lock_guard lock(send_mutex);
      batch.swap(outbound_messages);
    }
    if (batch.empty()) {
      return;
    }
    std::lock_guard wlock(write_mutex);
    for (QueuedMessage& queued : batch) {
      if (!proc.Write(std::move(queued).TakeSerialized())) {
        SetLastError("failed to send message to language server");
        stop_io.store(true, std::memory_order_release);
        return;
      }
    }
  }

  bool SendMessageImmediate(const util::JsonValue& msg, bool allow_during_shutdown = false) {
    if (!allow_during_shutdown && shutting_down.load(std::memory_order_acquire)) {
      return false;
    }
    const std::string serialized = SerializeMessage(msg);
    std::lock_guard wlock(write_mutex);
    return proc.Write(serialized);
  }

  bool SendMessageAfterInitialize(const util::JsonValue& msg) {
    return SendSerializedMessageAfterInitialize(SerializeMessage(msg));
  }

  bool SendSerializedMessageAfterInitialize(std::string serialized) {
    return SendMessageBuilderAfterInitialize(
        [serialized = std::move(serialized)]() mutable { return std::move(serialized); });
  }

  bool SendMessageBuilderAfterInitialize(std::function<std::string()> build_serialized) {
    if (shutting_down.load(std::memory_order_acquire)) {
      return false;
    }
    std::lock_guard lock(send_mutex);
    if (shutting_down.load(std::memory_order_acquire)) {
      return false;
    }
    if (!initialized.load(std::memory_order_acquire)) {
      deferred_messages.push_back(QueuedMessage{
          .serialized = {},
          .build_serialized = std::move(build_serialized),
      });
      return true;
    }
    if (test_stub_mode.load(std::memory_order_acquire)) {
      return true;
    }
    if (!proc.IsRunning()) {
      return false;
    }
    outbound_messages.push_back(QueuedMessage{
        .serialized = {},
        .build_serialized = std::move(build_serialized),
    });
    Wake();
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
    readiness_snapshot = LspClient::ReadinessSnapshot{};
  }

  static int ExtractIndexedCount(std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
        continue;
      }
      int value = 0;
      std::size_t cursor = i;
      while (cursor < text.size() &&
             std::isdigit(static_cast<unsigned char>(text[cursor]))) {
        value = value * 10 + (text[cursor] - '0');
        ++cursor;
      }
      return value;
    }
    return 0;
  }

  void SetProgressReadiness(const util::JsonValue& value) {
    const std::string kind = value["kind"].IsString() ? value["kind"].AsString() : "";
    const std::string title = value["title"].IsString() ? value["title"].AsString() : "";
    const std::string message = value["message"].IsString() ? value["message"].AsString() : "";
    const int percentage = value["percentage"].AsInt(0);

    std::lock_guard lock(mutex);
    if (kind == "end") {
      readiness_snapshot.state = initialized.load(std::memory_order_acquire)
                                     ? LspClient::ReadinessSnapshot::State::Ready
                                     : LspClient::ReadinessSnapshot::State::Starting;
      readiness_snapshot.message =
          initialized.load(std::memory_order_acquire) ? "Ready" : "Starting...";
      readiness_snapshot.indexed_count = 0;
      return;
    }

    readiness_snapshot.state = LspClient::ReadinessSnapshot::State::Indexing;
    readiness_snapshot.message = !message.empty() ? message
                               : !title.empty() ? title
                                                : "Indexing...";
    readiness_snapshot.indexed_count = std::max({ExtractIndexedCount(message),
                                                 ExtractIndexedCount(title), percentage});
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

  // Wait until stdout has data/EOF or an outbound wake arrives. Returns true when
  // stdout should be read. On POSIX this is a single poll() over stdout + the wake
  // pipe, so an idle server makes no fixed-cadence wakeups; elsewhere it degrades
  // to a short read timeout (no wake fd available).
  bool WaitStdoutReadable(int timeout_ms) {
#if defined(__unix__) || defined(__APPLE__)
    if (cached_stdout_fd_ >= 0) {
      pollfd fds[2] = {};
      int nfds = 0;
      const int out_index = nfds;
      fds[nfds].fd = cached_stdout_fd_;
      fds[nfds].events = POLLIN | POLLHUP;
      ++nfds;
      int wake_index = -1;
      if (wake_pipe_[0] >= 0) {
        wake_index = nfds;
        fds[nfds].fd = wake_pipe_[0];
        fds[nfds].events = POLLIN;
        ++nfds;
      }
      const int ready = ::poll(fds, nfds, timeout_ms);
      if (ready <= 0) {
        return false;  // timeout or EINTR — loop drains outbound and re-polls
      }
      if (wake_index >= 0 && (fds[wake_index].revents & POLLIN) != 0) {
        DrainWakePipe();
      }
      const short out_revents = fds[out_index].revents;
      return (out_revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0;
    }
#endif
    // Fallback: no pollable fd; let Read() block briefly and report data directly.
    return true;
  }

  void ParseBufferedMessages() {
    while (true) {
      auto msg_opt = TryParseOneMessage(io_buf);
      if (!msg_opt) break;
      DispatchMessage(std::move(*msg_opt));
    }
  }

  void IoMain() {
    cached_stdout_fd_ = proc.stdout_fd();
    const int read_timeout = cached_stdout_fd_ >= 0 ? 0 : 50;
    while (!stop_io.load(std::memory_order_acquire)) {
      // Parse anything already buffered first: the initialize handoff can leave
      // server-pushed messages in io_buf with no *new* stdout data behind them,
      // and a single read can carry several messages. Dispatch may enqueue replies.
      ParseBufferedMessages();
      DrainOutbound();
      if (stop_io.load(std::memory_order_acquire)) {
        break;
      }
      if (!WaitStdoutReadable(1000)) {
        continue;  // woke for outbound/timeout — re-parse, re-drain, re-poll
      }
      auto chunk = proc.Read(4096, read_timeout);
      if (!chunk) break;  // EOF / fatal read error
      if (!chunk->empty()) io_buf.append(*chunk);
    }
    ParseBufferedMessages();
    DrainOutbound();  // final flush (e.g. an exit notification queued during stop)
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

  // Reply to a server-initiated request. `id` is echoed verbatim (it may be an
  // int or a string per JSON-RPC). Server requests only arrive post-initialize,
  // so these flow through the normal outbound queue.
  void SendResponseResult(const util::JsonValue& id, util::JsonValue result) {
    using namespace util;
    JsonObject msg;
    msg["jsonrpc"] = JsonValue("2.0");
    msg["id"] = id;
    msg["result"] = std::move(result);
    SendMessageAfterInitialize(JsonValue(std::move(msg)));
  }

  void SendResponseError(const util::JsonValue& id, int code, std::string message) {
    using namespace util;
    JsonObject error;
    error["code"] = JsonValue(static_cast<std::int64_t>(code));
    error["message"] = JsonValue(std::move(message));
    JsonObject msg;
    msg["jsonrpc"] = JsonValue("2.0");
    msg["id"] = id;
    msg["error"] = JsonValue(std::move(error));
    SendMessageAfterInitialize(JsonValue(std::move(msg)));
  }

  // Server -> client requests must always get a reply, or chatty servers
  // (clangd, Roslyn/OmniSharp) log errors or stall.
  void HandleServerRequest(const util::JsonValue& id, const std::string& method,
                           const util::JsonValue& params) {
    using namespace util;
    if (method == "workspace/configuration") {
      JsonArray result;
      if (params["items"].IsArray()) {
        for (const auto& item : params["items"].AsArray()) {
          const std::string section =
              item["section"].IsString() ? item["section"].AsString() : "";
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
      // Host-side edit application is not wired here; report not-applied so the
      // server can recover rather than wait forever.
      JsonObject obj;
      obj["applied"] = JsonValue(false);
      SendResponseResult(id, JsonValue(std::move(obj)));
      return;
    }
    SendResponseError(id, -32601, "method not found: " + method);
  }

  void DispatchMessage(util::JsonValue msg) {
    const bool has_method = msg.HasKey("method") && msg["method"].IsString();
    const bool has_id = msg.HasKey("id") && !msg["id"].IsNull();
    if (shutting_down.load(std::memory_order_acquire)) {
      if (has_id && !has_method && msg["id"].IsInt()) {
        const int id = msg["id"].AsInt();
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
    if (has_id && !has_method && msg["id"].IsInt()) {
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
        std::vector<Diagnostic> diags = lsp_protocol::ParseDiagnostics(params["diagnostics"]);
        std::lock_guard lock(mutex);
        if (diagnostics_callback) {
          auto cb = diagnostics_callback;
          ready_callbacks.push_back([cb = std::move(cb), u = std::move(uri), ds = std::move(diags)]() mutable {
            cb(std::move(u), std::move(ds));
          });
        }
        PushWakeEvent();
      } else if (method == "$/progress") {
        SetProgressReadiness(msg["params"]["value"]);
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

  // Shared scaffolding for every async request: register the response handler,
  // send the request, and on send failure clean up and invoke the failure path.
  // Keeps the per-method code to "build params" + "parse result".
  void DispatchRequest(const std::string& method, util::JsonValue params,
                       std::function<void(util::JsonValue)> response_handler,
                       std::function<void()> on_send_failure) {
    const int id = RegisterPendingRequest(std::move(response_handler));
    if (!SendMessageAfterInitialize(MakeRequest(id, method, std::move(params)))) {
      RemovePendingRequest(id);
      on_send_failure();
    }
  }

  void DoInitializeBlocking() {
    util::StartupTrace::Scope trace_scope("LspClient::DoInitializeBlocking");
    initializing.store(true, std::memory_order_release);
    {
      std::lock_guard lock(mutex);
      readiness_snapshot.state = LspClient::ReadinessSnapshot::State::Starting;
      readiness_snapshot.message = "Starting...";
      readiness_snapshot.indexed_count = 0;
    }

    using namespace util;
    JsonObject text_doc_sync;
    text_doc_sync["dynamicRegistration"] = JsonValue(false);
    text_doc_sync["willSave"] = JsonValue(false);
    text_doc_sync["didSave"] = JsonValue(true);

    JsonObject completion_item_caps;
    completion_item_caps["snippetSupport"] = JsonValue(true);
    {
      JsonArray doc_formats;
      doc_formats.push_back(JsonValue("markdown"));
      doc_formats.push_back(JsonValue("plaintext"));
      completion_item_caps["documentationFormat"] = JsonValue(std::move(doc_formats));
    }
    JsonObject completion_caps;
    completion_caps["dynamicRegistration"] = JsonValue(false);
    completion_caps["completionItem"] = JsonValue(std::move(completion_item_caps));

    JsonObject hover_caps;
    hover_caps["dynamicRegistration"] = JsonValue(false);
    {
      JsonArray hover_formats;
      hover_formats.push_back(JsonValue("markdown"));
      hover_formats.push_back(JsonValue("plaintext"));
      hover_caps["contentFormat"] = JsonValue(std::move(hover_formats));
    }

    JsonObject signature_caps;
    signature_caps["dynamicRegistration"] = JsonValue(false);

    JsonObject definition_caps;
    definition_caps["dynamicRegistration"] = JsonValue(false);
    definition_caps["linkSupport"] = JsonValue(true);

    JsonObject references_caps;
    references_caps["dynamicRegistration"] = JsonValue(false);

    JsonObject rename_caps;
    rename_caps["dynamicRegistration"] = JsonValue(false);
    rename_caps["prepareSupport"] = JsonValue(false);

    JsonObject code_action_caps;
    code_action_caps["dynamicRegistration"] = JsonValue(false);
    {
      JsonArray kinds;
      for (const char* kind : {"quickfix", "refactor", "refactor.extract", "refactor.inline",
                               "refactor.rewrite", "source", "source.organizeImports"}) {
        kinds.push_back(JsonValue(kind));
      }
      JsonObject kind_obj;
      kind_obj["valueSet"] = JsonValue(std::move(kinds));
      JsonObject literal;
      literal["codeActionKind"] = JsonValue(std::move(kind_obj));
      code_action_caps["codeActionLiteralSupport"] = JsonValue(std::move(literal));
    }

    JsonObject formatting_caps;
    formatting_caps["dynamicRegistration"] = JsonValue(false);

    JsonObject document_symbol_caps;
    document_symbol_caps["dynamicRegistration"] = JsonValue(false);

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
    text_document_caps["documentSymbol"] = JsonValue(std::move(document_symbol_caps));

    JsonObject workspace_caps;
    workspace_caps["configuration"] = JsonValue(true);
    workspace_caps["workspaceFolders"] = JsonValue(true);
    workspace_caps["applyEdit"] = JsonValue(false);
    {
      JsonObject did_change_config;
      did_change_config["dynamicRegistration"] = JsonValue(false);
      workspace_caps["didChangeConfiguration"] = JsonValue(std::move(did_change_config));
    }

    JsonObject window_caps;
    window_caps["workDoneProgress"] = JsonValue(true);

    JsonObject caps;
    caps["textDocument"] = JsonValue(std::move(text_document_caps));
    caps["workspace"] = JsonValue(std::move(workspace_caps));
    caps["window"] = JsonValue(std::move(window_caps));

    JsonObject client_info;
    client_info["name"] = JsonValue("microide");

    JsonObject init_params;
#if defined(__unix__) || defined(__APPLE__)
    init_params["processId"] = JsonValue(static_cast<std::int64_t>(::getpid()));
#else
    init_params["processId"] = JsonValue(static_cast<std::int64_t>(0));
#endif
    init_params["clientInfo"] = JsonValue(std::move(client_info));
    init_params["rootUri"] = JsonValue(root_uri);
    if (!root_uri.empty()) {
      JsonObject folder;
      folder["uri"] = JsonValue(root_uri);
      const auto slash = root_uri.find_last_of('/');
      folder["name"] =
          JsonValue(slash == std::string::npos ? root_uri : root_uri.substr(slash + 1));
      JsonArray folders;
      folders.push_back(JsonValue(std::move(folder)));
      init_params["workspaceFolders"] = JsonValue(std::move(folders));
    }
    if (!initialization_options.IsNull()) {
      init_params["initializationOptions"] = initialization_options;
    }
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

    ReadBuf& buf = io_buf;
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
      // No I/O thread is running yet, so this is the only writer — write directly.
      bool sent_initialized = false;
      {
        std::lock_guard wlock(write_mutex);
        sent_initialized = proc.Write(
            SerializeMessage(MakeNotification("initialized", JsonValue(JsonObject{}))));
      }
      if (!sent_initialized) {
        SetLastError("failed to send initialized notification to language server");
        deferred_messages.clear();
        ShutdownProcessOnce();
        initializing.store(false, std::memory_order_release);
        return;
      }
      initialized.store(true, std::memory_order_release);
      // Push configuration before any queued didOpen so servers that read
      // settings (clangd, OmniSharp/Roslyn) see them before touching documents.
      if (settings.IsObject()) {
        JsonObject config_params;
        config_params["settings"] = settings;
        outbound_messages.push_back(QueuedMessage{
            .serialized = SerializeMessage(MakeNotification(
                "workspace/didChangeConfiguration", JsonValue(std::move(config_params)))),
            .build_serialized = {},
        });
      }
      for (QueuedMessage& message : deferred_messages) {
        outbound_messages.push_back(std::move(message));
      }
      deferred_messages.clear();
    }

    OpenWakePipe();
    stop_io.store(false, std::memory_order_release);
    io_thread = std::thread([this]() { IoMain(); });
    Wake();  // flush the queued config/didOpen without waiting for the first poll

    {
      std::lock_guard lock(mutex);
      if (readiness_snapshot.state != LspClient::ReadinessSnapshot::State::Indexing) {
        readiness_snapshot.state = LspClient::ReadinessSnapshot::State::Ready;
        readiness_snapshot.message = "Ready";
        readiness_snapshot.indexed_count = 0;
      }
    }
    initializing.store(false, std::memory_order_release);
  }

  void DoShutdown() {
    TraceLspLifecycle(language_id, proc.pid(), "shutdown-begin");
    shutting_down.store(true, std::memory_order_release);
    stop_init.store(true);
    if (test_stub_mode.load(std::memory_order_acquire)) {
      ClearDeferredMessages();
      ResetProtocolState();
      {
        std::lock_guard hook_lock(mutex);
        test_document_symbol_handler = nullptr;
      }
      initialized.store(false, std::memory_order_release);
      initializing.store(false, std::memory_order_release);
      supports_incremental_sync.store(false, std::memory_order_release);
      test_stub_mode.store(false, std::memory_order_release);
      shutting_down.store(false, std::memory_order_release);
      shutdown_complete.store(true, std::memory_order_release);
      PushWakeEvent();
      return;
    }
    if (!initialized.load(std::memory_order_acquire)) {
      TraceLspLifecycle(language_id, proc.pid(), "preinit-cancel");
      stop_io.store(true, std::memory_order_release);
      Wake();
      ShutdownProcessOnce(0);
      if (init_thread.joinable()) {
        init_thread.join();
      }
      // The init thread may have just promoted itself to the running state and
      // launched the I/O thread between our check and here; join it if so.
      if (io_thread.joinable()) {
        io_thread.join();
      }
      ClearDeferredMessages();
      CloseWakePipe();
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
    // Keep the I/O thread running through the handshake below so it can receive
    // the shutdown response; its writes are serialized with ours via write_mutex.
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

    stop_io.store(true, std::memory_order_release);
    Wake();
    if (proc.IsRunning()) {
      TraceLspLifecycle(language_id, proc.pid(), "forced-shutdown");
      ShutdownProcessOnce(1000);
    }

    if (io_thread.joinable()) {
      io_thread.join();
    }
    CloseWakePipe();

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
