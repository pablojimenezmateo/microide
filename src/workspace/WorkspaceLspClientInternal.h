#pragma once

#include "workspace/WorkspaceLspClient.h"
#include "workspace/LspProtocol.h"
#include "util/MainThreadMailbox.h"

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
#include "workspace/LspClientTrace.h"
#include "workspace/LspMessageFraming.h"

namespace microide::workspace {

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
  // Serializes every proc.Write so stdin is never written by two threads. A
  // timed_mutex so the shutdown path can bound its acquisition: the io_thread can
  // hold this across a proc.Write that blocks indefinitely on a wedged-but-alive
  // server, and the shutdown's force-kill (which unblocks that write) must not be
  // gated behind acquiring this lock.
  std::timed_mutex write_mutex;

  // Single I/O thread state. One thread per server reads stdout and writes stdin;
  // it blocks in poll() over stdout + a self-pipe wakeup, so it makes no
  // fixed-cadence idle wakeups and reacts immediately to data or new outbound.
  // framer_ is filled first by the initialize handshake and then handed to the I/O
  // thread; keeping it a member preserves any bytes the server pushed right after
  // the initialize response (e.g. clangd's early registerCapability / progress /
  // configuration requests) across that handoff, along with any partial frame.
  LspMessageFramer framer_;
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

  // Per-request pending callbacks (keyed by request id), guarded by mutex. Each
  // carries a deadline so a silent server can never strand a request (and its UI
  // loading state) forever: the I/O loop sweeps expired requests and synthesizes an
  // empty response, which every handler treats as "no result". Mirrors the DAP
  // client's timeout sweep.
  struct PendingRequest {
    std::function<void(util::JsonValue)> callback;
    std::chrono::steady_clock::time_point deadline;
  };
  std::unordered_map<int, PendingRequest> pending_requests;

  // Callbacks marshalled to the main thread and drained once per frame. Carries
  // its own mutex + SDL wake event, so it is the innermost lock: producers may
  // post while holding `mutex`, and DrainCallbacks() never takes `mutex`.
  util::MainThreadMailbox main_mailbox;

  // When true, the client behaves as a connected server for unit tests (no subprocess).
  std::atomic<bool> test_stub_mode{false};
  std::function<void(std::string, DocumentSymbolCallback)> test_document_symbol_handler;
  std::function<void(std::string, SemanticTokensCallback)> test_semantic_tokens_handler;
  std::function<void(std::string, HoverCallback)> test_hover_handler;
  std::function<void(std::string, FormattingCallback)> test_formatting_handler;
  std::function<void(std::string, std::string, RenameCallback)> test_rename_handler;
  std::function<void(std::string, Position, CompletionCallback)> test_completion_handler;
  std::function<void(std::string, Position, SignatureHelpCallback)> test_signature_help_handler;

  // Server semantic-token legend (index -> type name), captured at initialize.
  // Guarded by `mutex`. Empty when the server advertises no semanticTokens provider.
  std::vector<std::string> semantic_token_types;
  std::atomic<bool> supports_semantic_tokens{false};

  // Negotiated position encoding (LSP `capabilities.positionEncoding`), captured at
  // initialize. Guarded by `mutex`. We advertise utf-8 first, so a conformant server
  // that supports it reports "utf-8" and our editor byte offsets are then exact LSP
  // positions with zero conversion. Per spec the default when unreported is "utf-16".
  std::string position_encoding = "utf-16";

  // Diagnostics callback — set from main thread, called on main thread via main_mailbox.
  OnPublishDiagnostics diagnostics_callback;
  // Server-initiated workspace/applyEdit handler — set from the main thread and
  // invoked on the main thread (it mutates buffers / writes files); returns
  // whether the edit was applied. Null => reply applied:false.
  std::function<bool(WorkspaceEdit)> apply_edit_handler;

  std::unordered_map<std::string, int> document_versions;
  std::deque<QueuedMessage> deferred_messages;
  std::deque<QueuedMessage> outbound_messages;
  // One-shot guard so the wedged-server overflow warning logs once, not per message.
  bool outbound_overflow_logged_ = false;
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
  // SendMessageImmediate on stdin. Defined in WorkspaceLspClientTransport.cpp.
  void DrainOutbound();

  // lock_timeout > 0 bounds how long we wait for write_mutex before giving up:
  // used only by the shutdown path so a stuck io_thread write cannot wedge
  // teardown. Returns false on lock timeout (as well as on a failed write); the
  // shutdown caller treats both as "graceful send failed" and force-kills.
  // Defined in WorkspaceLspClientTransport.cpp (SendMessageImmediate keeps this
  // exact qualified name — tsan.supp targets it by mangled symbol).
  bool SendMessageImmediate(const util::JsonValue& msg, bool allow_during_shutdown = false,
                            std::chrono::milliseconds lock_timeout = std::chrono::milliseconds::zero());

  bool SendMessageAfterInitialize(const util::JsonValue& msg) {
    return SendSerializedMessageAfterInitialize(SerializeMessage(msg));
  }

  bool SendSerializedMessageAfterInitialize(std::string serialized) {
    return SendMessageBuilderAfterInitialize(
        [serialized = std::move(serialized)]() mutable { return std::move(serialized); });
  }

  // Queue a message for the I/O thread (deferred until initialized). Refuses on a
  // wedged/dead server rather than growing unbounded. Defined in
  // WorkspaceLspClientTransport.cpp.
  bool SendMessageBuilderAfterInitialize(std::function<std::string()> build_serialized);

  void ClearDeferredMessages() {
    std::lock_guard lock(send_mutex);
    deferred_messages.clear();
    outbound_messages.clear();
  }

  void ResetProtocolState() {
    std::lock_guard lock(mutex);
    pending_requests.clear();
    main_mailbox.Clear();
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

  // Update the readiness snapshot from a $/progress notification's value. Defined
  // in WorkspaceLspClientLifecycle.cpp.
  void SetProgressReadiness(const util::JsonValue& value);

  void ShutdownProcessOnce(int timeout_ms = 3000) {
    bool expected = false;
    if (!process_shutdown_started.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      return;
    }
    proc.Shutdown(timeout_ms);
  }

  // Wait until stdout has data/EOF or an outbound wake arrives. Returns true when
  // stdout should be read. On POSIX this is a single poll() over stdout + the wake
  // pipe, so an idle server makes no fixed-cadence wakeups; elsewhere it degrades
  // to a short read timeout. Defined in WorkspaceLspClientTransport.cpp.
  bool WaitStdoutReadable(int timeout_ms);

  void ParseBufferedMessages() {
    while (true) {
      auto msg_opt = framer_.Next();
      if (!msg_opt) break;
      DispatchMessage(std::move(*msg_opt));
    }
  }

  // The single I/O thread body: parse buffered frames, drain outbound, sweep
  // timed-out requests, then poll stdout. Defined in
  // WorkspaceLspClientTransport.cpp.
  void IoMain();

  // Reply to a server-initiated request. `id` is echoed verbatim (it may be an
  // int or a string per JSON-RPC). Defined in WorkspaceLspClientDispatch.cpp.
  void SendResponseResult(const util::JsonValue& id, util::JsonValue result);
  void SendResponseError(const util::JsonValue& id, int code, std::string message);

  // Server -> client requests must always get a reply, or chatty servers
  // (clangd, Roslyn/OmniSharp) log errors or stall. Defined in
  // WorkspaceLspClientDispatch.cpp.
  void HandleServerRequest(const util::JsonValue& id, const std::string& method,
                           const util::JsonValue& params);

  // Route one inbound frame: response callback, server request, diagnostics, or
  // progress. Defined in WorkspaceLspClientDispatch.cpp.
  void DispatchMessage(util::JsonValue msg);

  int RegisterPendingRequest(std::function<void(util::JsonValue)> cb) {
    std::lock_guard lock(mutex);
    const int id = next_id++;
    pending_requests[id] = PendingRequest{
        .callback = std::move(cb),
        .deadline = std::chrono::steady_clock::now() + kLspRequestTimeout,
    };
    return id;
  }

  void RemovePendingRequest(int id) {
    std::lock_guard lock(mutex);
    pending_requests.erase(id);
  }

  // Synthesize empty responses for pending requests so a non-responding server never
  // strands a request (and its UI loading state). `only_expired` fails just those
  // past their deadline (the periodic sweep); otherwise fails all (the server exited
  // and no response can ever arrive). Runs on the I/O thread: handlers are posted to
  // the main-thread mailbox. The empty `{}` envelope has no "result" key, so every
  // response handler degrades to its no-result path.
  void FailPendingRequests(bool only_expired);

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

  // Result-shaped request helper: owns the boilerplate every feature request
  // repeats — the empty-`callback` guard, the "no `result` key" -> nullopt path,
  // and the send-failure -> nullopt path — so a request method reduces to "build
  // params + parse result". `parse_result` maps the response's `result` value to
  // the callback's argument (an `std::optional<...>`); the absent/failure cases
  // deliver a default-constructed argument (i.e. `std::nullopt`).
  template <typename Callback, typename Parser>
  void DispatchResultRequest(const std::string& method, util::JsonValue params, Callback callback,
                             Parser parse_result) {
    if (!callback) {
      return;
    }
    Callback failure = callback;
    DispatchRequest(
        method, std::move(params),
        [cb = std::move(callback), parse_result = std::move(parse_result)](
            util::JsonValue resp) mutable {
          if (!resp.HasKey("result")) {
            cb({});
            return;
          }
          cb(parse_result(resp["result"]));
        },
        [failure = std::move(failure)]() mutable { failure({}); });
  }

  // The blocking initialize handshake: advertise client capabilities, read and
  // parse the server's initialize response (sync kind, position encoding,
  // semantic-token legend), send `initialized`, then flush queued config/didOpen
  // and spawn the I/O thread. Runs on init_thread. Defined in
  // WorkspaceLspClientLifecycle.cpp.
  void DoInitializeBlocking();

  // Graceful shutdown: shutdown/exit handshake with bounded waits, then join the
  // I/O thread and force-kill if the server does not exit. Runs on
  // shutdown_thread. Defined in WorkspaceLspClientLifecycle.cpp.
  void DoShutdown();

  void BeginShutdown();
  void WaitForShutdown();
};

}  // namespace microide::workspace
